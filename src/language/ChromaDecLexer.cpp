// xfsWinPad - Chroma 3380 .dec 设备定义文件词法器（批次 73）
//
// 【.dec 是什么】
//   设备定义文件：把测试机板卡通道映射到 DUT 的 pin。它是 .pat 与 .pln 的共同
//   依赖（两者都用 `SET_DEC_FILE "x.dec"` 引入），也是**唯一**定义 pin 名的地方。
//   手册第 2 章规定它只有 7 个顶层块：
//     DEC_MODE / PIN_LIST / PIN_GROUP / UR_PIN_GROUP /
//     POWER_PIN_GROUP / TIME_NAME_DEF / LEVEL_NAME_DEF
//
// 【真实形态（手册 2.2.2 / 2.3.3 原文示例）】
//     DEC_MODE  APAS;                    <- 末尾有分号，与 .pat 的 SET_DEC_FILE 相反
//     PIN_LIST   (LPC_BOARD_00 _2sites) {
//     /*name = ATE channel = DUT channel = Type */
//     p0     =0  : 4    =1     =IO;
//     Vdps   =576 : 577 =21    =DPS;
//     }
//     PIN_GROUP { CTRL = CLR+SEL0+SEL1;  QQ = QA+QB; }
//     POWER_PIN_GROUP { DPS_OS_PINS = Vdps; }
//     TIME_NAME_DEF { TM1=1; }
//
// 【着色策略：只给「结构列」上色，不给 pin 名上色】
//   .dec 里 pin 名是绝对多数派，把多数派染色等于没高亮。真正有信息量的是：
//     · pin_type（IO/DPS/MLDPS…，手册明列 14 种，写成 `IN` 还是 `In` 是常见错误）
//     · 通道号（写错会指到别人的板卡，且范围因机型不同：3380P 0..575 / 3380 0..1279）
//     · 顶层块名（结构骨架）
//     · 模式取值（APAS/NORM，APAS 会让 IMATCH 失效，是影响全局的开关）
//   所以 pin 名只在**定义位**（行内首个 `=` 左边的名字）上色：那是「这一行在
//   定义哪个 pin」的锚点，每行恰好一个，颜色用量可控。`+`/`-` 组表达式里的
//   引用不上色（多数派），只靠运算符已足够读出结构。
//
// 【跨行状态】
//   只有块注释 `/* */` 跨行，经 LexSpan 返回值在窗口/增量重排之间传递。
//   其余全部行局部。

#include "ChromaDecLexer.h"
#include "Chroma3380Db.h"
#include "XfsLexerStyles.h"

#include <vector>

namespace xfs {

namespace {

enum { kStateDefault = 0, kStateBlockComment = 1 };

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool IsIdentChar(char c) { return IsIdentStart(c) || IsDigit(c); }

// pin_type 表里最短的是 `IN`(2) 最长 `MLDPS`(5)，无需长度限制，靠词表判定。

void LexLine(const char* s, std::size_t n, const KeywordSet& blocks,
             const KeywordSet& pintypes, const KeywordSet& modevals,
             const KeywordSet& builtins, XfsStyleSink& sink, bool& inComment) {
    std::size_t i = 0;
    bool seenAssign = false;    // 本行是否已过首个 `=`（用于「定义位」判据）

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
            sink.Push(SCE_DEC_COMMENT, i - start);
            continue;
        }

        if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
            sink.Push(SCE_DEC_COMMENT, n - i);
            return;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            // 开符必须 Push（批次 75）：消耗 2 字节不出样式 = 样式游标落后文本，
            // 整个文档颜色左移 2 字节。与 ChromaPlanLexer 同病同修，StilLexer 是正确参照。
            sink.Push(SCE_DEC_COMMENT, 2);
            i += 2;
            inComment = true;
            continue;
        }

