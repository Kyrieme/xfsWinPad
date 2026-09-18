// xfsWinPad - Chroma 3380 .pln 测试计划词法器（批次 73）
//
// 【.pln 是什么】
//   测试计划，相当于 C 的 main()。手册第 4~5 章（p69-694，占全书 85%）全在讲它，
//   共 315 条语句。手册 4.1 原话：「3380 language program is a C language」——
//   它真的是 C 语法 + 一层硬件语句库 + 一套流程 DSL。
//
// 【真实形态（手册 4.2.5 与培训教材 LS299 样例）】
//     SET_DEC_FILE ".\PAT\FW_ls299_4sites_pin.dec"   <- 无分号
//     int  rst_lvl = 1,  os_lvl = 2;                 <- 外骨骼是 C
//     HW_BIN_DEF { all_pass = 1;  fail_os_test = !2; }
//     START_UP() { LOAD_PAT("./PAT/ls299_pat.ppo"); }
//     TEST_PRO {                                     <- 取代 C 的 main()
//       BEFORE_TEST: test_start;
//       open_short_test   ?                : #C(fail_os_test, fail_os_test);
//       idd_dynamic1_test ?  #F(match_test) : #F(idd_dynamic2_test);
//       AFTER_TEST: test_end;
//     }
//     TEST_ITEM1(){ FORCE_V_DPS(...); JUDGE_I_DPS(...); }
//
// 【流程 DSL 是独立子语言】
//   `测试项 ? passdo : faildo ;`
//     #F(项名)         跳转到测试项
//     #C(硬件分类,软件分类) 分料（bin）
//     `项名=>#C(...)`  flag 形式，`!` 前缀表示失败分支
//   允许省略 passdo/faildo。这套记号与 C 的三目运算符形状相同但语义不同，
//   所以单独给它一个颜色（SCE_PLN_FLOW），一眼能看出「这行在分料」。
//
// 【为什么把语句分成 5 张表】（见 Chroma3380Db.h 的词表注释）
//   测试语句 / CRAFT 宏 / C 库函数 / 块语句 / 类型 的**错误表现完全不同**：
//     · 测试语句写错 → CRAFT 报参数或未定义
//     · 宏（TEST_LOT_ID 等）是内建变量，不是函数，写成 `TEST_LOT_ID()` 是错的
//     · C 库函数（CRAFT_c_*）从 C 侧调用，参数是类型占位符
//     · 块语句决定流程骨架
//   同色会让这几类混成一片，排查时无法目视归类。
//
// 【跨行状态】块注释 `/* */` 与 C 字符串一致：字符串**不跨行**（现场文件如此，
//   且 C 的多行字符串在 CRAFT 里也不常见），块注释跨行经 LexSpan 返回值传递。

#include "ChromaPlanLexer.h"
#include "Chroma3380Db.h"
#include "XfsLexerStyles.h"

#include <vector>

namespace xfs {

namespace {

enum { kStateDefault = 0, kStateBlockComment = 1 };

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsHexDigit(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool IsIdentChar(char c) { return IsIdentStart(c) || IsDigit(c); }

void LexLine(const char* s, std::size_t n, const KeywordSet& blocks,
             const KeywordSet& stmts, const KeywordSet& macros,
             const KeywordSet& clib, const KeywordSet& ctl,
             const KeywordSet& types, const KeywordSet& pintypes,
             XfsStyleSink& sink, bool& inComment) {
    std::size_t i = 0;

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
            sink.Push(SCE_PLN_COMMENT, i - start);
            continue;
        }

        if (c == '#' && !(i + 1 < n && (s[i + 1] == 'C' || s[i + 1] == 'F'))) {
            // `#` 开头的预处理指令（#include / #define）走注释色不合适，
            // 但 CRAFT 里极少；这里只把 `#C`/`#F` 让给流程 DSL，其余按行注释处理。
            // ⚠ 扫描必须从 `#` 的**下一位**起步（批次 74 修复）：j 若从 i 起步，
            //   `#` 不是标识符字符，循环一次都不走 → j==i → Push(0) 空操作 →
            //   i 原地踏步 → 死循环。现场 Eagle 的 .pln 头部全是 #include，
            //   一打开整个进程就挂死（开发机 LS299 样例没有 #include，E2E 漏网）。
            if (i + 1 < n && (s[i + 1] == 'i' || s[i + 1] == 'd')) {
                std::size_t j = i + 1;
                while (j < n && IsIdentChar(s[j])) ++j;
                sink.Push(SCE_PLN_FLOW, j - i);   // j > i 恒成立，至少覆盖 '#'
                i = j;
                continue;
            }
            sink.Push(SCE_PLN_COMMENT, n - i);
            return;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            sink.Push(SCE_PLN_COMMENT, n - i);
            return;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            // 开符必须 Push（批次 75）：`/*` 消耗 2 字节却不出样式的话，样式游标
            // 从此落后文本 2 字节，**整个文档**的颜色左移两个字节（现场 AAA
            // .pln 以 /* 头注释开始，实测 #include 的 flow 段 8→6）。StilLexer
            // 一直是 Push(COMMENT,2) 的正确写法。
            sink.Push(SCE_PLN_COMMENT, 2);
            i += 2;
            inComment = true;
            continue;
        }

        if (IsSpace(c)) {
            std::size_t j = i;
            while (j < n && IsSpace(s[j])) ++j;
            sink.Push(SCE_PLN_DEFAULT, j - i);
            i = j;
            continue;
        }

        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            bool esc = false;
            while (j < n) {
                if (esc) { esc = false; ++j; continue; }
                if (s[j] == '\\') { esc = true; ++j; continue; }
                if (s[j] == c) { ++j; break; }
                ++j;
            }
            sink.Push(SCE_PLN_STRING, j - i);
            i = j;
            continue;
        }

