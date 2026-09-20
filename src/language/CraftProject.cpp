#include "language/CraftProject.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace xfs {
namespace craft {

namespace {

// ---------------------------------------------------------------------------
// 小工具（本文件自足，不引 Util / 不碰 Windows API —— 内核要能单测）
// ---------------------------------------------------------------------------

std::string TrimAscii(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (std::isspace((unsigned char)s[b]))) ++b;
    while (e > b && (std::isspace((unsigned char)s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::wstring TrimWide(const std::wstring& s) {
    std::size_t b = 0, e = s.size();
    auto ws = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\0';
    };
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// makefile / 中间产物都是 ASCII（含少量 latin-1 字节），逐字节升位即可 ——
// 不走 Utf8ToWide 是为了让内核不依赖代码页，也不依赖 core/Util。
std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

wchar_t LowerW(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
    return c;
}

std::wstring LowerWide(const std::wstring& s) {
    std::wstring r = s;
    for (wchar_t& c : r) c = LowerW(c);
    return r;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    const std::wstring a = LowerWide(s.substr(s.size() - suffix.size()));
    return a == LowerWide(suffix);
}

bool IsIdentStart(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
}
bool IsIdentChar(wchar_t c) {
    return IsIdentStart(c) || (c >= L'0' && c <= L'9');
}

std::vector<std::wstring> TokenizeWide(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' ||
                                s[i] == L'\n'))
            ++i;
        std::size_t b = i;
        while (i < s.size() && !(s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' ||
                                 s[i] == L'\n'))
            ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

// 按 \n 切行并去掉行尾 \r（makefile / 编译器输出 / 各种 .ver 都是 CRLF）
std::vector<std::string> SplitLinesAscii(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= text.size()) {
        std::size_t nl = text.find('\n', i);
        std::string line = (nl == std::string::npos) ? text.substr(i)
                                                     : text.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    // 末尾若是文件最后一个换行产生的空串，去掉它（不是"丢内容"）
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::wstring JoinNonEmpty(const std::vector<std::wstring>& parts) {
    std::wstring r;
    for (const std::wstring& p : parts) {
        if (p.empty()) continue;
        if (!r.empty()) r.push_back(L' ');
        r += p;
    }
    return r;
}

std::uint32_t ReadU32LE(const std::string& s, std::size_t off) {
    return (std::uint32_t)(unsigned char)s[off] |
           ((std::uint32_t)(unsigned char)s[off + 1] << 8) |
           ((std::uint32_t)(unsigned char)s[off + 2] << 16) |
           ((std::uint32_t)(unsigned char)s[off + 3] << 24);
}

// 纯 ASCII 数字串 → int。非数字返回 0（调用方已保证全是数字）。
// 不走 std::atoi(wstring→string) 是为了不触发 C4244 窄化警告。
int ParseDigits(const std::wstring& s) {
    int v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return 0;
        v = v * 10 + (int)(c - L'0');
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// 一、工具链
// ---------------------------------------------------------------------------

const wchar_t* ToolOriginName(ToolOrigin o) {
    switch (o) {
        case ToolOrigin::Settings:  return L"设置";
        case ToolOrigin::CraftHome: return L"CRAFT_HOME";
        case ToolOrigin::Path:      return L"PATH";
        default:                    return L"";
    }
}

namespace {

// 依次尝试三个来源。候选可以是完整路径（设置目录 / CRAFT_HOME 下），
// 也可以是裸名字（PATH 查找）—— 都由宿主那个回调统一裁决"存不存在"。
bool TryFind(const std::function<bool(const std::wstring&, std::wstring&)>& find,
             const std::wstring& candidate, std::wstring& full) {
    if (candidate.empty() || !find) return false;
    return find(candidate, full);
}

// 一个工具在三个来源里依次找：设置目录 → CRAFT_HOME\bin → PATH。
// **没配置的来源要跳过**，不能退化成"用裸名字去 PATH 找" —— 否则设置目录为空时
// 会误报成"从设置里找到的"，UI 提示就指错了地方。
// 每个来源都先试带 .exe 再试不带（现场两种装法都见过）。
bool FindTool(const std::wstring& name,
              const std::wstring& settingsDir,
              const std::wstring& craftHome,
              const std::function<bool(const std::wstring&, std::wstring&)>& find,
              std::wstring& full, ToolOrigin& origin) {
    const std::wstring exe = name + L".exe";
    struct Src { ToolOrigin o; std::wstring dir; bool isPath; };
    const Src srcs[] = {
        { ToolOrigin::Settings,  settingsDir, false },
        { ToolOrigin::CraftHome,
          craftHome.empty() ? std::wstring() : craftHome + L"\\bin", false },
        { ToolOrigin::Path,      std::wstring(), true },
    };
    for (const Src& s : srcs) {
        if (!s.isPath && s.dir.empty()) continue;   // 没配置 → 跳过
        const std::wstring prefix = s.isPath ? std::wstring() : s.dir + L"\\";
        if (TryFind(find, prefix + exe, full) || TryFind(find, prefix + name, full)) {
            origin = s.o;
            return true;
        }
    }
    full.clear();
    origin = ToolOrigin::None;
    return false;
}

} // namespace

Toolchain DetectToolchain(
    const std::wstring& settingsDir,
    const std::wstring& craftHome,
    const std::function<bool(const std::wstring&, std::wstring&)>& findOnPath) {
    Toolchain t;
    t.craftHome = TrimWide(craftHome);
    FindTool(L"plncmp", settingsDir, t.craftHome, findOnPath, t.plncmp, t.plncmpOrigin);
    FindTool(L"patcmp", settingsDir, t.craftHome, findOnPath, t.patcmp, t.patcmpOrigin);
    return t;
}

// ---------------------------------------------------------------------------
// 二、makefile
// ---------------------------------------------------------------------------

namespace {

// 把"续行"接起来：行尾 `\` 表示下一行是同一逻辑行。
//
// **坑（真实工程里踩到的）**：厂商 makefile 的续行反斜杠**后面还跟着一个空格**，
// 写成 `...\func.pat\ `（`cat -A` 下是 `\ $`）。所以判续行必须先剥行尾空白
// 再看最后一个字符 —— 只判"行尾是反斜杠"会把整条续行丢掉，后果是**少编译一个
// .pat**（命令少跑一条，而且不报错）。单测里钉了这一条。
std::vector<std::string> LogicalLines(const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (const std::string& line : raw) {
        std::string s = line;
        // `#` 到行尾是注释。**只在这里剥**，剥之前先判续行（注释行不会带续行）。
        const std::size_t hash = s.find('#');
        const std::string probe = (hash == std::string::npos) ? s : s.substr(0, hash);
        std::size_t pe = probe.size();
        while (pe > 0 && (probe[pe - 1] == ' ' || probe[pe - 1] == '\t')) --pe;
        const bool continued = (pe > 0 && probe[pe - 1] == '\\');

        if (hash != std::string::npos) s = s.substr(0, hash);
        if (continued) {
            // 去掉行尾空白，再去掉那个反斜杠本身
            std::size_t se = s.size();
            while (se > 0 && (s[se - 1] == ' ' || s[se - 1] == '\t')) --se;
            if (se > 0) s = s.substr(0, se - 1);
            cur += s;
            cur += ' ';   // 换行处补空格，避免 `$(A)\` + `$(B)` 粘成一个 token
        } else {
            cur += s;
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 只展开一层 `$(NAME)`；认不出的名字**原样保留**（例如 $(CRAFT_HOME) 是环境变量）。
std::wstring ExpandVars(const std::wstring& s,
                        const std::vector<std::pair<std::wstring, std::wstring>>& vars,
                        int depth) {
    if (depth > 4) return s;
    std::wstring out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == L'$' && i + 1 < s.size() && s[i + 1] == L'(') {
            std::size_t close = s.find(L')', i + 2);
            if (close != std::wstring::npos) {
                const std::wstring name = s.substr(i + 2, close - i - 2);
                const std::wstring* val = nullptr;
                for (const auto& kv : vars) {
                    if (kv.first == name) { val = &kv.second; break; }
                }
                if (val) {
                    out += ExpandVars(*val, vars, depth + 1);
                    i = close + 1;
                    continue;
                }
                out += s.substr(i, close - i + 1);   // 未定义：原样保留
                i = close + 1;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

} // namespace

MakeVars ParseMakefile(const std::string& text) {
    MakeVars v;
    const std::vector<std::string> raw = SplitLinesAscii(text);
    const std::vector<std::string> logical = LogicalLines(raw);

    // 第一遍：收集所有 `NAME = value`（保留出现顺序，后定义覆盖先定义）
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    std::vector<std::string> ruleLines;
    for (const std::string& ln : logical) {
        std::string s = ln;
        // 行首空白（续行会带进来）
        std::size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
        std::string t = s.substr(b);
        if (t.empty()) continue;

        // 赋值：NAME = value  /  NAME := value  /  NAME ?= value
        std::size_t i = 0;
        if (i < t.size() && (std::isalpha((unsigned char)t[i]) || t[i] == '_')) {
            while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_')) ++i;
            std::size_t j = i;
            while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
            bool assign = false;
            if (j < t.size() && t[j] == '=') { assign = true; ++j; }
            else if (j + 1 < t.size() && (t[j] == ':' || t[j] == '?') && t[j + 1] == '=') {
                assign = true; j += 2;
            }
            if (assign) {
                const std::wstring name = Widen(t.substr(0, i));
                const std::wstring val = TrimWide(Widen(t.substr(j)));
                bool replaced = false;
                for (auto& kv : vars) {
                    if (kv.first == name) { kv.second = val; replaced = true; break; }
                }
                if (!replaced) vars.emplace_back(name, val);
                continue;
            }
        }

        // 规则行：含 ':'（且不是赋值，上面已排除）
        if (t.find(':') != std::string::npos) ruleLines.push_back(t);
    }

    auto get = [&](const wchar_t* name) -> std::wstring {
        for (const auto& kv : vars) {
            if (kv.first == name) return ExpandVars(kv.second, vars, 0);
        }
        return std::wstring();
    };
    // 优先 `<NAME>0`（厂商工程的实际写法），退回 `<NAME>`
    auto get0 = [&](const wchar_t* base) -> std::wstring {
        std::wstring a = get((std::wstring(base) + L"0").c_str());
        if (!a.empty()) return a;
        return get(base);
    };

    v.hasMakefile = true;
    v.plnSource = TrimWide(get(L"PLN_SOURCE"));
    v.plnTarget = TrimWide(get(L"PLN_TARGET"));
    v.plnCFlags = TrimWide(get(L"PLN_CFLAGS"));
    v.patPath   = TrimWide(get(L"PATH_PAT0"));
    if (v.patPath.empty()) v.patPath = TrimWide(get(L"PATH_PAT"));
    v.patCFlags = TrimWide(get0(L"PAT_CFLAGS"));
    v.patLFlags = TrimWide(get0(L"PAT_LFLAGS"));
    v.patTarget = TrimWide(get0(L"PAT_TARGET"));

    for (const std::wstring& tok : TokenizeWide(get0(L"PAT_SOURCE"))) {
        v.patSources.push_back(tok);
    }

    // 调用前缀：找第一条**命令行**里的 plncmp，取 `@` 之后、`plncmp` 之前的部分。
    // 注意扫的是**全部逻辑行**，不是只有规则行 —— 命令是独立的一行（以 `@` 开头），
    // 它本身不含 `:`，用"规则行"筛会把它们全漏掉（这个 bug 被单测当场抓住）。
    for (const std::string& t : logical) {
        const std::wstring w = Widen(t);
        const std::wstring lw = LowerWide(w);
        const std::size_t pos = lw.find(L"plncmp");
        if (pos == std::wstring::npos) continue;
        std::size_t b = 0;
        while (b < pos && (w[b] == L' ' || w[b] == L'\t' || w[b] == L'@')) ++b;
        v.toolPrefix = w.substr(b, pos - b);
        v.usesCraftHome = (v.toolPrefix.find(L"CRAFT_HOME") != std::wstring::npos);
        break;
    }

    // .dec 依赖：规则行里以 .dec 结尾的 token（去重、保序）
    for (const std::string& t : ruleLines) {
        for (const std::wstring& tok : TokenizeWide(Widen(t))) {
            if (!EndsWithNoCase(tok, L".dec")) continue;
            if (std::find(v.decDeps.begin(), v.decDeps.end(), tok) == v.decDeps.end())
                v.decDeps.push_back(tok);
        }
    }

    v.ok = !v.plnSource.empty();
    return v;
}

// ---------------------------------------------------------------------------
// 三、工程模型
// ---------------------------------------------------------------------------

std::wstring StemOf(const std::wstring& path) {
    std::wstring s = path;
    // 去引号（.ful 里是带引号的）
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    const std::size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s = s.substr(slash + 1);
    const std::size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) s = s.substr(0, dot);
    return s;
}

namespace {

std::wstring DirOf(const std::wstring& path) {
    std::wstring s = path;
    const std::size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return s.substr(0, slash);
}

std::wstring BaseOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

} // namespace

std::wstring ResolveRelative(const std::wstring& root, const std::wstring& rel) {
    if (rel.empty()) return std::wstring();
    std::wstring r = rel;
    if (r.size() >= 2 && r[1] == L':') return r;                 // C:\...
    if (r.size() >= 2 && (r[0] == L'\\' || r[0] == L'/') && (r[1] == L'\\' || r[1] == L'/'))
        return r;                                                // UNC
    if (r.size() >= 1 && (r[0] == L'\\' || r[0] == L'/')) return r;  // 根相对
    // 归一化：`.\PAT\x` → `PAT\x`
    if (r.size() >= 2 && r[0] == L'.' && (r[1] == L'\\' || r[1] == L'/')) r = r.substr(2);
    for (wchar_t& c : r) if (c == L'/') c = L'\\';
    if (root.empty()) return r;
    return root + L"\\" + r;
}

Project BuildProject(const std::wstring& anyPath,
                     const std::string& makefileText,
                     bool makefileExists,
                     bool parentHasMakefile,
                     const std::string& parentMakefileText) {
    Project p;
    const std::wstring dir = DirOf(anyPath);
    if (dir.empty()) return p;

    // 工程根：本目录有 makefile 就用本目录；否则（现场常见：源码在 PAT\ 子目录里）
    // 用上一层。两层都没有也照样建（用户可能只是打开了一个孤立的 .pln）。
    std::wstring root = dir;
    std::string mfText = makefileText;
    bool mfExists = makefileExists;
    if (!mfExists && parentHasMakefile) {
        const std::wstring up = DirOf(dir);
        if (!up.empty()) {
            root = up;
            mfText = parentMakefileText;
            mfExists = true;
        }
    }

    p.root = root;
    p.makefilePath = root + L"\\makefile";
    p.vars = mfExists ? ParseMakefile(mfText) : MakeVars{};
    p.vars.hasMakefile = mfExists;

    if (!p.vars.plnSource.empty()) {
        p.plnName = BaseOf(p.vars.plnSource);
    } else if (EndsWithNoCase(anyPath, L".pln")) {
        p.plnName = BaseOf(anyPath);
    }
    p.stem = StemOf(p.plnName);
    if (!p.stem.empty()) p.interDir = root + L"\\." + p.stem;

    if (!p.vars.decDeps.empty()) p.decStem = StemOf(p.vars.decDeps.front());
    if (!p.decStem.empty()) {
        p.fulPath = p.interDir + L"\\" + p.decStem + L".ful";
        p.pxPath  = p.interDir + L"\\" + p.decStem + L".px";
    }
    if (!p.stem.empty()) {
        p.stnLinePath = p.interDir + L"\\" + p.stem + L".stnline";
        p.versionPath = p.interDir + L"\\compilied.ver";
        p.decCfgPath  = p.interDir + L"\\decconfg";
    }

    p.ok = !p.root.empty() && !p.plnName.empty();
    return p;
}

// ---------------------------------------------------------------------------
// 四、中间产物
// ---------------------------------------------------------------------------

std::vector<int> ParseStnLine(const std::string& text) {
    std::vector<int> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
        std::size_t b = i;
        while (i < text.size() && !std::isspace((unsigned char)text[i])) ++i;
        if (i == b) break;
        for (std::size_t k = b; k < i; ++k) {
            if (!std::isdigit((unsigned char)text[k])) return {};   // 认不出 → 整表作废
        }
        const long v = std::strtol(text.substr(b, i - b).c_str(), nullptr, 10);
        if (v <= 0) return {};                                      // 行号从 1 起
        out.push_back((int)v);
    }
    return out;
}

std::wstring ParseCraftVersion(const std::string& text) {
    return TrimWide(Widen(text));
}

bool ParseDecConfig(const std::string& text, int& apas, int& maxSite) {
    bool any = false;
    for (const std::string& line : SplitLinesAscii(text)) {
        const std::vector<std::wstring> tok = TokenizeWide(Widen(line));
        if (tok.size() < 2) continue;
        const std::wstring key = LowerWide(tok[0]);
        const long v = std::wcstol(tok[1].c_str(), nullptr, 10);
        if (key == L"apas") { apas = (int)v; any = true; }
        else if (key == L"maxsite") { maxSite = (int)v; any = true; }
    }
    return any;
}

std::wstring ParseDecUsed(const std::string& text) {
    std::wstring s = TrimWide(Widen(text));
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return TrimWide(s);
}

std::wstring ParsePinListBlock(const std::string& text) {
    if (text.size() < 8) return std::wstring();
    const std::uint32_t count = ReadU32LE(text, 0);
    if (count == 0 || count > 64) return std::wstring();   // 认不出就不猜
    const std::uint32_t len = ReadU32LE(text, 4);
    if (len == 0 || len > 4096) return std::wstring();
    if ((std::size_t)8 + len > text.size()) return std::wstring();
    std::string s = text.substr(8, len);
    // 只收可打印 ASCII，否则说明长度字段理解错了
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) return std::wstring();
    }
    return Widen(s);
}

Artifacts LoadArtifacts(
    const Project& p,
    const std::function<bool(const std::wstring&, std::string&)>& readFile) {
    Artifacts a;
    if (!readFile) return a;
    std::string buf;

    if (!p.versionPath.empty() && readFile(p.versionPath, buf)) {
        a.craftVersion = ParseCraftVersion(buf);
        if (!a.craftVersion.empty()) a.ok = true;
    }
    buf.clear();
    if (!p.decCfgPath.empty() && readFile(p.decCfgPath, buf)) {
        if (ParseDecConfig(buf, a.apas, a.maxSite)) a.ok = true;
    }
    buf.clear();
    if (!p.fulPath.empty() && readFile(p.fulPath, buf)) {
        a.decUsed = ParseDecUsed(buf);
        if (!a.decUsed.empty()) a.ok = true;
    }
    buf.clear();
    if (!p.pxPath.empty() && readFile(p.pxPath, buf)) {
        a.pinListBlock = ParsePinListBlock(buf);
        if (!a.pinListBlock.empty()) a.ok = true;
    }
    buf.clear();
    if (!p.stnLinePath.empty() && readFile(p.stnLinePath, buf)) {
        a.stnLines = ParseStnLine(buf);
        if (!a.stnLines.empty()) a.ok = true;
    }
    return a;
}

int SourceLineOfStatement(const std::vector<int>& stnLines, int stmtIndex1) {
    if (stmtIndex1 <= 0) return 0;
    const std::size_t idx = (std::size_t)(stmtIndex1 - 1);
    if (idx >= stnLines.size()) return 0;
    return stnLines[idx];
}

// ---------------------------------------------------------------------------
// 五、构建计划
// ---------------------------------------------------------------------------

std::vector<BuildStep> PlanBuild(const Project& p) {
    std::vector<BuildStep> steps;
    if (!p.ok) return steps;

    // 1) .pln → .pin
    if (!p.vars.plnSource.empty()) {
        BuildStep s;
        s.exe = L"plncmp";
        s.args = JoinNonEmpty({ p.vars.plnCFlags, p.vars.plnSource });
        s.cwd = p.root;
        s.label = L"编译测试计划 " + p.vars.plnSource;
        steps.push_back(std::move(s));
    }

    // 2) 每个 .pat → .pdt（`-c -s` 是厂商 makefile 的写法）
    for (const std::wstring& pat : p.vars.patSources) {
        BuildStep s;
        s.exe = L"patcmp";
        s.args = JoinNonEmpty({ L"-c", L"-s", p.vars.patCFlags, pat });
        s.cwd = p.root;
        s.label = L"编译向量 " + BaseOf(pat);
        steps.push_back(std::move(s));
    }

    // 3) 链接 → .ppo
    if (!p.vars.patSources.empty()) {
        BuildStep s;
        s.exe = L"patcmp";
        s.args = JoinNonEmpty({ p.vars.patLFlags, L"-f makefile_pdt0.lst" });
        s.cwd = p.root;
        s.label = L"链接向量目标";
        steps.push_back(std::move(s));
    }
    return steps;
}

namespace {

std::string ToAsciiLines(const std::vector<std::wstring>& in, bool toPdt) {
    std::string out;
    for (const std::wstring& s : in) {
        std::wstring line = s;
        if (toPdt) {
            const std::size_t dot = line.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0) line = line.substr(0, dot) + L".pdt";
        }
        // 显式逐字符窄化（这些名字来自 makefile，本来就是 ASCII）。
        // 不走 `out.append(line.begin(), line.end())` 是因为那会触发 C4244。
        for (wchar_t c : line) out.push_back((char)(c & 0xFF));
        out += "\r\n";
    }
    return out;
}

} // namespace

std::string MakePatListFile(const std::vector<std::wstring>& patSources) {
    return ToAsciiLines(patSources, /*toPdt=*/false);
}

std::string MakePdtListFile(const std::vector<std::wstring>& patSources) {
    return ToAsciiLines(patSources, /*toPdt=*/true);
}

// ---------------------------------------------------------------------------
// 六、编译输出解析（保守）
// ---------------------------------------------------------------------------

namespace {

bool LooksLikeSourcePath(const std::wstring& s) {
    if (s.empty()) return false;
    // 保守闸：必须带路径分隔符，或者以已知源码扩展名结尾。
    // 这一条把 `Error: bad thing (2)` 这种"括号里恰好是数字"的误判挡在外面。
    if (s.find(L'\\') != std::wstring::npos) return true;
    if (s.find(L'/') != std::wstring::npos) return true;
    static const wchar_t* kExt[] = { L".pln", L".dec", L".pat", L".c", L".cpp",
                                     L".h",   L".hpp", L".pdt", L".ppo" };
    for (const wchar_t* e : kExt) {
        if (EndsWithNoCase(s, e)) return true;
    }
    return false;
}

// 整串都是十进制数字（空串不算）。ParseDigits 对非数字返回 0，分不出"就是 0"与
// "根本不是数字"，所以判定要单独用一个谓词。
bool AllDigitsW(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;
}

// `file(line)` / `file(line,col)` —— MSVC 的形态。真机失败输出（见 test_craftproj 里
// 逐字节抄下来的样本）给出四种真实写法：
//   C:\Program Files (x86)\...\VC\INCLUDE\xlocale(337) : warning C4530: ...
//   open_short.pln(659) : error C2601: "LOADPIN": 本地函数定义是非法的
//   open_short.pln(663) : fatal error C1075: 与左侧的 大括号"{"(位于"x.pln(623)")匹配…
//           open_short.pln(623):  此行有一个"{"没有匹配项      ← 不带级别的附注行
//
// 三条"必须是"，缺一不认：
//   ① 括号里只能是 `N` 或 `N,M`；
//   ② 括号后（可带空白）**必须是冒号**。这条不是保险，是必需的：C1075 那条的正文里
//      就含 `(位于"x.pln(623)")`，括号里同样是数字 —— 只按①判就会把位置认到正文里
//      去。行尾的 `... Exit Code(2)` 也被这一条挡掉。
//   ③ 括号**前**那一段必须像源码路径。
//
// 前缀从**行首**取，不是"最后一个空白之后"：MSVC 的路径可以含空格，按空白切会把
// `C:\Program Files (x86)\...\xlocale` 腰斩成 `12.0\VC\INCLUDE\xlocale` —— 真机输出
// 里那条 xlocale 警告就是被这样切错的。行首取不到（不像路径）就放弃，不退回按空白
// 切：**认不出（不跳）比认到半截路径上（跳错）好**。
//
// 从左往右取**第一个**同时满足三条的括号：C1075 那条若从右往左找，先撞上的是正文里
// 的 `(623)`，会把位置认到正文里去。
bool TryParen(const std::wstring& line, std::wstring& file, int& ln, int& col) {
    for (std::size_t open = line.find(L'('); open != std::wstring::npos;
         open = line.find(L'(', open + 1)) {
        const std::size_t close = line.find(L')', open + 1);
        if (close == std::wstring::npos) return false;   // 括号不闭合 → 整行不认

        const std::wstring inner = line.substr(open + 1, close - open - 1);
        const std::size_t comma = inner.find(L',');
        if (comma != std::wstring::npos &&
            inner.find(L',', comma + 1) != std::wstring::npos) {
            continue;                                    // 多于一个逗号 → 不是位置
        }
        const std::wstring a = (comma == std::wstring::npos) ? inner
                                                             : inner.substr(0, comma);
        const std::wstring b = (comma == std::wstring::npos) ? std::wstring()
                                                             : inner.substr(comma + 1);
        if (!AllDigitsW(a)) continue;
        if (comma != std::wstring::npos && !AllDigitsW(b)) continue;

        std::size_t k = close + 1;                       // ② 后面必须是冒号
        while (k < line.size() && (line[k] == L' ' || line[k] == L'\t')) ++k;
        if (k >= line.size() || line[k] != L':') continue;

        const std::wstring f = TrimWide(line.substr(0, open));   // ③ 前缀必须像路径
        if (!LooksLikeSourcePath(f)) continue;

        file = f;
        ln = ParseDigits(a);
        col = (comma == std::wstring::npos) ? 0 : ParseDigits(b);
        return true;
    }
    return false;
}

// `file:line` / `file:line:col`
bool TryColon(const std::wstring& line, std::wstring& file, int& ln, int& col) {
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == L':') {
            std::size_t j = i + 1;
            std::size_t d = j;
            while (d < line.size() && line[d] >= L'0' && line[d] <= L'9') ++d;
            if (d > j) {
                const std::wstring f = line.substr(0, i);
                if (LooksLikeSourcePath(f)) {
                    file = f;
                    ln = ParseDigits(line.substr(j, d - j));
                    col = 0;
                    if (d < line.size() && line[d] == L':') {
                        std::size_t e = d + 1, c = e;
                        while (c < line.size() && line[c] >= L'0' && line[c] <= L'9') ++c;
                        if (c > e) col = ParseDigits(line.substr(e, c - e));
                    }
                    return true;
                }
            }
        }
        ++i;
    }
    return false;
}

bool HasWord(const std::wstring& lower, const wchar_t* word) {
    const std::wstring w = LowerWide(word);
    std::size_t p = lower.find(w);
    while (p != std::wstring::npos) {
        const bool lb = (p == 0) || !IsIdentChar(lower[p - 1]);
        const bool rb = (p + w.size() >= lower.size()) || !IsIdentChar(lower[p + w.size()]);
        if (lb && rb) return true;
        p = lower.find(w, p + 1);
    }
    return false;
}

// `word : N` —— 词（整词）+ 可选空白 + ':' + 可选空白 + 数字。
// 比"行里出现了这个词"严得多：`error: warning count mismatch` 这种正文
// 不会命中（error 后面跟着的是字母，不是数字）。
bool HasCountField(const std::wstring& lower, const wchar_t* word) {
    const std::wstring w = LowerWide(word);
    std::size_t p = lower.find(w);
    while (p != std::wstring::npos) {
        const bool lb = (p == 0) || !IsIdentChar(lower[p - 1]);
        const std::size_t q = p + w.size();
        const bool rb = (q >= lower.size()) || !IsIdentChar(lower[q]);
        if (lb && rb) {
            std::size_t r = q;
            while (r < lower.size() && (lower[r] == L' ' || lower[r] == L'\t')) ++r;
            if (r < lower.size() && lower[r] == L':') {
                ++r;
                while (r < lower.size() && (lower[r] == L' ' || lower[r] == L'\t')) ++r;
                if (r < lower.size() && lower[r] >= L'0' && lower[r] <= L'9') return true;
            }
        }
        p = lower.find(w, p + 1);
    }
    return false;
}

// CRAFT 的**统计行**：`          Errors :  0                    Warning : 0`
//
// 这是**一行的汇总**，不是一条诊断 —— 而真机输出（见 test_craftproj 里逐字节
// 抄下来的样本）在编译**完全成功**时也会打这一行。麻烦在于 "Warning" 在这里
// 是个**独立的词**（前后都是空格），按"按词判级"的口径会被判成一条警告
// ⇒ 一个编译成功的工程，面板上会多出一条橙色的假警告。
//
// 判定：同一行里 `errors : N` 与 `warning : M` **两个计数域都在**才算统计行。
// 这样逐条诊断行（`demo.pat(3) : error: ...`，只有 error 一个词）不会被误吞。
//
// 【为什么不去读那两个数字】到批次 97 为止，**仍然没有拿到 N > 0 的统计行**：
//   · 成功编译的统计行是 `Errors : 0  Warning : 0`（多次实测）；
//   · 批次 97 拿到了**第一份失败输出**，但那是 plncmp 在 step 0 就挂了
//     （MSVC 报 C2601/C1075），整条链没走到 patcmp，所以报告里根本没有统计行；
//   · 为逼出 patcmp 失败而做的第二份注入副本（`SPM_PATTERN(func_pat, NORM, K_SET)`，
//     手册 §3.4.1.4 明文说是 compiler error）**反而编译通过了** —— 手册那条断言
//     被实测推翻，详见 Chroma3380Diagnostics.h 规则 4 的取证注释。
// 所以"CRAFT 出错时统计行长什么样、是否另有逐条错误行"依然**没有样本**。既然不知道
// 就不猜：统计行一律原样保留、不判级别、不进计数；"这一步失败了"由退出码决定
// （BuildResult.allOk），那才是可靠信号。等拿到 N > 0 的统计行再补这一块。
bool IsCraftSummaryLine(const std::wstring& lower) {
    return HasCountField(lower, L"errors") && HasCountField(lower, L"warning");
}

} // namespace

// 【这份输出的真实形态，批次 97 实测】
//
// 1) 编码：CRAFT 与它调起来的 cl.exe 都按**系统 ANSI 代码页**输出，中文 Windows 上
//    就是 **GBK**（实测字节：`用于` = D3 C3 D3 DA，不是 UTF-8 的 E7 94 A8…）。
//    Widen 是**逐字节升位**（见其定义处的说明），所以 GBK 字节会变成 0x80..0xFF 区间
//    的单个 wchar，界面上的中文是乱码 —— 这是**已知且刻意**的：内核不碰代码页，
//    显示层的转码是另一个问题（当前批次不做）。
//    对解析本身无影响：判级只认 ASCII 整词，取位置只认 ASCII 的括号/冒号/数字，
//    而 GBK 的**尾字节**（0x40..0xFE）虽可落在 ASCII 区间，却不会落在
//    `(` `)` `:` 与数字上（它们都 < 0x40）—— 实测这份样本里 0 个尾字节落在 ASCII 区间。
//
// 2) 行尾：**同一份输出里两种行尾混用**。CRAFT 自己打的行走 `\r\n`；它中继 cl.exe 的
//    那一段（从 `----[Current Compiler ...]----` 开始）走 `\r\r\n` —— 因为 cl.exe 输出
//    本就带 `\r\n`，再经一次文本模式写出时 `\n` 又被展开成 `\r\n`。
//    SplitLinesAscii 只剥一个 `\r`、TrimWide 再剥掉剩下的空白，所以这里不需要
//    "先归一化行尾"这类动作。测试样本刻意保留了这个混用形态，用来钉住这一点。
CompileOutput ParseCompilerOutput(const std::string& out, const std::wstring& projectRoot) {
    CompileOutput r;
    for (const std::string& rawLine : SplitLinesAscii(out)) {
        const std::wstring trimmed = TrimWide(Widen(rawLine));
        if (trimmed.empty()) continue;   // 空行没有信息量，不算"丢内容"

        CompileIssue it;
        it.text = trimmed;
        const std::wstring lower = LowerWide(trimmed);

        // 统计行必须**先于**判级别处理：它同时含 errors 与 warning 两个词，
        // 按下面那条路走会给成功的编译挂一条假警告（见 IsCraftSummaryLine）。
        if (IsCraftSummaryLine(lower)) {
            r.issues.push_back(std::move(it));   // 原文不丢，级别保持 Plain
            continue;
        }

        if (HasWord(lower, L"error") || HasWord(lower, L"failed") ||
            HasWord(lower, L"illegal")) {
            it.kind = IssueKind::Error;
        } else if (HasWord(lower, L"warning") || HasWord(lower, L"warn")) {
            it.kind = IssueKind::Warning;
        }

        std::wstring file;
        int ln = 0, col = 0;
        if (TryParen(trimmed, file, ln, col) || TryColon(trimmed, file, ln, col)) {
            it.file = file;
            it.line = ln;
            it.column = col;
            ++r.locatedCount;
        }

        if (it.kind == IssueKind::Error) { ++r.errorCount; r.sawAnyError = true; }
        else if (it.kind == IssueKind::Warning) ++r.warnCount;

        r.issues.push_back(std::move(it));
    }
    r.parsed = (r.locatedCount > 0);
    // projectRoot 仍然只用来占位，**不做**路径归一 —— 现在有样本了，样本说不需要：
    // 实测出现的两种形态是"光秃秃的相对文件名"（`open_short.pln`，连目录都没有）与
    // "绝对路径"（`C:\Program Files (x86)\...\xlocale`）。前者由 JumpToCompileLocation
    // 拼到工程根上、后者直接打开，两边都已经处理好了；在这里再归一化一遍反而会把
    // "相对/绝对"这个区别抹掉，让那一层没法按形态分流。
    (void)projectRoot;
    return r;
}

} // namespace craft
} // namespace xfs
