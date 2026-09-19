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
#include <set>
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

// 行首标识符（col,len，来自 LeadingIdentIn）与关键字比较，大小写不敏感、**零分配**。
// 这一层是逐行调用的：上千万行的 `Upper(substr(...))` 副本分配扛不住，所以走逐字符
// 比较。语义与 `Upper(s.substr(col,len)) == word` 完全等价。
bool LeadingKeywordIs(const std::string& s, std::size_t col, std::size_t len,
                      const char* word) {
    std::size_t k = 0;
    for (; word[k]; ++k) {
        if (k >= len) return false;
        if (std::toupper((unsigned char)s[col + k]) !=
            std::toupper((unsigned char)word[k])) return false;
    }
    return k == len;
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

// pin 类型的"资源域"。手册与编译器都把 pin 分成**互不相通**的若干域：
//   §2.4.1  IO_ALLPINS   = { IN, OUT, IO }         —— 唯一性在这里面判定
//   §2.4.1  Notice：TMU pin-type & pin_group **can not** be assigned in the same
//           IO_ALLPINS  ⇒ TMU 自成一域
//   §2.5.1  UR_ALLPINS   = { UR }
//   §5.4.1  MLDPS_ALLPINS（功率脚族，MLDPS / DPS / UVI / PREF）
// 厂商编译器生成的 pin 初始化源码里确实只声明这四个 `*_ALLPINS` 默认组，佐证了该划分。
//
// **为什么要按域判定**（两条硬证据，都不是推断）：
//   ① 手册 §2.5.3 自己的 UR 官方示例就让 UR 脚复用信号脚的 ATE 号：
//        SEL0 = 0:288:320:352 = 1 = IN ;   …   UR_C0 = 0:1:2:3 = = UR ;
//      ATE 0/1/2/3 同时被 SEL0/SEL1/G1/SL 与 UR_C0 使用，手册判为**正确**写法。
//   ② 厂商的 GANG（多工位并测）范例工程里，功率脚与信号脚共用 dut#、用户继电器脚
//      与信号脚共用 ATE 通道号，而该工程**编译成功**：编译器生成的 pin 初始化源码
//      把全部 54 个 pin 原样声明（无去重、无报错），并产出了可加载的 DLL 与
//      测试程序可执行文件；同工程的编译记录显示最后一次编译成功于 2019-02-01。
//      （完整取证见项目私密文档，代码里不写那些文件名。）
// 因此"同一号码定义多次"只在**同一域内**才是错误；跨域复用是合法且常见的
// （多站点 pin 列表、功率脚模块编号、用户继电器复用通道）。
// 这与内核纪律一致：**宁可少报，不可误报**。
std::string PinDomain(const std::string& typeUpper) {
    if (typeUpper == "IN" || typeUpper == "OUT" || typeUpper == "IO") return "IO";
    return typeUpper;   // 其余每个类型自成一域（TMU / UR / MLDPS / DPS / UVI / PREF / GND…）
}

// 一块 PIN_LIST 里的三个"不得重复"的集合。
struct PinListScope {
    std::map<std::string, int> nameFirstLine;   // pin 名 → 首次出现的 0-based 行
    std::map<std::string, int> ateFirstLine;    // 域 + ATE 通道号 → 首次行
    std::map<std::string, int> dutFirstLine;    // 域 + DUT pin 号 → 首次行
};

// 把 (域, 号码) 合成一个键。用 0x1F（单元分隔符）分隔，号码本身是纯数字串，
// 不会与之冲突，因此不会出现"域 A 的 1"和"域 A1 的 x"撞键。
std::string DomainKey(const std::string& domain, const std::string& num) {
    std::string k = domain;
    k.push_back('\x1f');
    k += num;
    return k;
}

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

    // 末段 = pin_type（`pin_name = ate_pin = dut_pin = pin_type ;`）。
    // 类型认不出（空 / 非标识符）就**不判** 002/003：域未知时无法区分"跨域复用"
    // 与"同域重复"，按内核纪律宁漏不误。
    std::size_t tb = parts.back().b, te = parts.back().e;
    while (tb < te && std::isspace((unsigned char)s[tb])) ++tb;
    while (te > tb && std::isspace((unsigned char)s[te - 1])) --te;
    if (tb >= te) return;
    const std::string typeUp = Upper(s.substr(tb, te - tb));
    for (char c : typeUp) if (!IsIdChar((unsigned char)c)) return;
    const std::string domain = PinDomain(typeUp);

    // 倒数第二段 = dut_pin，其余中段 = ate_pin 列表
    const Seg& dutSeg = parts[parts.size() - 2];
    for (std::size_t k = 1; k + 2 < parts.size(); ++k) {
        for (const Num& n : NumbersIn(s, parts[k].b, parts[k].e)) {
            note(scope.ateFirstLine, DomainKey(domain, s.substr(n.b, n.e - n.b)),
                 n.b, n.e - n.b, "C3380-DEC-002", "ATE 通道");
        }
    }
    for (const Num& n : NumbersIn(s, dutSeg.b, dutSeg.e)) {
        note(scope.dutFirstLine, DomainKey(domain, s.substr(n.b, n.e - n.b)),
             n.b, n.e - n.b, "C3380-DEC-003", "DUT pin 号");
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
//   落入受检集的 .pat 语句只有 APM_PATTERN / SPM_PATTERN 两条，而这两条的签名本身
//   带可选方括号、参数表又整体可选，能推导出的边界没有信息量（对 .pln 有效的 229 条
//   语句签名库里它们本来就被分类器排除）。`.pat` 侧另有一条独立的规则 1
//   （C3380-PAT-001，向量宽度），不依赖签名库。

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

// ---------------------------------------------------------------------------
// 规则 1（.pat）：HEADER 声明的 pin 个数必须等于向量数据宽度
//
// 【取证】
//   · 培训教材 p45「Pattern档案格式」注意事项第 2 条（原文）：
//     「宣告由左至右的信号管脚顺序,**必须与向量主体字符宽度数目一一对应**」
//     —— 这是本规则唯一的**明文**依据；同一页第 1 条正是规则 2（SET_DEC_FILE 不加分号）。
//   · LM §3.3 HEADER（p40–41）定义 HEADER 是"the order of entry and display of the
//     actual binary information, on a pin basis"，§3.4.1.2 的 Pattern Symbol List
//     逐字符给出含义（0/1/L/H/X/Z…）⇒ **一个字符就是一个 pin**。
//   · LM §3.3.2 示例自洽：16 个 pin ↔ 16 个字符（p41）；§3.4.1.5 示例同为 16↔16。
//
// 【为什么 severity 是 Warning 而不是 Error】
//   按本模块的分档约定，Error 需要手册明文 `An error will occur`（全文只 2 处，都在
//   .dec 的 pin 规则）或"手册从未定义过这种写法"。本规则的"必须"出自**培训教材**而非
//   语言手册，且下一条的闸 B 说明手册自身有反例 ⇒ 依据强度不足以判 Error。
//   若将来拿到 CRAFT 编译器对此情形报错的实证，可提升为 Error。
//
// 【三条闸（每条都能独立证伪；这是零误报的全部理由）】
//   闸 A —— 文件里**恰好一条** HEADER，以 `;` 结束，且每一项都是 `[%]?identifier`。
//           多于一条 = 歧义；出现 `[`/空格/其它字符 = 我们没看懂这个写法 → 一律不报。
//           （LM §3.3.1 的 Format 行 `HEADER [%]pin_name, …` 本身就会被闸 A 挡掉。）
//   闸 B —— **向量行的分组结构必须与 HEADER 的 `%` 分组结构同形**（组数相等）。
//           LM §3.3.1 原文：`%` 是"A separator, indicating the compiler to use one space
//           between the previous pin and next pin, in all of the output from the system"，
//           即 **HEADER 里 `%` 的位置 = 向量数据里空格的位置**。这条对应关系是
//           独立于"宽度"的第二个观测量：只有两个观测量都指向"我们在比较同一件事"
//           时才允许判宽度。
//           ⚠️ 闸 B 是**必需的**，不是保守起见：LM §3.4.1.2 里两个 3360 时代的示例
//           （`HEADER CTRL1, %CLK, %QQ, %OAH;` 配 18 / 11 字符的向量）按字面就不满足
//           一一对应（4 项 vs 7 组 / 5 组）。没有闸 B，照手册自己的示例回放就会误报。
//           把手册全文 32 处 HEADER 全部回放：**所有真实配对的示例都通过闸 B 且宽度
//           一致（0 命中）**，被判红的只有那两个自相矛盾的示例 —— 它们被闸 B 拦下。
//   闸 C —— 块内向量行的宽度与 pin 数不等时才报，且**每个模块只报一条**
//           （锚在第一条不符的向量行）。这是必须的：一个模块可能有上千万条向量行，
//           逐行上报会把诊断面板和波浪线一起压垮，而"HEADER 写错"这种最常见的情形
//           本来就该只提示一次。
//
// 【为什么锚在向量行而不是 HEADER】
//   宽度是在向量行上**量**出来的，第一条不符的行就是最直接的证据；消息里带上
//   HEADER 的行号，两个位置都能找到。锚在 HEADER 上在"HEADER 对、个别向量行写错"
//   时会指向错误的地方。
//
// 【与 pin_group 的关系（已知的规则边界）】
//   LM §3.3.1 说明 HEADER 的项可以是 pin_group，而 pin_group 在向量里是
//   **十六进制**表示（一个字符可能代表 4 个 pin），此时宽度与 pin 数本来就不相等。
//   本模块**无法**从 .pat 单独判断某个名字是 pin 还是 pin_group（那要读 .dec），
//   所以规则不区分二者 —— 这类写法由闸 B 兜底（十六进制表示的分组结构与 `%` 声明
//   通常不同形）。这是**有意的保守**：宁可对 pin_group 文件不报，也不误报。
//
// 【输出上限】每个模块一条，另设 64 条硬上限防止畸形文件（几十万个空块）把面板刷爆。

struct HeaderDecl {
    bool             found     = false;
    bool             ambiguous = false;   // 出现多于一条 HEADER
    bool             valid     = false;   // 闸 A 通过
    int              line      = 0;       // HEADER 关键字所在行（0-based）
    int              pinCount  = 0;
    std::vector<int> groupSizes;          // 按 `%` 切出的各组 pin 数
};

// 把 HEADER 的 pin 列表累积起来（可跨行，LM §3.3.2 的示例就跨了两行）。
// 返回 false = 在合理行数内没有遇到 `;`（未闭合）→ 闸 A 不放行。
bool AccumulateHeaderBody(const std::vector<std::string>& code, std::size_t line,
                          std::size_t from, std::string& body) {
    for (std::size_t k = line; k < code.size() && k < line + 16; ++k) {
        const std::string& L = code[k];
        const std::size_t start = (k == line) ? from : 0;
        const std::size_t semi = L.find(';', start);
        if (semi != kNone) {
            body += L.substr(start, semi - start);
            return true;
        }
        body += L.substr(start);
    }
    return false;
}

HeaderDecl FindHeaderDecl(const std::vector<std::string>& code) {
    HeaderDecl h;

    // 第一遍：数出所有 HEADER 语句。**必须两遍**——一条一条边找边解析时，遇到第一条
    // 合法 HEADER 就会返回，后面的 HEADER 根本看不到，"多于一条=歧义"永远不成立
    // （这个 bug 单测 t2 抓到过）。LM 的 .pat 只有一条 HEADER；多条时我们无法判断
    // 向量该对哪一条 → 整体放弃，宁可漏报。
    int          count     = 0;
    int          firstLine = -1;
    std::size_t  firstFrom = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(code[i], 0, code[i].size(), col, len)) continue;
        if (Upper(code[i].substr(col, len)) != "HEADER") continue;
        ++count;
        if (firstLine < 0) { firstLine = (int)i; firstFrom = col + len; }
    }
    if (count == 0) return h;
    h.found = true;
    if (count > 1) { h.ambiguous = true; return h; }

    h.line = firstLine;

    std::string body;
    if (!AccumulateHeaderBody(code, (std::size_t)firstLine, firstFrom, body)) return h;

    const std::vector<Seg> items = SplitTopIn(body, 0, body.size(), ',');
    if (items.empty()) return h;

    int cur = 0;
    for (const Seg& g : items) {
        std::size_t p = g.b;
        const bool startsNew = (p < g.e && body[p] == '%');   // `%` 开启新分组
        if (p < g.e && body[p] == '%') ++p;
        if (p >= g.e || !IsIdStart((unsigned char)body[p])) return h;
        std::size_t q = p;
        while (q < g.e && IsIdChar((unsigned char)body[q])) ++q;
        if (q != g.e) return h;
        if (startsNew && cur > 0) { h.groupSizes.push_back(cur); cur = 0; }
        ++cur;
    }
    if (cur > 0) h.groupSizes.push_back(cur);
    h.pinCount = (int)items.size();
    h.valid = true;
    return h;
}

