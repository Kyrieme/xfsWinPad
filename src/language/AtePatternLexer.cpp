// xfsWinPad - ATE Pattern (.pat) 词法器实现（批次 72）
//
// 【本词法器存在的理由】
//   .pat 是 ATE 向量/时序文件的通用后缀（Chroma、Teradyne、Advantest 各家方言
//   不同但记号种类一致）。通用词法器对它无能为力：向量数据全是 0/1/X 单字符，
//   会被当成普通文本；而 0/1 又是计数器的取值，纯数字高亮会把两者混为一谈。
//   所以这里做的是**结构性**判据而不是词表堆砌：先判断整行是不是向量行，再在
//   向量行内把单字符分成驱动 / 比较 / 掩码三类 —— pattern 调试最常问的就是
//   「这一拍是驱动还是比较」，这是本词法器要直接回答的问题。
//
// 【判据：什么算向量行】
//   整行打分，≥2 分即视为向量行：单个向量记号（0 1 X H L T Z N U D，含小写）+1；
//   ≥4 位的纯 01 位块（0101 这种挤在一起的写法）+2。行首记号若是已知 opcode 则
//   一律否定。于是 `RPT 10`、`LBL_MAIN:`、`TSET ts1` 不会被误判，而
//   `0 1 X`、`( P1 P2 ) 1 0 H`、`0101` 会被正确识别。
//   位块给 2 分是因为它不可能来自计数：`10`/`20` 这类两位数在配置行里是普通
//   数值，所以位块要求 ≥4 位，才算压倒性的向量证据。
//
// 【行局部性】
//   本语言的注释（# //）与字符串都不跨行，因此整个词法器是**行局部**的：每个
//   窗口都从行首开始（基类分块保证），每行独立完成样式，不携带跨行状态。这让
//   分块拼接绝对安全，也不存在「状态丢失导致样式整体错位」的风险。
//
// 【折叠】按缩进（tab 展开为 4 列）：下一行缩进更深的行 = 块头。
//   ATE 向量文件普遍以缩进表达 pattern 内的层级（label 下的向量块），该判据
//   不需要理解各家方言的语法即可成立，比死记 ENDxxx 关键字更稳。

#include "AtePatternLexer.h"
#include "XfsLexerStyles.h"

#include <vector>

namespace xfs {

namespace {

// ---- 记号字符判据 -------------------------------------------------------------

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsHexDigit(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}
inline bool IsIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) ||
           c == '_' || c == '.' || c == '$';
}

// 是否 ≥4 位的纯 01 串（向量位块）。长度门槛很关键：`10`/`20` 这种两位数
// 在配置行里是普通数值，只有 ≥4 位的 01 串才是压倒性的向量证据。
bool IsBitBlock(const char* s, std::size_t n) {
    if (n < 4) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (s[i] != '0' && s[i] != '1') return false;
    return true;
}

// 向量字符三分类。D 归掩码而不是驱动：在 pattern 文件里 D 更常表示
// "don't care / disable" 而非 STIL 那种 drive 语义，放掩码色更不容易误导。
enum VecClass { VecNone, VecDrive, VecExpect, VecMask };

VecClass VectorClass(char c) {
    switch (c) {
        case '0': case '1': return VecDrive;
        case 'H': case 'L': case 'T': case 'h': case 'l': case 't': return VecExpect;
        case 'X': case 'N': case 'Z': case 'U': case 'D':
        case 'x': case 'n': case 'z': case 'u': case 'd': return VecMask;
        default: return VecNone;
    }
}

int VectorStyle(char c) {
    switch (VectorClass(c)) {
        case VecDrive:  return SCE_ATEP_VECTOR;
        case VecExpect: return SCE_ATEP_EXPECT;
        case VecMask:   return SCE_ATEP_MASK;
        default:        return SCE_ATEP_DEFAULT;
    }
}

// ---- 内置记号表 ---------------------------------------------------------------
// 放词法器里而不是 LanguageMap 里：LanguageInfo 只有 keywords[2] 两个槽位，
// 而本语言需要「opcode / pin / timing / 跳转」四张表。

