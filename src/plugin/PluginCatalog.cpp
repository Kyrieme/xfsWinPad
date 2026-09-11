#include "PluginCatalog.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>

namespace xfs {

// ---------------------------------------------------------------------------
// 极简 JSON 走读器（面向清单这种"已知键、未知键忽略"的结构；意向同 Settings）
// ---------------------------------------------------------------------------
namespace {

struct Cur {
    const wchar_t* p;
    const wchar_t* end;
};

void SkipWs(Cur& c) {
    while (c.p < c.end && (*c.p == L' ' || *c.p == L'\t' || *c.p == L'\r' || *c.p == L'\n'))
        ++c.p;
}

// 读一个带引号字符串，处理常见转义 + \uXXXX。
bool ReadString(Cur& c, std::wstring* out) {
    SkipWs(c);
    if (c.p >= c.end || *c.p != L'"') return false;
    ++c.p;
    std::wstring s;
    while (c.p < c.end) {
        wchar_t ch = *c.p++;
        if (ch == L'"') { *out = std::move(s); return true; }
        if (ch == L'\\' && c.p < c.end) {
            wchar_t e = *c.p++;
            switch (e) {
                case L'"':  s += L'"';  break;
                case L'\\': s += L'\\'; break;
                case L'/':  s += L'/';  break;
                case L'n':  s += L'\n'; break;
                case L't':  s += L'\t'; break;
                case L'r':  s += L'\r'; break;
                case L'b':  s += L'\b'; break;
                case L'f':  s += L'\f'; break;
                case L'u': {
                    if (c.p + 4 <= c.end) {
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            wchar_t h = *c.p++;
                            cp <<= 4;
                            if (h >= L'0' && h <= L'9')       cp |= (h - L'0');
                            else if (h >= L'a' && h <= L'f')  cp |= (h - L'a' + 10);
                            else if (h >= L'A' && h <= L'F')  cp |= (h - L'A' + 10);
                        }
                        s += (wchar_t)cp;   // BMP 足够；补充面字符可后接低替代
                    }
                    break;
                }
                default: s += e; break;
            }
        } else {
            s += ch;
        }
    }
    return false;
}

// 尝试读字符串；若非字符串则回退位置（交由调用方 SkipValue）。
bool ReadOptionalString(Cur& c, std::wstring* out) {
    Cur save = c;
    SkipWs(c);
    if (c.p < c.end && *c.p == L'"') return ReadString(c, out);
    c = save;
    return false;
}

// 跳过任意 JSON 值（字符串/数字/字面量/嵌套对象/数组）。
void SkipValue(Cur& c) {
    SkipWs(c);
    if (c.p >= c.end) return;
    const wchar_t ch = *c.p;
    if (ch == L'"') { std::wstring s; ReadString(c, &s); return; }
    if (ch == L'{' || ch == L'[') {
        const wchar_t open = ch, close = (ch == L'{') ? L'}' : L']';
        int depth = 0; bool inStr = false;
        while (c.p < c.end) {
            wchar_t x = *c.p++;
            if (inStr) {
                if (x == L'\\' && c.p < c.end) ++c.p;
                else if (x == L'"') inStr = false;
                continue;
            }
            if (x == L'"') inStr = true;
            else if (x == open) ++depth;
            else if (x == close) { if (--depth == 0) return; }
        }
        return;
    }
    while (c.p < c.end && *c.p != L',' && *c.p != L'}') ++c.p;
}

// 解析 npp-plugins 数组中的一个对象。
void ParsePluginObject(Cur& c, CatalogPlugin& plt) {
    SkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return;
    ++c.p;                                  // '{'
    while (true) {
        SkipWs(c);
        if (c.p >= c.end) return;
        if (*c.p == L'}') { ++c.p; return; }
        std::wstring key;
        if (!ReadString(c, &key)) { SkipValue(c); continue; }
        SkipWs(c);
        if (c.p < c.end && *c.p == L':') ++c.p;
        std::wstring val;
        const bool hit = ReadOptionalString(c, &val);
        if (!hit) { SkipValue(c); continue; }
        if      (key == L"folder-name")  plt.folderName  = val;
        else if (key == L"display-name") plt.displayName = val;
        else if (key == L"version")      plt.version     = val;
        else if (key == L"repository")   plt.repository  = val;
        else if (key == L"description")  plt.description = val;
        else if (key == L"author")       plt.author      = val;
        else if (key == L"homepage")     plt.homepage    = val;
        SkipWs(c);
        if (c.p < c.end && *c.p == L',') ++c.p;
    }
}

} // namespace

bool ParsePluginList(const std::string& utf8Json, PluginCatalog& catalog) {
    std::wstring json = Utf8ToWide(utf8Json);
    if (json.size() >= 1 && json[0] == 0xFEFF) json.erase(0, 1);  // 容忍 BOM

    PluginCatalog list;
    Cur c{json.data(), json.data() + json.size()};
    SkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return false;
    ++c.p;                                  // '{'

    bool sawArray = false;
    while (true) {
        SkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        std::wstring key;
        if (!ReadString(c, &key)) { SkipValue(c); continue; }
        SkipWs(c);
        if (c.p < c.end && *c.p == L':') ++c.p;

        if (key == L"name")        { ReadOptionalString(c, &list.listName); }
        else if (key == L"version"){ ReadOptionalString(c, &list.listVersion); }
        else if (key == L"npp-plugins") {
            SkipWs(c);
            if (c.p < c.end && *c.p == L'[') {
                ++c.p;                      // '['
                while (true) {
                    SkipWs(c);
                    if (c.p >= c.end) break;
                    if (*c.p == L']') { ++c.p; sawArray = true; break; }
                    if (*c.p == L'{') {
                        CatalogPlugin plt;
                        ParsePluginObject(c, plt);
                        if (!plt.folderName.empty() || !plt.displayName.empty())
                            list.plugins.push_back(std::move(plt));
                    } else {
                        SkipValue(c);
                    }
                    SkipWs(c);
                    if (c.p < c.end && *c.p == L',') ++c.p;
                }
            }
        } else {
            SkipValue(c);                   // arch / 未知顶层键
        }
        SkipWs(c);
        if (c.p < c.end && *c.p == L',') ++c.p;
    }

    catalog = std::move(list);
    return sawArray;
}

