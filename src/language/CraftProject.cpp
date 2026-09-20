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

// `<< File:[<路径>] Line:[<N>] Last_Token:[<记号>] >>` —— CRAFT **声明文件编译器**
// （吃 .dec 的那个子编译器）的诊断记录头。批次 99 的真机样本：
//
//   Make Declaration File ...Compile Failed !! Exit Code(59)
//   Delaration file compiler for CRAFT_3380_2.50 Copyright (c) 2011 CHROMA
//
//   << File:[.\PAT\ls299_pin.dec] Line:[24] Last_Token:[;] >>
//       Message:[Error : Duplicate Declare Pin "QA"]
//
// 这是**第三种**位置形态，和前面两种都不一样：路径与行号各自被 `[]` 包住，
// 行号在 `Line:` 之后，行首是 `<<`。既不满足 `TryParen`（没有括号），
// 也不满足 `TryColon`（`:` 后面跟的是 `[` 不是数字）—— 所以**不认它，
// `.dec` 报错就没有任何跳转目标**，用户只能看着报错手动去找行。
//
// 形状很死（前两个标签名固定、值被 `[]` 包住、数字必须是纯数字），所以直接按
// 形状匹配，不做"从右往左找数字"那种猜测。**认不出不跳，好过跳错**（硬约束 ③）。
bool TryCraftDeclRecord(const std::wstring& line, std::wstring& file, int& ln) {
    const std::size_t head = line.find(L"<<");
    if (head == std::wstring::npos) return false;

    const std::size_t fk = line.find(L"File:[", head);
    if (fk == std::wstring::npos) return false;
    const std::size_t fv = fk + 6;                       // 跳过 `File:[`
    const std::size_t fe = line.find(L']', fv);
    if (fe == std::wstring::npos) return false;

    const std::size_t lk = line.find(L"Line:[", fe);     // 必须在 File 之后
    if (lk == std::wstring::npos) return false;
    const std::size_t lv = lk + 6;                       // 跳过 `Line:[`
    const std::size_t le = line.find(L']', lv);
    if (le == std::wstring::npos) return false;

    const std::wstring f = TrimWide(line.substr(fv, fe - fv));
    const std::wstring n = line.substr(lv, le - lv);
    if (!AllDigitsW(n)) return false;
    if (!LooksLikeSourcePath(f)) return false;

    file = f;
    ln = ParseDigits(n);
    return true;
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
// 【为什么不去读那两个数字】批次 99 拿到了 N > 0 的统计行，样本长这样：
//   `          Errors :  4                    Warning : 0`
// 结论仍然是**不读**，但理由从"没有样本所以不猜"变成了三条实测依据：
//
//   ① 统计行**不是每次都打**。同一批六份失败副本里，`badpat_header`（HEADER 里
//      写了个 PIN LIST 里没有的 pin）只有一行
//      `…(3): error C1000: pin:[ZZZ] …`，后面**直接没有统计行**就结束了 ——
//      那是一条"致命错误、立刻中止"的路径，压根走不到汇总那一步。
//      所以"统计行存在"不能当失败信号，反过来"统计行不存在"也不能当成功信号。
//      唯一可靠的信号还是退出码（BuildResult.allOk）。
//   ② 逐条行自己数出来的个数**与统计行完全一致**：`badpat_multi` 一个 step 里
//      4 条 `error C1000`，统计行就写 `Errors : 4`；`badpat_brace` / `badpat_label`
//      各 1 条，统计行就写 `Errors : 1`。既然逐条数得出的数与厂商自报的数相等，
//      再去解析统计行就是**同一个数的第二个来源**，多一份解析多一份出错的机会。
//   ③ 统计行里那两个数**分不出是哪一步的**：`patcmp -c` 编译一步、`-o` 链接一步，
//      各自打各自的统计行，而行文里没有 step 号。按行累加会重复计数。
//
// 所以统计行一律原样保留、不判级别、不进计数（r.issues 里仍有它，只是 kind=Plain）。
// 这一条从"没样本所以不猜"升级成了"有样本，样本说不该读"。
//
// 【批次 99 顺带拿到的另外两条】
//   · patcmp **不在第一个错误上停**：`badpat_multi` 一份文件里报了 4 条，行号
//     17/17/22/35 全都列了出来，是一次跑完的。
//   · 一条诊断行里 `pin:[…]` / `symbol:[…]` / `label:[…]` 这个字段**可以没有**：
//     `'SPM_PATTERN' unmatch` 那条就只有 `Message:[…]`。所以判级只看 error 整词，
//     不要去要求这个字段存在。
bool IsCraftSummaryLine(const std::wstring& lower) {
    return HasCountField(lower, L"errors") && HasCountField(lower, L"warning");
}

// 链接阶段报出来的位置指向的是 **.pdt**，不是用户写的 .pat（批次 99 真机样本）：
//   `Z:\…\badpat_label\PAT\ls299_func.pdt(49): error C1000: label:[no_such_label] …`
// 而 .pdt 正是本文件上半部分 `MakePdtListFile` 把 .pat **换扩展名**生成出来的，
// 所以这里就是把项目自己做过的那次改名**反过来**。不还原的话，跳转要么落在二进制
// 中间产物上（编辑器里一屏乱码），要么因为文件不存在而整个跳不成 —— 两种都没用。
//
// **只改扩展名，不动行号**：样本里源文件第 49 行、报的就是 `.pdt(49)`，编号是保留的。
// 退一步说，就算某个版本不再保留行号，这样跳到的仍然是**正确的文件**、只是行可能偏，
// 比跳到二进制上、或者干脆不跳，都强。
void MapPdtToPat(std::wstring& file) {
    if (!EndsWithNoCase(file, L".pdt")) return;
    file.replace(file.size() - 4, 4, L".pat");
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
// 2) 行尾：**同一份输出里两种行尾混用**。CRAFT 自己打的行走 `\r\n`；凡是被它**中继**
//    的子进程输出都走 `\r\r\n` —— 因为子进程输出本就带 `\r\n`，再经一次文本模式写出时
//    `\n` 又被展开成 `\r\n`。触发条件是"中继"这个动作，**跟是哪个子进程无关**：
//    批次 97 的样本里是 cl.exe（从 `----[Current Compiler ...]----` 起），批次 99 的
//    `kPlncmpDecDupName` 里是**声明文件编译器**，而那份样本里根本没有那行分节线。
//    SplitLinesAscii 只剥一个 `\r`、TrimWide 再剥掉剩下的空白，所以这里不需要
//    "先归一化行尾"这类动作。测试样本刻意保留了这个混用形态，用来钉住这一点。
CompileOutput ParseCompilerOutput(const std::string& out, const std::wstring& projectRoot) {
    CompileOutput r;

    // 声明文件编译器的诊断是**两行一条**（批次 99 真机样本）：
    //     << File:[.\PAT\ls299_pin.dec] Line:[24] Last_Token:[;] >>   ← 有位置，没级别
    //         Message:[Error : Duplicate Declare Pin "QA"]            ← 有级别，没位置
    // 用户会去点带 Error 的那一行，所以位置得挂到它身上：记录头先把位置存起来，
    // 交给**紧接着的那一行**。只对紧接着的一行有效 —— 用没用掉都清空，
    // 免得一个孤立的记录头把位置漂到很远的地方去。
    std::wstring pendFile;
    int pendLine = 0;

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
            pendFile.clear();
            pendLine = 0;
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
        const bool direct = TryParen(trimmed, file, ln, col) ||
                            TryColon(trimmed, file, ln, col);
        const bool header = !direct && TryCraftDeclRecord(trimmed, file, ln);

        if (direct) {
            MapPdtToPat(file);
            it.file = file;
            it.line = ln;
            it.column = col;
            ++r.locatedCount;
        } else if (!header && !pendFile.empty()) {
            it.file = pendFile;          // 接住上一条记录头的位置
            it.line = pendLine;
            ++r.locatedCount;
        }

        // 记录头的位置只对**紧接着的一行**有效：本行不管用没用掉都重设一次。
        // 孤立的记录头（后面跟空行、或紧跟另一条记录头）不会把位置漂出去。
        if (header) {
            pendFile = file;
            pendLine = ln;
        } else {
            pendFile.clear();
            pendLine = 0;
        }

        if (it.kind == IssueKind::Error) { ++r.errorCount; r.sawAnyError = true; }
        else if (it.kind == IssueKind::Warning) ++r.warnCount;

        r.issues.push_back(std::move(it));
    }
    r.parsed = (r.locatedCount > 0);
    // projectRoot 仍然只用来占位，**不做**路径归一 —— 现在有样本了，样本说不需要：
    // 实测出现的形态是"光秃秃的相对文件名"（`open_short.pln`，连目录都没有）、
    // "相对路径"（`.\PAT\ls299_func.pat`）、"绝对路径"
    // （`C:\Program Files (x86)\...\xlocale`、`Z:\...\PAT\ls299_func.pdt`）。
    // 前两种由 JumpToCompileLocation 拼到工程根上、绝对路径直接打开，那一层都已经
    // 处理好了；在这里再归一化一遍反而会把"相对/绝对"这个区别抹掉，让它没法按形态分流。
    // （批次 99 补测了"相对路径"这一种，`.\PAT\…` 就是它 —— 之前的样本里没有。）
    (void)projectRoot;
    return r;
}

} // namespace craft
} // namespace xfs