// 一条向量行的"形状"：数据段按空白切出的组数、非空白字符总数（= 向量宽度），
// 以及数据段在行内的范围。
// 只认两种形状：行首（跳过空白）是 `*`，或 `Label::` / `Label:` 前缀后接 `*`。
// 含 `#`（ape_field，LM §3.4.1.1 的第三种向量形态）的行不参与比较。
//
// ⚠️ 这里的写法**不是**随意的，请不要"顺手简化"回逐字符循环：
//   最初的实现是最自然的"逐字符 range-for，遇到空白就把当前计数 push_back 进
//   分组数组"。MSVC 14.51（VS 2026，PlatformToolset v145）在 /O2 下把它误编译成
//   "统计非空白字符总数、只在循环结束后 push 一次"——`00 0 0` 得到 `{4}` 而不是
//   `{2,1,1}`（/Od 下正确）。后果是闸 B 判定"组数不同形"而静默放行 → 规则 1 漏报，
//   且**只在 Release 下漏**。
//   已用约 20 行独立程序复现：同一算法写成 7 种形态，只有"逐字符 range-for（或裸
//   指针循环）+ `else { ++cur; }`"这一形态出错；换成下标循环、`continue` 风格或
//   下面的标准库写法都正确。⚠️ 换写法是**可能再次踩中**的：只有这一种形态出错，
//   不是因为"range-for 不能用于字符串"。改动后必须在 Release 下重跑
//   tests/test_chromadiag.cpp 的 RunHeaderVectorWidth（Debug 全绿不算数）。
//   现在的写法把"逐字符分支 + 条件写入"整个拆掉了：宽度交给 std::count_if，分组
//   边界交给 find_first_of。两段都是标准库调用，没有可供优化器改写的归纳变量。
//   Release / Debug 输出逐字节一致（已用独立程序对撞验证）。
bool VectorShape(const std::string& s, int& groups, int& width,
                 int& dataStart, int& dataLen) {
    std::size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) ++i;
    if (i >= s.size()) return false;
    if (s[i] != '*') {
        if (!IsIdStart((unsigned char)s[i])) return false;
        std::size_t j = i;
        while (j < s.size() && IsIdChar((unsigned char)s[j])) ++j;
        if (j >= s.size() || s[j] != ':') return false;
        ++j;
        if (j < s.size() && s[j] == ':') ++j;      // `::` 全局标签
        while (j < s.size() && IsSpace(s[j])) ++j;
        if (j >= s.size() || s[j] != '*') return false;
        i = j;
    }
    const std::size_t a = i;
    const std::size_t b = s.rfind('*');
    if (b <= a) return false;
    const std::string inner = s.substr(a + 1, b - a - 1);
    if (inner.find('#') != kNone) return false;

    width = (int)std::count_if(inner.begin(), inner.end(),
                               [](char c) { return !IsSpace(c); });
    groups = 0;
    for (std::size_t p = 0; p < inner.size();) {
        const std::size_t q = inner.find_first_of(" \t", p);
        const std::size_t e = (q == kNone) ? inner.size() : q;
        if (e > p) ++groups;
        p = (q == kNone) ? inner.size() : q + 1;
    }
    if (groups == 0) return false;

    dataStart = (int)(a + 1);
    dataLen = (int)(b - a - 1);
    return true;
}

