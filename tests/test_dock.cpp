// test_dock.cpp — 4d 端到端：真实 NPP 形态插件注册可停靠面板。
//
// 验证全链路（不再用 FakeDockHost）：
//   插件 DLL(test_npp_dock.dll) → SendMessage(NPPM_DMMREGASDCKDLG)
//     → PluginManager::ForwardNppMessage → DockHost(真实 DockManager)
//     → wrapper 窗口创建 / hClient 重挂 / DMN_DOCK 通知 / 布局 / 关闭协商
//     / DMM_CLOSE 退出。
//
// 本测试创建真实宿主窗口（普通 Win32 顶层窗）作为 MainWindow 替身；
// 其余断言全部走真实 Win32 行为（IsWindow/IsWindowVisible/GetParent/
// GetClassNameW/MapWindowPoints）。
#include "../src/plugin/PluginManager.h"
#include "../src/plugin/DockManager.h"
#include "../src/core/Log.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const wchar_t kHostClass[] = L"xfsDockTestHost";
static xfs::PluginManager* g_mgr = nullptr;
static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // 镜像 MainWindow::Handle：先让 NPP 消息垫片拦截（NPPM_*/RUNCOMMAND），
    // 未处理的才落 DefWindowProc —— 插件 SendMessage(NPPM_*) 到宿主才有回响。
    if (g_mgr) {
        bool handled = false;
        LRESULT res = g_mgr->ForwardNppMessage(m, w, l, handled);
        if (handled) return res;
    }
    return ::DefWindowProcW(h, m, w, l);
}

