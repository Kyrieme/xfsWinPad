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
              const char* codeStr, int page, std::string msg) {
    Diagnostic d;
    d.line = line;
    d.start = (int)col;
    d.length = (int)(len > 0 ? len : 1);
    d.severity = DiagSeverity::Error;
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

    std::stable_sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.start < b.start;
    });
    return out;
}

} // namespace chroma3380
} // namespace xfs
