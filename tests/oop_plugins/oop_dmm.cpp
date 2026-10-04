// oop_dmm.cpp — 进程外桥 e2e 用插件：验证 NPPM_DMM* 停靠族**过桥**（批次 117）。
//
// 与其它 oop_* 夹具同规则：不包含宿主头文件，手写公开契约布局。
//
// 与 oop_nppm 的关键差别：本插件不只是"发一条消息"，而是**注册一个真的窗口**。
// 那个窗口住在代理进程里，宿主会把它 SetParent 进自己的 dock 面板 —— 于是
// 这条 e2e 覆盖的是一条**跨进程窗口父子关系**，以及围绕它的双向通知链
// （DMN_* 由代理进程内转发、DMM_* 由代理进程内派发）。
//
// 为什么必须 e2e（而不是只靠形状表/DockManager 单测）：
//   本批的核心主张有两条 —— "跨进程 SetParent 可行"、"跨进程 WM_NOTIFY 被系统
//   拒绝所以必须由代理中继"。两条都是**只有真跑两个进程才能证伪**的性质；
//   单测只能证明"我们想怎么做"，证明不了"系统允许这么做"。
//
// 观察出口：两种标记经 WM_COPYDATA 回传（与 oop_nppm 同格式）。
//   'NPVL' {tag, value}
//   'NPST' {tag, text[128]}（本夹具不用，保留格式以免与 oop_nppm 分叉）
#include <windows.h>
#include <cstring>

struct NPData { HWND npp; HWND sciMain; HWND sciSecond; };
struct NotifyHdr { HWND hwndFrom; UINT_PTR idFrom; unsigned int code; };

// DockedWidgetData（tTbData）布局契约镜像：字段顺序/宽度来自官方接口事实
// （与 src/plugin/npp/NppDocking.h 同源）。写错偏移 ⇒ 宿主重建出的结构体
// 字段全错，而症状是"注册成功但面板名字乱码/查不到"——静默错，所以钉死。
struct DWD {
    HWND           hClient;
    const wchar_t* pszName;
    int            dlgID;
    UINT           uMask;
    HICON          hIconTab;
    const wchar_t* pszAddInfo;
    RECT           rcFloat;
    int            iPrevCont;
    const wchar_t* pszModuleName;
};
static_assert(offsetof(DWD, pszName) == sizeof(void*), "DWD layout: pszName");
static_assert(offsetof(DWD, dlgID) == 2 * sizeof(void*), "DWD layout: dlgID");
static_assert(offsetof(DWD, uMask) == 2 * sizeof(void*) + sizeof(int),
              "DWD layout: uMask");
static_assert(offsetof(DWD, rcFloat) == 4 * sizeof(void*) + 2 * sizeof(int),
              "DWD layout: rcFloat");
static_assert(offsetof(DWD, pszModuleName) ==
                  5 * sizeof(void*) + 2 * sizeof(int) + sizeof(RECT),
              "DWD layout: pszModuleName");

// ---- 线上消息常量（与 src/plugin/npp/NppMessages.h 同源事实值）--------------
// NPPMSG = WM_USER + 1000
#define NPPMSG                    (WM_USER + 1000)
#define NPPM_DMMSHOW              (NPPMSG + 30)
#define NPPM_DMMHIDE              (NPPMSG + 31)
#define NPPM_DMMUPDATEDISPINFO    (NPPMSG + 32)
#define NPPM_DMMREGASDCKDLG       (NPPMSG + 33)
#define NPPM_DMMVIEWOTHERTAB      (NPPMSG + 35)
#define NPPM_DMMGETPLUGINHWNDBYNAME (NPPMSG + 43)

// DMN_* / DMM_*（契约：DMN_FIRST=1050、DMM_MSG=0x5000）
#define DMN_CLOSE                 1051
#define DMN_DOCK                  1052
#define DMN_SWITCHIN              1054
#define DMM_CLOSE                 0x5001
#define DMM_UPDATEDISPINFO        0x5007

static NPData g_data{};

// 面板窗口的观测点（探针读它们回传标记）
static HWND     g_panel = nullptr;
static HWND     g_other = nullptr;   // 从不注册的窗口（信任模型的另一半）
static int      g_notifyCount = 0;
static int      g_lastNotifyCode = 0;
static UINT_PTR g_lastNotifyIdFrom = 0;
// 1 = 通知的 hwndFrom 恰好是插件眼里的"编辑器窗口"（代理中转窗）；0 = 不是；
// -1 = 还没收到过通知。★ 这条是本批的关键判据：NppExec 一类插件只认
//   `hwndFrom == npp` 的 DMN_*，填错了等于"通知到了但插件当它是野消息"。
static int      g_notifyFromIsNpp = -1;
static int      g_dmmCount = 0;
static int      g_lastDmm = 0;
// 是否对 DMN_CLOSE 行使 veto（保持打开）。测试用它验证"返回值真的跨回了宿主"。
static BOOL     g_vetoClose = TRUE;

static constexpr UINT_PTR kValMagic = 0x4E50564C;   // 'NPVL'

