#include "Theme.h"
#include "../core/JsonLite.h"
#include "../core/Util.h"
#include <cwctype>
#include <deque>
#include <filesystem>
#include <shlobj.h>

namespace xfs {
namespace {

// 两套内建主题使用 **C++20 指定初始化**（批次 90）：每个字段显式点名（.editorBg = …）。
//   在 Theme.h 里插入/删除字段时，两处初始化列表不再整体错位——漏写的字段按 0
//   补齐（test_styler 的钉值断言当场抓 0），写错顺序/重复字段则是**编译错误**。
//   此前的位置初始化真实踩过：给 light 漏了一行 dim，于是 dim 拿到 tabActiveBg
//   的白色、tab* 七个配色全体前移一格、tabEdge 退化成 0（批次 72）。

// Light theme (VS-light inspired, original values)
constexpr ThemeDef g_light = {
    .name            = L"light",
    .editorBg        = RGB(0xFF, 0xFF, 0xFF),
    .editorFg        = RGB(0x1E, 0x1E, 0x1E),
    .caret           = RGB(0x00, 0x00, 0x00),
    .currentLineBack = RGB(0xF2, 0xF6, 0xFC),
    .selectionBack   = RGB(0xB4, 0xD7, 0xFF),
    .lineNumFg       = RGB(0x6E, 0x76, 0x81),
    .lineNumBack     = RGB(0xF0, 0xF0, 0xF0),
    .foldArrow       = RGB(0x60, 0x60, 0x60),
    .bookmark        = RGB(0x00, 0x66, 0xCC),
    .keyword         = RGB(0x04, 0x51, 0xA5),
    .keyword2        = RGB(0x79, 0x5E, 0x26),
    .comment         = RGB(0x00, 0x80, 0x00),
    .str             = RGB(0xA3, 0x15, 0x15),
    .number          = RGB(0x09, 0x86, 0x58),
    .op              = RGB(0x1E, 0x1E, 0x1E),
    .cls             = RGB(0x26, 0x7F, 0x99),
    .preproc         = RGB(0x81, 0x00, 0x00),
    .special         = RGB(0x81, 0x1F, 0x3F),
    .pass            = RGB(0x0B, 0x6E, 0x3B),
    .fail            = RGB(0xC3, 0x1B, 0x1B),
    .dim             = RGB(0x8C, 0x8C, 0x8C),
    .vector          = RGB(0x6A, 0x1B, 0x9A),   // 紫：驱动 0/1
    .expect          = RGB(0xE6, 0x51, 0x00),   // 橙：只比较 H/L/Z
    .both            = RGB(0xC2, 0x18, 0x5B),   // 洋红：驱动+比较 R/S/T/U
    .ctrl            = RGB(0x00, 0x6C, 0x7A),   // 深青：V/K/2
    .tabActiveBg     = RGB(0xFF, 0xFF, 0xFF),
    .tabInactiveBg   = RGB(0xDE, 0xE1, 0xE6),
    .tabActiveText   = RGB(0x1B, 0x1B, 0x1B),
    .tabInactiveText = RGB(0x44, 0x47, 0x4A),
    .tabAccent       = RGB(0x00, 0x78, 0xD4),
    .tabCloseGlyph   = RGB(0x60, 0x60, 0x60),
    .tabEdge         = RGB(0xC8, 0xC8, 0xC8),
};

// Dark theme (VS-dark inspired, original values)
constexpr ThemeDef g_dark = {
    .name            = L"dark",
    .editorBg        = RGB(0x1E, 0x1E, 0x1E),
    .editorFg        = RGB(0xD4, 0xD4, 0xD4),
    .caret           = RGB(0xF0, 0xF0, 0xF0),
    .currentLineBack = RGB(0x28, 0x28, 0x28),
    .selectionBack   = RGB(0x26, 0x4F, 0x78),
    .lineNumFg       = RGB(0x85, 0x85, 0x85),
    .lineNumBack     = RGB(0x1E, 0x1E, 0x1E),
    .foldArrow       = RGB(0x90, 0x90, 0x90),
    .bookmark        = RGB(0x4D, 0xA6, 0xFF),
    .keyword         = RGB(0x56, 0x9C, 0xD6),
    .keyword2        = RGB(0xD7, 0xBA, 0x7D),
    .comment         = RGB(0x6A, 0x99, 0x55),
    .str             = RGB(0xCE, 0x91, 0x78),
    .number          = RGB(0xB5, 0xCE, 0xA8),
    .op              = RGB(0xD4, 0xD4, 0xD4),
    .cls             = RGB(0x4E, 0xC9, 0xB0),
    .preproc         = RGB(0xC5, 0x86, 0xC0),
    .special         = RGB(0xD7, 0xBA, 0x7D),
    .pass            = RGB(0x6A, 0xC9, 0x7A),
    .fail            = RGB(0xF2, 0x7A, 0x7A),
    .dim             = RGB(0x80, 0x80, 0x80),
    .vector          = RGB(0xC7, 0x92, 0xEA),   // 浅紫：驱动 0/1
    .expect          = RGB(0xFF, 0xB7, 0x4D),   // 浅橙：只比较 H/L/Z
    .both            = RGB(0xF0, 0x62, 0x92),   // 粉：驱动+比较 R/S/T/U
    .ctrl            = RGB(0x4D, 0xD0, 0xE1),   // 亮青：V/K/2
    .tabActiveBg     = RGB(0x1E, 0x1E, 0x1E),
    .tabInactiveBg   = RGB(0x2D, 0x2D, 0x30),
    .tabActiveText   = RGB(0xD4, 0xD4, 0xD4),
    .tabInactiveText = RGB(0x96, 0x96, 0x96),
    .tabAccent       = RGB(0x00, 0x78, 0xD4),
    .tabCloseGlyph   = RGB(0xA0, 0xA0, 0xA0),
    .tabEdge         = RGB(0x3F, 0x3F, 0x46),
};

const ThemeDef* const g_all[] = { &g_light, &g_dark };

} // namespace

namespace theme {

const ThemeDef* Find(const wchar_t* name) {
    if (name && *name) {
        for (int i = 0; i < Count(); ++i) {
            const ThemeDef* t = At(i);
            const wchar_t* a = t->name;
            const wchar_t* b = name;
            while (*a && *b && towlower(*a) == towlower(*b)) { ++a; ++b; }
            if (*a == L'\0' && *b == L'\0') return t;
        }
    }
    return &g_light;
}

// ---- 用户主题（themes\*.json，阶段 2a）---------------------------------------
//
// 注册表 = 内建 light/dark（兜底，永远在）+ 用户 JSON 主题。
// 存储用 deque：向尾部追加不失效已有元素的引用/指针——MainWindow 等处持有
// 的 const ThemeDef* 在注册表扩展后仍然有效。

namespace {

struct ThemeField { const wchar_t* key; COLORREF ThemeDef::*ptr; };
const ThemeField kThemeFields[] = {
    {L"editorBg",        &ThemeDef::editorBg},
    {L"editorFg",        &ThemeDef::editorFg},
    {L"caret",           &ThemeDef::caret},
    {L"currentLineBack", &ThemeDef::currentLineBack},
    {L"selectionBack",   &ThemeDef::selectionBack},
    {L"lineNumFg",       &ThemeDef::lineNumFg},
    {L"lineNumBack",     &ThemeDef::lineNumBack},
    {L"foldArrow",       &ThemeDef::foldArrow},
    {L"bookmark",        &ThemeDef::bookmark},
    {L"keyword",         &ThemeDef::keyword},
    {L"keyword2",        &ThemeDef::keyword2},
    {L"comment",         &ThemeDef::comment},
    {L"str",             &ThemeDef::str},
    {L"number",          &ThemeDef::number},
    {L"op",              &ThemeDef::op},
    {L"cls",             &ThemeDef::cls},
    {L"preproc",         &ThemeDef::preproc},
    {L"special",         &ThemeDef::special},
    {L"pass",            &ThemeDef::pass},
    {L"fail",            &ThemeDef::fail},
    {L"dim",             &ThemeDef::dim},
    {L"vector",          &ThemeDef::vector},
    {L"expect",          &ThemeDef::expect},
    {L"both",            &ThemeDef::both},
    {L"ctrl",            &ThemeDef::ctrl},
    {L"tabActiveBg",     &ThemeDef::tabActiveBg},
    {L"tabInactiveBg",   &ThemeDef::tabInactiveBg},
    {L"tabActiveText",   &ThemeDef::tabActiveText},
    {L"tabInactiveText", &ThemeDef::tabInactiveText},
    {L"tabAccent",       &ThemeDef::tabAccent},
    {L"tabCloseGlyph",   &ThemeDef::tabCloseGlyph},
    {L"tabEdge",         &ThemeDef::tabEdge},
};

std::deque<std::pair<std::wstring, ThemeDef>> g_user;   // name ↔ def（指针稳定）

} // namespace

int Count() { return 2 + (int)g_user.size(); }

const ThemeDef* At(int index) {
    if (index < 0) return &g_light;
    if (index < 2) return g_all[index];
    int u = index - 2;
    if (u >= (int)g_user.size()) return &g_light;
    return &g_user[(size_t)u].second;
}

std::wstring ThemesDir() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    return base + L"\\xfsWinPad\\themes";
}

int LoadUserThemes() {
    g_user.clear();
    const std::wstring dir = ThemesDir();
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return 0;
    int loaded = 0;
    for (auto& ent : std::filesystem::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file(ec)) continue;
        if (ent.path().extension().wstring() != L".json") continue;
        std::string raw;
        if (!ReadFileBytes(ent.path().wstring(), raw)) continue;
        std::wstring j = Utf8ToWide(raw);
        if (!j.empty() && j[0] == 0xFEFF) j.erase(0, 1);
        json::Value root;
        if (!json::Parse(j, &root)) continue;
        std::wstring name = ent.path().stem().wstring();
        if (const json::Value* n = json::Find(root, L"name"))
            if (n->kind == json::Value::Str && !n->str.empty()) name = n->str;
        ThemeDef def = g_light;                     // 缺省字段用 light 兜底
        def.name = L"";                             // 由注册表在插入后回填
        for (const ThemeField& f : kThemeFields)
            if (const json::Value* v = json::Find(root, f.key))
                if (v->kind == json::Value::Str)
                    def.*f.ptr = json::ParseColor(v->str, def.*f.ptr);
        // 同名用户主题覆盖内建：先移除旧条目
        for (size_t i = 0; i < g_user.size(); ++i)
            if (_wcsicmp(g_user[i].first.c_str(), name.c_str()) == 0) { g_user.erase(g_user.begin() + i); break; }
        g_user.emplace_back(name, def);
        g_user.back().second.name = g_user.back().first.c_str();
        ++loaded;
    }
    return loaded;
}

