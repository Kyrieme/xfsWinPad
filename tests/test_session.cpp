// test_session.cpp - unit test for session persistence: the extended format
// must round-trip both split views (left "entries" + right "entries1" + their
// active indices + which view had focus), and older single-view files (no
// entries1/active1/activeView keys) must still load as one view.
#include "../src/session/Session.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::wstring TempPath(const wchar_t* name) {
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + name;
}

int main() {
    std::wstring path = TempPath(L"xfs_session_test.json");

    // ---- 1. round-trip a two-view session --------------------------------
    {
        SessionState in;
        in.entries.push_back(SessionEntry{L"D:\\a.txt", 3, 1});
        in.entries.push_back(SessionEntry{L"D:\\b.txt", 5, 7});
        in.entries1.push_back(SessionEntry{L"E:\\c.cpp", 12, 4});
        in.entries1.push_back(SessionEntry{L"E:\\d.cpp", 99, 2});
        in.activeIndex = 1;
        in.activeIndex1 = 0;
        in.activeView = 1;
        CHECK(SessionSave(path, in));

        SessionState out;
        CHECK(SessionLoad(path, &out));
        CHECK(out.entries.size() == 2);
        CHECK(out.entries1.size() == 2);
        CHECK(out.entries[0].path == L"D:\\a.txt" && out.entries[0].line == 3);
        CHECK(out.entries[1].path == L"D:\\b.txt" && out.entries[1].line == 5 && out.entries[1].col == 7);
        CHECK(out.entries1[0].path == L"E:\\c.cpp" && out.entries1[0].line == 12);
        CHECK(out.entries1[1].path == L"E:\\d.cpp" && out.entries1[1].line == 99);
        CHECK(out.activeIndex == 1);
        CHECK(out.activeIndex1 == 0);
        CHECK(out.activeView == 1);
    }

    // ---- 2. backward-compatible single-view file (old format) ------------
    {
        std::wstring old = TempPath(L"xfs_session_old.json");
        std::string j =
            "{\r\n  \"active\": 1,\r\n  \"entries\": [\r\n"
            "    { \"path\": \"C:\\\\x.txt\", \"line\": 9, \"col\": 1 },\r\n"
            "    { \"path\": \"C:\\\\y.txt\", \"line\": 2, \"col\": 3 }\r\n"
            "  ]\r\n}\r\n";
        HANDLE h = ::CreateFileW(old.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD w = 0; ::WriteFile(h, j.data(), (DWORD)j.size(), &w, nullptr);
        ::CloseHandle(h);

        SessionState out;
        CHECK(SessionLoad(old, &out));
        CHECK(out.entries.size() == 2);
        CHECK(out.entries1.empty());
        CHECK(out.activeIndex == 1);
        CHECK(out.activeIndex1 == 0);   // default
        CHECK(out.activeView == 0);     // default = left view (legacy)
        ::DeleteFileW(old.c_str());
    }

    // ---- 3. empty / missing file returns false ---------------------------
    {
        SessionState out;
        CHECK(!SessionLoad(TempPath(L"xfs_session_nope.json"), &out));
    }

    ::DeleteFileW(path.c_str());

    if (g_fail == 0) { printf("ALL SESSION TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}