void CheckHeaderVectorWidth(const std::vector<std::string>& code,
                            std::vector<Diagnostic>& out) {
    const HeaderDecl h = FindHeaderDecl(code);
    if (!h.found || h.ambiguous || !h.valid) return;     // 闸 A

    int emitted = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(code[i], 0, code[i].size(), col, len)) continue;
        const std::string tok = Upper(code[i].substr(col, len));
        if (tok != "SPM_PATTERN" && tok != "APM_PATTERN" && tok != "RPM_PATTERN") continue;

        // 找块开括号（可以在语句的下一行，真实 .pat 就是这种写法）。
        std::size_t bl = 0, bc = 0;
        bool opened = false;
        for (std::size_t k = i; k < code.size() && k < i + 8; ++k) {
            const std::size_t p = code[k].find('{', k == i ? col + len : 0);
            if (p != kNone) { bl = k; bc = p; opened = true; break; }
        }
        if (!opened) continue;
        std::size_t el = 0, ec = 0;
        if (!FindBlockEnd(code, bl, bc, el, ec)) continue;   // 未闭合 → 不报

        int firstLine = -1, firstStart = 0, firstLen = 0, firstWidth = 0;
        int mismatched = 0;
        for (std::size_t k = bl; k <= el; ++k) {
            int groups = 0, w = 0, ds = 0, dl = 0;
            if (!VectorShape(code[k], groups, w, ds, dl)) continue;
            if (groups != (int)h.groupSizes.size()) continue;        // 闸 B
            if (w == h.pinCount) continue;
            ++mismatched;
            if (firstLine < 0) {
                firstLine = (int)k; firstStart = ds; firstLen = dl; firstWidth = w;
            }
        }
        if (firstLine < 0) continue;

        std::string msg = "向量宽度 " + std::to_string(firstWidth) +
                          " 与 HEADER（第 " + std::to_string(h.line + 1) +
                          " 行）声明的 " + std::to_string(h.pinCount) + " 个 pin 不一致";
        if (mismatched > 1) msg += "（本模块共 " + std::to_string(mismatched) + " 行如此）";
        PushDiag(out, firstLine, (std::size_t)firstStart, (std::size_t)firstLen,
                 "C3380-PAT-001", 41, std::move(msg), DiagSeverity::Warning);

        if (++emitted >= 64) break;   // 畸形文件保护：正常 .pat 的模块数远小于此
    }
}