// DMM 消息数值（契约：NPPMSG=WM_USER+1000，DMMSHOW=+30、DMMHIDE=+31）
enum { kNPPM_DMMSHOW = WM_USER + 1000 + 30, kNPPM_DMMHIDE = WM_USER + 1000 + 31 };
// wrapper 内关闭钮控制 id（DockManager.cpp 私有常量，契约值）
enum { kCloseId = 2401 };

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: test_dock <dir-containing-dll>\n");
        printf("DOCK TEST SKIPPED (no dir)\n");
        return 0;   // not an infra failure
    }
    std::wstring dir = Utf8ToWide(argv[1]);

    // ---- 真实宿主窗口（MainWindow 替身）------------------------------------
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kHostClass;
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(0, kHostClass, L"host", WS_OVERLAPPEDWINDOW,
                                  0, 0, 800, 600, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    CHECK(host != nullptr);
    if (!host) return 2;
    ::ShowWindow(host, SW_SHOW);   // 显示宿主，否则整棵窗口树 IsWindowVisible=FALSE

    // ---- 生产链路装配：真实 DockManager 注入为 DockHost ---------------------
    DockManager dock;
    dock.Init(host, wc.hInstance);
    PluginManager mgr;
    mgr.SetHostWindow(host);
    mgr.SetDockHost(&dock);
    g_mgr = &mgr;   // 让宿主窗口 proc 转发 NPPM_*（镜像 MainWindow::Handle）

    int loaded = mgr.LoadAllFrom(dir);
    CHECK(loaded == 1);
    CHECK(mgr.CommandCount() == 1);

    unsigned int cmd = 0;
    for (const auto& c : mgr.Commands()) {
        CHECK(c.grouped);
        CHECK(c.category == L"npp-dock-test");
        cmd = c.id;
    }
    CHECK(cmd != 0);

    HMODULE dll = ::GetModuleHandleW((dir + L"\\test_npp_dock.dll").c_str());
    CHECK(dll != nullptr);
    auto hClientFn = dll ? (HWND (*)(void))::GetProcAddress(dll, "test_npp_dock_hClient") : nullptr;
    auto regResFn  = dll ? (BOOL (*)(void))::GetProcAddress(dll, "test_npp_dock_regResult") : nullptr;
    auto regCntFn  = dll ? (int (*)(void))::GetProcAddress(dll, "test_npp_dock_regCount") : nullptr;
    auto setVetoFn = dll ? (void (*)(BOOL))::GetProcAddress(dll, "test_npp_dock_setVetoClose") : nullptr;
    auto dmnNFn    = dll ? (int (*)(void))::GetProcAddress(dll, "test_npp_dock_dmnCount") : nullptr;
    auto dmnCFn    = dll ? (DWORD (*)(int))::GetProcAddress(dll, "test_npp_dock_dmnCode") : nullptr;
    auto dmnIFn    = dll ? (UINT_PTR (*)(int))::GetProcAddress(dll, "test_npp_dock_dmnId") : nullptr;
    auto dmnFromFn = dll ? (HWND (*)(int))::GetProcAddress(dll, "test_npp_dock_dmnFrom") : nullptr;
    CHECK(hClientFn && regResFn && regCntFn && setVetoFn && dmnNFn && dmnCFn && dmnIFn && dmnFromFn);
    if (!hClientFn || !dmnNFn) {
        printf("DOCK TEST ABORTED (missing exports)\n");
        return 2;
    }

    // ---- 触发命令：插件建对话框 + NPPM_DMMREGASDCKDLG 注册 ------------------
    fprintf(stderr, "[dock] executing cmd=%u\n", cmd); fflush(stderr);
    CHECK(mgr.Execute(cmd) == true);
    fprintf(stderr, "[dock] after execute\n"); fflush(stderr);
    CHECK(dock.PanelCount() == 1);
    CHECK(regCntFn() == 1);
    CHECK(regResFn() == TRUE);
    HWND hc = hClientFn();
    CHECK(hc != nullptr && ::IsWindow(hc));

    // reparent 契约：hClient 的父窗口是 DockManager 的 wrapper
    HWND wrap = ::GetParent(hc);
    CHECK(wrap != nullptr);
    wchar_t cls[64] = {};
    ::GetClassNameW(wrap, cls, 64);
    CHECK(wcscmp(cls, L"xfsWinPadPluginDock") == 0);

    // DMN_DOCK (1052) 到达插件；idFrom = 打开它的 FuncItem cmdID
    // hwndFrom 必须是宿主主窗口句柄（NppExec 等插件只认这个来源的 DMN_*）
    bool sawDock = false;
    UINT_PTR dockId = 0;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == 1052) { sawDock = true; dockId = dmnIFn(i); }
    CHECK(sawDock);
    CHECK(dockId == (UINT_PTR)cmd);
    bool dockFromOk = false;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == 1052 && dmnFromFn(i) == host) dockFromOk = true;
    CHECK(dockFromOk);

    // 注册后宿主立即显示
    CHECK(::IsWindowVisible(hc));

    // ---- NPPM_DMMHIDE / DMMSHOW 转发到真实 DockManager ----------------------
    const int dpi = ::GetDpiForWindow(host);
    {
        bool handled = false;
        LRESULT r = mgr.ForwardNppMessage(kNPPM_DMMHIDE, 0, (LPARAM)hc, handled);
        CHECK(handled && r == TRUE);
        CHECK(!::IsWindowVisible(hc));
        CHECK(dock.TotalHeight(dpi) == 0);     // 隐藏的面板不占布局高度
    }
    {
        bool handled = false;
        LRESULT r = mgr.ForwardNppMessage(kNPPM_DMMSHOW, 0, (LPARAM)hc, handled);
        CHECK(handled && r == TRUE);
        CHECK(::IsWindowVisible(hc));
        CHECK(dock.TotalHeight(dpi) > 0);
    }
    // Show 触发 DMN_SWITCHIN (1054)
    bool sawSwitch = false;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == 1054) sawSwitch = true;
    CHECK(sawSwitch);
    bool switchFromOk = false;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == 1054 && dmnFromFn(i) == host) switchFromOk = true;
    CHECK(switchFromOk);

    // ---- 布局：TotalHeight/Layout 布置 wrapper（client 坐标）----------------
    int yEnd = dock.Layout(10, 20, 300, dpi);
    CHECK(yEnd > 20);
    POINT pt = {0, 0};
    ::MapWindowPoints(wrap, host, &pt, 1);
    CHECK(pt.x == 10 && pt.y == 20);
    RECT rc{};
    ::GetWindowRect(wrap, &rc);
    CHECK((rc.right - rc.left) == 300);

    // ---- 查询：按窗口名取句柄 ------------------------------------------------
    CHECK(dock.FindHwndByName(L"npp-dock", nullptr) == hc);
    CHECK(dock.FindHwndByName(L"no-such", nullptr) == nullptr);

    // ---- 关闭协商（无 veto）：wrapper 关闭钮 → DMN_CLOSE → 隐藏 -------------
    ::SendMessageW(wrap, WM_COMMAND, MAKEWPARAM(kCloseId, BN_CLICKED), 0);
    CHECK(!::IsWindowVisible(hc));
    // DMN_CLOSE 的 hwndFrom 也必须是宿主主窗口（问题5：NppExec 靠这个收关闭通知）
    bool closeFromOk = false;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == 1051 && dmnFromFn(i) == host) closeFromOk = true;
    CHECK(closeFromOk);

    // ---- veto 路径：插件返回 TRUE → 保持打开 ---------------------------------
    setVetoFn(TRUE);
    {
        bool handled = false;
        CHECK(mgr.ForwardNppMessage(kNPPM_DMMSHOW, 0, (LPARAM)hc, handled) == TRUE);
        CHECK(::IsWindowVisible(hc));
        ::SendMessageW(wrap, WM_COMMAND, MAKEWPARAM(kCloseId, BN_CLICKED), 0);
        fprintf(stderr, "[dock] veto dmnCount=%d\n", dmnNFn());
        for (int i = 0; i < dmnNFn(); ++i)
            fprintf(stderr, "[dock]   dmn[%d]=code %u id %u\n", i,
                    (unsigned)dmnCFn(i), (unsigned)dmnIFn(i));
        CHECK(::IsWindowVisible(hc));          // veto 生效，面板仍在
    }
    setVetoFn(FALSE);

    // ---- 宿主退出：DMM_CLOSE(0x5001) → 插件自毁对话框 ------------------------
    dock.Destroy();
    CHECK(dock.PanelCount() == 0);
    CHECK(!::IsWindow(hc));                    // 插件收到 DMM_CLOSE 后 DestroyWindow

    // ---- 重建：同一 DLL 命令再次注册应得到全新面板（资源未泄漏）-------------
    CHECK(mgr.Execute(cmd) == true);
    CHECK(dock.PanelCount() == 1);
    HWND hc2 = hClientFn();
    CHECK(hc2 != nullptr && hc2 != hc && ::IsWindow(hc2));
    dock.Destroy();

    mgr.UnloadAll();
    CHECK(mgr.CommandCount() == 0);            // 干净卸载

    ::DestroyWindow(host);

    if (g_fail == 0) { printf("ALL DOCK TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
