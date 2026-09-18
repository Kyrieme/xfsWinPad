// xfsWinPad - Chroma 3380 静态校验内核实现（批次 80）
//
// 【实现原则：先把注释和字符串"抹平"，再做纯字符串判断】
//   校验最容易出的错是"注释里的分号被当成代码"、"字符串里的花括号把块配平搞乱"。
//   所以第一步就把每一行的**注释字符替换成等长空格**、把**字符串内部字符替换成
//   'x'**（引号本身保留），长度不变 ⇒ 后面所有下标都能直接对应原文偏移，既不用
//   维护偏移映射表，也不会因为注释/字符串里的字符误判。
//
// 【宽容解析】
//   每个解析器认不出就 `return`，绝不猜测。宁可漏报（用户下次编译仍能看到官方
//   报错），也绝不误报（用户会直接关掉校验，而且再也不会打开）。

#include "Chroma3380Diagnostics.h"

#include "Chroma3380Complete.h"   // IsStatementStart：与补全共用同一个"语句起始"口径
#include "Chroma3380Db.h"         // 语句签名库（规则 8 用）

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xfs {
namespace chroma3380 {

namespace {

const std::size_t kNone = std::string::npos;

bool IsIdStart(unsigned char c) { return std::isalpha(c) != 0 || c == '_'; }
bool IsIdChar(unsigned char c)  { return std::isalnum(c) != 0 || c == '_'; }
bool IsSpace(char c)            { return c == ' ' || c == '\t'; }

std::string Upper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = (char)std::toupper((unsigned char)c);
    return r;
}

// ---------------------------------------------------------------------------
// 第一步：切行 + 抹平注释/字符串
// ---------------------------------------------------------------------------

struct LineSpan { std::size_t begin; std::size_t end; };

std::vector<LineSpan> SplitLines(const std::string& t) {
    std::vector<LineSpan> out;
    std::size_t i = 0;
    for (;;) {
        std::size_t nl = t.find('\n', i);
        std::size_t e = (nl == kNone) ? t.size() : nl;
        if (e > i && t[e - 1] == '\r') --e;       // 真实 .pln 是 CRLF
        out.push_back(LineSpan{i, e});
        if (nl == kNone) break;
        i = nl + 1;
    }
    return out;
}

// 抹平一行：注释 → 空格（长度不变）；字符串内部 → 'x'（引号保留）。
// `inBlock` 跨行携带（`/* … */` 可以跨行）。
//
// 关于 `#`：**只在它是本行第一个非空白字符时**当作整行注释。
//   不能一见到 `#` 就当注释——`.pln` 里 `#define` / `#include` 是预处理指令；
//   也不能完全不当注释——`.pat` 里有 `# 说明` 这种整行注释。取"行首才认"这条
//   最窄的规则：既能正确处理两种用法，又不会影响下面任何一条判断（我们只在行首
//   看关键字是不是 SET_DEC_FILE / PIN_LIST 之类）。
std::string BlankComments(const std::string& text, std::size_t b, std::size_t e,
                          bool& inBlock) {
    std::string out(e - b, ' ');
    if (!inBlock) {
        std::size_t k = b;
        while (k < e && IsSpace(text[k])) ++k;
        if (k < e && text[k] == '#') return out;          // 整行注释
    }
    std::size_t i = b;
    while (i < e) {
        if (inBlock) {
            std::size_t p = text.find("*/", i);
            if (p == kNone || p >= e) break;             // 块注释一直延伸到后面的行
            i = p + 2;
            inBlock = false;
            continue;
        }
        if (text[i] == '/' && i + 1 < e && text[i + 1] == '/') break;   // 行注释
        if (text[i] == '/' && i + 1 < e && text[i + 1] == '*') { inBlock = true; i += 2; continue; }
        if (text[i] == '"' || text[i] == '\'') {
            char q = text[i];
            out[i - b] = q;
            ++i;
            while (i < e) {
                if (text[i] == '\\' && i + 1 < e) {       // 转义整体吃掉
                    i += 2;                              // 保持 out 里是空格
                    continue;
                }
                if (text[i] == q) { out[i - b] = q; ++i; break; }
                i += 1;                                  // 串内字符 → 留空格
            }
            continue;
        }
        out[i - b] = text[i];
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 行内小工具（都带 [b, e) 区间，因为块头那一行的 `{` 之后也要当条目的开头）
// ---------------------------------------------------------------------------

// 行首（跳过空白）的标识符。找不到返回 false。
bool LeadingIdentIn(const std::string& s, std::size_t b, std::size_t e,
                    std::size_t& col, std::size_t& len) {
    std::size_t i = b;
    while (i < e && IsSpace(s[i])) ++i;
    if (i >= e || !IsIdStart((unsigned char)s[i])) return false;
    std::size_t j = i;
    while (j < e && IsIdChar((unsigned char)s[j])) ++j;
    col = i;
    len = j - i;
    return true;
}

// 区间内最后一个非空白字符的下标（没有则 kNone）。
std::size_t LastCodeIn(const std::string& s, std::size_t b, std::size_t e) {
    std::size_t i = e;
    while (i > b) {
        --i;
        if (!IsSpace(s[i])) return i;
    }
    return kNone;
}

struct Seg { std::size_t b; std::size_t e; };   // 已去掉两端空白

std::vector<Seg> SplitTopIn(const std::string& s, std::size_t b, std::size_t e, char sep) {
    std::vector<Seg> out;
    std::size_t cur = b;
    for (std::size_t i = b; i <= e; ++i) {
        if (i == e || s[i] == sep) {
            std::size_t x = cur, y = i;
            while (x < y && IsSpace(s[x])) ++x;
            while (y > x && IsSpace(s[y - 1])) --y;
            out.push_back(Seg{x, y});
            cur = i + 1;
        }
    }
    return out;
}

// 区间内所有连续数字段（形如 `0 : 288 : 320` 会给出 4 段）。
struct Num { std::size_t b; std::size_t e; };

std::vector<Num> NumbersIn(const std::string& s, std::size_t b, std::size_t e) {
    std::vector<Num> out;
    std::size_t i = b;
    while (i < e) {
        if (std::isdigit((unsigned char)s[i]) != 0) {
            std::size_t j = i;
            while (j < e && std::isdigit((unsigned char)s[j]) != 0) ++j;
            out.push_back(Num{i, j});
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

// 从 `{`（line, col）起配平，找出对应 `}`。
bool FindBlockEnd(const std::vector<std::string>& code, std::size_t line, std::size_t col,
                  std::size_t& endLine, std::size_t& endCol) {
    int depth = 0;
    for (std::size_t i = line; i < code.size(); ++i) {
        const std::string& s = code[i];
        for (std::size_t j = (i == line ? col : 0); j < s.size(); ++j) {
            if (s[j] == '{') {
                ++depth;
            } else if (s[j] == '}') {
                --depth;
                if (depth == 0) { endLine = i; endCol = j; return true; }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 规则实现
// ---------------------------------------------------------------------------

// C3380-COM-001 —— SET_DEC_FILE 末尾不能有分号。
//   手册 §3.3.2（p41）的 .pat 示例与真实 .pln 第 21 行都是
//   `SET_DEC_FILE ".\x.dec"`，无分号。这条同时适用 .pln 与 .pat。
void CheckSetDecFileNoSemicolon(const std::vector<std::string>& code,
                                std::vector<Diagnostic>& out) {
    for (std::size_t i = 0; i < code.size(); ++i) {
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(code[i], 0, code[i].size(), col, len)) continue;
        if (Upper(code[i].substr(col, len)) != "SET_DEC_FILE") continue;
        std::size_t last = LastCodeIn(code[i], 0, code[i].size());
        if (last == kNone || code[i][last] != ';') continue;
        Diagnostic d;
        d.line = (int)i;
        d.start = (int)last;
        d.length = 1;
        d.severity = DiagSeverity::Error;
        d.code = "C3380-COM-001";
        d.manualPage = 41;
        d.message = "SET_DEC_FILE 末尾不能有分号";
        out.push_back(d);
    }
}

void PushDiag(std::vector<Diagnostic>& out, int line, std::size_t col, std::size_t len,
              const char* codeStr, int page, std::string msg,
              DiagSeverity sev = DiagSeverity::Error) {
    Diagnostic d;
    d.line = line;
    d.start = (int)col;
    d.length = (int)(len > 0 ? len : 1);
    d.severity = sev;
    d.code = codeStr;
    d.manualPage = page;
    d.message = std::move(msg);
    out.push_back(d);
}

// 一块 PIN_LIST 里的三个"不得重复"的集合。
struct PinListScope {
    std::map<std::string, int> nameFirstLine;   // pin 名 → 首次出现的 0-based 行
    std::map<std::string, int> ateFirstLine;    // ATE 通道号（十进制串）→ 首次行
    std::map<std::string, int> dutFirstLine;    // DUT pin 号 → 首次行
};

// 解析一行 pin 条目：`pin_name = ate[:ate]… = dut = pin_type ;`
//   宽容：段数 < 4、首段不是标识符、末尾不是 `;` —— 一律跳过，不报。
void ParsePinEntry(const std::string& s, std::size_t b, std::size_t e, int lineNo,
                   PinListScope& scope, std::vector<Diagnostic>& out) {
    std::size_t last = LastCodeIn(s, b, e);
    if (last == kNone || s[last] != ';') return;

    std::vector<Seg> parts = SplitTopIn(s, b, last, '=');   // 末尾的 `;` 不参与分段
    if (parts.size() < 4) return;                           // 非标准四段写法 → 跳过

    const std::string name = s.substr(parts[0].b, parts[0].e - parts[0].b);
    if (name.empty() || !IsIdStart((unsigned char)name[0])) return;
    for (char c : name) if (!IsIdChar((unsigned char)c)) return;

    auto note = [&](std::map<std::string, int>& m, const std::string& key,
                    std::size_t col, std::size_t len, const char* codeStr,
                    const std::string& what) {
        auto it = m.find(key);
        if (it == m.end()) {
            m.emplace(key, lineNo);
            return;
        }
        PushDiag(out, lineNo, col, len, codeStr, 25,
                 what + " " + key + " 重复（第 " + std::to_string(it->second + 1) + " 行已定义）");
    };

    note(scope.nameFirstLine, name, parts[0].b, name.size(), "C3380-DEC-001", "pin 名");

    // 倒数第二段 = dut_pin，其余中段 = ate_pin 列表
    const Seg& dutSeg = parts[parts.size() - 2];
    for (std::size_t k = 1; k + 2 < parts.size(); ++k) {
        for (const Num& n : NumbersIn(s, parts[k].b, parts[k].e)) {
            note(scope.ateFirstLine, s.substr(n.b, n.e - n.b), n.b, n.e - n.b,
                 "C3380-DEC-002", "ATE 通道");
        }
    }
    for (const Num& n : NumbersIn(s, dutSeg.b, dutSeg.e)) {
        note(scope.dutFirstLine, s.substr(n.b, n.e - n.b), n.b, n.e - n.b,
             "C3380-DEC-003", "DUT pin 号");
    }
}

// 解析一行 PIN_GROUP 条目：`pin_group_name = pin_name operand … ;`
//   只取左边的组名做唯一性判断（右侧分组表达式不校验，见头文件说明）。
void ParseGroupEntry(const std::string& s, std::size_t b, std::size_t e, int lineNo,
                     std::map<std::string, int>& firstLine, std::vector<Diagnostic>& out) {
    std::size_t last = LastCodeIn(s, b, e);
    if (last == kNone || s[last] != ';') return;
    if (s.find('=', b) == kNone || s.find('=', b) > last) return;

    std::size_t col = 0, len = 0;
    if (!LeadingIdentIn(s, b, e, col, len)) return;
    const std::string name = s.substr(col, len);

    auto it = firstLine.find(name);
    if (it == firstLine.end()) {
        firstLine.emplace(name, lineNo);
        return;
    }
    PushDiag(out, lineNo, col, len, "C3380-DEC-004", 27,
             "pin_group 名 " + name + " 重复（第 " + std::to_string(it->second + 1) + " 行已定义）");
}

// .dec：只认 PIN_LIST / PIN_GROUP 两个顶层块（其它顶层块与本批规则无关）。
void CheckDeviceDefinition(const std::vector<std::string>& code,
                           std::vector<Diagnostic>& out) {
    std::size_t i = 0;
    while (i < code.size()) {
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(code[i], 0, code[i].size(), col, len)) { ++i; continue; }
        const std::string kw = Upper(code[i].substr(col, len));
        const bool isPinList  = (kw == "PIN_LIST");
        const bool isPinGroup = (kw == "PIN_GROUP");
        if (!isPinList && !isPinGroup) { ++i; continue; }

        // 关键字之后找 `{`：可以在同一行，也可以在后面几行（手册示例就是换行的）。
        std::size_t braceLine = kNone, braceCol = 0;
        for (std::size_t k = i; k < code.size() && k <= i + 4; ++k) {
            std::size_t p = code[k].find('{', k == i ? col + len : 0);
            if (p != kNone) { braceLine = k; braceCol = p; break; }
            if (k > i) {
                std::size_t c2 = 0, l2 = 0;
                if (LeadingIdentIn(code[k], 0, code[k].size(), c2, l2)) break;  // 撞上别的块头
            }
        }
        if (braceLine == kNone) { ++i; continue; }

        std::size_t endLine = 0, endCol = 0;
        if (!FindBlockEnd(code, braceLine, braceCol, endLine, endCol)) { ++i; continue; }

        if (isPinList) {
            PinListScope scope;
            for (std::size_t k = braceLine; k <= endLine; ++k) {
                std::size_t b = (k == braceLine) ? braceCol + 1 : 0;
                std::size_t e = (k == endLine) ? endCol : code[k].size();
                if (b < e) ParsePinEntry(code[k], b, e, (int)k, scope, out);
            }
        } else {
            std::map<std::string, int> firstLine;
            for (std::size_t k = braceLine; k <= endLine; ++k) {
                std::size_t b = (k == braceLine) ? braceCol + 1 : 0;
                std::size_t e = (k == endLine) ? endCol : code[k].size();
                if (b < e) ParseGroupEntry(code[k], b, e, (int)k, firstLine, out);
            }
        }
        i = endLine + 1;
    }
}

// ---------------------------------------------------------------------------
// 规则 8：参数个数（批次 86）
// ---------------------------------------------------------------------------
//
// 【为什么不是"拿参数槽表对位置"（这条是批次 86 最重要的发现）】
//   kParams 的槽表**不可信到可以做必填性判断**。实测两例：
//     · `JUDGE_VARIABLE` 签名的 `[, "string" ]` 槽没被标成可选（flags 里没有可选位）；
//     · `SOCKET_INC` 的槽名直接被枚举值顶替（"参数名"变成了 `FRZ_ON`）。
//   而 `StatementDef::signature` 是**手册原文**（签名提示 UI 显示的就是它），
//   所以判定一律从签名串推导，并且加一把"双钥"：推导出的上限必须与 paramCount
//   相等，否则说明这条语句的签名抽取有问题 —— 直接跳过，不报。
//
// 【为什么只认两种形状】
//   手册签名里歧义写法很多：整体可选 `[( … )]`、交替 `x[|x]`、重复组 `[…]*`、
//   组内再嵌组、`|` 并列多个方括号。宽解必然误报，所以只认两种无歧义的：
//     NAME(a, b, c)           → 必填 3  上限 3
//     NAME(a, b [, c, d])     → 必填 2  上限 4     （`b [ , c ]` 写法同义）
//   其余一律跳过。实测 309 条语句里 229 条落进可用集（占 74%）。
//
// 【为什么要一张"排除表"——手册自己也不自洽】
//   和 §2.4.2/§2.4.3 的 pin_group 冲突同类，手册在**参数个数**上同样自相矛盾：
//     `SET_JUDGE_MODE` 的 Format 只有 1 个参数，而 §4.15 自己给出
//     `SET_JUDGE_MODE( NORM )` 与 `SET_JUDGE_MODE( NORM , FEOP_ON )` 两种示例；
//     `PIN_MODE_HV(pin_name, d_format, io_format)` 三参，示例却是五参
//     `PIN_MODE_HV( G1, NRZ, EDGE, ENABLE, IO_NRZ )`。
//   这类语句一律排除 —— 漏报可以，误报不行。排除集的求法是**证据**而非判断：
//   把手册全文里所有语句调用回放本规则，凡被本规则判红过的语句进排除表，
//   迭代到手册语料 0 命中为止（最终恰好 10 条，见下表）。
//
// 【severity 为什么两档不同】
//   手册**没有**"参数个数不对就报错"这类明文（全文 `An error will occur` 只出现
//   2 次，都属 .dec 的 pin 规则）。本条规则的取证是两处较弱但明确的东西：
//   各语句 Format 块（调用形式的规范定义）＋ 必填参数的 `No entry: illegal` 措辞。
//   据此分档：
//     · 实参**多于**签名 → 手册从未定义过这种形式 → Error；
//     · 实参**少于**必填项 → 依据是散文措辞，且手册存在"看似必填、实可省略"的
//       明文例外（JUDGE 族 min/max：*Omitting the parameter is possible*）
//       → Warning。
//
// 【只对 .pln 开，不对 .pat 开】
//   落入受检集的 .pat 语句只有 APM_PATTERN / SPM_PATTERN 两条，而 .pat 没有任何
//   真实样本可回归 —— 按本模块"没有真实样本就不开口"的纪律（规则 1 同理暂缓），
//   不在这条规则里覆盖 .pat。

// 从 i 处起读一个标识符（i 处必须已是标识符首字符）。
bool IdentAt(const std::string& s, std::size_t i, std::size_t e,
             std::size_t& col, std::size_t& len) {
    if (i >= e || !IsIdStart((unsigned char)s[i])) return false;
    std::size_t j = i;
    while (j < e && IsIdChar((unsigned char)s[j])) ++j;
    col = i;
    len = j - i;
    return true;
}

// 深度感知的实参切分：只认**顶层**逗号。
// 不能复用 SplitTopIn —— 那个不带深度，`FOO(BAR(1,2), 3)` 会被切成三段。
std::vector<Seg> SplitArgsIn(const std::string& s, std::size_t b, std::size_t e) {
    std::vector<Seg> out;
    std::size_t cur = b;
    int depth = 0;
    for (std::size_t i = b; i <= e; ++i) {
        const char c = (i == e) ? '\0' : s[i];
        if (c == '(' || c == '[' || c == '{') { ++depth; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth > 0) --depth; continue; }
        if (i == e || (c == ',' && depth == 0)) {
            std::size_t x = cur, y = i;
            while (x < y && IsSpace(s[x])) ++x;
            while (y > x && IsSpace(s[y - 1])) --y;
            out.push_back(Seg{x, y});
            cur = i + 1;
        }
    }
    return out;
}

// 把签名某一层的内容数成「槽」数。false = 认不出（出现圆括号 / 方括号未闭合）。
//
//   逗号分层；**以逗号开头的方括号组**是同级兄弟槽（手册的可选参数写法
//   `[ , a, b ]`）；**不以逗号开头的方括号**是交替写法（`PMU_number[|PMU_number]`、
//   `[st_addr, sp_addr ] | [total_log_cnt]`），附着于当前槽，不新增槽。
bool CountSigSlots(const std::string& s, int& n) {
    n = 0;
    bool hasAtom = false;
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (IsSpace(c)) { ++i; continue; }
        if (c == '(' || c == ')') return false;
        if (c == '[') {
            int d = 1;
            std::size_t j = i + 1;
            while (j < s.size() && d) {
                if (s[j] == '[') ++d;
                else if (s[j] == ']') --d;
                ++j;
            }
            if (d != 0) return false;                       // 方括号没闭合
            const std::string inner = s.substr(i + 1, j - 1 - (i + 1));
            const std::size_t t = inner.find_first_not_of(" \t");
            if (t != kNone && inner[t] == ',') {
                if (hasAtom) { ++n; hasAtom = false; }
                int sub = 0;
                if (!CountSigSlots(inner.substr(t + 1), sub)) return false;
                n += sub;
            } else {
                hasAtom = true;                             // 交替写法：属于当前槽
            }
            i = j;
            continue;
        }
        if (c == ',') {
            if (hasAtom) { ++n; hasAtom = false; }
            ++i;
            continue;
        }
        hasAtom = true;
        ++i;
    }
    if (hasAtom) ++n;
    return true;
}

struct ArgBounds { int required; int maxArgs; };

// 从手册签名推导 (必填数, 上限数)。false = 签名有歧义，**不得**据此判定。
bool DeriveArgBounds(const std::string& sig, ArgBounds& out) {
    const std::size_t lp = sig.find('(');
    if (lp == kNone) return false;
    int d = 0;
    std::size_t rp = kNone;
    for (std::size_t i = lp; i < sig.size(); ++i) {
        if (sig[i] == '(') ++d;
        else if (sig[i] == ')') { if (--d == 0) { rp = i; break; } }
    }
    if (rp == kNone) return false;
    const std::string content = sig.substr(lp + 1, rp - lp - 1);
    if (content.find("...") != kNone) return false;         // 重复组

    std::size_t h = lp;
    while (h > 0 && IsSpace(sig[h - 1])) --h;
    if (h > 0 && sig[h - 1] == '[') return false;           // 整体可选 `NAME [( … )]`

    // 整体可选的另一种写法：参数表**本身**就是一个可选组 —— `NAME([ mode ])`，
    // 手册 §4.16 的 `POWER_DOWN_FAIL_SITE([ mode ])` 与 §4.16 示例
    // `POWER_DOWN_FAIL_SITE( )` 配套：一个参数都不给是合法的，所以必填数 = 0。
    // 本条在批次 86 的"Python 参考实现 vs C++ 实现"对撞中被抓出来（当时 C++ 漏了它，
    // 于是在手册自己的示例上误报），留着注释是为了不再丢第二次。
    {
        const std::size_t t = content.find_first_not_of(" \t");
        if (t != kNone && content[t] == '[' &&
            (t + 1 >= content.size() || content[t + 1] != ',')) {
            int mx = 0;
            if (!CountSigSlots(content, mx)) return false;
            out.required = 0;
            out.maxArgs = mx;
            return true;
        }
    }

    // 必填 = 第一个「以逗号开头的方括号组」之前的槽数
    std::size_t cut = content.size();
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] != '[') continue;
        const std::size_t t = content.find_first_not_of(" \t", i + 1);
        if (t != kNone && content[t] == ',') { cut = i; break; }
    }
    int req = 0, mx = 0;
    if (!CountSigSlots(content.substr(0, cut), req)) return false;
    if (!CountSigSlots(content, mx)) return false;
    out.required = req;
    out.maxArgs = mx;
    return true;
}

// 手册 Format 与手册**自己的示例**互相矛盾的语句：本条规则一律跳过。
// 表里每一条都有至少一个手册示例能打红它。绊线在 tests/test_chromadiag.cpp
// （`SET_JUDGE_MODE( NORM , FEOP_ON )` 必须 0 诊断）：谁把这表删了，测试立刻红。
const char* const kArgCountExcluded[] = {
    "SET_JUDGE_MODE",        // §4.15 Format 1 参，示例同时有 (NORM) 与 (NORM, FEOP_ON)
    "SET_CAPTURE_MEM_MODE",  // §4.21
    "SET_OSC_CLK",           // §4.13 Format 2 参，示例 4 参 (clk, 100nS, 25nS, 75nS)
    "JUDGE_PAT",             // §4.15
    "JUDGE_VARIABLE",        // §4.17 可选尾巴在示例里时有时无
    "SET_SHMOO_X",           // §4.22 Format 2 参，另有 1 参示例
    "USE_WD_WAVEFORM",       // §4.23 Format 3 参，示例 2 参与 3 参并存
    "LOAD_ADDA_WAVEFORM",    // §4.23 Format 2 参，示例 1 参
    "PIN_MODE_HV",           // §5.5  Format 3 参，示例 5 参（含空槽）
    "INPUT_BOX",             // §5.1  实为 printf 式变参，Format 只写了 3 个
};
const int kArgCountExcludedCount =
    (int)(sizeof(kArgCountExcluded) / sizeof(kArgCountExcluded[0]));

bool IsArgCountExcluded(const char* name, std::size_t len) {
    for (int i = 0; i < kArgCountExcludedCount; ++i) {
        const char* e = kArgCountExcluded[i];
        std::size_t j = 0;
        for (; e[j] && j < len; ++j) {
            if (std::toupper((unsigned char)e[j]) != std::toupper((unsigned char)name[j])) break;
        }
        if (e[j] == '\0' && j == len) return true;
    }
    return false;
}

// 规则 8 主体：逐行找「语句起始位置」的语句名，读它的实参表，与签名推导的
// 必填数 / 上限比。
//
// 宽容之处（全都是为了不误报）：
//   · 实参表本行没闭合 `)`（跨行书写）→ 跳过；
//   · 表里出现**空槽**（`F, , G` 或 `…, ,)`）→ 跳过。手册的
//     `PIN_MODE_HV(hv_pins, NRZ, ,MASK, IO_NRZ)` 与真实工程文件的
//     `SET_LEVELN(…, 0V,,)` 都靠空槽占位来省略参数，而"空槽怎么算"手册没写；
//   · 签名有歧义，或推导上限与 paramCount 不等（双钥）→ 跳过；
//   · 语句不在库中、不在语句起始位置、或 (st->flags & kStmtPositional) 未置位 → 跳过。
//
// 高亮范围：两种诊断都只标**语句名**（不是实参表）。理由：少参数时"缺的那段"
// 在原文里根本不存在，标名字是唯一稳定的锚点；实参表的字节跨度可能很长，
// 画出来反而看不清。细节在 message 里（含手册章节号）。
void CheckArgumentCount(const std::vector<std::string>& code, std::vector<Diagnostic>& out) {
    for (std::size_t i = 0; i < code.size(); ++i) {
        const std::string& ln = code[i];
        for (std::size_t p = 0; p < ln.size();) {
            std::size_t col = 0, len = 0;
            if (!IdentAt(ln, p, ln.size(), col, len)) { ++p; continue; }
            p = col + len;

            if (!IsStatementStart(ln, col)) continue;
            if (IsArgCountExcluded(ln.c_str() + col, len)) continue;

            const StatementDef* st = FindStatement(ln.c_str() + col, len);
            if (!st) continue;
            if ((st->flags & kStmtPositional) == 0) continue;    // 槽序不可信
            if (st->paramCount <= 0) continue;

            ArgBounds b{0, 0};
            if (!DeriveArgBounds(st->signature ? st->signature : "", b)) continue;
            if (b.maxArgs <= 0 || b.maxArgs != st->paramCount) continue;   // 双钥

            std::size_t j = col + len;
            while (j < ln.size() && IsSpace(ln[j])) ++j;
            if (j >= ln.size() || ln[j] != '(') continue;        // 块头 / 无括号写法

            int depth = 0;
            std::size_t closed = kNone;
            for (std::size_t k = j; k < ln.size(); ++k) {
                if (ln[k] == '(') ++depth;
                else if (ln[k] == ')') { if (--depth == 0) { closed = k; break; } }
            }
            if (closed == kNone) continue;                       // 跨行 → 跳过

            int n = 0;
            const std::string inner = ln.substr(j + 1, closed - (j + 1));
            if (inner.find_first_not_of(" \t") != kNone) {
                const std::vector<Seg> args = SplitArgsIn(ln, j + 1, closed);
                bool emptySlot = false;
                for (const Seg& a : args) {
                    if (a.b >= a.e) { emptySlot = true; break; }
                }
                if (emptySlot) continue;                         // 空槽写法 → 跳过
                n = (int)args.size();
            }

            const std::string sec = st->section ? st->section : "";
            if (n > b.maxArgs) {
                PushDiag(out, (int)i, col, len, "C3380-PLN-010", 0,
                         std::string(st->name) + " 实参太多：手册 §" + sec + " 签名最多 " +
                             std::to_string(b.maxArgs) + " 个参数，这里给了 " +
                             std::to_string(n) + " 个");
            } else if (n < b.required) {
                PushDiag(out, (int)i, col, len, "C3380-PLN-011", 0,
                         std::string(st->name) + " 实参不足：手册 §" + sec + " 签名至少 " +
                             std::to_string(b.required) + " 个参数，这里只给了 " +
                             std::to_string(n) + " 个",
                         DiagSeverity::Warning);
            }
        }
    }
}

} // namespace

ChromaFileKind FileKindFromPath(const std::string& path) {
    std::size_t dot = path.rfind('.');
    if (dot == kNone) return ChromaFileKind::Unknown;
    const std::string ext = Upper(path.substr(dot));
    if (ext == ".PLN") return ChromaFileKind::Plan;
    if (ext == ".DEC") return ChromaFileKind::Dec;
    if (ext == ".PAT") return ChromaFileKind::Pattern;
    return ChromaFileKind::Unknown;
}

std::vector<Diagnostic> ValidateChromaSource(const std::string& text,
                                            ChromaFileKind kind) {
    std::vector<Diagnostic> out;
    if (kind == ChromaFileKind::Unknown) return out;

    const std::vector<LineSpan> lines = SplitLines(text);
    std::vector<std::string> code;
    code.reserve(lines.size());
    bool inBlock = false;
    for (const LineSpan& L : lines) code.push_back(BlankComments(text, L.begin, L.end, inBlock));

    if (kind == ChromaFileKind::Plan || kind == ChromaFileKind::Pattern) {
        CheckSetDecFileNoSemicolon(code, out);   // .dec 不适用（DEC_MODE 反而必须有分号）
    } else if (kind == ChromaFileKind::Dec) {
        CheckDeviceDefinition(code, out);
    }
    // 规则 8 只对 .pln 开：受检的 .pat 语句只有两条，而 .pat 没有真实样本可回归
    // （见 CheckArgumentCount 上方的说明）。
    if (kind == ChromaFileKind::Plan) {
        CheckArgumentCount(code, out);
    }

    std::stable_sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.start < b.start;
    });
    return out;
}

} // namespace chroma3380
} // namespace xfs