// ---------------------------------------------------------------------------
// 规则 4：SPM_PATTERN 的 NORM / DBL 配 K_SET / Z_SET = compiler error（批次 92）
// ---------------------------------------------------------------------------
// 取证、severity 分档、为什么**只对 SPM_PATTERN 开** —— 见头文件同节注释。
// 实现要点：
//   · 只在**同时**认出"第 2 个实参是三个 mode 之一"且"第 3 个实参是三个 set 之一"
//     时才判。任一个认不出（变量、拼写变体、手册将来新增的关键字）→ 整体放弃，
//     宁可漏报。
//   · 实参个数不是 2 / 3 一律不判：>3 是手册从未定义过的形式，不属本规则的地盘。
//   · 只认**同一行内闭合**的实参表。跨行的 SPM_PATTERN 头在手册与真实样本里都没
//     出现过，不猜。
//   · 高亮锚在**第 3 个实参**上：两种修法（删掉 set，或把 mode 改成 DBL_2X）都落在
//     那个 token 上。
bool PatternModeIsIllegal(const std::string& line, std::size_t& col, std::size_t& len,
                          std::string& mode, std::string& set) {
    std::size_t ic = 0, il = 0;
    if (!LeadingIdentIn(line, 0, line.size(), ic, il)) return false;
    if (!LeadingKeywordIs(line, ic, il, "SPM_PATTERN")) return false;

    const std::size_t p = line.find('(', ic + il);
    if (p == kNone) return false;
    std::size_t q = kNone;
    int depth = 0;
    for (std::size_t i = p; i < line.size(); ++i) {
        if (line[i] == '(') {
            ++depth;
        } else if (line[i] == ')') {
            --depth;
            if (depth == 0) { q = i; break; }
        }
    }
    if (q == kNone) return false;                        // 实参表未闭合 → 不判

    const std::vector<Seg> args = SplitArgsIn(line, p + 1, q);
    if (args.size() != 2 && args.size() != 3) return false;

    mode = Upper(line.substr(args[1].b, args[1].e - args[1].b));
    if (mode != "NORM" && mode != "DBL" && mode != "DBL_2X") return false;
    if (args.size() < 3) return false;                   // 只给 mode → 合法

    set = Upper(line.substr(args[2].b, args[2].e - args[2].b));
    if (set != "NORM_SET" && set != "K_SET" && set != "Z_SET") return false;
    if (mode == "DBL_2X" || set == "NORM_SET") return false;   // 手册明文允许的组合

    col = args[2].b;
    len = args[2].e - args[2].b;
    return true;
}

