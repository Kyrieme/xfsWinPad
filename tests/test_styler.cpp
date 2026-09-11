// test_styler.cpp - StylerStore + theme JSON unit tests (settings-plan 2a):
// stylers.json round trip, colorStyle resolution rules, global override
// precedence, theme json save/load round trip, corruption tolerance.
#include "../src/theme/Styler.h"
#include "../src/theme/Theme.h"
#include "../src/core/JsonLite.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

static void WriteRaw(const std::wstring& path, const char* data) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (f) { fwrite(data, 1, strlen(data), f); fclose(f); }
}

// themes 目录中的 *.json 数量（注册表大小 = 2 内建 + 目录内文件数）。
// 用户可能有自己的主题文件，不能假设目录为空。
static int JsonCountInDir(const std::wstring& dir) {
    int n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; ++it)
        if (it->path().extension() == L".json") ++n;
    return n;
}

int main() {
    // --- 内建主题表基线 ---------------------------------------------------------
    {
        CHECK(theme::Count() >= 2);
        const ThemeDef* light = theme::Find(L"light");
        const ThemeDef* dark = theme::Find(L"DARK");     // 大小写不敏感
        CHECK(light && dark && light != dark);
        CHECK(light->editorBg == RGB(0xFF, 0xFF, 0xFF));
        CHECK(wcscmp(theme::Find(L"不存在的主题")->name, L"light") == 0);  // 兜底
    }
    // --- 解析链：无覆盖时用主题角色色 -------------------------------------------
    {
        StylerStore s;
        const ThemeDef* light = theme::Find(L"light");
        CHECK(s.ResolveFg("cpp", SR_Comment, *light) == light->comment);
        CHECK(s.ResolveFg("cpp", SR_Keyword, *light) == light->keyword);
        CHECK(s.ResolveBg("cpp", SR_Comment, *light) == light->editorBg);
    }
    // --- colorStyle 语义（对齐 NPP：-1 自有 / 0 全继承 / 1 fg 覆盖 / 2 bg 覆盖）--
    {
        StylerStore s;
        const ThemeDef* light = theme::Find(L"light");

        StyleOverride& o = s.MutableOverride("cpp", SR_Comment);
        o.defined = true; o.colorStyle = 1;
        o.fg = RGB(1, 2, 3);
        CHECK(s.ResolveFg("cpp", SR_Comment, *light) == RGB(1, 2, 3));    // fg 覆盖
        CHECK(s.ResolveBg("cpp", SR_Comment, *light) == light->editorBg); // bg 继承

        o.colorStyle = 0;
        CHECK(s.ResolveFg("cpp", SR_Comment, *light) == light->editorFg); // 全继承
        o.colorStyle = 2;
        o.bg = RGB(9, 9, 9);
        CHECK(s.ResolveFg("cpp", SR_Comment, *light) == light->comment);  // fg 回主题
        CHECK(s.ResolveBg("cpp", SR_Comment, *light) == RGB(9, 9, 9));    // bg 覆盖
    }
    // --- colorStyle=-1（fg+bg 双自定义，配置器勾选两项时写入）-------------------
    {
        StylerStore s;
        const ThemeDef* light = theme::Find(L"light");
        StyleOverride& o = s.MutableOverride("cpp", SR_Comment);
        o.defined = true; o.colorStyle = -1;
        o.fg = RGB(1, 2, 3); o.bg = RGB(9, 9, 9);
        CHECK(s.ResolveFg("cpp", SR_Comment, *light) == RGB(1, 2, 3));    // fg 覆盖
        CHECK(s.ResolveBg("cpp", SR_Comment, *light) == RGB(9, 9, 9));    // bg 覆盖
    }
    // --- Global override 优先于一切 ---------------------------------------------
    {
        StylerStore s;
        const ThemeDef* light = theme::Find(L"light");
        StyleOverride& o = s.MutableOverride("python", SR_String);
        o.defined = true; o.colorStyle = 1; o.fg = RGB(1, 1, 1);
        GlobalOverride& g = s.MutableGlobal();
        g.enableFg = true; g.fg = RGB(0, 0, 0);
        CHECK(s.ResolveFg("python", SR_String, *light) == RGB(0, 0, 0));
        g.enableBg = true; g.bg = RGB(5, 5, 5);
        CHECK(s.ResolveBg("python", SR_String, *light) == RGB(5, 5, 5));
    }
    // --- stylers.json round trip ------------------------------------------------
    {
        StylerStore s;
        StyleOverride& o = s.MutableOverride("cpp", SR_Comment);
        o.defined = true; o.colorStyle = 1;
        o.fg = RGB(0x12, 0x34, 0x56); o.bg = RGB(0xFF, 0xFF, 0xFF);
        o.italic = true;
        s.MutableOverride("python", SR_Number).defined = true;
        std::wstring path = TempPath(L"xfs_stylers_rt.json");
        CHECK(s.Save(path));

        StylerStore r;
        CHECK(r.Load(path));
        const StyleOverride& ro = r.Override("cpp", SR_Comment);
        CHECK(ro.defined && ro.colorStyle == 1);
        CHECK(ro.fg == RGB(0x12, 0x34, 0x56));
        CHECK(ro.bg == RGB(0xFF, 0xFF, 0xFF));
        CHECK(ro.italic && !ro.bold && !ro.underline);
        CHECK(r.Override("python", SR_Number).defined);
        CHECK(!r.Override("cpp", SR_Keyword).defined);
        DeleteFileW(path.c_str());
    }
    // --- 损坏/缺失容错 ------------------------------------------------------------
    {
        StylerStore s;
        CHECK(!s.Load(TempPath(L"xfs_stylers_missing.json")));
        std::wstring bad = TempPath(L"xfs_stylers_bad.json");
        WriteRaw(bad.c_str(), "{{{ nope");
        CHECK(!s.Load(bad));
        DeleteFileW(bad.c_str());
        // 解析失败后覆盖层保持为空
        CHECK(!s.Override("cpp", SR_Comment).defined);
    }
    // --- 主题 JSON save/load round trip（用户主题注册表） --------------------------
    {
        ThemeDef t = *theme::Find(L"light");
        t.name = L"test_custom";
        t.editorBg = RGB(0x11, 0x22, 0x33);
        t.comment = RGB(0x44, 0x55, 0x66);
        // 写到主题目录 → 注册表应能找到它（同名覆盖内建逻辑不适用于新名）
        const std::wstring dir = theme::ThemesDir();
        CreateDirectoryW(dir.c_str(), nullptr);
        // 同名自清理：先删掉历史残留的测试主题，保证本块可重复运行
        DeleteFileW((dir + L"\\test_custom.json").c_str());
        theme::LoadUserThemes();
        CHECK(theme::SaveThemeJson(&t));
        theme::LoadUserThemes();
        const ThemeDef* loaded = theme::Find(L"TEST_CUSTOM");   // 大小写不敏感
        CHECK(loaded != nullptr && loaded != &t);
        if (loaded) {
            CHECK(wcscmp(loaded->name, L"test_custom") == 0);
            CHECK(loaded->editorBg == RGB(0x11, 0x22, 0x33));
            CHECK(loaded->comment == RGB(0x44, 0x55, 0x66));
        }
        // 注册表 = 2 内建 + 目录内全部 *.json（目录里可能有用户自己的主题，
        // 不能断言 "before + 1"）
        CHECK(theme::Count() == 2 + JsonCountInDir(dir));
        DeleteFileW((dir + L"\\test_custom.json").c_str());
        theme::LoadUserThemes();   // 清理注册表中的测试主题
        CHECK(theme::Find(L"test_custom")->editorBg != RGB(0x11, 0x22, 0x33));
    }

    // --- 每语言整行字体 / 用户扩展名 round trip ---------------------------------
    {
        StylerStore s;
        s.SetLanguageFont("cpp", L"Consolas", 14);
        s.SetLanguageFont("python", L"", 0);   // 仅占位（应被清掉）
        s.ClearLanguageFont("python");
        s.SetUserExt("foo", "cpp");
        s.SetUserExt("bar", "python");
        s.SetUserExt("gone", "cpp");
        s.SetUserExt("gone", nullptr);          // 删除映射
        std::wstring path = TempPath(L"xfs_stylers_extra.json");
        CHECK(s.Save(path));

        StylerStore r;
        CHECK(r.Load(path));
        const std::wstring* f = r.LanguageFont("cpp");
        CHECK(f && *f == L"Consolas");
        CHECK(r.LanguageFontSize("cpp") == 14);
        CHECK(r.LanguageFont("python") == nullptr);       // 占位已清
        CHECK(r.LanguageFontSize("python") == 0);
        const char* lex = r.UserLexerForExt("foo");
        CHECK(lex && strcmp(lex, "cpp") == 0);
        CHECK(r.UserLexerForExt("bar") && strcmp(r.UserLexerForExt("bar"), "python") == 0);
        CHECK(r.UserLexerForExt("gone") == nullptr);      // 已删除
        DeleteFileW(path.c_str());
    }
    // --- ResolveFont：语言级字体优先于默认 ---------------------------------------
    {
        StylerStore s;
        s.SetLanguageFont("cpp", L"JetBrains Mono", 13);
        std::wstring f; int sz = 0;
        bool b = true, i = true, u = true;
        s.ResolveFont("cpp", *theme::Find(L"light"), &f, &sz, &b, &i, &u);
        CHECK(f == L"JetBrains Mono");
        CHECK(sz == 13);
        f.clear(); sz = 0;
        s.ResolveFont("python", *theme::Find(L"light"), &f, &sz, &b, &i, &u);
        CHECK(f.empty());                                  // 未设语言级 → 默认字体
        CHECK(sz == 0);
    }

    if (g_fail == 0) { printf("ALL STYLER TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
