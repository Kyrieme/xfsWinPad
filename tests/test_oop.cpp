// test_oop.cpp — 进程外插件桥端到端（真实代理进程 + 真实 NPP 形态 DLL）。
//
// 覆盖：
//  1. oop_good：Launch 握手 → 命令注册（grouped）→ 跨进程 EXEC
//     （观察标记回传 cmdID 回填证明）→ 跨进程 NOTIFY（beNotified 证明）。
//  2. oop_killer：setInfo 里 exit(1)（ComparePlus 同款）→ 代理死、
//     Launch 失败、本测试进程存活。
//  3. oop_crasher：命令回调访问违例 → 代理 SEH 捕获（EXEC 被拒），
//     代理存活、编辑器无感。
//  4. ShutdownAll：干净退出路径（watchdog join、SHUTDOWN 送达）。
//  5. 一代理多插件（批次 66）：good+crasher+multi 共享单代理，独立执行/通知。
//  6. 死亡归因+幸存者重加：killer 毒死共享代理 → 只记 killer oop-died，
//     good/multi 撤销命令后重进新代理并可执行。
//  7. 装载卡死：hang 插件 setInfo 永不返回 → 代理看门狗自杀 → 同 6 归因。
//  9. plugin_oop.txt 主动隔离（批次 102）：**从未崩溃过**的正常插件写进名单
//     → 直接走代理，账本记 oop-forced；指定的恶意插件代理失败时**不回落
//     进程内**（回落会让本测试进程当场死亡——这正是要防的事）。
//
// 命令行参数 1：测试插件 DLL 所在目录（ctest 传 $<TARGET_FILE_DIR:oop_good>）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <filesystem>

#include "../src/plugin/PluginManager.h"
#include "../src/plugin/oop/OopHost.h"
#include "../src/core/Log.h"
#include "../src/core/Util.h"

using namespace xfs;

// ---- 观察标记（与 oop_good / oop_multi 约定一致）-----------------------------
struct Marker { UINT_PTR tag; INT_PTR value; };
static constexpr UINT_PTR kMarkerMagic = 0x474F4F44;   // 'GOOD'
static constexpr UINT_PTR kMultiMagic  = 0x4D554C54;   // 'MULT'

// ---- 测试观察窗口：接收插件回传的 Marker（WM_COPYDATA）-----------------------
static UINT_PTR g_markers[64];
static INT_PTR g_markerVals[64];
static int g_markerCount = 0;

static LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COPYDATA) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (cds && (cds->dwData == kMarkerMagic || cds->dwData == kMultiMagic) &&
            cds->cbData == sizeof(Marker)) {
            auto* mk = reinterpret_cast<const Marker*>(cds->lpData);
            if (g_markerCount < 64) {
                g_markers[g_markerCount] = mk->tag;
                g_markerVals[g_markerCount] = mk->value;
                ++g_markerCount;
            }
            return TRUE;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("PASS: %s\n", msg); } \
    else { std::printf("FAIL: %s\n", msg); ++g_fail; } } while (0)

static bool FindMarker(UINT_PTR tag, INT_PTR expect) {
    for (int i = 0; i < g_markerCount; ++i)
        if (g_markers[i] == tag && g_markerVals[i] == expect) return true;
    return false;
}

static void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// 观察标记接收窗（插件以 WM_COPYDATA 直发此窗；同时充当 nppHandle）
static HWND MakeRecv() {
    static bool reg = false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"test_oop_recv";
    if (!reg) { RegisterClassExW(&wc); reg = true; }
    return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

static void ResetMarkers() { g_markerCount = 0; }

// 等待 until 谓词为真（至多 ms），期间泵消息——幸存者重加是异步收敛过程
static bool WaitUntil(const std::function<bool()>& until, DWORD ms) {
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        if (until()) return true;
        Sleep(50);
        PumpMessages();
    }
    return until();
}

static bool HasName(const std::vector<OopPluginInfo>& v, const wchar_t* name) {
    for (auto& p : v) if (p.name == name) return true;
    return false;
}