void CheckPatternMode(const std::vector<std::string>& code, std::vector<Diagnostic>& out) {
    for (std::size_t i = 0; i < code.size(); ++i) {
        std::size_t col = 0, len = 0;
        std::string mode, set;
        if (!PatternModeIsIllegal(code[i], col, len, mode, set)) continue;
        std::string msg = "SPM_PATTERN 的 " + mode + " 模式不接受 " + set +
                          "（手册：只有 DBL_2X 允许 K_SET / Z_SET）";
        PushDiag(out, (int)i, col, len, "C3380-PAT-002", 45, std::move(msg),
                 DiagSeverity::Error);
    }
}

// ---------------------------------------------------------------------------
// 规则 10：RPT 的重复次数必须在 2 .. 16777215（批次 92）
// ---------------------------------------------------------------------------
// 取证、severity 分档见头文件同节注释。实现要点：
//   · 只在模块块内的**向量行**上、且只在**最后一个 `*` 之后**扫描 —— 两个 `*` 之间
//     是 pattern_data 数据字符，不参与。
//   · `RPT` 必须**整词**：`RPTN`（寄存器版重复）与 `TS_RPT` 这类都不算。
//   · 紧跟 `RPT` 的必须是**纯十进制字面量**（中间只许空白，数字后不能再接标识符
//     字符）。认不出（变量、表达式、别的助记符）→ 不判：RPT 只吃字面量，寄存器版
//     另有 RPTN。
//   · 解析做溢出保护：位数 > 8 位直接按"越界"处理，不做整数转换。
//   · 与规则 1 同样设 64 条硬上限：一个模块可能有上千万条向量行。
const long long kRptMinValue = 2;
const long long kRptMaxValue = 16777215;      // 手册：24bit register

