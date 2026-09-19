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

        // 内建主题护栏（批次 90 起升级为**全字段钉值**）。Theme.cpp 已改用
        // C++20 指定初始化（.field = …）：写错顺序/重复字段 = 编译错误，
        // 但**漏写字段仍按 0 静默补齐**——所以把全部 32 个颜色字段逐个钉死，
        // 任何缺漏/改值都会在这里当场失败。历史事故（位置初始化时代）：
        // light 漏了一行 dim，dim 拿到 tabActiveBg 的白色、tab* 七个配色
        // 全体前移、tabEdge 退化成 0。
        // light（32 字段逐一核对）
        CHECK(light->editorBg        == RGB(0xFF, 0xFF, 0xFF));
        CHECK(light->editorFg        == RGB(0x1E, 0x1E, 0x1E));
        CHECK(light->caret           == RGB(0x00, 0x00, 0x00));
        CHECK(light->currentLineBack == RGB(0xF2, 0xF6, 0xFC));
        CHECK(light->selectionBack   == RGB(0xB4, 0xD7, 0xFF));
        CHECK(light->lineNumFg       == RGB(0x6E, 0x76, 0x81));
        CHECK(light->lineNumBack     == RGB(0xF0, 0xF0, 0xF0));
        CHECK(light->foldArrow       == RGB(0x60, 0x60, 0x60));
        CHECK(light->bookmark        == RGB(0x00, 0x66, 0xCC));
        CHECK(light->keyword         == RGB(0x04, 0x51, 0xA5));
        CHECK(light->keyword2        == RGB(0x79, 0x5E, 0x26));
        CHECK(light->comment         == RGB(0x00, 0x80, 0x00));
        CHECK(light->str             == RGB(0xA3, 0x15, 0x15));
        CHECK(light->number          == RGB(0x09, 0x86, 0x58));
        CHECK(light->op              == RGB(0x1E, 0x1E, 0x1E));
        CHECK(light->cls             == RGB(0x26, 0x7F, 0x99));
        CHECK(light->preproc         == RGB(0x81, 0x00, 0x00));
        CHECK(light->special         == RGB(0x81, 0x1F, 0x3F));
        CHECK(light->pass            == RGB(0x0B, 0x6E, 0x3B));
        CHECK(light->fail            == RGB(0xC3, 0x1B, 0x1B));
        CHECK(light->dim             == RGB(0x8C, 0x8C, 0x8C));
        CHECK(light->vector          == RGB(0x6A, 0x1B, 0x9A));
        CHECK(light->expect          == RGB(0xE6, 0x51, 0x00));
        CHECK(light->both            == RGB(0xC2, 0x18, 0x5B));
        CHECK(light->ctrl            == RGB(0x00, 0x6C, 0x7A));
        CHECK(light->tabActiveBg     == RGB(0xFF, 0xFF, 0xFF));
        CHECK(light->tabInactiveBg   == RGB(0xDE, 0xE1, 0xE6));
        CHECK(light->tabActiveText   == RGB(0x1B, 0x1B, 0x1B));
        CHECK(light->tabInactiveText == RGB(0x44, 0x47, 0x4A));
        CHECK(light->tabAccent       == RGB(0x00, 0x78, 0xD4));
        CHECK(light->tabCloseGlyph   == RGB(0x60, 0x60, 0x60));
        CHECK(light->tabEdge         == RGB(0xC8, 0xC8, 0xC8));
        // dark（32 字段逐一核对）
        CHECK(dark->editorBg         == RGB(0x1E, 0x1E, 0x1E));
        CHECK(dark->editorFg         == RGB(0xD4, 0xD4, 0xD4));
        CHECK(dark->caret            == RGB(0xF0, 0xF0, 0xF0));
        CHECK(dark->currentLineBack  == RGB(0x28, 0x28, 0x28));
        CHECK(dark->selectionBack    == RGB(0x26, 0x4F, 0x78));
        CHECK(dark->lineNumFg        == RGB(0x85, 0x85, 0x85));
        CHECK(dark->lineNumBack      == RGB(0x1E, 0x1E, 0x1E));
        CHECK(dark->foldArrow        == RGB(0x90, 0x90, 0x90));
        CHECK(dark->bookmark         == RGB(0x4D, 0xA6, 0xFF));
        CHECK(dark->keyword          == RGB(0x56, 0x9C, 0xD6));
        CHECK(dark->keyword2         == RGB(0xD7, 0xBA, 0x7D));
        CHECK(dark->comment          == RGB(0x6A, 0x99, 0x55));
        CHECK(dark->str              == RGB(0xCE, 0x91, 0x78));
        CHECK(dark->number           == RGB(0xB5, 0xCE, 0xA8));
        CHECK(dark->op               == RGB(0xD4, 0xD4, 0xD4));
        CHECK(dark->cls              == RGB(0x4E, 0xC9, 0xB0));
        CHECK(dark->preproc          == RGB(0xC5, 0x86, 0xC0));
        CHECK(dark->special          == RGB(0xD7, 0xBA, 0x7D));
        CHECK(dark->pass             == RGB(0x6A, 0xC9, 0x7A));
        CHECK(dark->fail             == RGB(0xF2, 0x7A, 0x7A));
        CHECK(dark->dim              == RGB(0x80, 0x80, 0x80));
        CHECK(dark->vector           == RGB(0xC7, 0x92, 0xEA));
        CHECK(dark->expect           == RGB(0xFF, 0xB7, 0x4D));
        CHECK(dark->both             == RGB(0xF0, 0x62, 0x92));
        CHECK(dark->ctrl             == RGB(0x4D, 0xD0, 0xE1));
        CHECK(dark->tabActiveBg      == RGB(0x1E, 0x1E, 0x1E));
        CHECK(dark->tabInactiveBg    == RGB(0x2D, 0x2D, 0x30));
        CHECK(dark->tabActiveText    == RGB(0xD4, 0xD4, 0xD4));
        CHECK(dark->tabInactiveText  == RGB(0x96, 0x96, 0x96));
        CHECK(dark->tabAccent        == RGB(0x00, 0x78, 0xD4));
        CHECK(dark->tabCloseGlyph    == RGB(0xA0, 0xA0, 0xA0));
        CHECK(dark->tabEdge          == RGB(0x3F, 0x3F, 0x46));
        // 字段表（Style Configurator 按索引读写、JSON 按名读写）必须覆盖全部字段，
        // 否则新加的字段存不进主题 JSON。
        CHECK(theme::ThemeFieldCount() == 32);
    }
    // --- ATE 语义角色（批次 72）-------------------------------------------------
    // 锁两件事：
    //  1. 角色名 ↔ 数值稳定（stylers.json 以名称为键，数值变动会让按序索引错位）
    //  2. mask 用的 dim **不能等于 editorFg**。这是本批踩过的坑：mask 原先映射到
    //     ROperator，而 op 在明暗两套主题里都等于正文前景色，于是 X/N/Z/U 和普通
    //     标识符画成一样，「drive/expect/mask 一眼可分」的设计意图落空，而
    //     单测/端到端当时都只断言「三者互不相同」，谁也不报错。
    {
        CHECK(strcmp(StyleRoleName(SR_Pass), "pass") == 0);
        CHECK(strcmp(StyleRoleName(SR_Fail), "fail") == 0);
        CHECK(strcmp(StyleRoleName(SR_Dim), "dim") == 0);
        CHECK(StyleRoleFromName("dim") == SR_Dim);
        CHECK(StyleRoleFromName("pass") == SR_Pass);

        StylerStore s;
        for (const wchar_t* nm : {L"light", L"dark"}) {
            const ThemeDef* t = theme::Find(nm);
            CHECK(t->dim != t->editorFg);      // 弱化色必须真的弱化
            CHECK(t->dim != t->comment);
            CHECK(t->dim != t->number);        // 不能撞 drive（drive 用 number）
            CHECK(t->dim != t->special);       // 不能撞 expect（expect 用 special）
            CHECK(s.ResolveFg("ate_pattern", SR_Dim, *t) == t->dim);
        }
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