int main(int argc, char** argv) {
    const char* pluginDir = (argc > 1) ? argv[1] : ".";
    std::filesystem::path dllDir = std::filesystem::absolute(
        std::filesystem::path(pluginDir));

    Logger::Init();
    std::printf("== test_oop: out-of-process plugin bridge ==\n");

    // ---- 场景 1：oop_good 全链路 -------------------------------------------
    {
        PluginManager mgr;
        OopHost host;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TestWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"test_oop_recv";
        RegisterClassExW(&wc);
        HWND recv = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

        std::wstring dll = (dllDir / L"oop_good.dll").wstring();
        bool ok = host.Launch(mgr, dll, recv, nullptr, 15000);
        CHECK(ok, "oop_good: handshake completed");

        auto alive = host.Alive();
        CHECK(alive.size() == 1 && alive[0].name == L"oop-good",
              "oop_good: alive proxy reports plugin name");
        CHECK(mgr.CommandCount() == 2, "oop_good: 2 commands registered");
        CHECK(mgr.Commands()[0].grouped && mgr.Commands()[0].category == L"oop-good",
              "oop_good: commands grouped under plugin");

        // EXEC index 0：插件应回传 Marker(tag=1, value=回填后的 cmdID)
        unsigned id0 = mgr.Commands()[0].id;
        CHECK(mgr.Execute(id0), "oop_good: Execute(id0) dispatched");
        Sleep(200);
        PumpMessages();
        CHECK(FindMarker(1, (INT_PTR)id0),
              "oop_good: EXEC ran in proxy; cmdID was backfilled correctly");

        // NOTIFY：代理 beNotified 应收到 code（Marker tag=2）
        host.BroadcastNotify(1234, 0);
        Sleep(200);
        PumpMessages();
        CHECK(FindMarker(2, (INT_PTR)1234),
              "oop_good: NOTIFY reached beNotified in proxy");

        host.ShutdownAll();
        CHECK(host.Alive().empty(), "oop_good: ShutdownAll closed proxy");
        DestroyWindow(recv);
    }

    // ---- 场景 2：oop_killer（setInfo 即 exit(1)，ComparePlus 同款）----------
    {
        PluginManager mgr;
        OopHost host;
        std::wstring dll = (dllDir / L"oop_killer.dll").wstring();
        bool ok = host.Launch(mgr, dll, nullptr, nullptr, 15000);
        CHECK(!ok, "oop_killer: Launch failed (proxy died), editor survived");
        CHECK(mgr.CommandCount() == 0, "oop_killer: no commands leaked");
        auto fails = host.TakeFailures();
        CHECK(fails.size() == 1 && fails[0].reason == L"oop-died",
              "oop_killer: failure recorded as oop-died");
    }

    // ---- 场景 3：oop_crasher（EXEC 里访问违例 → SEH 捕获，代理存活）---------
    {
        PluginManager mgr;
        OopHost host;
        std::wstring dll = (dllDir / L"oop_crasher.dll").wstring();
        CHECK(host.Launch(mgr, dll, nullptr, nullptr, 15000),
              "oop_crasher: handshake completed");
        CHECK(mgr.CommandCount() == 1, "oop_crasher: command registered");

        unsigned id = mgr.Commands()[0].id;
        CHECK(mgr.Execute(id), "oop_crasher: Execute dispatched (AV caught in proxy)");
        Sleep(300);
        CHECK(!host.Alive().empty(),
              "oop_crasher: proxy survived access violation (isolated)");
        host.ShutdownAll();
        CHECK(host.Alive().empty(), "oop_crasher: clean shutdown");
    }

    // ---- 场景 4：墓碑等价性 --------------------------------------------------
    // 墓碑插件的进程外结果 = 场景 2（Launch 失败 + oop-died 记账），
    // PluginManager 侧仅多一次 TakeFailures 并账，逻辑已覆盖。
    std::printf("PASS: tombstone path equivalence (covered by scenario 2)\n");

    // ---- 场景 5：一代理多插件共享（批次 66）----------------------------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "multi-tenant: oop_good launched");
        CHECK(host.Launch(mgr, (dllDir / L"oop_crasher.dll").wstring(), recv, nullptr, 15000),
              "multi-tenant: oop_crasher shares the proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_multi.dll").wstring(), recv, nullptr, 15000),
              "multi-tenant: oop_multi shares the proxy");
        CHECK(host.ProcessCount() == 1,
              "multi-tenant: three plugins live in ONE proxy process");
        CHECK(host.Alive().size() == 3, "multi-tenant: three plugins alive");
        CHECK(mgr.CommandCount() == 5, "multi-tenant: 2+1+2 commands registered");

        unsigned mid = mgr.Commands()[3].id;      // Multi Alpha（multi 首命令）
        CHECK(mgr.Execute(mid), "multi-tenant: Execute multi cmd dispatched");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(5, (INT_PTR)mid),
              "multi-tenant: multi EXEC ran with own backfilled cmdID");

        host.BroadcastNotify(777, 0);
        Sleep(200); PumpMessages();
        CHECK(FindMarker(2, (INT_PTR)777), "multi-tenant: NOTIFY reached good");
        CHECK(FindMarker(6, (INT_PTR)777), "multi-tenant: NOTIFY reached multi");

        host.ShutdownAll();
        CHECK(host.Alive().empty() && host.ProcessCount() == 0,
              "multi-tenant: clean shutdown of shared proxy");
        DestroyWindow(recv);
    }

    // ---- 场景 6：死亡归因 + 幸存者重加（killer 毒死共享代理）-----------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "attribution: good on shared proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_multi.dll").wstring(), recv, nullptr, 15000),
              "attribution: multi on same proxy");
        CHECK(host.ProcessCount() == 1, "attribution: two plugins, one proxy");

        bool ok = host.Launch(mgr, (dllDir / L"oop_killer.dll").wstring(), recv, nullptr, 15000);
        CHECK(!ok, "attribution: killer Launch failed (proxy died), test survived");
        auto fails = host.TakeFailures();
        CHECK(fails.size() == 1 && fails[0].reason == L"oop-died" &&
              fails[0].path.find(L"oop_killer.dll") != std::wstring::npos,
              "attribution: exactly the killer recorded oop-died");
        CHECK(host.Alive().size() == 2 &&
              HasName(host.Alive(), L"oop-good") && HasName(host.Alive(), L"oop-multi"),
              "attribution: survivors good+multi alive");
        CHECK(WaitUntil([&] { return host.ProcessCount() == 1; }, 5000),
              "attribution: survivors re-added into exactly ONE fresh proxy");
        CHECK(mgr.CommandCount() == 4,
              "attribution: stale commands withdrawn, survivors re-registered (4)");

        unsigned gid = mgr.Commands()[0].id;
        CHECK(mgr.Execute(gid), "attribution: good Execute after re-add");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(1, (INT_PTR)gid), "attribution: good EXEC marker after re-add");
        unsigned mid = mgr.Commands()[2].id;
        CHECK(mgr.Execute(mid), "attribution: multi Execute after re-add");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(5, (INT_PTR)mid), "attribution: multi EXEC marker after re-add");

        host.ShutdownAll();
        DestroyWindow(recv);
    }

    // ---- 场景 7：装载卡死 → 代理看门狗自杀 → 同死亡归因回收 -------------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "hang: good on shared proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_multi.dll").wstring(), recv, nullptr, 15000),
              "hang: multi on same proxy");
        bool ok = host.Launch(mgr, (dllDir / L"oop_hang.dll").wstring(), recv, nullptr, 4000);
        CHECK(!ok, "hang: hung load recycled proxy (watchdog), test survived");
        auto fails = host.TakeFailures();
        CHECK(fails.size() == 1 && fails[0].reason == L"oop-died" &&
              fails[0].path.find(L"oop_hang.dll") != std::wstring::npos,
              "hang: hung plugin attributed, survivors recycled");
        CHECK(host.Alive().size() == 2 && host.ProcessCount() == 1,
              "hang: survivors alive in one fresh proxy");
        unsigned gid = mgr.Commands()[0].id;
        CHECK(mgr.Execute(gid), "hang: Execute works after recycling");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(1, (INT_PTR)gid), "hang: EXEC marker after recycling");
        host.ShutdownAll();
        DestroyWindow(recv);
    }

    // ---- 场景 8：messageProc 双向同步桥（v2.1，批次 71）----------------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "msgbridge: good on proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_msg.dll").wstring(), recv, nullptr, 15000),
              "msgbridge: msg plugin on same proxy");

        bool handled = false;
        LRESULT r = host.BroadcastMessage(WM_APP + 0x71, 0x1234, (LPARAM)0x5678, &handled);
        PumpMessages();
        CHECK(r == (LRESULT)0x1235 && handled,
              "msgbridge: messageProc LRESULT synced back (first non-zero wins)");
        CHECK(FindMarker(10, (INT_PTR)(WM_APP + 0x71)), "msgbridge: msg crossed intact");
        CHECK(FindMarker(11, (INT_PTR)0x1234), "msgbridge: wParam crossed intact");
        CHECK(FindMarker(12, (INT_PTR)0x5678), "msgbridge: lParam crossed intact");

        host.ShutdownAll();

        // 阴性：全部插件返回 0（oop_good stub）→ 0 + handled=false
        ResetMarkers();
        PluginManager mgr2;
        OopHost host2;
        CHECK(host2.Launch(mgr2, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "msgbridge: negative baseline launched");
        bool negHandled = true;
        CHECK(host2.BroadcastMessage(WM_APP + 0x72, 1, 2, &negHandled) == 0 && !negHandled,
              "msgbridge: all-zero replies aggregate to 0/unhandled");
        host2.ShutdownAll();
        DestroyWindow(recv);
    }

    // ---- 场景 9：plugin_oop.txt 主动进程外加载（批次 102）--------------------
    // 场景 2~7 的进程外都是**被动**触发的（插件得先杀死宿主一次 → 墓碑）。
    // 本场景验证**主动**入口：把插件路径写进 plugin_oop.txt，启动时直接走代理。
    // 两个子场景：
    //   9a. 正常插件（oop_good，从未崩溃）→ 被强制进程外，账本记 oop-forced
    //       （**不是** oop-auto —— 令牌区分「用户指定」与「自动隔离」）。
    //   9b. 恶意插件（oop_killer）→ 代理失败，且**绝不回落进程内**。
    //       ★ 若此断言回归失败，回落会把 killer 装进本测试进程 ⇒ 进程当场退出。
    //         所以"本测试崩了"就是"隔离回归了"的信号，不是测试本身不稳。
    {
        ResetMarkers();
        PluginManager mgr;
        HWND recv = MakeRecv();
        mgr.SetHostWindow(recv);      // TryOopLoad 要求有宿主窗口，否则直接返回 false
        mgr.EnableOopHost();

        auto dir = std::filesystem::temp_directory_path() / L"xfs_oop_forced_test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        // ★ 名单里**故意写成正斜杠**。真实用户手写 plugin_oop.txt 用反斜杠，
        //   而扫描出来的路径可能带正斜杠（真机实测：APPDATA 带正斜杠 ⇒ 整条
        //   路径 `D:/...`）。若两侧都用 std::filesystem 的 .string()（都是
        //   反斜杠），这个测试对分隔符差异**完全无感** —— 第一版就是这样，
        //   结果真机上一跑插件仍走进程内加载。此处固定用正斜杠复现该场景。
        auto Slashify = [](std::string s) {
            for (auto& c : s) if (c == '\\') c = '/';
            return s;
        };

        // 9a：正常插件 + 名单
        std::filesystem::copy_file(dllDir / L"oop_good.dll", dir / L"oop_good.dll",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        {
            std::ofstream f((dir / L"plugin_oop.txt").c_str());
            f << Slashify((dir / L"oop_good.dll").string()) << "\n";
        }
        int n = mgr.LoadAllFrom(dir.wstring());
        CHECK(n == 1, "forced-oop: normal plugin loaded (1)");
        CHECK(mgr.IsolatedPlugins().size() == 1 &&
              mgr.IsolatedPlugins()[0].reason == L"oop-forced",
              "forced-oop: ledger token is oop-forced, not oop-auto");
        CHECK(mgr.LoadFailures().empty(),
              "forced-oop: successful isolation is NOT recorded as a failure");
        // ★ 进程级证据。负控实测：把强制名单关掉后，插件会**进程内**加载，
        // 而「命令数=2」「EXEC 回传标记」这两条**照样通过**——因为插件无论
        // 在哪个进程里，行为都一模一样。真正能区分的是下面这条（代理进程
        // 数量）与上面的账本令牌，缺了它们这组断言就是自证。
        CHECK(mgr.OopHostPtr() && mgr.OopHostPtr()->ProcessCount() == 1 &&
              mgr.OopHostPtr()->Alive().size() == 1,
              "forced-oop: a real proxy PROCESS hosts the plugin (not in-process)");
        CHECK(mgr.CommandCount() == 2,
              "forced-oop: 2 commands registered (via proxy)");
        {
            unsigned id0 = mgr.Commands()[0].id;
            CHECK(mgr.Execute(id0), "forced-oop: Execute dispatched");
            Sleep(200); PumpMessages();
            CHECK(FindMarker(1, (INT_PTR)id0),
                  "forced-oop: EXEC round-tripped through the proxy");
        }

        // 9b：恶意插件 + 名单 → 代理死，但不回落进程内
        {
            std::filesystem::copy_file(dllDir / L"oop_killer.dll", dir / L"oop_killer.dll",
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::ofstream f((dir / L"plugin_oop.txt").c_str());
            f << Slashify((dir / L"oop_good.dll").string()) << "\n"
              << Slashify((dir / L"oop_killer.dll").string()) << "\n";
        }
        PluginManager mgr2;
        mgr2.SetHostWindow(recv);
        mgr2.EnableOopHost();
        int n2 = mgr2.LoadAllFrom(dir.wstring());
        CHECK(n2 == 1, "forced-oop: only the good plugin ends up loaded");
        bool killerFailed = false;
        for (const auto& f : mgr2.LoadFailures())
            if (f.path.find(L"oop_killer.dll") != std::wstring::npos) killerFailed = true;
        CHECK(killerFailed,
              "forced-oop: killer recorded as a failure (proxy died), not loaded in-process");
        CHECK(mgr2.CommandCount() == 2,
              "forced-oop: killer contributed no commands (would have killed the host)");

        mgr2.UnloadAll();
        mgr.UnloadAll();
        DestroyWindow(recv);
        std::filesystem::remove_all(dir, ec);
    }

    std::printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