// 整词、大小写不敏感地在 [from, to) 里找 word，返回下标；没有则 kNone。
// 刻意不用 Upper() 造副本：这一层是**逐行**调用的，上千万次分配扛不住。
std::size_t FindWholeWordIn(const std::string& s, std::size_t from, std::size_t to,
                            const char* word) {
    std::size_t wl = 0;
    while (word[wl]) ++wl;
    if (wl == 0 || to < wl) return kNone;
    for (std::size_t i = from; i + wl <= to; ++i) {
        if (i > 0 && IsIdChar((unsigned char)s[i - 1])) continue;          // `TS_RPT`
        if (i + wl < to && IsIdChar((unsigned char)s[i + wl])) continue;   // `RPTN`
        std::size_t k = 0;
        while (k < wl && std::toupper((unsigned char)s[i + k]) ==
                             std::toupper((unsigned char)word[k])) ++k;
        if (k == wl) return i;
    }
    return kNone;
}

// `RPT` 之后（从 from 起）必须紧跟十进制字面量。false = 认不出，不判。
bool RptValueAfter(const std::string& s, std::size_t from, std::size_t to,
                   long long& value, std::size_t& numCol, std::size_t& numLen) {
    std::size_t i = from;
    while (i < to && IsSpace(s[i])) ++i;
    if (i >= to || std::isdigit((unsigned char)s[i]) == 0) return false;
    std::size_t j = i;
    while (j < to && std::isdigit((unsigned char)s[j]) != 0) ++j;
    if (j < to && IsIdChar((unsigned char)s[j])) return false;   // `RPT 100x` 认不出
    numCol = i;
    numLen = j - i;
    if (numLen > 8) { value = kRptMaxValue + 1; return true; }   // 溢出保护 → 越界
    long long v = 0;
    for (std::size_t k = i; k < j; ++k) v = v * 10 + (s[k] - '0');
    value = v;
    return true;
}

