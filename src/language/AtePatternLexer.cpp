// xfsWinPad - Chroma .pat 向量文件词法器（批次 73 按真实手册重写）
//
// 【本批为什么重写】
//   批次 72 的版本是按「ATE 各家方言的共同记号」**自拟**的一套语法：关键词表是
//   LBL/END/CALL/SEQ/DCAP/MAIN 这些，与真实 Chroma .pat 几乎不重合；向量只分
//   三类（drive/expect/mask），而手册 3.4.1 的语义是「驱动与比较的组合」。
//   批次 73 对照 EN_3380_Language_Manual 第 3 章（p37-68）重写，数据源
//   src/language/Chroma3380Db.cpp（由脚本从手册生成）。
//
// 【真实 .pat 的文件形态（手册 3.1.2 / 3.2.3 原文示例）】
//     SET_DEC_FILE "./ls299_16sites_pin.dec"        <- 末尾**没有**分号
//     HEADER   CLR,%SEL0,SEL1,%G1,G2,%CLK,          <- 列表可跨行，以 ; 收尾
//               QD,QE,QF,QG,QH;
//     SPM_PATTERN  (os_pat) {
//     os_st::    *0 00 00 0 00 00000000 *TS15;      <- :: 标签 + 向量 + 时序集
//                    *0 00 00 0 00 00000000 * RPT 100;
//                    *Z 00 00 0 00 00000000 *;
//          os_sp::   *0 00 00 0 00 00000000 *;
//     }
//   要点：
//     1) 向量数据在 `* ... *` 之间，**组内是逐字符的独立符号**：`Z0` 是两个
//        符号（Z 然后 0），不是「Z0 这个记号」。所以必须逐字符分类，
//        不能把 `Z0` 当一个 token 查表。
//     2) `%pin` 的 `%` 表示「输出时插一个空格」，pin 名本身是标识符。
//     3) 收尾的 `*` 之后可以是时序集引用（TS15）或微指令（RPT 100）。
//
// 【向量五分类的依据（手册 3.4.1）】
//     0 1        驱动             -> VEC_DRIVE
//     H L Z      只比较           -> VEC_CMP
//     R S T U    驱动 + 比较      -> VEC_DRV_CMP
//     X          不驱动不比较     -> VEC_MASK
//     V K 2      特殊控制记号     -> VEC_CTRL
//   批次 72 把 U 当掩码是错的（U = 驱动低 + 比较低）；N 与 D 在真实语言里
//   **不存在**，本版已删除。
//
// 【行局部性 / 跨行状态】
//   本语言的注释（# //）不跨行，`/* */` 跨行。跨行块注释的状态通过 LexSpan 的
//   返回值在窗口间传递（基类契约），所以增量重排（Scintilla 从中间行开始 Lex）
//   也能正确续上。
//   HEADER 的 pin 列表可以跨行，但**不用跨行状态**去记它：续行的判据是
//   「整行只由标识符 / % / 逗号 / 分号构成」——这是行内自证的，不依赖前文，
//   因此在任意位置开始 Lex 都不会错。该判据由 IsHeaderListLine() 实现。
//
// 【折叠】按缩进（tab 展开为 4 列）：下一行缩进更深的行 = 块头。

#include "AtePatternLexer.h"
#include "Chroma3380Db.h"
#include "XfsLexerStyles.h"

#include <vector>