std::wstring PluginListFilePath(const std::wstring& pluginsDir) {
    return pluginsDir + L"\\pluginList.json";
}

const std::string& DefaultPluginListJson() {
    // 精选高频 N++ 64 位插件（官方 GitHub release 资产，URL 已逐一验证）。
    // 兼容性备注：NppExec 实测 24 FuncItem 可用；DoxyIt 实测 7 FuncItem 可用；
    // ComparePlus 会在 setInfo 里 exit() 杀死宿主（断路器墓碑拦截）——刻意不收录。
    static const std::string kJson = R"PLUGINS(
{
    "name": "xfsWinPad Plugin List",
    "version": "1.1.0",
    "arch": "64",
    "npp-plugins": [
        {
            "folder-name": "NppExec",
            "display-name": "NppExec - Execute commands",
            "version": "0.8.12.1",
            "repository": "",
            "description": "控制台/脚本引擎：执行命令、运行脚本（离线安装包，实测兼容）。",
            "author": "DV ? dkeg",
            "homepage": "https://github.com/d0vgan/nppexec"
        },
        {
            "folder-name": "DoxyIt",
            "display-name": "DoxyIt - Doxygen comments",
            "version": "0.4.4",
            "repository": "https://github.com/dail8859/DoxyIt/releases/download/v0.4.4/DoxyIt_v0.4.4_x64.zip",
            "description": "一键插入 Doxygen 注释块（实测兼容，7 命令）。",
            "author": "dail8859",
            "homepage": "https://github.com/dail8859/DoxyIt"
        },
        {
            "folder-name": "BetterMultiSelection",
            "display-name": "BetterMultiSelection",
            "version": "1.5",
            "repository": "https://github.com/dail8859/BetterMultiSelection/releases/download/v1.5/BetterMultiSelection_v1.5_x64.zip",
            "description": "增强多选区编辑：Ctrl+D 选中下一匹配、多光标同步编辑。",
            "author": "dail8859",
            "homepage": "https://github.com/dail8859/BetterMultiSelection"
        },
        {
            "folder-name": "DSpellCheck",
            "display-name": "DSpellCheck",
            "version": "1.5.0",
            "repository": "https://github.com/Predelnik/DSpellCheck/releases/download/v1.5.0/DSpellCheck_x64.zip",
            "description": "拼写检查：Hunspell/Aspell 实时校对、错误下划线与建议。",
            "author": "Predelnik",
            "homepage": "https://github.com/Predelnik/DSpellCheck"
        },
        {
            "folder-name": "XMLTools",
            "display-name": "XML Tools",
            "version": "3.1.1.13",
            "repository": "https://github.com/morbac/XMLTools/releases/download/3.1.1.13/XMLTools-3.1.1.13-x64.zip",
            "description": "XML 工具箱：格式化/校验/转义/XPath 求值。",
            "author": "morbac",
            "homepage": "https://github.com/morbac/XMLTools"
        }
    ]
}
)PLUGINS";
    return kJson;
}

bool LoadPluginCatalog(const std::wstring& pluginsDir, PluginCatalog& catalog,
                       bool& usedDefaults) {
    usedDefaults = false;
    const std::wstring path = PluginListFilePath(pluginsDir);
    std::string raw;
    if (ReadFileBytes(path, raw) && ParsePluginList(raw, catalog))
        return true;                        // 磁盘清单优先
    if (ParsePluginList(DefaultPluginListJson(), catalog)) {
        usedDefaults = true;
        return true;
    }
    usedDefaults = true;
    return false;
}

int ComparePluginVersions(const std::wstring& a, const std::wstring& b) {
    if (a == b) return 0;
    size_t ia = 0, ib = 0;
    while (true) {
        size_t ea = a.find(L'.', ia);
        size_t eb = b.find(L'.', ib);
        const std::wstring ta = a.substr(ia, ea == std::wstring::npos ? std::wstring::npos : ea - ia);
        const std::wstring tb = b.substr(ib, eb == std::wstring::npos ? std::wstring::npos : eb - ib);
        auto tot = [](const std::wstring& s) -> long long {
            long long v = 0;
            for (wchar_t ch : s) { if (ch < L'0' || ch > L'9') break; v = v * 10 + (ch - L'0'); }
            return v;
        };
        const long long va = tot(ta), vb = tot(tb);
        if (va != vb) return va < vb ? -1 : 1;
        if (ea == std::wstring::npos || eb == std::wstring::npos) {
            if (ea == eb) return 0;
            return ea == std::wstring::npos ? -1 : 1;  // 较短的先结束视为更旧
        }
        ia = ea + 1; ib = eb + 1;
    }
}

} // namespace xfs