        if (IsSpace(c)) {
            std::size_t j = i;
            while (j < n && IsSpace(s[j])) ++j;
            sink.Push(SCE_DEC_DEFAULT, j - i);
            i = j;
            continue;
        }

        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < n && s[j] != c) ++j;
            if (j < n) ++j;
            sink.Push(SCE_DEC_STRING, j - i);
            i = j;
            continue;
        }

        if (IsDigit(c)) {
            std::size_t j = i;
            while (j < n && IsDigit(s[j])) ++j;
            sink.Push(SCE_DEC_CHANNEL, j - i);
            i = j;
            continue;
        }

        if (IsIdentStart(c)) {
            std::size_t j = i;
            while (j < n && IsIdentChar(s[j])) ++j;
            const std::size_t idLen = j - i;
            int style = SCE_DEC_DEFAULT;

            if (blocks.Has(s + i, idLen)) {
                style = SCE_DEC_BLOCK;
            } else if (pintypes.Has(s + i, idLen)) {
                style = SCE_DEC_PINTYPE;
            } else if (modevals.Has(s + i, idLen) || builtins.Has(s + i, idLen)) {
                style = SCE_DEC_MODEVAL;
            } else {
                // 「定义位」：行内首个 `=` 左边那个标识符。先跳过空白看下一个
                // 非空白字符是不是 `=`（`SEL0 = 94` 与 `SEL0=94` 两种写法都覆盖）。
                if (!seenAssign) {
                    std::size_t k = j;
                    while (k < n && IsSpace(s[k])) ++k;
                    if (k < n && s[k] == '=') style = SCE_DEC_PIN;
                }
            }

            sink.Push(style, idLen);
            i = j;
            continue;
        }

        if (c == '=') seenAssign = true;
        sink.PushOne(SCE_DEC_OPERATOR);
        ++i;
    }
}

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

ChromaDecLexer::ChromaDecLexer() : XfsLexerBase(kLexChromaDec, kLexIdChromaDec) {
    MutableWords(0).Set(chroma3380::kDecBlockWords);
    MutableWords(1).Set(chroma3380::kDecPinTypeWords);
    MutableWords(2).Set(chroma3380::kDecModeWords);
    MutableWords(3).Set(chroma3380::kDecNameWords);
}

int ChromaDecLexer::LexSpan(const char* text, std::size_t len, Sci_Position /*basePos*/,
                            int initStyle, XfsStyleSink& sink) {
    const KeywordSet& blocks = Words(0);
    const KeywordSet& pintypes = Words(1);
    const KeywordSet& modevals = Words(2);
    const KeywordSet& builtins = Words(3);

    bool inComment = (initStyle == kStateBlockComment);

    ForEachLine(text, len, [&](const char* line, std::size_t lineLen, std::size_t eol) {
        LexLine(line, lineLen, blocks, pintypes, modevals, builtins, sink, inComment);
        if (eol) sink.Push(inComment ? SCE_DEC_COMMENT : SCE_DEC_DEFAULT, eol);
    });

    return inComment ? kStateBlockComment : kStateDefault;
}

void ChromaDecLexer::FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                              Sci_Position last) {
    if (!doc || last < first) return;

    const Sci_Position lineCount = doc->LineFromPosition(doc->Length());
    if (first > lineCount) return;
    if (last > lineCount) last = lineCount;

    // .dec 的块体用花括号而不是缩进（`PIN_LIST (...) {` 后内容顶格写），
    // 所以缩进折叠在这里几乎不产生可折叠点。仍沿用缩进判据：现场文件普遍会
    // 自行缩进，且该判据不需要解析花括号配对，对不完整的中间态文件也稳定。
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
            while (!indent.empty() && indent.back() > ind) indent.pop_back();
            if (indent.empty() || indent.back() < ind) indent.push_back(ind);
            lvl = (int)indent.size() - 1;
            lastNonBlank = lvl;
        }
        if (havePrev) {
            if (prevLineNo >= first)
                SetFoldLine(doc, prevLineNo, prevLevel, lvl, prevBlank,
                            !prevBlank && (lvl > prevLevel));
            havePrev = false;
        }
        if (line >= first) {
            prevLineNo = line;
            prevLevel = lvl;
            prevBlank = white;
            havePrev = true;
        }
    }
    if (havePrev && prevLineNo >= first)
        SetFoldLine(doc, prevLineNo, prevLevel, prevLevel, prevBlank, false);
}

} // namespace xfs