namespace xfs {

namespace {

// 跨窗口 / 增量重排的状态标记。基类把 LexSpan 的返回值传给下一个窗口，Scintilla
// 增量 Lex 时也会把该位置原有的状态当 initStyle 传进来，所以用它承载「块注释
// 未闭合」这一个跨行状态即可。取值刻意避开样式号区间（样式号 ≥64）。
enum { kStateDefault = 0, kStateBlockComment = 1 };

// 常见电源/时钟/JTAG 信号名。这是「初始线索」而非白名单——.pat 里 pin 名的
// 权威来源是 .dec 的 PIN_LIST，词法器读不到跨文件信息，所以只认两类：
//   · 上下文（HEADER 列表 / `%pin` 前缀）—— 主力判据
//   · 这张常见信号名表 —— 兜底，让裸写 `VDD` 也有颜色
const char* const kCommonPins =
    "VDD VSS VCC VEE VCCIO VDDIO VDDQ AVDD AVSS DVDD DVSS CLK CLOCK XCLK TCLK "
    "RESET RST RESETN EN ENABLE TRIG STROBE TEST TCK TMS TDI TDO TRST OSC";

// ---- 字符判据 -----------------------------------------------------------------

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsHexDigit(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool IsIdentChar(char c) { return IsIdentStart(c) || IsDigit(c); }

// 向量符号五分类。返回 -1 表示不是向量符号。
int VectorStyle(char c) {
    switch (c) {
        case '0': case '1': return SCE_ATEP_VEC_DRIVE;
        case 'H': case 'L': case 'Z': return SCE_ATEP_VEC_CMP;
        case 'R': case 'S': case 'T': case 'U': return SCE_ATEP_VEC_DRV_CMP;
        case 'X': return SCE_ATEP_VEC_MASK;
        case 'V': case 'K': case '2': return SCE_ATEP_VEC_CTRL;
        default: return -1;
    }
}

// 十六进制引导字符：`dA0`（驱动数据 A0）、`c8`（比较 8）。
// 只在**小写**时成立 —— 符号格式用大写、十六进制引导用小写，手册两者不混，
// 靠大小写就能无歧义地区分（否则 `Z0` 到底是两个符号还是一个十六进制组就说不清）。
bool IsHexLead(char c) {
    return c == 'c' || c == 'd' || c == 'z' || c == 'x' || c == 't' || c == 's';
}

// ---- 行级预判 -----------------------------------------------------------------

// HEADER 的 pin 列表（含跨行续行）。判据是**行内自证**的：整行只由标识符、
// `%`、逗号、分号、空白构成，且至少两个标识符。
// 为什么不用「上一行以逗号结尾」这种跨行判据：Scintilla 会从被编辑的行开始
// 增量 Lex，跨行状态会丢，续行就会掉色。行内自证的判据在任意位置开始都正确。
bool IsHeaderListLine(const char* s, std::size_t n, const KeywordSet& module) {
    (void)module;
    int idents = 0;
    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (IsSpace(c)) { ++i; continue; }
        if (c == '%' || c == ',' || c == ';') { ++i; continue; }
        if (!IsIdentStart(c)) return false;
        std::size_t j = i;
        while (j < n && IsIdentChar(s[j])) ++j;
        idents++;
        i = j;
    }
    return idents >= 1;
}

// 首记号是否（大小写不敏感地）等于 HEADER。
bool FirstTokenIsHeader(const char* s, std::size_t n) {
    std::size_t i = 0;
    while (i < n && IsSpace(s[i])) ++i;
    std::size_t j = i;
    while (j < n && IsIdentChar(s[j])) ++j;
    if (j - i != 6) return false;
    static const char kHdr[] = "HEADER";
    for (int k = 0; k < 6; ++k) {
        char c = s[i + k];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != kHdr[k]) return false;
    }
    return true;
}

// ---- 单行词法 -----------------------------------------------------------------

struct LineCtx {
    bool headerList = false;   // 本行是 HEADER 的 pin 列表（首行或续行）
    bool headerStmt = false;   // 本行以 HEADER 开头
};

void LexLine(const char* s, std::size_t n, const LineCtx& ctx,
             const KeywordSet& module, const KeywordSet& micro,
             const KeywordSet& pins, XfsStyleSink& sink, bool& inComment) {
    std::size_t i = 0;
    bool afterPercent = false;   // 刚读过 `%`，下一个标识符是 pin
    bool firstToken = true;

    while (i < n) {
        const char c = s[i];

        if (inComment) {
            const std::size_t start = i;
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                    i += 2;
                    inComment = false;
                    break;
                }
                ++i;
            }
            sink.Push(SCE_ATEP_COMMENT, i - start);
            continue;
        }