bool SaveThemeJson(const ThemeDef* t) {
    if (!t || !t->name || !t->name[0]) return false;
    return SaveThemeJsonTo(t,
        ThemesDir() + L"\\" + t->name + L".json");
}

bool SaveThemeJsonTo(const ThemeDef* t, const std::wstring& path) {
    if (!t || !t->name || !t->name[0] || path.empty()) return false;
    std::wstring j = L"{\r\n";
    j += std::wstring(L"  \"name\": \"") + t->name + L"\",\r\n";
    bool first = true;
    for (const ThemeField& f : kThemeFields) {
        if (!first) j += L",\r\n";
        first = false;
        j += std::wstring(L"  \"") + f.key + L"\": \"" + json::ColorToWstr(t->*f.ptr) + L"\"";
    }
    j += L"\r\n}\r\n";
    std::string utf8 = WideToUtf8(j);
    std::error_code dirc;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), dirc);
    return WriteFileBytes(path, utf8.data(), utf8.size());
}

// ---- 阶段 4：主题基色编辑 ------------------------------------------------------

ThemeDef* MutableUserTheme(const wchar_t* name) {
    if (!name || !name[0]) return nullptr;
    for (auto& kv : g_user)
        if (_wcsicmp(kv.first.c_str(), name) == 0) return &kv.second;
    return nullptr;
}