        // 流程 DSL 的 `#C(...)` / `#F(...)`：记号 = `#` + 一个字母
        if (c == '#' && i + 1 < n && (s[i + 1] == 'C' || s[i + 1] == 'F')) {
            sink.Push(SCE_PLN_FLOW, 2);
            i += 2;
            continue;
        }

        if (IsDigit(c)) {
            std::size_t j = i;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                j += 2;
                while (j < n && IsHexDigit(s[j])) ++j;
            } else {
                while (j < n && IsDigit(s[j])) ++j;
                if (j < n && s[j] == '.') {
                    ++j;
                    while (j < n && IsDigit(s[j])) ++j;
                }
                if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                    std::size_t k = j + 1;
                    if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
                    if (k < n && IsDigit(s[k])) {
                        j = k;
                        while (j < n && IsDigit(s[j])) ++j;
                    }
                }
            }
            sink.Push(SCE_PLN_NUMBER, j - i);
            i = j;
            continue;
        }

        if (IsIdentStart(c)) {
            std::size_t j = i;
            while (j < n && IsIdentChar(s[j])) ++j;
            const std::size_t idLen = j - i;
            int style = SCE_PLN_DEFAULT;

            // 判定顺序即优先级：
            // 1) `名:` 标签定义先于词表 —— 用户自定义的测试项名可能恰好与语句
            //    同名（TEST_ITEM 之类），有 `:` 就一定是标签定义位。
            if (j < n && s[j] == ':') {
                style = SCE_PLN_LABEL;
            } else if (blocks.Has(s + i, idLen)) {
                style = SCE_PLN_BLOCK;
            } else if (stmts.Has(s + i, idLen)) {
                style = SCE_PLN_STMT;
            } else if (macros.Has(s + i, idLen)) {
                style = SCE_PLN_MACRO;
            } else if (clib.Has(s + i, idLen)) {
                style = SCE_PLN_CLIB;
            } else if (pintypes.Has(s + i, idLen)) {
                style = SCE_PLN_PINTYPE;
            } else if (types.Has(s + i, idLen)) {
                style = SCE_PLN_CTYPE;
            } else if (ctl.Has(s + i, idLen)) {
                style = SCE_PLN_CKEYWORD;
            }

            sink.Push(style, idLen);
            i = j;
            continue;
        }

        // 流程 DSK 记号的单字符部分
        if (c == '?' || c == '!' || c == ':' || c == '=' || c == '>') {
            // `=>` 是一个流程记号，要整体着色
            if (c == '=' && i + 1 < n && s[i + 1] == '>') {
                sink.Push(SCE_PLN_FLOW, 2);
                i += 2;
                continue;
            }
            if (c == '?' || c == '!' ) {
                // `!=` 是 C 的不等号，不是流程取反
                if (c == '!' && i + 1 < n && s[i + 1] == '=') {
                    sink.Push(SCE_PLN_OPERATOR, 2);
                    i += 2;
                    continue;
                }
                sink.PushOne(SCE_PLN_FLOW);
                ++i;
                continue;
            }
        }

        sink.PushOne(SCE_PLN_OPERATOR);
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

ChromaPlanLexer::ChromaPlanLexer() : XfsLexerBase(kLexChromaPlan, kLexIdChromaPlan) {
    MutableWords(0).Set(chroma3380::kPlanBlockWords);
    MutableWords(1).Set(chroma3380::kPlanStmtWords);
    MutableWords(2).Set(chroma3380::kPlanMacroWords);
    MutableWords(3).Set(chroma3380::kPlanClibWords);
    MutableWords(4).Set(chroma3380::kPlanTypeWords);
    MutableWords(5).Set(chroma3380::kPlanPinTypeWords);
    MutableWords(6).Set(chroma3380::kPlanCtlWords);
}

int ChromaPlanLexer::LexSpan(const char* text, std::size_t len, Sci_Position /*basePos*/,
                             int initStyle, XfsStyleSink& sink) {
    const KeywordSet& blocks = Words(0);
    const KeywordSet& stmts = Words(1);
    const KeywordSet& macros = Words(2);
    const KeywordSet& clib = Words(3);
    const KeywordSet& types = Words(4);
    const KeywordSet& pintypes = Words(5);
    const KeywordSet& ctl = Words(6);

    bool inComment = (initStyle == kStateBlockComment);

    ForEachLine(text, len, [&](const char* line, std::size_t lineLen, std::size_t eol) {
        LexLine(line, lineLen, blocks, stmts, macros, clib, ctl, types, pintypes,
                sink, inComment);
        if (eol) sink.Push(inComment ? SCE_PLN_COMMENT : SCE_PLN_DEFAULT, eol);
    });

    return inComment ? kStateBlockComment : kStateDefault;
}

void ChromaPlanLexer::FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                               Sci_Position last) {
    if (!doc || last < first) return;

    const Sci_Position lineCount = doc->LineFromPosition(doc->Length());
    if (first > lineCount) return;
    if (last > lineCount) last = lineCount;

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
