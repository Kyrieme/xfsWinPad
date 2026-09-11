// test_plugin.cpp - unit test for PluginManager: load a real plugin DLL, verify
// its command is published, dispatch it, and unload cleanly. The v2 editor
// callbacks run with NO workspace attached here, so executing the command also
// asserts their graceful "nothing open" degradation (no crash, expected values).
#include "../src/plugin/PluginManager.h"
#include "../src/plugin/xfs_plugin_api.h"
#include "../src/core/Log.h"
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

// path to the test plugin dll directory (set via argv[1])
int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: test_plugin <dir-containing-dll>\n");
        printf("PLUGIN TEST SKIPPED (no dir)\n");
        return 0;   // not an infra failure
    }
    std::wstring dir = Utf8ToWide(argv[1]);

    PluginManager mgr;
    // deliberately NO SetWorkspace()/SetHostWindow(): every v2 callback must
    // degrade safely, and NPP-compat setInfo receives a zeroed handle triple.
    // The directory contains TWO kinds of plugins (mixed-load regression):
    //   test_plugin_core.dll  — native xfsPlugin ABI, 1 command
    //   test_npp_core.dll     — NPP-style six exports, 2 FuncItems
    int loaded = mgr.LoadAllFrom(dir);
    CHECK(loaded == 2);

    // unified command table across both channels
    CHECK(mgr.CommandCount() == 3);

    unsigned int pluginId = 0;
    unsigned int nppOne = 0, nppTwo = 0;
    for (const auto& c : mgr.Commands()) {
        CHECK(c.id >= PluginCmdFirst && c.id <= PluginCmdMax);
        if (!c.grouped) {
            // native channel stays flat
            CHECK(c.category == L"测试");
            CHECK(c.label == L"Test Command");
            pluginId = c.id;
        } else {
            // NPP channel: grouped under the plugin-name submenu
            CHECK(c.category == L"npp-test");
            CHECK(c.hasKey && c.fVirt == (FVIRTKEY | FCONTROL | FSHIFT));
            if (c.label == L"NPP Action One") {
                nppOne = c.id;
                CHECK(c.vk == '7');
            } else if (c.label == L"NPP Action Two") {
                nppTwo = c.id;
                CHECK(c.vk == '8');
            }
        }
    }
    CHECK(pluginId != 0);
    CHECK(nppOne != 0 && nppTwo != 0 && nppOne != nppTwo);

    HMODULE nppDll = ::GetModuleHandleW((dir + L"\\test_npp_core.dll").c_str());
    CHECK(nppDll != nullptr);
    auto firesFn = nppDll ? (int (*)(void))::GetProcAddress(nppDll, "test_npp_fires")
                          : nullptr;
    auto seenFn = nppDll ? (int (*)(void))::GetProcAddress(nppDll, "test_npp_setInfoSeen")
                         : nullptr;
    auto cid0Fn = nppDll ? (int (*)(void))::GetProcAddress(nppDll, "test_npp_cmdId0")
                         : nullptr;
    auto cid1Fn = nppDll ? (int (*)(void))::GetProcAddress(nppDll, "test_npp_cmdId1")
                         : nullptr;
    CHECK(firesFn && seenFn && cid0Fn && cid1Fn);

    // ---- NPP channel: FuncItem dispatch through our command table ----------
    if (seenFn) CHECK(seenFn() == 1);           // setInfo called exactly once
    if (firesFn && nppOne && nppTwo) {
        CHECK(mgr.Execute(nppOne) == true);      // fa: +1
        CHECK(mgr.Execute(nppTwo) == true);      // fb: +10 (slot wiring exact)
        CHECK(firesFn() == 11);
    }
    // contract write-back: the ints inside OUR-copied FuncItems are exactly
    // the host ids of the matching published commands
    if (cid0Fn && cid1Fn && nppOne && nppTwo) {
        CHECK(cid0Fn() == (int)nppOne);
        CHECK(cid1Fn() == (int)nppTwo);
    }

    // ---- 4c: NPPN_* 通知桥（beNotified）----------------------------------
    // 无宿主窗口（本测试故意不 SetHostWindow）→ hwndFrom 应为空；
    // BufferID=Document* 空间：宿主把不透明 idFrom 透传给插件。
    auto notifCountFn = nppDll ? (int (*)(void))::GetProcAddress(
        nppDll, "test_npp_notifyCount") : nullptr;
    auto notifCodeFn = nppDll ? (unsigned (*)(int))::GetProcAddress(
        nppDll, "test_npp_notifyCode") : nullptr;
    auto notifIdFn = nppDll ? (UINT_PTR (*)(int))::GetProcAddress(
        nppDll, "test_npp_notifyId") : nullptr;
    auto notifHwndFn = nppDll ? (HWND (*)(int))::GetProcAddress(
        nppDll, "test_npp_notifyHwnd") : nullptr;
    CHECK(notifCountFn && notifCodeFn && notifIdFn && notifHwndFn);

    // 便捷合成器：READY（idFrom=0）+ FILEOPENED（任意不透明 BufferID）
    const UINT_PTR fakeBuf = (UINT_PTR)0x1234;
    mgr.EmitNppNotification(npp::NPPN_READY, 0);
    mgr.EmitNppNotification(npp::NPPN_FILEOPENED, fakeBuf);
    if (notifCountFn && notifCodeFn && notifIdFn && notifHwndFn) {
        CHECK(notifCountFn() == 2);
        if (notifCountFn() == 2) {
            // 契约数值：NPPN_FIRST=1000 → READY=1001、FILEOPENED=1004
            CHECK(notifCodeFn(0) == 1001u);
            CHECK(notifIdFn(0) == 0);
            CHECK(notifHwndFn(0) == nullptr);   // 无宿主窗口降级
            CHECK(notifCodeFn(1) == 1004u);
            CHECK(notifIdFn(1) == fakeBuf);
        }
    }

    // 空指针安全：BroadcastNppNotification(nullptr) 不得崩、不得计数
    mgr.BroadcastNppNotification(nullptr);
    if (notifCountFn) CHECK(notifCountFn() == 2);

    // ---- native channel: dispatch executes callback on this thread ---------
    // a crash in any host-callback bridge would abort before these CHECKs.
    if (pluginId) {
        CHECK(mgr.Execute(pluginId) == true);

        // observe v2 flags + v3 hook ids the recorded callback produced
        HMODULE dll = ::GetModuleHandleW((dir + L"\\test_plugin_core.dll").c_str());
        CHECK(dll != nullptr);
        if (dll) {
            auto flagsFn = (unsigned (*)(void))::GetProcAddress(
                dll, "test_plugin_lastFlags");
            auto callsFn = (int (*)(void))::GetProcAddress(dll, "test_plugin_queryCalls");
            auto allIdFn = (int (*)(void))::GetProcAddress(dll, "test_plugin_hookAllId");
            auto narrowIdFn = (int (*)(void))::GetProcAddress(dll, "test_plugin_hookNarrowId");
            auto broadNFn = (int (*)(void))::GetProcAddress(dll, "test_plugin_broadCount");
            auto broadTFn = (unsigned (*)(int))::GetProcAddress(dll, "test_plugin_broadType");
            auto narrowNFn = (int (*)(void))::GetProcAddress(dll, "test_plugin_narrowCount");
            auto narrowTFn = (unsigned (*)(int))::GetProcAddress(dll, "test_plugin_narrowType");
            CHECK(flagsFn && callsFn && allIdFn && narrowIdFn &&
                  broadNFn && broadTFn && narrowNFn && narrowTFn);
            if (flagsFn && callsFn) {
                CHECK(callsFn() == 1);
                const unsigned kExpectAllNoDoc = 32767u;   // bits 0..14 set
                unsigned f = flagsFn();
                CHECK(f == kExpectAllNoDoc);
                if (f != kExpectAllNoDoc)
                    printf("  no-doc degradation bitmap = 0x%04X (want 0x%04X)\n",
                           f, kExpectAllNoDoc);
            }

            // ---- v3: mask filtering + mid-dispatch removal -----------------
            // The command registered two hooks: ALL + (DOC_SAVED|CURSOR_MOVED).
            const int hAll = allIdFn ? allIdFn() : 0;
            const int hNarrow = narrowIdFn ? narrowIdFn() : 0;
            CHECK(hAll >= 1);
            CHECK(hNarrow >= 1 && hNarrow != hAll);

            mgr.Raise(XFS_EVT_DOC_OPENED, "a.txt");     // narrow NOT subscribed
            mgr.Raise(XFS_EVT_DOC_SAVED, "b.txt");      // narrow records, then
                                        // broad's cb removes narrow mid-dispatch
            mgr.Raise(XFS_EVT_CURSOR_MOVED, "1,2");     // only broad still gets it

            if (broadNFn && broadTFn && narrowNFn && narrowTFn) {
                CHECK(broadNFn() == 3);
                if (broadNFn() == 3) {
                    CHECK(broadTFn(0) == XFS_EVT_DOC_OPENED);
                    CHECK(broadTFn(1) == XFS_EVT_DOC_SAVED);
                    CHECK(broadTFn(2) == XFS_EVT_CURSOR_MOVED);
                }
                // narrow saw exactly DOC_SAVED before being unsubscribed
                CHECK(narrowNFn() == 1);
                if (narrowNFn() == 1)
                    CHECK(narrowTFn(0) == XFS_EVT_DOC_SAVED);
            }
            CHECK(mgr.HookCount() == 1);   // only broad remains subscribed

            // explicit removal path is idempotent for unknown ids
            mgr.HostRemoveEventHook(hAll);
            mgr.HostRemoveEventHook(99999);
            CHECK(mgr.HookCount() == 0);
        }
    }
    CHECK(mgr.Execute(1234) == false);   // built-in/unknown id is not a plugin

    // ---- load-failure ledger (plugin admin "incompatible" tab) -------------
    // The dir contains only good DLLs, so the ledger must be empty here.
    CHECK(mgr.LoadFailures().empty());
    {
        // A non-DLL garbage file must be refused and recorded with a reason.
        std::wstring junk = dir + L"\\test_garbage.dll";
        FILE* f = nullptr;
        _wfopen_s(&f, junk.c_str(), L"wb");
        if (f) { fwrite("not a dll", 1, 9, f); fclose(f); }
        PluginManager mgr2;
        const int loaded2 = mgr2.LoadAllFrom(dir);
        CHECK(loaded2 == 2);                       // good DLLs still load
        CHECK(mgr2.LoadFailures().size() == 1);    // exactly the junk file
        if (mgr2.LoadFailures().size() == 1) {
            CHECK(mgr2.LoadFailures()[0].reason == L"load");
        }
        DeleteFileW(junk.c_str());
    }

    mgr.UnloadAll();
    CHECK(mgr.CommandCount() == 0);      // clean teardown, no crash

    if (g_fail == 0) { printf("ALL PLUGIN TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