void CheckRptCount(const std::vector<std::string>& code, std::vector<Diagnostic>& out) {
    int emitted = 0;
    for (std::size_t i = 0; i < code.size() && emitted < 64; ++i) {
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(code[i], 0, code[i].size(), col, len)) continue;
        if (!LeadingKeywordIs(code[i], col, len, "SPM_PATTERN") &&
            !LeadingKeywordIs(code[i], col, len, "APM_PATTERN") &&
            !LeadingKeywordIs(code[i], col, len, "RPM_PATTERN")) continue;

        std::size_t bl = 0, bc = 0;
        bool opened = false;
        for (std::size_t k = i; k < code.size() && k < i + 8; ++k) {
            const std::size_t p = code[k].find('{', k == i ? col + len : 0);
            if (p != kNone) { bl = k; bc = p; opened = true; break; }
        }
        if (!opened) continue;
        std::size_t el = 0, ec = 0;
        if (!FindBlockEnd(code, bl, bc, el, ec)) continue;   // 未闭合 → 不报

        for (std::size_t k = bl; k <= el && emitted < 64; ++k) {
            const std::string& L = code[k];
            const std::size_t star = L.rfind('*');
            if (star == kNone || star + 1 >= L.size()) continue;
            // 必须**至少两个** `*`（pattern_data 的起止符）。只有一个说明这行残缺，
            // 那"最后一个 `*` 之后"其实是数据区，扫它等于在向量数据里找 RPT。
            if (L.find('*') == star) continue;
            const std::size_t at = FindWholeWordIn(L, star + 1, L.size(), "RPT");
            if (at == kNone) continue;
            long long v = 0;
            std::size_t nc = 0, nl = 0;
            if (!RptValueAfter(L, at + 3, L.size(), v, nc, nl)) continue;
            if (v >= kRptMinValue && v <= kRptMaxValue) continue;
            std::string msg = "RPT 次数 " + L.substr(nc, nl) +
                              " 超出手册允许的 2 .. 16777215";
            PushDiag(out, (int)k, nc, nl, "C3380-PAT-003", 44, std::move(msg),
                     DiagSeverity::Warning);
            ++emitted;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 规则 3：DEC_MODE APAS → IMATCH 失效（批次 88，跨文件）
// ---------------------------------------------------------------------------
// 取证、severity 分档、为什么这条跨文件规则能开口 —— 全部见头文件同节注释。
// 实现只补头文件没写的细节。

namespace {

bool EqualCI(const std::string& s, std::size_t at, const char* word) {
    for (std::size_t i = 0; word[i]; ++i) {
        if (at + i >= s.size()) return false;
        if (std::toupper((unsigned char)s[at + i]) !=
            std::toupper((unsigned char)word[i])) return false;
    }
    return true;
}

} // namespace

std::vector<DecFileRef> FindDecFileRefs(const std::string& text) {
    std::vector<DecFileRef> out;
    const std::vector<LineSpan> lines = SplitLines(text);
    bool inBlock = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string m = BlankComments(text, lines[i].begin, lines[i].end, inBlock);
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(m, 0, m.size(), col, len)) continue;
        if (Upper(m.substr(col, len)) != "SET_DEC_FILE") continue;
        // 抹平行里引号保留原位 —— 用它定位**原文**里的路径字节区间（路径内容
        // 在抹平行里已变成空格，必须回原文取）。
        const std::size_t q1 = m.find('"', col + len);
        if (q1 == kNone) continue;                       // 没引号 → 跳过
        const std::size_t q2 = m.find('"', q1 + 1);
        if (q2 == kNone || q2 <= q1 + 1) continue;       // 没闭合 / 空路径 → 跳过
        DecFileRef r;
        r.line = (int)i;
        r.start = (int)(q1 + 1);
        r.length = (int)(q2 - q1 - 1);
        r.path = text.substr(lines[i].begin + q1 + 1, (std::size_t)r.length);
        out.push_back(std::move(r));
    }
    return out;
}

bool DecDeclaresApas(const std::string& decText) {
    const std::vector<LineSpan> lines = SplitLines(decText);
    bool inBlock = false;
    bool sawApas = false, sawNorm = false;
    for (const LineSpan& L : lines) {
        const std::string m = BlankComments(decText, L.begin, L.end, inBlock);
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(m, 0, m.size(), col, len)) continue;
        if (Upper(m.substr(col, len)) != "DEC_MODE") continue;
        // 整词扫这一行剩下的标识符：见到 APAS 记 APAS，见到 NORM 记 NORM。
        // 两种同时出现 = 声明歧义（语义手册没写）→ 最后按"无歧义"口径裁决。
        for (std::size_t p = col + len; p < m.size();) {
            std::size_t c2 = 0, l2 = 0;
            if (!IdentAt(m, p, m.size(), c2, l2)) { ++p; continue; }
            const std::string w = Upper(m.substr(c2, l2));
            if (w == "APAS") sawApas = true;
            else if (w == "NORM") sawNorm = true;
            p = c2 + l2;
        }
    }
    return sawApas && !sawNorm;
}