const char* const kOpcodes =
    "RPT RPTE RPTD JMP JMPC JMPZ JMPNZ GOTO CALL CALLS RET RETC LBL LABEL SEQ "
    "SUBR SUB SUBR END ENDE ENDS ENDC HALT STOP PAUSE WAIT TRIG TRIGGER SYNC DUP "
    "LOOP WHILE IF THEN ELSE ENDIF SET CLR CLEAR INC DEC IDX IDXI REG REGS DCAP "
    "DCAS DCSS CAPTURE MASK NOOP NOP RESET INIT PATTERN PATTERNSET MAIN BURST "
    "BURSTS EXEC START STOPP BEG BEGIN FINISH DONE ENABLE DISABLE PINS PINLIST "
    "GROUPS GROUP POR TAP SCAN SHIFT NIBBLE BYTE WORD DWORD DATA ADDR";

const char* const kJumpOps =
    "JMP JMPC JMPZ JMPNZ GOTO CALL CALLS LOOP WHILE";

const char* const kTimingRefs =
    "TSET TSETS TIM TIMSET EDGE EDGES PERIOD PERIODS RATE CLOCK CLOCKS RESOLUTION "
    "LEVELS LEVEL VIL VIH VOL VOH VTERM VTH VTIL VTIH DRIVE COMPARE STROBE "
    "STROBES WMODE TS";

// 常见电源/时钟/JTAG 信号名。这是「初始线索」而非白名单——文件里其他 pin 名
// 靠结构判据（pin 列表行 / 括号 pin 组）识别，不依赖这张表。
const char* const kCommonPins =
    "VDD VSS VCC VEE VCCIO VDDIO VDDQ AVDD AVSS DVDD DVSS CLK CLOCK XCLK TCLK "
    "RESET RST RESETN EN ENABLE TRIG STROBE TEST TCK TMS TDI TDO TRST OSC";

// ---- 整行预判 ----------------------------------------------------------------

// 行首首个非空白记号是否是 opcode（用于否定向量行 / pin 列表行）。
bool FirstTokenIsOpcode(const char* s, std::size_t n, const KeywordSet& ops) {
    std::size_t i = 0;
    while (i < n && IsSpace(s[i])) ++i;
    const std::size_t t0 = i;
    while (i < n && !IsSpace(s[i])) ++i;
    return i > t0 && ops.Has(s + t0, i - t0);
}

bool IsVectorLine(const char* s, std::size_t n, const KeywordSet& ops) {
    if (FirstTokenIsOpcode(s, n, ops)) return false;
    int score = 0;
    std::size_t i = 0;
    while (i < n) {
        if (IsSpace(s[i])) { ++i; continue; }
        // 注释截断：注释里出现的 0/1/X 不算向量记号
        if (s[i] == '#' || (s[i] == '/' && i + 1 < n && s[i + 1] == '/')) break;
        std::size_t j = i;
        while (j < n && !IsSpace(s[j])) ++j;
        if (j - i == 1 && VectorClass(s[i]) != VecNone) score += 1;
        else if (IsBitBlock(s + i, j - i)) score += 2;   // `0101` 是很强的向量证据
        i = j;
    }
    return score >= 2;
}

// 「pin 列表行」：所有记号都是标识符（容许 ( ) , ; 分隔），≥2 个，且首记号不是
// opcode。用于识别 Chroma 那种 ( P1 P2 P3 ) 的 pin 顺序头，跨行书写也能覆盖。
// 首记号是 **timing 引用**时同样否定：`TSET ts_main` 的形状与 pin 目录行完全
// 一样（两个标识符、首记号不是 opcode），但它的第二列是 timing set 名而不是
// pin；不排除的话 ts_main 会被染成 pin 色。
bool IsPinListLine(const char* s, std::size_t n, const KeywordSet& ops,
                   const KeywordSet& timing) {
    int idents = 0;
    bool first = true;
    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (IsSpace(c)) { ++i; continue; }
        if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) break;
        if (c == '(' || c == ')' || c == ',' || c == ';') { ++i; continue; }
        if (!IsIdentStart(c)) return false;
        std::size_t j = i;
        while (j < n && IsIdentChar(s[j])) ++j;
        if (first && (ops.Has(s + i, j - i) || timing.Has(s + i, j - i))) return false;
        ++idents;
        first = false;
        i = j;
    }
    return idents >= 2;
}

// ---- 单行词法 -----------------------------------------------------------------

struct LineCtx {
    bool vectorLine = false;
    bool pinListLine = false;
};