        // 行注释：# 或 //
        if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
            sink.Push(SCE_ATEP_COMMENT, n - i);
            return;
        }
        // 块注释开头
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            // 开符必须 Push（批次 75）：消耗 2 字节不出样式 = 样式游标落后文本，
            // 整个文档颜色左移 2 字节。与 ChromaPlanLexer 同病同修，StilLexer 是正确参照。
            sink.Push(SCE_ATEP_COMMENT, 2);
            i += 2;
            inComment = true;
            continue;
        }

        if (IsSpace(c)) {
            std::size_t j = i;
            while (j < n && IsSpace(s[j])) ++j;
            sink.Push(SCE_ATEP_DEFAULT, j - i);
            i = j;
            continue;
        }

        // 向量界定符：* ... *
        if (c == '*') {
            sink.PushOne(SCE_ATEP_SEP);
            ++i;
            firstToken = false;
            // 两个 * 之间全是向量数据，逐字符分类
            while (i < n && s[i] != '*') {
                if (IsSpace(s[i])) { sink.PushOne(SCE_ATEP_DEFAULT); ++i; continue; }
                // 十六进制组：小写引导 + 十六进制数字
                if (IsHexLead(s[i]) && i + 1 < n && IsHexDigit(s[i + 1])) {
                    std::size_t j = i + 1;
                    while (j < n && IsHexDigit(s[j])) ++j;
                    sink.Push(SCE_ATEP_HEX, j - i);
                    i = j;
                    continue;
                }
                const int st = VectorStyle(s[i]);
                sink.PushOne(st < 0 ? SCE_ATEP_DEFAULT : st);
                ++i;
            }
            if (i < n) { sink.PushOne(SCE_ATEP_SEP); ++i; }   // 收尾 *
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

        // `%` 前缀：本身是运算符，紧跟的标识符是 pin
        if (c == '%') {
            sink.PushOne(SCE_ATEP_OPERATOR);
            ++i;
            afterPercent = true;
            continue;
        }

        // 数字 / 0x 十六进制
        if (IsDigit(c)) {
            std::size_t j = i;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                j += 2;
                while (j < n && IsHexDigit(s[j])) ++j;
                sink.Push(SCE_ATEP_HEX, j - i);
            } else {
                while (j < n && IsDigit(s[j])) ++j;
                sink.Push(SCE_ATEP_NUMBER, j - i);
            }
            i = j;
            firstToken = false;
            continue;
        }

        // 标识符：模块语句 / 微指令 / 时序集 / 标签 / pin
        if (IsIdentStart(c)) {
            std::size_t j = i;
            while (j < n && IsIdentChar(s[j])) ++j;
            const std::size_t idLen = j - i;
            int style = SCE_ATEP_DEFAULT;

            if (afterPercent) {
                style = SCE_ATEP_PIN;
            } else if (ctx.headerList) {
                // HEADER 的列全是 pin 名（含 `%` 前缀的）
                style = (module.Has(s + i, idLen) && firstToken)
                            ? SCE_ATEP_MODULE
                            : SCE_ATEP_PIN;
            } else if (j < n && s[j] == ':') {
                style = SCE_ATEP_LABEL;            // os_st:: / os_sp:
            } else if (module.Has(s + i, idLen)) {
                style = SCE_ATEP_MODULE;           // SET_DEC_FILE / HEADER / SPM_PATTERN
            } else if (micro.Has(s + i, idLen)) {
                style = SCE_ATEP_MICRO;            // RPT / JNZ0 / IMATCH
            } else if (idLen == 3 && s[i] == 'T' && s[i + 1] == 'S' &&
                       IsDigit(s[i + 2])) {
                style = SCE_ATEP_TIMESET;          // TS1 ~ TS9
            } else if (idLen == 4 && s[i] == 'T' && s[i + 1] == 'S' &&
                       IsDigit(s[i + 2]) && IsDigit(s[i + 3])) {
                style = SCE_ATEP_TIMESET;          // TS10 ~ TS15
            } else if (pins.Has(s + i, idLen)) {
                style = SCE_ATEP_PIN;
            }

            sink.Push(style, idLen);
            afterPercent = false;
            firstToken = false;
            i = j;
            continue;
        }

        // 其余一律运算符
        sink.PushOne(SCE_ATEP_OPERATOR);
        ++i;
        firstToken = false;
        afterPercent = false;
    }
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

AtePatternLexer::AtePatternLexer() : XfsLexerBase(kLexAtePattern, kLexIdAtePattern) {
    // 词表来自 Chroma3380Db（由手册生成），不再内联在词法器里 ——
    // 手写词表是批次 72 「自拟语言」问题的根因，改成同源生成才不会再次漂移。
    MutableWords(0).Set(chroma3380::kPatModuleWords);
    MutableWords(1).Set(chroma3380::kPatMicroWords);
    MutableWords(2).Set(kCommonPins);
}

int AtePatternLexer::LexSpan(const char* text, std::size_t len, Sci_Position /*basePos*/,
                             int initStyle, XfsStyleSink& sink) {
    const KeywordSet& module = Words(0);
    const KeywordSet& micro = Words(1);
    const KeywordSet& pins = Words(2);

    bool inComment = (initStyle == kStateBlockComment);

    ForEachLine(text, len, [&](const char* line, std::size_t lineLen, std::size_t eol) {
        LineCtx ctx;
        if (lineLen) {
            ctx.headerStmt = FirstTokenIsHeader(line, lineLen);
            ctx.headerList = ctx.headerStmt ||
                             IsHeaderListLine(line, lineLen, module);
            // 纯空白行不算 header 列表（否则空行会被判成一列 pin）
            if (ctx.headerList && !ctx.headerStmt) {
                bool any = false;
                for (std::size_t k = 0; k < lineLen; ++k)
                    if (!IsSpace(line[k])) { any = true; break; }
                if (!any) ctx.headerList = false;
            }
        }
        LexLine(line, lineLen, ctx, module, micro, pins, sink, inComment);
        if (eol) sink.Push(inComment ? SCE_ATEP_COMMENT : SCE_ATEP_DEFAULT, eol);
    });

    return inComment ? kStateBlockComment : kStateDefault;
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

    // 末行没有下一行可比，不自成块头
    if (havePrev && prevLineNo >= first)
        SetFoldLine(doc, prevLineNo, prevLevel, prevLevel, prevBlank, false);
}

} // namespace xfs
