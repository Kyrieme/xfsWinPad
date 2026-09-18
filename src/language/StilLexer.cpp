// xfsWinPad - STIL (.stil, IEEE 1450) 词法器实现（批次 72）
//
// 【为什么不能继续用 xml 词法器】
//   批次 72 之前 .stil 挂在 xml 词法器下（因为 STIL 长得有点像标记语言）。
//   但 STIL 的语义记号 xml 一个都认不出：块关键字（Signals/Timing/WaveformTable/
//   PatternBurst）、单引号名（'CLK'）、带单位量（'20ns'）、波形事件字母
//   （D/U/Z/X/N/T）——这些才是 STIL 文件里真正需要一眼分辨的东西。
//
// 【行局部性与唯一跨行状态】
//   // 注释、单双引号字符串、带单位量都不跨行；唯一跨行的是 /* */ 块注释，
//   所以词法状态只有两态（默认 / 块注释内），作为 LexSpan 的返回值在窗口之间
//   传递（基类分块会对齐行首，状态因此不会错位）。
//
// 【折叠】按花括号深度，层级编码与 LexCPP 一致：本行层级 = 进入深度 - 本行
//   `}` 个数，下一行层级 = 进入深度 + 净增量，`下一层 > 本层` 的行即块头。
//   于是 `}` 归外层：折叠块头时右花括号保持可见，嵌套闭合行也不会比块头更深。
//   统计 `{` `}` 时会跳过注释与引号内容，避免字符串里的花括号把折叠树带偏。
//   深度在扫描时钳到 ≥0，容忍语法不完整的片段（现场 .stil 被截断是常态）。

#include "StilLexer.h"
#include "XfsLexerStyles.h"

#include <algorithm>

namespace xfs {

namespace {

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool IsIdentChar(char c) {
    return IsIdentStart(c) || IsDigit(c) || c == '_';
}

// ---- 内置记号表 ---------------------------------------------------------------

// 块关键字：这些词后面要么跟 { ，要么是文件级语句头
const char* const kBlocks =
    "STIL Header Signals SignalGroups Timing WaveformTable Waveforms PatternBurst "
    "PatList PatternExec Pattern Patterns Procedures MacroDefs ScanStructures "
    "Spec Selector Category DomainGroups Domains Combinational Vector "
    "TimingList WaveformCharList Procedure";

// 普通关键字：信号方向、属性名、pattern 语句字母
const char* const kKeywords =
    "In Out InOut Supply Bidirectional Title Date Source History Period Waveforms "
    "W V B Annotate Lo Hi Alignment Terminate Spec Based Default WaveformChar "
    "Domain Argument Value ScanIn ScanOut ScanInOut ScanEnable ScanMode "
    "Alignment Event ScanStructures Terminations BitType Range Min Max Scale";

// 波形事件字母（STIL 里都是单字符，出现在波形表与向量块内）
const char* const kEvents = "D U Z X N T L H P p";

// 单引号内容以数字开头 → 带单位量（'20ns'），其余 → 名（'CLK'）
bool LooksLikeUnit(const char* s, std::size_t n) {
    if (n == 0) return false;
    std::size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    if (i >= n) return false;
    return IsDigit(s[i]) || s[i] == '.';
}

enum { StDefault = SCE_STIL_DEFAULT, StBlockComment = SCE_STIL_COMMENT };

// 单行词法：state 传入/返回（只可能是 StDefault 或 StBlockComment）
int LexLine(const char* s, std::size_t n, int state, const KeywordSet& blocks,
            const KeywordSet& keywords, const KeywordSet& events,
            XfsStyleSink& sink) {
    std::size_t i = 0;
    while (i < n) {
        if (state == StBlockComment) {
            std::size_t j = i;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) ++j;
            if (j + 1 < n) {
                j += 2;
                state = StDefault;
            } else {
                j = n;
            }
            sink.Push(SCE_STIL_COMMENT, j - i);
            i = j;
            continue;
        }

        const char c = s[i];

        // 注释
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            sink.Push(SCE_STIL_COMMENT, n - i);
            return state;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            sink.Push(SCE_STIL_COMMENT, 2);
            i += 2;
            state = StBlockComment;
            continue;
        }

        if (IsSpace(c)) {
            std::size_t j = i;
            while (j < n && IsSpace(s[j])) ++j;
            sink.Push(SCE_STIL_DEFAULT, j - i);
            i = j;
            continue;
        }

        // 单/双引号串（不跨行）
        if (c == '\'' || c == '"') {
            std::size_t j = i + 1;
            while (j < n && s[j] != c) ++j;
            const bool closed = (j < n);
            const std::size_t contentLen = (j - i) - 1;
            if (closed) ++j;
            int st;
            if (c == '"') st = SCE_STIL_STRING;
            else st = LooksLikeUnit(s + i + 1, contentLen) ? SCE_STIL_UNIT : SCE_STIL_NAME;
            sink.Push(st, j - i);
            i = j;
            continue;
        }

