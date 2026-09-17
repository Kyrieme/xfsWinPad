// xfsWinPad - ATE Log (.log) 词法器实现（批次 72）
//
// 【本词法器必须足够保守】
//   .log 不只是 ATE 测试日志，也覆盖通用应用程序日志。把通用日志染花是净损失，
//   所以这里设了一道硬门：**只有被判定为 ATE 行的行才着色**。判定依据是行内
//   出现**自足证据**之一 —— SITE 记号（带编号）/ [min, max] 限值区间 /
//   强判定词 PASS·FAIL / 强记录词 LOT·BIN·WAFER·DIE·YIELD。不满足的行整行保持
//   默认样式（注释除外）。
//
// 【为什么证据要分强弱（本批被真实文件逼出来的修正）】
//   OK / ERROR / TIMEOUT 这些词在通用应用日志里到处都是（`heartbeat ok`、
//   `ERROR db connection`），拿它们当触发条件会让一篇普通服务日志成片染花 ——
//   实测样例行 `08:12:33 heartbeat ok` 就因此被整行着色。所以判定词与记录词都
//   分两级：**强词自己就能认定本行是 ATE 记录**，**弱词只在本行已被认定之后
//   着色**。裸 `SITE` 同理（`site config` 是通用日志的常见说法），要求带编号
//   或紧跟括号。
//
// 【行判定感知（本批的关键设计）】
//   FAIL 的价值在于「一眼看到哪一行坏了」。日志的坏消息往往不在行尾的判定词上，
//   而在中间那一串测量值和限值里。所以这里先整行预扫得出判定结论，再回填样式：
//   **FAIL 行的测量值与限值整体染红**，PASS/WARN 行保持中性。这样失败的整条
//   记录是一个红色块，而通过的行安静如常 —— 不需要额外记号或过滤就能扫出问题。
//
// 【行局部性】每行独立完成，无跨行状态。窗口对齐行首（基类保证），
//   所以不存在状态跨窗口丢失的问题。

#include "AteLogLexer.h"
#include "XfsLexerStyles.h"