static void SendMarker(UINT_PTR tag, INT_PTR value) {
    if (!g_data.npp) return;
    struct { UINT_PTR tag; INT_PTR value; } m{ tag, value };
    COPYDATASTRUCT cds{};
    cds.dwData = kValMagic;
    cds.cbData = sizeof(m);
    cds.lpData = &m;
    ::SendMessageW(g_data.npp, WM_COPYDATA, 0, (LPARAM)&cds);
}

static LRESULT Npp(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_data.npp) return 0;
    return ::SendMessageW(g_data.npp, msg, wp, lp);
}

// 面板窗口过程：只记录 + 对 DMN_CLOSE 表态。真实插件会在这里做自己的事。
static LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NOTIFY: {
        const auto* nm = reinterpret_cast<const NotifyHdr*>(lp);
        if (nm) {
            ++g_notifyCount;
            g_lastNotifyCode = static_cast<int>(nm->code);
            g_lastNotifyIdFrom = nm->idFrom;
            g_notifyFromIsNpp = (nm->hwndFrom == g_data.npp) ? 1 : 0;
            if (static_cast<int>(nm->code) == DMN_CLOSE)
                return g_vetoClose ? TRUE : FALSE;
        }
        return 0;
    }
    case DMM_CLOSE:
    case DMM_UPDATEDISPINFO:
        // 反向动作（编辑器 → 插件）。wp/lp 恒 0（契约见 DmmRelayWire）。
        if (wp != 0 || lp != 0) {
            // 参数不为 0 说明"动作请求"被塞了东西 —— 记一个不可能的哨兵值，
            // 让测试能看见（而不是静默忽略）。
            ++g_dmmCount;
            g_lastDmm = -1;
            return 0;
        }
        ++g_dmmCount;
        g_lastDmm = static_cast<int>(msg);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

// 造一个 16×16 的真图标（32bpp 彩色 + 1bpp 掩码）。批次 137：验证 hIconTab 这个
// **用户对象句柄**跨进程没有意义，但"图标"可以 —— 代理把它转码成图像块过桥，
// 宿主重建本进程的 HICON 并上屏。像素随位置变化，行序错会在宿主侧被像素断言抓到。
static HICON MakePanelIcon(void) {
    const int n = 16;
    HDC dc = ::GetDC(nullptr);
    if (!dc) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = n;
    bi.bmiHeader.biHeight = -n;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* cbits = nullptr;
    HBITMAP color = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &cbits, nullptr, 0);
    struct M { BITMAPINFOHEADER h; RGBQUAD pal[2]; } mb{};
    mb.h.biSize = sizeof(BITMAPINFOHEADER);
    mb.h.biWidth = n;
    mb.h.biHeight = -n;
    mb.h.biPlanes = 1;
    mb.h.biBitCount = 1;
    mb.h.biCompression = BI_RGB;
    void* mbits = nullptr;
    HBITMAP mask = ::CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&mb),
                                      DIB_RGB_COLORS, &mbits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (!color || !mask || !cbits || !mbits) {
        if (color) ::DeleteObject(color);
        if (mask) ::DeleteObject(mask);
        return nullptr;
    }
    DWORD* px = static_cast<DWORD*>(cbits);
    for (int i = 0; i < n * n; ++i)
        px[i] = 0xFF000000u | (DWORD)((i * 37) & 0xFF) * 0x00010101u;
    std::memset(mbits, 0, (((n + 31) / 32) * 4) * n);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = ::CreateIconIndirect(&ii);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return ic;
}

