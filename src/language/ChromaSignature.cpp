// xfsWinPad - Chroma 3380 签名提示：位置解析实现（批次 73）

#include "ChromaSignature.h"

namespace xfs {
namespace chroma3380 {
namespace {

// 往回扫的上限。判断「光标是不是在实参里」只需要看光标附近的一小段，
// 没必要为一个几 KB 的调用去取百万行文档的全部前缀。超过这个距离直接
// 判定「不在实参里」——误判的代价只是不出提示，比卡住用户好。
constexpr std::size_t kScanBackLimit = 8192;

bool IsIdentChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// 实参内部的分隔符：出现在光标之前时，它右侧就是当前实参的起点。
//
// 【为什么 `-` `+` `.` `@` 不在这里】
//   它们可能是实参本身的一部分（`-200uA`、`2.5mA`、`@6V`）。若把 `-` 当分隔符，
//   用户打了 `-2` 再按 Tab，替换区间就从 `2` 开始，结果是 `-@6V`——一段非法代码。
//   宁可把 `-2` 整段替换掉，也不能拼出 `-@6V`。
bool IsArgSeparator(char c) {
    switch (c) {
        case ' ': case '\t': case '\r': case '\n':
        case ',': case '(': case ')': case ';':
        case '=': case ':': case '[': case ']':
        case '{': case '}':
            return true;
        default:
            return false;
    }
}

// 语句边界：回溯时撞到这些字符说明已经走出当前语句，不必再往前找。
//
// 【换行**不是**边界】手册自己的示例就把调用写成两行：
//     FORCE_V_MLDPS(VDDUSB+VPP, 0.0V, @6V, @500mA, 300mA, NORM, ON,
//                   5mS);
// 把 `\n` 当边界会让第二行往后的实参全部失去提示。
bool IsStatementBoundary(char c) {
    return c == ';' || c == '{' || c == '}';
}

} // namespace

bool ResolveSignatureHint(const std::string& t, std::size_t caret,
                          SignatureHint& out) {
    out = SignatureHint{};
    if (caret == 0 || caret > t.size()) return false;

    const std::size_t lo = (caret > kScanBackLimit) ? caret - kScanBackLimit : 0;

    // ---- 1) 往回找未闭合的左括号（取最内层那个）----------------------------
    //     `)` 计数、`(` 抵消，depth 归零处就是当前所在的调用。
    //     如果光标在 `foo(bar(` 里，命中的是内层的 `bar(`，外层语句不再考虑
    //     ——内层实参的取值语义由内层语句定义。
    std::size_t open = std::string::npos;
    int depth = 0;
    for (std::size_t i = caret; i > lo; --i) {
        const char c = t[i - 1];
        if (c == ')') { ++depth; continue; }
        if (c == '(') {
            if (depth == 0) { open = i - 1; break; }
            --depth;
            continue;
        }
        if (depth == 0 && IsStatementBoundary(c)) return false;
    }
    if (open == std::string::npos) return false;

    // ---- 2) 左括号前的标识符就是语句名 ------------------------------------
    std::size_t e = open;
    while (e > lo && (t[e - 1] == ' ' || t[e - 1] == '\t')) --e;
    std::size_t b = e;
    while (b > lo && IsIdentChar(t[b - 1])) --b;
    if (b == e) return false;           // 括号前不是标识符（如 `(a + b)`）→ 不是调用

    const StatementDef* st = FindStatement(t.data() + b, e - b);
    if (!st) return false;              // 不是 3380 已收录的语句，与本功能无关

    // ---- 3) 数到光标为止跨过了几个顶层逗号 --------------------------------
    //     方括号/花括号也算深度：`JUDGE_PAT(a, [b, c])` 里的逗号属于内层。
    //     引号内的逗号跳过（`TDO_PRINTF("a,b", x)` 不能算成两个实参）。
    //     圆括号同样计深，但 `open` 本身不计——它已经在外面了。
    int idx = 0;
    int inner = 0;
    char quote = 0;
    for (std::size_t i = open + 1; i < caret; ++i) {
        const char c = t[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(' || c == '[' || c == '{') {
            ++inner;
        } else if (c == ')' || c == ']' || c == '}') {
            if (inner > 0) --inner;
        } else if (c == ',' && inner == 0) {
            ++idx;
        }
    }
    // 越界：实参比手册签名多（写错了，或者本语句是重复组语法退回的旧口径）。
    // 越界一律不出提示——宁可不出，也不能把别的参数的候选值挂过来。
    if (idx < 0 || idx >= st->paramCount) return false;

    // ---- 4) 当前实参的替换起点 --------------------------------------------
    //     往回吃到分隔符为止。下界用 open+1 而不是 lo：光标右侧的 `(` 一定是
    //     本实参的绝对左界，比扫描窗口更紧也更准。
    std::size_t s = caret;
    while (s > open + 1 && !IsArgSeparator(t[s - 1])) --s;

    out.stmt       = st;
    out.paramIndex = idx;
    out.param      = &kParams[st->paramStart + idx];
    out.argStart   = static_cast<int>(s);
    out.positional = ((st->flags & kStmtPositional) != 0);
    // 候选值只在**槽序可信**时才给。实测 10 条退回旧口径的语句里有 5 条带着
    // 枚举（LOAD_VI_WAVEFORM.start_value、JUDGE_TMU_RESULT.start_type 等），
    // 按下标挂下去就会把 A 参数的档位表挂到 B 参数上——语法合法、数值全错。
    out.hasEnum    = out.positional &&
                     ((out.param->flags & kParamHasEnum) != 0) &&
                     out.param->valCount > 0;
    return true;
}

std::string ParamDisplayName(const SignatureHint& h) {
    if (!h.param || !h.param->name) return {};
    const std::string n = h.param->name;
    // `arg0`/`arg3` 这类是抽取期合成的占位名（手册该处是字符串字面量或纯取值），
    // 展示出来只会让人困惑，按「没有名字」处理。
    if (n.size() > 3 && n.compare(0, 3, "arg") == 0) {
        bool allDigits = true;
        for (std::size_t i = 3; i < n.size(); ++i)
            if (n[i] < '0' || n[i] > '9') { allDigits = false; break; }
        if (allDigits) return {};
    }
    return n;
}

bool SignatureArgRange(const std::string& sig, int index, int& start, int& end) {
    start = end = 0;
    if (index < 0) return false;

    const std::size_t open = sig.find('(');
    if (open == std::string::npos) return false;   // 没有参数表（如 CRAFT 宏）

    int depth = 0;         // 只数圆括号；方括号是可选参数的排版记号，透明
    char quote = 0;
    int seen = 0;          // 已数过的顶层实参个数
    std::size_t segStart = open + 1;

    for (std::size_t i = open + 1; i <= sig.size(); ++i) {
        const bool atEnd = (i == sig.size());
        const char c = atEnd ? ')' : sig[i];
        bool closes = atEnd;            // 参数表到此结束（真实的 ')' 或字符串末尾）
        if (!atEnd) {
            if (quote) {
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '[' || c == ']') continue;
            if (c == '(') { ++depth; continue; }
            if (c == ')') {
                if (depth > 0) { --depth; continue; }
                closes = true;
            }
        }
        if (depth != 0) continue;
        if (!closes && c != ',') continue;

        // [segStart, i) 即一个顶层实参，掐掉两端空白后交给调用方高亮。
        if (seen == index) {
            std::size_t b = segStart, e = i;
            // 掐空白，**同时掐方括号（两端都要掐）**。方括号在计数上一路透明
            // （上面 continue 掉了），但透明只解决了"整组被并成一个实参"，
            // 没解决"括号字符落进了区间里"——而手册的写法让两种残留都会出现：
            //   freq_range [, divide_count   → '[' 写在**分隔逗号之前**，
            //                                 于是落在**上一个**参数的尾部
            //                                 → 本段是 "freq_range ["（尾随 '['）
            //   timeout, TFTM_plus_count ])  → 本段是 " TFTM_plus_count ]"（尾随 ']'）
            //   freq_range [, divide_count   → 有版本写成 "[ , divide" 时
            //                                 下一段会是 " [ divide_count"（前导 '['）
            // 两端各掐一次、'[' 与 ']' 都算，三种形态一并覆盖。
            // 手册里方括号只作可选参数的排版记号，参数名不可能含方括号，故安全。
            while (b < e && (sig[b] == ' ' || sig[b] == '\t' ||
                             sig[b] == '[' || sig[b] == ']')) ++b;
            while (e > b && (sig[e - 1] == ' ' || sig[e - 1] == '\t' ||
                             sig[e - 1] == '[' || sig[e - 1] == ']')) --e;
            if (b >= e) return false;              // 签名该处没写名字，无处高亮
            start = static_cast<int>(b);
            end = static_cast<int>(e);
            return true;
        }
        ++seen;
        if (closes) break;
        segStart = i + 1;
    }
    return false;
}

} // namespace chroma3380
} // namespace xfs
