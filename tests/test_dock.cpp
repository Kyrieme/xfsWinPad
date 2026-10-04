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
#include "../src/plugin/npp/NppDockMask.h"   // 批次 119：DWS_* 掩码常量（判据用）
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

// DockManager.cpp 内部同款 DPI 换算（几何断言用）
static int Scale2(int v, int dpi) { return ::MulDiv(v, dpi, 96); }

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
    // 批次 126：拖拽吸附的坐标全部是**应用内物理坐标**——测试进程若保持
    // DPI-unaware，GetWindowRect 会被虚拟化，与应用内物理坐标错位 1.25×，
    // 拖动终点永远落不进吸附带（snap 恒 floating 的根因）。
    ::SetProcessDPIAware();
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
    CHECK(mgr.CommandCount() == 2);            // Dock Panel + Side Panels (批次 122)

    unsigned int cmd = 0;
    unsigned int sideCmd = 0;
    for (const auto& c : mgr.Commands()) {
        CHECK(c.grouped);
        CHECK(c.category == L"npp-dock-test");
        if (c.label == L"Dock Panel") cmd = c.id;
        if (c.label == L"Side Panels") sideCmd = c.id;
    }
    CHECK(cmd != 0 && sideCmd != 0 && sideCmd != cmd);

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

    // ---- 批次 119/122：容器分派 + kDwsAddInfo 标题上屏 ----------------------
    //
    // 判据必须有两半（本项目纪律：放行/降级类判据要含"名单外的活样本"）：
    //   · 请求 浮动 ⇒ **必须**计入降级账本（唯一仍降级的请求）；
    //   · 请求 右（批次 122 起为真实右列）/ 底部（含 uMask=0 默认）⇒ **不得**计入。
    // 少了后一半，这个"账本"就退化成"注册了几个面板"的计数器，照样全绿。
    {
        auto regExtraFn = dll ? (BOOL (*)(int, UINT, const wchar_t*, const wchar_t*))
                                    ::GetProcAddress(dll, "test_npp_dock_regExtra")
                              : nullptr;
        auto extraHwndFn = dll ? (HWND (*)(int))::GetProcAddress(dll, "test_npp_dock_extraHwnd")
                               : nullptr;
        CHECK(regExtraFn && extraHwndFn);
        if (regExtraFn && extraHwndFn) {
            // 刚 Destroy 过 ⇒ 面板与账本都该是空的（账本不跨 Destroy 累加）
            CHECK(dock.PanelCount() == 0);
            CHECK(dock.CoercedContainers() == 0);

            // ① 请求浮动 ⇒ 批次 123 起是真实浮动容器（有主顶级窗口），不降级
            CHECK(regExtraFn(0, npp::kDwsDfFloating, L"float-panel", nullptr) == TRUE);
            CHECK(dock.PanelCount() == 1);
            CHECK(dock.CoercedContainers() == 0);   // 五容器齐备 ⇒ 账本恒 0

            // ② 请求右容器 ⇒ 批次 122 起是真实右列，不降级
            CHECK(regExtraFn(1, npp::kDwsDfContRight, L"right-panel", nullptr) == TRUE);
            CHECK(dock.PanelCount() == 2);
            CHECK(dock.CoercedContainers() == 0);

            // ③ ★ 请求底部 + kDwsAddInfo ⇒ 不降级，账本必须还是 0
            CHECK(regExtraFn(2, npp::kDwsDfContBottom | npp::kDwsAddInfo,
                             L"info-panel", L"extra text") == TRUE);
            CHECK(dock.PanelCount() == 3);
            CHECK(dock.CoercedContainers() == 0);

            // 三个面板都真的进了宿主：wrapper 类名对、父窗口是宿主
            HWND info = extraHwndFn(2);
            CHECK(info != nullptr && ::IsWindow(info));
            HWND infoWrap = ::GetParent(info);
            wchar_t icls[64] = {};
            if (infoWrap) ::GetClassNameW(infoWrap, icls, 64);
            CHECK(wcscmp(icls, L"xfsWinPadPluginDock") == 0);
            CHECK(::GetParent(infoWrap) == host);

            // ★ kDwsAddInfo 真的上屏：标题条 STATIC 的文本 = "名字 — 附加信息"
            //   （在此之前 pszAddInfo 只被深拷贝、从未显示）。
            //   标签按控制 id 取（kLabelId=2400 契约值）——首 STATIC 可能是
            //   批次 125 的图标控件。
            wchar_t title[128] = {};
            HWND label = infoWrap ? ::GetDlgItem(infoWrap, 2400) : nullptr;
            CHECK(label != nullptr);
            if (label) ::GetWindowTextW(label, title, 128);
            CHECK(wcscmp(title, L"info-panel — extra text") == 0);

            // ④ 对照：**没**置 kDwsAddInfo 的面板，标题只有名字（配对判据）
            HWND fl = extraHwndFn(0);
            HWND flWrap = fl ? ::GetParent(fl) : nullptr;
            wchar_t t2[128] = {};
            HWND label2 = flWrap ? ::GetDlgItem(flWrap, 2400) : nullptr;
            CHECK(label2 != nullptr);
            if (label2) ::GetWindowTextW(label2, t2, 128);
            CHECK(wcscmp(t2, L"float-panel") == 0);

            // ---- 批次 123：浮动容器 = 有主顶级弹出窗口（不在宿主子树里）-----
            // 注意 WS_POPUP 窗口的 GetParent 返回 owner（≠ nullptr 是正常的）；
            // "是否子窗口"用 WS_CHILD 样式位判别。
            CHECK(flWrap != nullptr);
            CHECK((::GetWindowLongPtrW(flWrap, GWL_STYLE) & WS_CHILD) == 0);
            CHECK(::GetWindow(flWrap, GW_OWNER) == host);       // owner = 主框架
            // 级联缺省位：panels_ 注册时为空 ⇒ n=0 ⇒ (120,120)，400x300 逻辑
            const int dpiH = ::GetDpiForWindow(host);
            RECT frc{};
            ::GetWindowRect(flWrap, &frc);
            CHECK(frc.left == Scale2(dpiH, 120) && frc.top == Scale2(dpiH, 120));
            CHECK((frc.right - frc.left) == Scale2(dpiH, 400));
            CHECK((frc.bottom - frc.top) == Scale2(dpiH, 300));
            // 浮动面板不占底部链高度（上面 TotalHeight 已验证）；DMN_FLOAT 已发
            bool sawFloat = false;
            for (int i = 0; i < dmnNFn(); ++i)
                if (dmnCFn(i) == 1053) sawFloat = true;
            CHECK(sawFloat);

            // ---- 批次 122/123：容器几何分派 --------------------------------
            // 底部链只含 info（float 已是浮动容器、right 在右列）；
            // 右列宽 = right 面板的 widthLogical（默认 260 逻辑像素）。
            CHECK(dock.TotalHeight(dpiH) == Scale2(dpiH, 220));   // 仅 info
            CHECK(dock.TotalWidth(dpiH) ==
                  Scale2(dpiH, 260));                     // 右列宽（仅 right 面板）
            CHECK(dock.TopTotalHeight(dpiH) == 0);        // 尚无顶条面板

            // Layout 只布置底部 info；浮动/右面板不在底部链的几何里
            const int yEnd2 = dock.Layout(10, 20, 300, dpiH);
            CHECK(yEnd2 == 20 + Scale2(dpiH, 220));

            // LayoutRight 从 (50, 60) 向下布置右面板（列宽 = TotalWidth）
            const int yRight = dock.LayoutRight(50, 60, 800, dpiH);
            CHECK(yRight == 60 + Scale2(dpiH, 220));
            HWND rp = extraHwndFn(1);
            CHECK(rp != nullptr);
            POINT rpPt = {0, 0};
            ::MapWindowPoints(::GetParent(rp), host, &rpPt, 1);
            CHECK(rpPt.x == 50 && rpPt.y == 60);
            RECT rpRc{};
            ::GetWindowRect(::GetParent(rp), &rpRc);
            CHECK((rpRc.right - rpRc.left) == Scale2(dpiH, 260));

            // ⑤ 请求顶条 ⇒ 真实顶容器，不降级；TopTotalHeight/LayoutTop 生效
            CHECK(regExtraFn(3, npp::kDwsDfContTop, L"top-panel", nullptr) == TRUE);
            CHECK(dock.CoercedContainers() == 0);   // 批次 123：五容器齐备
            CHECK(dock.TopTotalHeight(dpiH) == Scale2(dpiH, 220));
            const int yTop = dock.LayoutTop(10, 15, 400, dpiH);
            CHECK(yTop == 15 + Scale2(dpiH, 220));
            HWND tp = extraHwndFn(3);
            POINT tpPt = {0, 0};
            ::MapWindowPoints(::GetParent(tp), host, &tpPt, 1);
            CHECK(tpPt.x == 10 && tpPt.y == 15);

            dock.Destroy();
            CHECK(dock.PanelCount() == 0);
            CHECK(dock.CoercedContainers() == 0);
            CHECK(dock.TotalWidth(dpiH) == 0);
            CHECK(dock.TopTotalHeight(dpiH) == 0);
        }
    }

    // ---- 批次 125：kDwsIconTab 图标上屏（仅 npp-dock 带图标）----------------
    // 夹具对 npp-dock 注册设了 hIconTab（LoadIcon IDI_APPLICATION）；图标
    // STATIC 控制id = kLabelId-1 = 2399，STM_GETICON 须非空；对照：标签
    // 控件（2400）不带 SS_ICON。独立节——放在 batch 段 Destroy 之后重建。
    CHECK(mgr.Execute(cmd) == true);
    CHECK(dock.PanelCount() == 1);
    HWND hc3 = hClientFn();
    HWND ndWrap = ::GetParent(hc3);
    CHECK(ndWrap != nullptr && ::IsWindow(ndWrap));
    HWND iconCtrl = ::GetDlgItem(ndWrap, 2399);
    CHECK(iconCtrl != nullptr);
    HICON got = (HICON)::SendMessageW(iconCtrl, 0x0171, 0, 0);   // STM_GETICON
    CHECK(got != nullptr);
    CHECK((::GetWindowLongPtrW(iconCtrl, GWL_STYLE) & SS_ICON) != 0);   // 图标位
    HWND lblCtrl = ::GetDlgItem(ndWrap, 2400);
    CHECK(lblCtrl != nullptr);
    CHECK((::GetWindowLongPtrW(lblCtrl, GWL_STYLE) & SS_ICON) == 0);    // 对照
    dock.Destroy();
    CHECK(dock.PanelCount() == 0);

    // ---- 批次 126：浮动面板拖回宿主重停靠（消息驱动的真实拖拽序列）----------
    auto regExtraFn = dll ? (BOOL (*)(int, UINT, const wchar_t*, const wchar_t*))
                                ::GetProcAddress(dll, "test_npp_dock_regExtra")
                          : nullptr;
    auto extraHwndFn = dll ? (HWND (*)(int))::GetProcAddress(dll, "test_npp_dock_extraHwnd")
                           : nullptr;
    CHECK(regExtraFn && extraHwndFn);
    CHECK(regExtraFn(0, npp::kDwsDfFloating, L"drag-panel", nullptr) == TRUE);
    CHECK(dock.PanelCount() == 1);
    CHECK(dock.CoercedContainers() == 0);      // 浮动已是真实容器
    HWND dcl = extraHwndFn(0);
    HWND dWrap = dcl ? ::GetParent(dcl) : nullptr;
    CHECK(dWrap != nullptr && ::IsWindow(dWrap));
    CHECK((::GetWindowLongPtrW(dWrap, GWL_STYLE) & WS_CHILD) == 0);  // 浮动=弹出
    HWND dLabel = ::GetDlgItem(dWrap, 2400);
    CHECK(dLabel != nullptr);

    RECT lr2{};
    ::GetWindowRect(dLabel, &lr2);             // 标签屏幕位（按下时刻）
    RECT hr2{};
    ::GetWindowRect(host, &hr2);               // 宿主框架（吸附带基准）
    const int dpiH = ::GetDpiForWindow(host);
    // 拖动终点 = 宿主底边中点（在吸附带内）。lambda 每次发送前**重查**标签
    // 当前屏幕位换算 lp —— 移动中 wrapper 跟随光标，旧矩形会让落点漂移
    // （实测漂移 = 旧标签位与新位之差，正好把终点推出吸附带）。
    const POINT drop{ (hr2.left + hr2.right) / 2, hr2.bottom - 10 };
    const POINT down{ lr2.left + 30, lr2.top + 8 };
    auto labelMsg = [&](UINT m, WPARAM wp, POINT scr) {
        RECT lrNow{};
        ::GetWindowRect(dLabel, &lrNow);
        ::SendMessageW(dLabel, m, wp,
                       MAKELPARAM(scr.x - lrNow.left, scr.y - lrNow.top));
    };
    labelMsg(WM_LBUTTONDOWN, MK_LBUTTON, down);
    POINT cur = down;
    for (int step = 1; step <= 5; ++step) {
        cur.x = down.x + (drop.x - down.x) * step / 5;
        cur.y = down.y + (drop.y - down.y) * step / 5;
        labelMsg(WM_MOUSEMOVE, MK_LBUTTON, cur);
    }
    labelMsg(WM_LBUTTONUP, 0, cur);

    // 重停靠断言：容器转底部（wrapper 成宿主子窗口 + WS_CHILD）
    CHECK(::GetParent(dWrap) == host);
    CHECK((::GetWindowLongPtrW(dWrap, GWL_STYLE) & WS_CHILD) != 0);
    CHECK(dock.TotalHeight(dpiH) >= Scale2(dpiH, 220));   // 底部链计入拖回面板
    // DMN_DOCK 重发（浮动期间发过 DMN_DOCK+DMN_FLOAT，重停靠再发 DMN_DOCK）
    int dockCount = 0;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == 1052) ++dockCount;
    CHECK(dockCount >= 2);

    // ---- 批次 127：停靠面板拖出槽位 ⇒ 转浮动；再拖回边缘 ⇒ 重停靠（闭环）----
    // 方向与上节相反（上节 = 浮动→拖回），两节合起来把"停靠⇄浮动"两个方向都
    // 用真实消息序列走一遍。
    CHECK(regExtraFn(1, npp::kDwsDfContBottom, L"out-panel", nullptr) == TRUE);
    CHECK(dock.PanelCount() == 2);
    HWND ocl = extraHwndFn(1);
    HWND oWrap = ocl ? ::GetParent(ocl) : nullptr;
    CHECK(oWrap != nullptr && ::IsWindow(oWrap));
    CHECK((::GetWindowLongPtrW(oWrap, GWL_STYLE) & WS_CHILD) != 0);   // 停靠态
    const int hDocked = dock.TotalHeight(dpiH);
    CHECK(hDocked >= Scale2(dpiH, 220) * 2);      // 两个底部面板都在链上
    HWND oLabel = ::GetDlgItem(oWrap, 2400);
    CHECK(oLabel != nullptr);

    // DMN_FLOAT 计数基线：只断言"最后有 ≥N 条"没有区分力（前面几节注册的
    // 浮动面板就会发过），必须断言**本次拖出恰好 +1**。
    int floatBefore = 0;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == (DWORD)npp::DMN_FLOAT) ++floatBefore;

    auto oLabelMsg = [&](UINT m, WPARAM wp, POINT scr) {
        RECT lrNow{};
        ::GetWindowRect(oLabel, &lrNow);          // 每步重查：拖出后 wrapper 跟随光标
        ::SendMessageW(oLabel, m, wp,
                       MAKELPARAM(scr.x - lrNow.left, scr.y - lrNow.top));
    };
    RECT olr{}, owr{};
    ::GetWindowRect(oLabel, &olr);
    ::GetWindowRect(oWrap, &owr);                 // 槽位矩形（拖出判据基准）
    const POINT oDown{ olr.left + 30, olr.top + 8 };
    // 拖出终点：槽位上方（越过 4 逻辑 px 容差），且离宿主四边都远 ⇒ 松手不吸附
    const POINT oOut{ (owr.left + owr.right) / 2, owr.top - Scale2(dpiH, 40) };

    oLabelMsg(WM_LBUTTONDOWN, MK_LBUTTON, oDown);
    oLabelMsg(WM_MOUSEMOVE, MK_LBUTTON, oOut);

    // 拖出断言：wrapper 转**有主顶级弹窗**、容器不再计入底部链
    // ★ 陷阱（批次 126 记录）：WS_POPUP 的 GetParent 返回 **owner**（≠ nullptr），
    //   与本窗口是子窗口时返回的父窗口**同值** ⇒ "是否已脱离宿主"只能用
    //   WS_CHILD 样式位判，GetParent 在这里没有区分力。
    CHECK((::GetWindowLongPtrW(oWrap, GWL_STYLE) & WS_CHILD) == 0);
    CHECK((::GetWindowLongPtrW(oWrap, GWL_STYLE) & WS_POPUP) != 0);
    CHECK(::GetParent(oWrap) == host);            // 这是 owner（不是父窗口）
    CHECK(dock.TotalHeight(dpiH) < hDocked);      // 槽位已回收
    int floatAfter = 0;
    for (int i = 0; i < dmnNFn(); ++i)
        if (dmnCFn(i) == (DWORD)npp::DMN_FLOAT) ++floatAfter;
    CHECK(floatAfter == floatBefore + 1);         // DMN_FLOAT 恰好 +1
    // 标题条子控件在转换中存活（原地转换的判据之一）
    CHECK(::IsWindow(oLabel) && ::GetDlgItem(oWrap, 2400) == oLabel);

    // 松手（终点离宿主边缘远）⇒ 保持浮动，不吸附
    oLabelMsg(WM_LBUTTONUP, 0, oOut);
    CHECK((::GetWindowLongPtrW(oWrap, GWL_STYLE) & WS_CHILD) == 0);
    CHECK(dock.TotalHeight(dpiH) < hDocked);

    // 反向：从浮动态拖回宿主底边松手 ⇒ 重停靠（FloatOut 的逆操作，闭环收口）
    RECT olr2{};
    ::GetWindowRect(oLabel, &olr2);
    const POINT oDown2{ olr2.left + 30, olr2.top + 8 };
    const POINT oBack{ (hr2.left + hr2.right) / 2, hr2.bottom - 10 };
    oLabelMsg(WM_LBUTTONDOWN, MK_LBUTTON, oDown2);
    POINT oCur = oDown2;
    for (int step = 1; step <= 5; ++step) {
        oCur.x = oDown2.x + (oBack.x - oDown2.x) * step / 5;
        oCur.y = oDown2.y + (oBack.y - oDown2.y) * step / 5;
        oLabelMsg(WM_MOUSEMOVE, MK_LBUTTON, oCur);
    }
    oLabelMsg(WM_LBUTTONUP, 0, oCur);
    CHECK((::GetWindowLongPtrW(oWrap, GWL_STYLE) & WS_CHILD) != 0);   // 重停靠
    CHECK(::GetParent(oWrap) == host);
    CHECK(dock.TotalHeight(dpiH) >= hDocked);     // 槽位重新占用（空间回补）

    dock.Destroy();
    CHECK(dock.PanelCount() == 0);

    // ---- 批次 136：kDwsIconTab 图标在"停靠 ⇄ 浮动"容器转换中存活 --------------
    // 批次 125 只证明注册即上屏（当时是停靠态）；批次 126/127 两节的夹具都不带
    // 图标 ⇒ "图标跨容器"这条路径此前**从未被实测覆盖**（TODO 里最后一条停靠
    // 小尾巴）。图标是 wrapper 的子控件，而转换是原地做的（改样式 + SetParent），
    // 所以图标句柄/父窗口/图标位/图柄/可见性都应当原样存活 —— 本节把"应当"变成
    // 断言。判据取**五项逐项比**而非"控件还在"：只断言存在的话，控件被重建、
    // 图柄被换掉、或转换时被隐藏都会照样全绿。
    auto regExtraIconFn =
        dll ? (BOOL (*)(int, UINT, const wchar_t*, const wchar_t*))
                  ::GetProcAddress(dll, "test_npp_dock_regExtraIcon")
            : nullptr;
    CHECK(regExtraIconFn != nullptr);
    if (regExtraIconFn) {
        CHECK(regExtraIconFn(0, npp::kDwsDfContBottom, L"icon-panel", nullptr) == TRUE);
        CHECK(dock.PanelCount() == 1);
        HWND icl = extraHwndFn(0);
        HWND iWrap = icl ? ::GetParent(icl) : nullptr;
        CHECK(iWrap != nullptr && ::IsWindow(iWrap));
        CHECK((::GetWindowLongPtrW(iWrap, GWL_STYLE) & WS_CHILD) != 0);   // 停靠态

        // 图标控件 id = kLabelId-1 = 2399（与批次 125 同一契约值）
        HWND iIcon = ::GetDlgItem(iWrap, 2399);
        HWND iLabel = ::GetDlgItem(iWrap, 2400);
        CHECK(iIcon != nullptr && ::IsWindow(iIcon));
        CHECK(iLabel != nullptr);
        CHECK((::GetWindowLongPtrW(iIcon, GWL_STYLE) & SS_ICON) != 0);
        CHECK(::GetParent(iIcon) == iWrap);            // 图标是 wrapper 的子控件
        HICON icon0 = (HICON)::SendMessageW(iIcon, 0x0171, 0, 0);   // STM_GETICON
        CHECK(icon0 != nullptr);

        auto iLabelMsg = [&](UINT m, WPARAM wp, POINT scr) {
            RECT lrNow{};
            ::GetWindowRect(iLabel, &lrNow);
            ::SendMessageW(iLabel, m, wp,
                           MAKELPARAM(scr.x - lrNow.left, scr.y - lrNow.top));
        };
        // 拖出槽位 ⇒ FloatOut（与批次 127 同一条真实消息序列）
        RECT ilr{}, iwr{};
        ::GetWindowRect(iLabel, &ilr);
        ::GetWindowRect(iWrap, &iwr);
        const POINT iDown{ ilr.left + 30, ilr.top + 8 };
        const POINT iOut{ (iwr.left + iwr.right) / 2, iwr.top - Scale2(dpiH, 40) };
        iLabelMsg(WM_LBUTTONDOWN, MK_LBUTTON, iDown);
        iLabelMsg(WM_MOUSEMOVE, MK_LBUTTON, iOut);
        CHECK((::GetWindowLongPtrW(iWrap, GWL_STYLE) & WS_CHILD) == 0);   // 已浮动
        // ★ 转换后图标五项逐项存活
        CHECK(::IsWindow(iIcon));
        CHECK(::GetDlgItem(iWrap, 2399) == iIcon);                        // 同句柄
        CHECK(::GetParent(iIcon) == iWrap);                               // 仍挂 wrapper
        CHECK((::GetWindowLongPtrW(iIcon, GWL_STYLE) & SS_ICON) != 0);    // 图标位在
        CHECK((HICON)::SendMessageW(iIcon, 0x0171, 0, 0) == icon0);       // 同图柄
        CHECK(::IsWindowVisible(iIcon));                                  // 仍可见
        iLabelMsg(WM_LBUTTONUP, 0, iOut);
        CHECK((::GetWindowLongPtrW(iWrap, GWL_STYLE) & WS_CHILD) == 0);   // 保持浮动

        // 反向：从浮动态拖回宿主底边松手 ⇒ FloatToDock，图标同样五项存活
        RECT ilr2{};
        ::GetWindowRect(iLabel, &ilr2);
        const POINT iDown2{ ilr2.left + 30, ilr2.top + 8 };
        const POINT iBack{ (hr2.left + hr2.right) / 2, hr2.bottom - 10 };
        iLabelMsg(WM_LBUTTONDOWN, MK_LBUTTON, iDown2);
        POINT iCur = iDown2;
        for (int step = 1; step <= 5; ++step) {
            iCur.x = iDown2.x + (iBack.x - iDown2.x) * step / 5;
            iCur.y = iDown2.y + (iBack.y - iDown2.y) * step / 5;
            iLabelMsg(WM_MOUSEMOVE, MK_LBUTTON, iCur);
        }
        iLabelMsg(WM_LBUTTONUP, 0, iCur);
        CHECK((::GetWindowLongPtrW(iWrap, GWL_STYLE) & WS_CHILD) != 0);   // 重停靠
        CHECK(::GetDlgItem(iWrap, 2399) == iIcon);
        CHECK(::GetParent(iIcon) == iWrap);
        CHECK((::GetWindowLongPtrW(iIcon, GWL_STYLE) & SS_ICON) != 0);
        CHECK((HICON)::SendMessageW(iIcon, 0x0171, 0, 0) == icon0);
        CHECK(::IsWindowVisible(iIcon));

        dock.Destroy();
        CHECK(dock.PanelCount() == 0);
    }

    mgr.UnloadAll();
    CHECK(mgr.CommandCount() == 0);            // 干净卸载

    ::DestroyWindow(host);

    if (g_fail == 0) { printf("ALL DOCK TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