const ThemeDef* CreateUserThemeCopy(const wchar_t* src, const wchar_t* newName) {
    if (!newName || !newName[0]) return nullptr;
    const ThemeDef* from = Find(src);
    if (!from) return nullptr;
    ThemeDef def = *from;                        // 复制后再动注册表
    // 同名覆盖（与 LoadUserThemes 的语义一致）
    for (size_t i = 0; i < g_user.size(); ++i)
        if (_wcsicmp(g_user[i].first.c_str(), newName) == 0) {
            g_user.erase(g_user.begin() + i);
            break;
        }
    g_user.emplace_back(newName, def);
    g_user.back().second.name = g_user.back().first.c_str();
    SaveThemeJson(&g_user.back().second);        // 落盘失败也保留注册表条目
    return &g_user.back().second;
}

int ThemeFieldCount() {
    return (int)(sizeof(kThemeFields) / sizeof(kThemeFields[0]));
}

const wchar_t* ThemeFieldKey(int i) {
    if (i < 0 || i >= ThemeFieldCount()) return L"";
    return kThemeFields[i].key;
}

COLORREF ThemeGetColor(const ThemeDef* t, int i) {
    if (!t || i < 0 || i >= ThemeFieldCount()) return RGB(0xFF, 0xFF, 0xFF);
    return t->*kThemeFields[i].ptr;
}

void ThemeSetColor(ThemeDef* t, int i, COLORREF c) {
    if (!t || i < 0 || i >= ThemeFieldCount()) return;
    t->*kThemeFields[i].ptr = c;
}

} // namespace theme
} // namespace xfs
