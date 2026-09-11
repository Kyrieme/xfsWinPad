// test_settings.cpp - unit tests for flat-JSON settings persistence
#include "../src/settings/Settings.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "shlwapi.lib")

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}

int main() {
    // --- round trip -----------------------------------------------------------
    {
        AppSettings s;
        s.fontName = L"JetBrains Mono";
        s.fontSize = 13;
        s.tabWidth = 2;
        s.wrapOn = true;
        s.caretWidth = 3;
        s.currentLineHighlight = false;
        s.showLineNumber = false;
        s.autoIndent = false;
        s.defaultEol = 1;
        s.autosaveEnabled = false;
        s.autosaveSeconds = 120;
        s.hasWindow = true;
        s.winX = -8; s.winY = 42; s.winW = 1280; s.winH = 900;
        s.winMax = true;
        s.showStatusBar = false;
        s.showTabBar = false;
        s.showToolbar = false;
        s.recentFilesMax = 20;
        s.braceMatch = false;
        s.indentGuides = false;
        s.showWhitespace = true;
        s.searchMatchCase = true;
        s.searchWholeWord = true;
        s.fullPathTitle = true;
        s.autoDetectLang = false;

        std::wstring path = TempPath(L"xfs_settings_rt.json");
        CHECK(SettingsSave(path, s));

        AppSettings r; // defaults
        CHECK(SettingsLoad(path, &r));
        CHECK(r.fontName == L"JetBrains Mono");
        CHECK(r.fontSize == 13);
        CHECK(r.tabWidth == 2);
        CHECK(r.wrapOn == true);
        CHECK(r.caretWidth == 3);
        CHECK(r.currentLineHighlight == false);
        CHECK(r.showLineNumber == false);
        CHECK(r.autoIndent == false);
        CHECK(r.defaultEol == 1);
        CHECK(r.autosaveEnabled == false);
        CHECK(r.autosaveSeconds == 120);
        CHECK(r.hasWindow == true);
        CHECK(r.winX == -8 && r.winY == 42);
        CHECK(r.winW == 1280 && r.winH == 900);
        CHECK(r.winMax == true);
        CHECK(r.showStatusBar == false);
        CHECK(r.showTabBar == false);
        CHECK(r.showToolbar == false);
        CHECK(r.recentFilesMax == 20);
        CHECK(r.braceMatch == false);
        CHECK(r.indentGuides == false);
        CHECK(r.showWhitespace == true);
        CHECK(r.searchMatchCase == true);
        CHECK(r.searchWholeWord == true);
        CHECK(r.fullPathTitle == true);
        CHECK(r.autoDetectLang == false);
        DeleteFileW(path.c_str());
    }
    // --- unknown keys ignored / missing keys keep defaults ----------------------
    {
        std::wstring path = TempPath(L"xfs_settings_unk.json");
        const char* json = "{ \"fontName\": \"Cascadia Code\", \"futureKey\": {\"a\":1}, "
                           "\"n\": 12, \"flag\": null }";
        std::string utf8(json);
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"wb");
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);

        AppSettings r; // defaults: Consolas/10/4
        CHECK(SettingsLoad(path, &r));
        CHECK(r.fontName == L"Cascadia Code");
        CHECK(r.fontSize == 10);
        CHECK(r.tabWidth == 4);
        DeleteFileW(path.c_str());
    }
    // --- corrupt file -> defaults, no crash ---------------------------------------
    {
        std::wstring path = TempPath(L"xfs_settings_bad.json");
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"wb");
        fwrite("{{{ not json !!!", 1, 16, f);
        fclose(f);

        AppSettings r;
        SettingsLoad(path, &r);
        CHECK(r.fontSize == 10);       // default survived
        CHECK(r.fontName == L"Consolas");
        DeleteFileW(path.c_str());
    }
    // --- backslash escaping (paths in strings) ------------------------------------
    {
        AppSettings s;
        s.fontName = L"C:\\Fonts\\My Font.ttf";
        std::wstring path = TempPath(L"xfs_settings_esc.json");
        CHECK(SettingsSave(path, s));
        AppSettings r;
        CHECK(SettingsLoad(path, &r));
        CHECK(r.fontName == s.fontName);
        DeleteFileW(path.c_str());
    }

    if (g_fail == 0) { printf("ALL SETTINGS TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