void LexLine(const char* s, std::size_t n, const LineCtx& ctx,
             const KeywordSet& ops, const KeywordSet& pins,
             const KeywordSet& timing, const KeywordSet& jumps,
             XfsStyleSink& sink) {
    std::size_t i = 0;
    bool firstToken = true;
    bool inPinGroup = false;    // 本行 '(' 之后的记号
    bool expectLabel = false;   // 刚读过跳转 opcode，下一个标识符是跳转目标

    while (i < n) {
        const char c = s[i];

        // 行注释：# 或 //，直到底
        if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
            sink.Push(SCE_ATEP_COMMENT, n - i);
            return;
        }

        // 空白
        if (IsSpace(c)) {
            std::size_t j = i;
            while (j < n && IsSpace(s[j])) ++j;
            sink.Push(SCE_ATEP_DEFAULT, j - i);
            i = j;
            continue;
        }

        // 字符串：到同引号或行尾（不跨行）
        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < n && s[j] != c) ++j;
            if (j < n) ++j;
            sink.Push(SCE_ATEP_STRING, j - i);
            i = j;
            firstToken = false;
            continue;
        }

        // 数字 / 0x 十六进制 / 小数
        if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(s[i + 1]))) {
            std::size_t j = i;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                j += 2;
                while (j < n && IsHexDigit(s[j])) ++j;
                sink.Push(SCE_ATEP_HEX, j - i);
            } else {
                if (s[j] == '.') ++j;                     // .5 形式
                while (j < n && IsDigit(s[j])) ++j;
                if (j < n && s[j] == '.') {
                    ++j;
                    while (j < n && IsDigit(s[j])) ++j;
                }
                bool allBits = true;
                for (std::size_t k = i; k < j; ++k)
                    if (s[k] != '0' && s[k] != '1') { allBits = false; break; }
                // 向量行里的 01 串是驱动位块，不是计数器。
                // 这里**不能**加长度门槛：数字分支在标识符分支之前，向量行里的
                // 单个 `0`/`1` 走的就是这里，而它与同一拍的字母 `H`/`X` 是同一
                // 维度的数据；若只给字母上色，同一列向量会一半有色一半无色。
                // 非向量行不满足 ctx.vectorLine，`RPT 10` 这类计数照样是号码色。
                const int st = (ctx.vectorLine && allBits)
                                   ? SCE_ATEP_VECTOR
                                   : SCE_ATEP_NUMBER;
                sink.Push(st, j - i);
            }
            i = j;
            firstToken = false;
            continue;
        }

        // 标识符：pin / opcode / timing / 标签 / 普通名
        if (IsIdentStart(c)) {
            std::size_t j = i;
            while (j < n && IsIdentChar(s[j])) ++j;
            const std::size_t idLen = j - i;
            int style = SCE_ATEP_DEFAULT;

            // 判定顺序即优先级。这里有两条「上下文压过词表」的规则，都是被真实
            // 文件逼出来的（见 temp/ate_samples/sample.pat 的 dump）：
            //   1) 标签定义 NAME: 先于任何词表 —— MAIN:/RESET: 是定义而非指令；
            //   2) pin 上下文（括号 pin 组 / pin 目录行）先于 timing/opcode 表 ——
            //      RESET/TRIG/STROBE 同时躺在 opcode、timing、pin 三张表里，只有
            //      上下文能消歧：`( VDD RESET )` 是 pin 列表，裸 `RESET` 是复位
            //      指令。原先纯按表顺序判定，pin 组里的 RESET 会变成 opcode 色，
            //      同一个括号里的 VDD 却是 pin 色 —— 一眼就能看出不对。
            if (idLen == 1 && ctx.vectorLine) {
                style = VectorStyle(s[i]);
            } else if (c == '.') {
                style = SCE_ATEP_DIRECTIVE;               // .INCLUDE / .SETUP
            } else if (expectLabel) {
                style = SCE_ATEP_LABEL;                   // 跳转目标
            } else if (j < n && s[j] == ':') {
                style = SCE_ATEP_LABEL;                   // NAME: 标签定义
            } else if (inPinGroup || ctx.pinListLine) {
                style = SCE_ATEP_PIN;                     // 上下文：pin 组 / pin 目录
            } else if (timing.Has(s + i, idLen)) {
                style = SCE_ATEP_TIMING;
            } else if (ops.Has(s + i, idLen)) {
                style = SCE_ATEP_OPCODE;
            } else if (pins.Has(s + i, idLen)) {
                style = SCE_ATEP_PIN;
            }

            sink.Push(style, idLen);

            // 跳转 opcode 之后紧跟的标识符是目标标签
            expectLabel = (style == SCE_ATEP_OPCODE) && jumps.Has(s + i, idLen);

            i = j;
            firstToken = false;
            continue;
        }

        // 其余一律运算符；顺带维护括号 pin 组
        if (c == '(') inPinGroup = true;
        else if (c == ')') inPinGroup = false;
        sink.PushOne(SCE_ATEP_OPERATOR);
        ++i;
        firstToken = false;
    }
    (void)firstToken;
}