std::vector<Diagnostic> CheckApasImatch(const std::string& text,
                                        const std::vector<std::string>& decTexts) {
    std::vector<Diagnostic> out;
    bool apas = false;
    for (const std::string& dec : decTexts) {
        if (DecDeclaresApas(dec)) { apas = true; break; }
    }
    if (!apas) return out;                               // 没读到 / 非 APAS → 静默

    const std::vector<LineSpan> lines = SplitLines(text);
    bool inBlock = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string m = BlankComments(text, lines[i].begin, lines[i].end, inBlock);
        for (std::size_t p = 0; p + 6 <= m.size();) {
            if (!EqualCI(m, p, "IMATCH")) { ++p; continue; }
            const bool leftOk = (p == 0) || !IsIdChar((unsigned char)m[p - 1]);
            const bool rightOk = (p + 6 >= m.size()) || !IsIdChar((unsigned char)m[p + 6]);
            if (leftOk && rightOk) {
                PushDiag(out, (int)i, p, 6, "C3380-XFILE-001", 44,
                         "IMATCH 仅支持 NORM 模式的向量（手册 §3.4.1.3），而本文件"
                         "引用的 .dec 声明了 DEC_MODE APAS —— APAS 下 IMATCH 失效"
                         "（培训教材 p43）",
                         DiagSeverity::Warning);
                p += 6;
                continue;
            }
            ++p;   // 词边界不成立（IMATCHX / X_IMATCH）→ 继续找下一个
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 批次 89：ExtractDecSymbols —— 跨文件补全的词源（见头文件同节注释）
// ---------------------------------------------------------------------------

std::vector<std::string> ExtractDecSymbols(const std::string& decText) {
    std::vector<std::string> out;
    std::set<std::string> uniq;
    const std::vector<LineSpan> lines = SplitLines(decText);
    std::vector<std::string> code;
    code.reserve(lines.size());
    bool inBlock = false;
    for (const LineSpan& L : lines)
        code.push_back(BlankComments(decText, L.begin, L.end, inBlock));

    // 块扫描与 CheckDeviceDefinition 同一套口径：行首标识符认块、`{` 可隔几行、
    // FindBlockEnd 配平；认不出就跳过（块头撞别的标识符 → 停止前瞻）。
    std::size_t i = 0;
    while (i < code.size()) {
        std::size_t col = 0, len = 0;
        if (!LeadingIdentIn(code[i], 0, code[i].size(), col, len)) { ++i; continue; }
        const std::string kw = Upper(code[i].substr(col, len));
        const bool isPinList = (kw == "PIN_LIST");
        const bool isNamedBlock = (kw == "PIN_GROUP" || kw == "UR_PIN_GROUP" ||
                                   kw == "POWER_PIN_GROUP" || kw == "TIME_NAME_DEF");
        if (!isPinList && !isNamedBlock) { ++i; continue; }

        std::size_t braceLine = kNone, braceCol = 0;
        for (std::size_t k = i; k < code.size() && k <= i + 4; ++k) {
            std::size_t p = code[k].find('{', k == i ? col + len : 0);
            if (p != kNone) { braceLine = k; braceCol = p; break; }
            if (k > i) {
                std::size_t c2 = 0, l2 = 0;
                if (LeadingIdentIn(code[k], 0, code[k].size(), c2, l2)) break;
            }
        }
        if (braceLine == kNone) { ++i; continue; }
        std::size_t endLine = 0, endCol = 0;
        if (!FindBlockEnd(code, braceLine, braceCol, endLine, endCol)) { ++i; continue; }

        auto addName = [&](std::string name) {
            if (name.empty() || name.size() > 64) return;   // 手册 §2.7：max 64 chars
            if (!IsIdStart((unsigned char)name[0])) return;
            for (char c : name) if (!IsIdChar((unsigned char)c)) return;
            if (uniq.insert(name).second) out.push_back(std::move(name));
        };

        for (std::size_t k = braceLine; k <= endLine; ++k) {
            const std::size_t b = (k == braceLine) ? braceCol + 1 : 0;
            const std::size_t e = (k == endLine) ? endCol : code[k].size();
            if (b >= e) continue;
            const std::size_t last = LastCodeIn(code[k], b, e);
            if (last == kNone || code[k][last] != ';') continue;   // 宽容：没分号不猜
            const std::size_t eq = code[k].find('=', b);
            if (eq == kNone || eq > last) continue;                // 无 `=` 的行不猜
            std::size_t c3 = 0, l3 = 0;
            if (!LeadingIdentIn(code[k], b, eq, c3, l3)) continue;
            addName(code[k].substr(c3, l3));
        }
        i = endLine + 1;
    }
    return out;
}

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
    if (kind == ChromaFileKind::Pattern) {
        CheckHeaderVectorWidth(code, out);       // 规则 1：HEADER pin 数 == 向量宽度
        CheckPatternMode(code, out);             // 规则 4：SPM_PATTERN 模式 / 设置组合
        CheckRptCount(code, out);                // 规则 10：RPT 重复次数区间
    }
    // 规则 8 只对 .pln 开：受检的 .pat 语句只有两条，且规则 8 的签名库按 .pln 语句
    // 建立（见 CheckArgumentCount 上方的说明）。.pat 侧的规则 1 已单独实现。
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