        // 数字（含小数与指数）
        if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(s[i + 1]))) {
            std::size_t j = i;
            if (s[j] == '.') ++j;
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
            sink.Push(SCE_STIL_NUMBER, j - i);
            i = j;
            continue;
        }

        // 标识符：块关键字 / 普通关键字 / 波形事件 / 块名
        if (IsIdentStart(c)) {
            std::size_t j = i;
            while (j < n && IsIdentChar(s[j])) ++j;
            const std::size_t idLen = j - i;
            int st;
            if (blocks.Has(s + i, idLen)) st = SCE_STIL_BLOCK;
            else if (keywords.Has(s + i, idLen)) st = SCE_STIL_KEYWORD;
            else if (idLen == 1 && events.Has(s + i, 1)) st = SCE_STIL_EVENT;
            else st = SCE_STIL_DEFAULT;
            sink.Push(st, idLen);
            i = j;
            continue;
        }

        // 其余一律运算符（{ } ( ) ; = + - * / , : < >）
        sink.PushOne(SCE_STIL_OPERATOR);
        ++i;
    }
    return state;
}

// 跳过注释与引号后的净花括号增量；*closers 回传本行 `}` 的个数
// （LexCPP 用「进入层级 - 闭括号数」决定本行自己的层级，见 FoldSpan）；
// *inBlock 记录行末是否仍在块注释里。
int BraceDelta(const char* s, std::size_t n, bool* inBlock, int* closers) {
    int delta = 0;
    int closeCount = 0;
    std::size_t i = 0;
    bool block = *inBlock;
    while (i < n) {
        if (block) {
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            if (i + 1 < n) { i += 2; block = false; }
            else { i = n; }
            continue;
        }
        const char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') break;          // 行注释到底
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { block = true; i += 2; continue; }
        if (c == '\'' || c == '"') {
            const char q = c;
            ++i;
            while (i < n && s[i] != q) ++i;
            if (i < n) ++i;
            continue;
        }
        if (c == '{') ++delta;
        else if (c == '}') { --delta; ++closeCount; }
        ++i;
    }
    *inBlock = block;
    *closers = closeCount;
    return delta;
}

} // namespace

StilLexer::StilLexer() : XfsLexerBase(kLexStil, kLexIdStil) {
    MutableWords(0).Set(kBlocks);
    MutableWords(1).Set(kKeywords);
    MutableWords(2).Set(kEvents);
}

int StilLexer::LexSpan(const char* text, std::size_t len, Sci_Position /*basePos*/,
                       int initStyle, XfsStyleSink& sink) {
    int state = (initStyle == StBlockComment) ? StBlockComment : StDefault;
    const KeywordSet& blocks = Words(0);
    const KeywordSet& keywords = Words(1);
    const KeywordSet& events = Words(2);

    ForEachLine(text, len, [&](const char* line, std::size_t lineLen, std::size_t eol) {
        state = LexLine(line, lineLen, state, blocks, keywords, events, sink);
        if (eol) sink.Push(state == StBlockComment ? SCE_STIL_COMMENT : SCE_STIL_DEFAULT, eol);
    });

    // 把块注释状态交给下一个窗口（基类会对齐行首，状态不会错位）
    return state;
}

void StilLexer::FoldSpan(Scintilla::IDocument* doc, Sci_Position first,
                         Sci_Position last) {
    if (!doc || last < first) return;

    const Sci_Position lineCount = doc->LineFromPosition(doc->Length());
    if (first > lineCount) return;
    if (last > lineCount) last = lineCount;

    std::string text;
    bool block = false;

    // 先只扫 [0, first) 取得进入 first 行时的深度——分层级必须从文档头累计，
    // 但这一段不回写，代价只是读文本。
    for (Sci_Position line = 0; line < first; ++line) {
        LineText(doc, line, &text);
        int closers = 0;
        (void)BraceDelta(text.data(), text.size(), &block, &closers);
    }

    int depth = 0;   // 相对深度即可：Scintilla 折叠只比较相对层级
    for (Sci_Position line = first; line <= last; ++line) {
        LineText(doc, line, &text);
        int closers = 0;
        const int delta = BraceDelta(text.data(), text.size(), &block, &closers);
        // 本行层级 = 进入层级减去本行的闭括号数（LexCPP 惯例）：
        // 闭括号行归**外层**，于是折叠 `Signals {` 只收起块内容、`}` 保持可见，
        // 与内建 C++/JSON 词法器的观感一致。若直接用进入层级，`}` 会被算成
        // 内层（层级 1），折叠头部时连右花括号一起消失；嵌套块的闭合行还会
        // 比块头更深，折叠树凭空多出一层。
        const int cur = (std::max)(0, depth - closers);
        const int next = (std::max)(0, depth + delta);
        SetFoldLine(doc, line, cur, next, text.empty(), next > cur);
        depth = next;
    }
}

} // namespace xfs