// ---- 缩进折叠 ----------------------------------------------------------------

int IndentWidth(const char* s, std::size_t n) {
    int col = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (s[i] == ' ') ++col;
        else if (s[i] == '\t') col = (col / 4 + 1) * 4;
        else break;
    }
    return col;
}

bool IsBlank(const char* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (s[i] != ' ' && s[i] != '\t') return false;
    return true;
}

} // namespace

AtePatternLexer::AtePatternLexer() : XfsLexerBase(kLexAtePattern, 7201) {
    MutableWords(0).Set(kOpcodes);
    MutableWords(1).Set(kCommonPins);
    MutableWords(2).Set(kTimingRefs);
    MutableWords(3).Set(kJumpOps);
}

int AtePatternLexer::LexSpan(const char* text, std::size_t len, Sci_Position /*basePos*/,
                             int /*initStyle*/, XfsStyleSink& sink) {
    const KeywordSet& ops = Words(0);
    const KeywordSet& pins = Words(1);
    const KeywordSet& timing = Words(2);
    const KeywordSet& jumps = Words(3);

    ForEachLine(text, len, [&](const char* line, std::size_t lineLen, std::size_t eol) {
        LineCtx ctx;
        ctx.vectorLine = IsVectorLine(line, lineLen, ops);
        ctx.pinListLine = !ctx.vectorLine && IsPinListLine(line, lineLen, ops, timing);
        LexLine(line, lineLen, ctx, ops, pins, timing, jumps, sink);
        if (eol) sink.Push(SCE_ATEP_DEFAULT, eol);
    });

    // 行局部语言：窗口之间不携带状态
    return SCE_ATEP_DEFAULT;
}

void AtePatternLexer::FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                               Sci_Position last) {
    if (!doc || last < first) return;

    const Sci_Position lineCount = doc->LineFromPosition(doc->Length());
    if (first > lineCount) return;
    if (last > lineCount) last = lineCount;

    // 缩进栈必须从行 0 建起才能得到正确的绝对层级。扫描是廉价的，真正有代价的
    // 是 SetLevel（会向文档发通知），所以只对 [first,last] 回写层级。
    // 采用一次前视：读到第 N+1 行时才写第 N 行，因为「是否块头」取决于下一行缩进。
    std::string text;
    std::vector<int> indent;
    indent.reserve((std::size_t)last + 2);

    int lastNonBlank = 0;
    int prevLevel = 0;
    bool prevBlank = true;
    bool havePrev = false;
    Sci_Position prevLineNo = -1;

    for (Sci_Position line = 0; line <= last; ++line) {
        LineText(doc, line, &text);
        const bool white = IsBlank(text.data(), text.size());
        int lvl;
        if (white) {
            lvl = lastNonBlank;
        } else {
            const int ind = IndentWidth(text.data(), text.size());
            // 弹出**更**深的缩进（严格 >）：同缩进的行属于同一层，不能互相弹掉。
            // 原来写成 >= 时，「连续两行同为块内容」的第二行会退化成父层，于是
            // 「下一行更深 → 本行是块头」永远不成立，pattern 块根本折不起来。
            // 另外只在缩进真正变深时才压栈，避免同值重复入栈把层级越撑越高。
            while (!indent.empty() && indent.back() > ind) indent.pop_back();
            if (indent.empty() || indent.back() < ind) indent.push_back(ind);
            lvl = (int)indent.size() - 1;
            lastNonBlank = lvl;
        }

        if (havePrev) {
            if (prevLineNo >= first) {
                // 空白行永远不当块头：空行之后的更深缩进属于下一个块，
                // 让空行可折叠会在折叠树里多出一层没有内容的节点。
                const bool header = !prevBlank && (lvl > prevLevel);
                SetFoldLine(doc, prevLineNo, prevLevel, lvl, prevBlank, header);
            }
            havePrev = false;
        }
        if (line >= first) {
            prevLineNo = line;
            prevLevel = lvl;
            prevBlank = white;
            havePrev = true;
        }
    }

    // 末行没有下一行可比较，不自成块头
    if (havePrev && prevLineNo >= first)
        SetFoldLine(doc, prevLineNo, prevLevel, prevLevel, prevBlank, false);
}

} // namespace xfs