namespace xfs {

namespace {

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsHexDigit(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool IsIdentChar(char c) {
    return IsIdentStart(c) || IsDigit(c) || c == '_' || c == '-' || c == '.';
}

// 大小写不敏感地比较 [s, s+n) 与字面量 lit
bool EqualsNoCase(const char* s, std::size_t n, const char* lit) {
    std::size_t i = 0;
    for (; i < n && lit[i]; ++i) {
        char a = s[i], b = lit[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return i == n && lit[i] == '\0';
}

bool StartsWithNoCase(const char* s, std::size_t n, const char* lit) {
    std::size_t i = 0;
    for (; i < n && lit[i]; ++i) {
        char a = s[i], b = lit[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return lit[i] == '\0';
}

enum Verdict { VNone = 0, VPass, VFail, VWarn };

// 「强 ATE 记录词」：出现即认为本行是 ATE 测试记录。只收那些在通用应用日志里
// 几乎不会出现的词 —— BIN/LOT/WAFER/DIE/YIELD 是半导体语义，且不像 SUM/COUNT
// 那样在日常日志里随口可见。
bool IsStrongRecord(const char* s, std::size_t n) {
    return EqualsNoCase(s, n, "BIN") || EqualsNoCase(s, n, "PART") ||
           EqualsNoCase(s, n, "PARTID") || EqualsNoCase(s, n, "LOT") ||
           EqualsNoCase(s, n, "WAFER") || EqualsNoCase(s, n, "DIE") ||
           EqualsNoCase(s, n, "YIELD") || EqualsNoCase(s, n, "RETEST");
}

// 「弱记录词」：只有在行已被判定为 ATE 之后才着色。SUM/COUNT/TOTAL 这些在
// 通用日志里太常见，不能拿它们当触发条件。
bool IsWeakRecord(const char* s, std::size_t n) {
    return EqualsNoCase(s, n, "NUM") || EqualsNoCase(s, n, "TOTAL") ||
           EqualsNoCase(s, n, "SUM") || EqualsNoCase(s, n, "COUNT") ||
           EqualsNoCase(s, n, "TEST") || EqualsNoCase(s, n, "STEP") ||
           EqualsNoCase(s, n, "SITE") || EqualsNoCase(s, n, "PART");
}

Verdict ClassifyVerdict(const char* s, std::size_t n) {
    if (EqualsNoCase(s, n, "PASS") || EqualsNoCase(s, n, "PASSED") ||
        EqualsNoCase(s, n, "OK") || EqualsNoCase(s, n, "GOOD"))
        return VPass;
    if (EqualsNoCase(s, n, "FAIL") || EqualsNoCase(s, n, "FAILED") ||
        EqualsNoCase(s, n, "NG") || EqualsNoCase(s, n, "BAD"))
        return VFail;
    if (EqualsNoCase(s, n, "WARN") || EqualsNoCase(s, n, "WARNING") ||
        EqualsNoCase(s, n, "ERROR") || EqualsNoCase(s, n, "ERR") ||
        EqualsNoCase(s, n, "ABORT") || EqualsNoCase(s, n, "TIMEOUT") ||
        EqualsNoCase(s, n, "RETEST"))
        return VWarn;
    return VNone;
}

// 「自足判定词」：只有这几个词能**单独**把一行判定为 ATE 记录。
// PASS/FAIL 这组裸大写词在通用应用日志里基本不出现；而 OK/ERROR/TIMEOUT
// 到处都是（`heartbeat ok`、`ERROR db connection`）—— 拿后者当触发条件，
// 一篇普通服务日志会被成片染花。所以它们只能锦上添花：本行已被其它证据
// 判定为 ATE 时才着色，不反过来制造证据。
// （RETEST 不含在内：它是半导体专有词，已由 IsStrongRecord 负责触发。）
bool IsStrongVerdict(const char* s, std::size_t n) {
    return EqualsNoCase(s, n, "PASS") || EqualsNoCase(s, n, "PASSED") ||
           EqualsNoCase(s, n, "FAIL") || EqualsNoCase(s, n, "FAILED");
}

// 行首时间戳长度（0 = 无）。识别：
//   2026-09-17 08:12:33.123     2026/09/17 08:12:33
//   08:12:33.123                08:12:33
//   [2026-09-17 08:12:33]       [08:12:33]
std::size_t TimestampLength(const char* s, std::size_t n) {
    std::size_t i = 0;
    std::size_t bracket = 0;
    if (i < n && s[i] == '[') { bracket = 1; ++i; }

    // 日期部分：YYYY[-/]MM[-/]DD
    bool haveDate = false;
    if (i + 10 <= n && IsDigit(s[i]) && IsDigit(s[i + 1]) && IsDigit(s[i + 2]) &&
        IsDigit(s[i + 3]) && (s[i + 4] == '-' || s[i + 4] == '/') &&
        IsDigit(s[i + 5]) && IsDigit(s[i + 6]) &&
        (s[i + 7] == '-' || s[i + 7] == '/') && IsDigit(s[i + 8]) && IsDigit(s[i + 9])) {
        i += 10;
        haveDate = true;
        while (i < n && IsSpace(s[i])) ++i;
    }

    // 时间部分：HH:MM:SS[.mmm]
    const std::size_t beforeTime = i;
    if (i + 8 <= n && IsDigit(s[i]) && IsDigit(s[i + 1]) && s[i + 2] == ':' &&
        IsDigit(s[i + 3]) && IsDigit(s[i + 4]) && s[i + 5] == ':' &&
        IsDigit(s[i + 6]) && IsDigit(s[i + 7])) {
        i += 8;
        if (i < n && s[i] == '.') {
            std::size_t j = i + 1;
            while (j < n && IsDigit(s[j])) ++j;
            if (j > i + 1) i = j;
        }
    } else {
        i = beforeTime;
    }
    const bool haveTime = (i > beforeTime);

    if (!haveDate && !haveTime) return 0;

    if (bracket) {
        if (i < n && s[i] == ']') ++i;
        else return 0;             // 有 '[' 无 ']'：不是时间戳括号，放弃
    }
    return i;
}

// SITE 记号的长度（0 = 无）。传入整行与标识符起点，因此可以越过标识符末尾
// 再看一个字符 —— 于是 SITE[1] 也认（'[' 不是标识符字符，标识符只有 "SITE"）。
// 裸 `SITE` 不算记号："site" 在通用应用日志里同样是常见词（`site config`），
// 拿它当 ATE 证据会把通用日志染花。必须带编号（SITE1 / SITE_1）或紧跟括号。
std::size_t SiteLengthAt(const char* s, std::size_t n, std::size_t at) {
    if (at + 4 > n) return 0;
    if (!StartsWithNoCase(s + at, n - at, "SITE")) return 0;
    std::size_t i = at + 4;
    if (i < n && (s[i] == '_' || s[i] == '#')) ++i;
    const std::size_t digitsFrom = i;
    while (i < n && IsDigit(s[i])) ++i;
    if (i > digitsFrom) return i - at;
    if (i < n && (s[i] == '[' || s[i] == '(')) return i - at;
    return 0;
}

struct LineFacts {
    Verdict verdict = VNone;
    bool ate = false;        // 行内出现强 ATE 信号
};

LineFacts ScanFacts(const char* s, std::size_t n) {
    LineFacts f;
    // 注意：**行首时间戳本身不算 ATE 证据**。绝大多数通用应用日志都以时间戳开头，
    // 拿它当触发条件会让整个 .log 都被染色 —— 这正是不想发生的事。
    // 时间戳只在该行已被判定为 ATE 之后才着色。

    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (IsSpace(c)) { ++i; continue; }
        if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) break;

        // 限值区间 [1.650, 1.950]：必须同时含数字与分隔符才算 ATE 证据，
        // 否则 `[INFO]`、`retry [1]` 之类的通用括号会误触发。
        if (c == '[' || c == '(') {
            const char close = (c == '[') ? ']' : ')';
            std::size_t j = i + 1;
            bool hasDigit = false, hasSep = false;
            while (j < n && s[j] != close) {
                if (IsDigit(s[j])) hasDigit = true;
                else if (s[j] == ',' || s[j] == ';') hasSep = true;
                else if (s[j] == '\n' || s[j] == '\r') break;
                ++j;
            }
            if (j < n && s[j] == close && hasDigit && hasSep) f.ate = true;
            i = (j < n && s[j] == close) ? j + 1 : i + 1;
            continue;
        }

        if (IsIdentStart(c)) {
            std::size_t j = i;
            while (j < n && IsIdentChar(s[j])) ++j;
            const std::size_t len = j - i;
            const Verdict v = ClassifyVerdict(s + i, len);
            if (v != VNone) {
                // 同一行出现多个判定词时，FAIL 优先（一行里既有 FAIL 又有 PASS
                // 的情况见于 pass/fail 汇总行，按失败处理更安全）
                f.verdict = (f.verdict == VFail || v == VFail) ? VFail : v;
                // 只有强判定词才能自足地把本行认定为 ATE
                if (IsStrongVerdict(s + i, len)) f.ate = true;
            }
            if (SiteLengthAt(s, n, i) > 0) f.ate = true;
            if (IsStrongRecord(s + i, len)) f.ate = true;
            i = j;
            continue;
        }
        ++i;
    }
    return f;
}

int StyleForVerdict(Verdict v) {
    switch (v) {
        case VPass: return SCE_ATEL_PASS;
        case VFail: return SCE_ATEL_FAIL;
        case VWarn: return SCE_ATEL_WARN;
        default:    return SCE_ATEL_DEFAULT;
    }
}

void LexLine(const char* s, std::size_t n, XfsStyleSink& sink) {
    const LineFacts facts = ScanFacts(s, n);

    // 保守门：非 ATE 行整行默认样式（注释仍着色，因为注释在任何日志里都该淡化）
    if (!facts.ate) {
        std::size_t i = 0;
        while (i < n) {
            if (s[i] == '#' || (s[i] == '/' && i + 1 < n && s[i + 1] == '/')) {
                if (i > 0) sink.Push(SCE_ATEL_DEFAULT, i);
                sink.Push(SCE_ATEL_COMMENT, n - i);
                return;
            }
            ++i;
        }
        sink.Push(SCE_ATEL_DEFAULT, n);
        return;
    }

    // FAIL 行：测量值与限值整体染红，让坏记录成块出现
    const bool escalate = (facts.verdict == VFail);

    std::size_t i = 0;

    // 行首时间戳
    const std::size_t tsLen = TimestampLength(s, n);
    if (tsLen > 0) {
        sink.Push(SCE_ATEL_TIMESTAMP, tsLen);
        i = tsLen;
    }

    while (i < n) {
        const char c = s[i];

        if (IsSpace(c)) {
            std::size_t j = i;
            while (j < n && IsSpace(s[j])) ++j;
            sink.Push(SCE_ATEL_DEFAULT, j - i);
            i = j;
            continue;
        }

        if (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) {
            sink.Push(SCE_ATEL_COMMENT, n - i);
            return;
        }

        // 限值区间
        if (c == '[' || c == '(') {
            const char close = (c == '[') ? ']' : ')';
            std::size_t j = i + 1;
            bool hasDigit = false;
            while (j < n && s[j] != close) {
                if (IsDigit(s[j])) hasDigit = true;
                ++j;
            }
            if (j < n && s[j] == close && hasDigit) {
                sink.Push(escalate ? SCE_ATEL_FAIL : SCE_ATEL_LIMIT, j + 1 - i);
                i = j + 1;
                continue;
            }
            sink.PushOne(SCE_ATEL_DEFAULT);
            ++i;
            continue;
        }

        // 数字（含 0x 前缀与小数）
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
            sink.Push(escalate ? SCE_ATEL_FAIL : SCE_ATEL_NUMBER, j - i);
            i = j;
            continue;
        }

        // 标识符：判定词 / SITE / 统计记录词 / 测试项名
        if (IsIdentStart(c)) {
            std::size_t j = i;
            while (j < n && IsIdentChar(s[j])) ++j;
            const std::size_t len = j - i;

            int st;
            const Verdict v = ClassifyVerdict(s + i, len);
            if (v != VNone) {
                st = StyleForVerdict(v);
            } else if (SiteLengthAt(s, n, i) > 0) {
                st = SCE_ATEL_SITE;
            } else if (IsStrongRecord(s + i, len) || IsWeakRecord(s + i, len)) {
                // 强/弱记录词在这里一视同仁地着色 —— 能走到这一步说明本行已经
                // 通过 ScanFacts 的保守门，「弱词会不会误染通用日志」已不成问题。
                st = SCE_ATEL_RECORD;
            } else {
                st = SCE_ATEL_TESTNAME;
            }
            sink.Push(st, len);
            i = j;
            continue;
        }

        sink.PushOne(SCE_ATEL_DEFAULT);
        ++i;
    }
}

} // namespace

AteLogLexer::AteLogLexer() : XfsLexerBase(kLexAteLog, 7203) {
    // 判定词/SITE/统计词的判据是硬编码的字面量比较，不走关键词表：
    // 这些词的集合很小且语义固定，注入式词表反而会让「保守门」失去确定性。
}

int AteLogLexer::LexSpan(const char* text, std::size_t len, Sci_Position /*basePos*/,
                         int /*initStyle*/, XfsStyleSink& sink) {
    ForEachLine(text, len, [&](const char* line, std::size_t lineLen, std::size_t eol) {
        LexLine(line, lineLen, sink);
        if (eol) sink.Push(SCE_ATEL_DEFAULT, eol);
    });
    return SCE_ATEL_DEFAULT;
}

} // namespace xfs