static HWND MakePanel(void) {
    static bool sClass = false;
    if (!sClass) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OopDmmPanel";
        sClass = ::RegisterClassExW(&wc) != 0;
    }
    // 建造成**弹窗**（真实插件 CreateDialog 的形态）：宿主会剥掉 WS_POPUP/
    // WS_CAPTION 再 SetParent —— 这一步跨进程是可行的，正是本批要证的。
    return ::CreateWindowExW(0, L"OopDmmPanel", L"OOP DMM Panel",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             0, 0, 240, 160, nullptr, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

// 标记 tag 分配（测试侧按同一编号断言）：
//   40 面板窗口句柄（值本身）     41 注册返回（BOOL）
//   42 注册后已收到的通知条数     43 最后一条通知的 code
//   44 通知的 hwndFrom == npp？   45 最后一条通知的 idFrom
//   46 DMMSHOW 返回              47 DMMHIDE 返回
//   48 DMMVIEWOTHERTAB 返回      49 隐藏后再切入的通知条数
//   50 那一条的 code             51 GETPLUGINHWNDBYNAME 返回的句柄 == 面板？
//   52 DMMUPDATEDISPINFO 返回    53 已收到的 DMM_* 条数
//   54 最后一条 DMM_* 的 code    55 通知的 hwndFrom 是否恒为 npp（0/1）
//   56 一个**从不注册**的窗口句柄（信任模型的另一半用）
//   57 夹具是否成功造出了注册用的图标（1/0；编码前的自检）
//   99 全部完成
static void probe(void) {
    g_panel = MakePanel();
    // 回传**句柄值本身**：测试侧要拿它做跨进程父子断言（`GetParent` 属不属于
    // 本进程），所以不能只回一个 0/1。
    SendMarker(40, (INT_PTR)g_panel);
    if (!g_panel) { SendMarker(99, 1); return; }

    // 再建一个**永不注册**的窗口：它与 g_panel 同进程、同代理，唯一差别是
    // 没走过 NPPM_DMMREGASDCKDLG ⇒ 宿主侧的白名单里没有它。测试用它验证
    // "陌生跨进程窗口仍被拒" —— 没有这一半，"受信任的代理承载可以 dock"
    // 那条断言在"把判据放宽成任何窗口"时照样全绿。
    g_other = MakePanel();
    SendMarker(56, (INT_PTR)g_other);

    // ① 注册：kDmmReg —— 结构体被**展平**过桥，宿主在自己的地址空间重建。
    //    返回 TRUE 才说明 DockManager 认了这个 hClient（它查的白名单条目
    //    正是注册请求里登记的那一条 ⇒ 登记早于守卫，见 OopHost.cpp 的注释）。
    {
        DWD d{};
        d.hClient = g_panel;
        d.pszName = L"Oop DMM Panel";
        d.dlgID = 7;
        d.uMask = 0x30000000u | 0x4u | 0x1u;  // 底部容器 + kDwsAddInfo + kDwsIconTab
        d.pszAddInfo = L"extra-info-42";
        d.pszModuleName = L"oop_dmm.dll";
        // 批次 137：hIconTab 是**用户对象句柄**（跨进程传值无意义），这里给一个
        // 真图标 —— 代理在**它自己的进程里**把它转码成图像块，宿主重建本进程的
        // HICON 并挂到面板标题条上。测试侧（编辑器进程）断言那个控件真的拿到了
        // 一个非空图标。注册是同步的 ⇒ 返回后本进程销毁原件是安全的（代理已经
        // 把像素读走了，宿主也已收编自己的副本）。
        d.hIconTab = MakePanelIcon();
        SendMarker(57, d.hIconTab ? 1 : 0);   // 编码前的自检（夹具自己没造出来要能看见）
        const LRESULT reg = Npp(NPPM_DMMREGASDCKDLG, 0, (LPARAM)&d);
        SendMarker(41, (INT_PTR)reg);
        if (d.hIconTab) ::DestroyIcon(d.hIconTab);
    }
    // 注册是**同步**的：DMN_DOCK 应在 Npp(...) 返回前就已经到达（中继是
    // SendMessageTimeout 请求/应答式）。所以这里读到的计数是可判定的。
    SendMarker(42, g_notifyCount);
    SendMarker(43, g_lastNotifyCode);
    SendMarker(44, g_notifyFromIsNpp);
    SendMarker(45, (INT_PTR)g_lastNotifyIdFrom);

    // ② 显示 / 隐藏 / 刷新：kValue（lp 是 **HWND 值**，句柄跨进程可传）
    SendMarker(46, (INT_PTR)Npp(NPPM_DMMSHOW, 0, (LPARAM)g_panel));
    SendMarker(47, (INT_PTR)Npp(NPPM_DMMHIDE, 0, (LPARAM)g_panel));
    // ③ 按名切入：kInWideStr。上一步刚隐藏 ⇒ 这一步真的改变可见性 ⇒
    //    宿主会补一条 DMN_SWITCHIN（第二次走通知中继）。
    SendMarker(48, (INT_PTR)Npp(NPPM_DMMVIEWOTHERTAB, 0,
                                (LPARAM)L"Oop DMM Panel"));
    SendMarker(49, g_notifyCount);
    SendMarker(50, g_lastNotifyCode);
    // ④ 按名/模块查句柄：kDmmTwoStr（**唯一**把指针放在 wp 的形状）
    {
        const LRESULT found = Npp(NPPM_DMMGETPLUGINHWNDBYNAME,
                                  (WPARAM)L"Oop DMM Panel",
                                  (LPARAM)L"oop_dmm.dll");
        SendMarker(51, (found == (LRESULT)g_panel) ? 1 : 0);
    }
    // ⑤ 刷新：kValue ⇒ DockManager::UpdateDisplayInfo 回发 DMM_UPDATEDISPINFO
    //    （反向动作中继；与通知是两条不同的路径）
    SendMarker(52, (INT_PTR)Npp(NPPM_DMMUPDATEDISPINFO, 0, (LPARAM)g_panel));
    SendMarker(53, g_dmmCount);
    SendMarker(54, g_lastDmm);
    // 55：所有通知的 hwndFrom 都必须 == npp（只要收到过就检查；没收到过 = -1，
    //     测试侧会先断言收到过，所以这里不会把"没通知"伪装成通过）。
    SendMarker(55, g_notifyCount > 0 ? g_notifyFromIsNpp : -1);

    SendMarker(99, 1);
}

// FuncItem 布局契约镜像（与其它 oop_* 夹具同款；代理按固定步长读它）。
struct FI {
    wchar_t itemName[64];
    void (*func)(void);
    int cmdID;
    bool initCheck;
    void* shortcut;
};
static FI items[1];

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-dmm"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"DMM Bridge Probe");
    items[0].func = probe;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = nullptr;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
