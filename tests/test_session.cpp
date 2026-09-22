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
        SessionEntry e0{L"D:\\a.txt", 3, 1};
        e0.lang = 5;                      // manual Language-menu pick
        in.entries.push_back(e0);
        in.entries.push_back(SessionEntry{L"D:\\b.txt", 5, 7});   // lang stays -1
        SessionEntry e2{L"E:\\c.cpp", 12, 4};
        e2.locked = true;
        e2.lang = 2;
        in.entries1.push_back(e2);
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
        CHECK(out.entries[0].lang == 5);
        CHECK(out.entries[1].lang == -1);
        CHECK(out.entries[1].path == L"D:\\b.txt" && out.entries[1].line == 5 && out.entries[1].col == 7);
        CHECK(out.entries1[0].path == L"E:\\c.cpp" && out.entries1[0].line == 12);
        CHECK(out.entries1[0].lang == 2 && out.entries1[0].locked);
        CHECK(out.entries1[1].path == L"E:\\d.cpp" && out.entries1[1].line == 99);
        CHECK(out.entries1[1].lang == -1);
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
        CHECK(out.entries[0].lang == -1);   // legacy file: no manual pick
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

    // ---- 4. multi-window slots: listing / pid-exclusion / age GC ---------
    {
        std::wstring dir = TempPath(L"xfs_slots_test");
        ::CreateDirectoryW(dir.c_str(), nullptr);
        auto touch = [&](const wchar_t* name, bool old) {
            std::wstring p = dir + L"\\" + name;
            HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0; ::WriteFile(h, "{}", 2, &w, nullptr);
            if (old) {
                FILETIME ft; ::GetSystemTimeAsFileTime(&ft);
                ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime;
                u.HighPart = ft.dwHighDateTime;
                u.QuadPart -= 31ull * 24 * 60 * 60 * 1000 * 1000 * 10;
                ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
                ::SetFileTime(h, nullptr, nullptr, &ft);
            }
            ::CloseHandle(h);
        };
        touch(L"session-12345.json", false);
        touch(L"session-99999.json", false);
        touch(L"session-777.json", true);     // stale → reclaimed
        touch(L"session-abc.json", false);    // malformed → ignored
        touch(L"session.json", false);        // legacy → never a slot

        auto slots = SessionSlots(dir, 99999);
        CHECK(slots.size() == 1);
        CHECK(!slots.empty() && slots[0] == dir + L"\\session-12345.json");
        CHECK(::GetFileAttributesW((dir + L"\\session-777.json").c_str()) ==
              INVALID_FILE_ATTRIBUTES);       // stale slot deleted
        CHECK(::GetFileAttributesW((dir + L"\\session-abc.json").c_str()) !=
              INVALID_FILE_ATTRIBUTES);       // malformed left untouched

        // slot path shape: primary == legacy, secondary carries the pid
        CHECK(SessionSlotPath(true) == SessionFilePath());
        std::wstring sec = SessionSlotPath(false);
        wchar_t needle[64];
        swprintf_s(needle, L"session-%lu.json", (unsigned long)::GetCurrentProcessId());
        CHECK(sec.size() >= 20 && sec.find(needle) != std::wstring::npos);

        for (auto& n : { L"session-12345.json", L"session-99999.json",
                         L"session-abc.json", L"session.json" })
            ::DeleteFileW((dir + L"\\" + n).c_str());
        ::RemoveDirectoryW(dir.c_str());
    }

    // ---- 5. close disposition: closing one window != quitting the app -----
    // 批次 108：非 primary 窗口在"还有别的窗口活着"时不许留下槽位 —— 否则它
    // 下次启动会复活成幽灵窗口；而且恢复出来的子窗口认领槽位后又会在自己退出
    // 时写一个新的槽位，这个幽灵是自我延续的（旧 bug 只能靠 30 天 GC 兜底）。
    {
        std::wstring dir = TempPath(L"xfs_close_policy_test");
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring slot = dir + L"\\session-4242.json";
        const std::wstring legacy = dir + L"\\session.json";
        SessionState s;
        s.entries.push_back(SessionEntry{L"D:\\keep.txt", 1, 1});

        // (a) another window is still alive -> retire: write nothing, and clear a
        //     same-pid leftover so pid recycling cannot resurrect it either.
        {
            HANDLE h = ::CreateFileW(slot.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0; ::WriteFile(h, "{}", 2, &w, nullptr);
            ::CloseHandle(h);
            CHECK(!SessionPersistOnClose(slot, false, s, 1));
            CHECK(::GetFileAttributesW(slot.c_str()) == INVALID_FILE_ATTRIBUTES);
        }

        // (b) last window -> persist: the app is exiting, restore me next launch
        CHECK(SessionPersistOnClose(slot, false, s, 0));
        {
            SessionState back;
            CHECK(SessionLoad(slot, &back));
            CHECK(back.entries.size() == 1);
            CHECK(!back.entries.empty() && back.entries[0].path == L"D:\\keep.txt");
        }

        // (c) the primary always persists: session.json is the canonical session
        CHECK(SessionPersistOnClose(legacy, true, s, 3));
        {
            SessionState back;
            CHECK(SessionLoad(legacy, &back));
            CHECK(back.entries.size() == 1);
        }

        // (d) the predicate itself (the branch both callers read)
        CHECK(SessionCloseDisposition(0) == CloseDisposition::PersistSession);
        CHECK(SessionCloseDisposition(1) == CloseDisposition::RetireSlot);
        CHECK(SessionCloseDisposition(7) == CloseDisposition::RetireSlot);

        ::DeleteFileW(slot.c_str());
        ::DeleteFileW(legacy.c_str());
        ::RemoveDirectoryW(dir.c_str());
    }

    ::DeleteFileW(path.c_str());

    if (g_fail == 0) { printf("ALL SESSION TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}

