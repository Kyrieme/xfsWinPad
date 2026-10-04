// test_npp_dock.cpp — NPP 形态插件：真实对话框 + 可停靠面板注册（4d e2e）。
//
// 用途：验证"真实第三方插件"路径 —— 插件用 CreateDialog 建 modeless 对话框
// 得到 hClient，再向宿主 SendMessage(NPPM_DMMREGASDCKDLG) 注册为可停靠面板。
// 本文件【故意】不 include 宿主的任何头文件：契约结构手写（成员顺序=二进制
// 事实，来源 插件系统设计笔记 §5.1/§5.8），像真实插件作者那样。
//
// 测试进程通过 test_npp_dock_* 导出观察口断言：
//   * 注册是否成功（regResult）
//   * hClient 是否被宿主重挂进 wrapper（父窗口类名 == xfsWinPadPluginDock）
//   * DMN_* 通知是否到达（code/idFrom 逐条记录）
//   * DMN_CLOSE veto 是否被尊重（setVetoClose）
//   * DMM_CLOSE 是否让插件销毁自己的对话框
#include <windows.h>
#include <cstdint>
#include <vector>

// ---- 与宿主镜像的契约结构（顺序即契约，勿改） ------------------------------
// 布局 = 现代 NPP 插件 SDK（PluginInterface.h）：内联名 buffer + func +
// cmdID + initCheck + ShortcutKey*。ShortcutKey 为 {ctrl,alt,shift,key}。
struct NPData { HWND npp; HWND sciMain; HWND sciSecond; };
struct SK { bool ctrl, alt, shift; unsigned char key; };
struct FI {
    wchar_t itemName[64];
    void (*func)(void);
    int cmdID;                 // 宿主回填
    bool initCheck;
    SK* shortcut;
};
// tTbData 二进制契约（NppDocking.h 镜像，字段顺序/宽度不可变）
struct DockedWidgetData {
    HWND hClient;
    const wchar_t* pszName;
    int dlgID;
    UINT uMask;
    HICON hIconTab;
    const wchar_t* pszAddInfo;
    RECT rcFloat;
    int iPrevCont;
    const wchar_t* pszModuleName;
};

// ---- 契约数值（官方：NPPMSG=WM_USER+1000、DMN_FIRST=1050、DMM_MSG=0x5000）---
enum {
    kNPPM_DMMREGASDCKDLG = WM_USER + 1000 + 33,
    kDMN_CLOSE   = 1050 + 1,
    kDMN_DOCK    = 1050 + 2,
    kDMN_SWITCHIN = 1050 + 4,
    kDMM_CLOSE   = 0x5000 + 1,
};

// ---- 插件状态与可观察出口 --------------------------------------------------
static HINSTANCE g_inst = nullptr;         // 自身模块句柄（资源所在模块）
static NPData g_data{};
static HWND g_hwnd = nullptr;
static HWND g_hwndRight = nullptr;   // 批次 122：右列面板
static HWND g_hwndTop = nullptr;     // 批次 122：顶条面板
static BOOL g_regResult = FALSE;
static BOOL g_regCount = 0;
static BOOL g_vetoClose = FALSE;      // 观察口可设：veto DMN_CLOSE
static DWORD g_dmnCodes[64];
static UINT_PTR g_dmnIds[64];
static HWND g_dmnFroms[64];
static int g_dmnCount = 0;
// 批次 119：额外注册的面板（给 e2e 造"请求了非底部容器"与"带附加信息"的面板）。
// 批次 122：槽位扩到 4（0=浮动 1=右列 2=底部+AddInfo 3=顶条）。
static HWND g_extra[4] = { nullptr, nullptr, nullptr, nullptr };

// 真实插件惯例：DllMain 记录自身 HINSTANCE，对话框资源从这里找（不能用
// GetModuleHandleW(nullptr)，那是宿主 exe 的句柄）。
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        g_inst = hinst;
    return TRUE;
}

extern "C" __declspec(dllexport) HWND test_npp_dock_hClient(void) { return g_hwnd; }
extern "C" __declspec(dllexport) BOOL test_npp_dock_regResult(void) { return g_regResult; }
extern "C" __declspec(dllexport) int test_npp_dock_regCount(void) { return g_regCount; }
extern "C" __declspec(dllexport) void test_npp_dock_setVetoClose(BOOL v) { g_vetoClose = v; }
extern "C" __declspec(dllexport) int test_npp_dock_dmnCount(void) { return g_dmnCount; }
extern "C" __declspec(dllexport) DWORD test_npp_dock_dmnCode(int i) {
    return (i >= 0 && i < g_dmnCount) ? g_dmnCodes[i] : 0;
}
extern "C" __declspec(dllexport) UINT_PTR test_npp_dock_dmnId(int i) {
    return (i >= 0 && i < g_dmnCount) ? g_dmnIds[i] : 0;
}
extern "C" __declspec(dllexport) HWND test_npp_dock_dmnFrom(int i) {
    return (i >= 0 && i < g_dmnCount) ? g_dmnFroms[i] : nullptr;
}

// ---- 对话框过程 -------------------------------------------------------------
// WM_NOTIFY 由宿主以 NMHDR{idFrom=dlgID, code=DMN_*} 发来；DMN_CLOSE 可 veto。
// DMM_CLOSE 是宿主直接发来的消息，插件应销毁自己的对话框（NPP 惯例）。
//
// 环境注意：本机 user32 的 DefDlgProcW 存在无限递归（0xC00000FD 栈溢出，
// 已用独立进程二分排查确认）；DefWindowProcW 正常。测试对话框只处理
// WM_NOTIFY(DMN_*)/DMM_CLOSE，其余交给 DefWindowProcW 即可，不涉及
// DMN_*/DMM_* 任何契约，不影响本测试的判定语义。
static INT_PTR CALLBACK DockDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp;
    switch (msg) {
    case WM_NOTIFY: {
        const NMHDR* nm = reinterpret_cast<const NMHDR*>(lp);
        if (nm && g_dmnCount < 64) {
            g_dmnCodes[g_dmnCount] = (DWORD)nm->code;
            g_dmnIds[g_dmnCount] = nm->idFrom;
            g_dmnFroms[g_dmnCount] = nm->hwndFrom;
            ++g_dmnCount;
        }
        fprintf(stderr, "[dockdll] WM_NOTIFY code=%u veto=%d\n",
                nm ? (unsigned)nm->code : 0, (int)g_vetoClose); fflush(stderr);
        if (nm && nm->code == kDMN_CLOSE && g_vetoClose) {
            fprintf(stderr, "[dockdll] VETO DMN_CLOSE\n"); fflush(stderr);
            // 对话框管理器不回传 DlgProc 的返回值：SendMessage(WM_NOTIFY) 的
            // 结果取 DWL_MSGRESULT（缺省 0）。要 veto（让宿主保持面板打开），
            // 必须像真实 NPP 插件那样显式置 DWL_MSGRESULT = TRUE。
            ::SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, (LONG_PTR)TRUE);
            return TRUE;
        }
        return 0;
    }
    case kDMM_CLOSE:
        // 批次 119：额外面板也走同一个 DlgProc，别把主面板的句柄误清掉。
        for (int i = 0; i < 3; ++i)
            if (g_extra[i] == hwnd) g_extra[i] = nullptr;
        if (g_hwnd == hwnd) g_hwnd = nullptr;
        ::DestroyWindow(hwnd);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 资源对话框建 dialog（与真实 NPP 插件一致：.rc 模板 + CreateDialogParamW）
#define IDD_DOCK 101          // 与 test_npp_dock.rc 一致
static HWND CreateDockDialog(HINSTANCE inst) {
    return ::CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_DOCK),
                                nullptr, DockDlgProc, 0);
}

// ---- 额外面板注册（批次 119，给 e2e 用）---------------------------------------
// 不走 FuncItem 命令（那会改 getFuncsArray 的条数与宿主的 CommandCount），直接建
// 对话框 + 走**同一条** NPPM_DMMREGASDCKDLG 注册路径 ⇒ 观察到的就是产品行为。
// mask 由调用方给，因此本文件不需要镜像 DWS_* 常量。
extern "C" __declspec(dllexport) HWND test_npp_dock_extraHwnd(int slot) {
    return (slot >= 0 && slot < 4) ? g_extra[slot] : nullptr;
}

// 两个导出共用同一条注册路径，唯一差别是 hIconTab 是否为空 —— 这样"图标"是
// 唯一自变量，测试红绿就只能归因到它。
static BOOL RegExtraPanel(int slot, UINT mask, const wchar_t* name,
                          const wchar_t* addInfo, BOOL withIcon) {
    if (slot < 0 || slot >= 4) return FALSE;
    if (g_extra[slot] && ::IsWindow(g_extra[slot])) return FALSE;
    HWND h = CreateDockDialog(g_inst);
    if (!h) return FALSE;
    g_extra[slot] = h;

    DockedWidgetData data{};
    data.hClient = h;
    data.pszName = name;
    data.dlgID = 1000 + slot;      // 任意 id；宿主只把它当 DMN_* 的 idFrom
    data.uMask = mask;
    data.pszAddInfo = addInfo;     // 仅当 mask 置了 kDwsAddInfo 才该上屏
    if (withIcon) data.hIconTab = ::LoadIconW(nullptr, IDI_APPLICATION);
    return (BOOL)::SendMessageW(g_data.npp, kNPPM_DMMREGASDCKDLG, 0, (LPARAM)&data);
}

extern "C" __declspec(dllexport) BOOL test_npp_dock_regExtra(int slot, UINT mask,
                                                            const wchar_t* name,
                                                            const wchar_t* addInfo) {
    return RegExtraPanel(slot, mask, name, addInfo, FALSE);
}

// 批次 136：带 hIconTab 的额外面板。regExtra 不设图标 ⇒ 图标在"停靠 ⇄ 浮动"
// 容器转换中是否存活此前无从实测；本导出专供那条路径。
extern "C" __declspec(dllexport) BOOL test_npp_dock_regExtraIcon(int slot, UINT mask,
                                                                const wchar_t* name,
                                                                const wchar_t* addInfo) {
    return RegExtraPanel(slot, mask, name, addInfo, TRUE);
}

// ---- FuncItem 命令：建对话框 + 注册 dock ------------------------------------
static wchar_t wName[] = L"npp-dock-test";
static wchar_t wA[] = L"Dock Panel";
static wchar_t wB[] = L"Side Panels";          // 批次 122：右列 + 顶条
static SK skA{};                               // 无快捷键
static FI items[2];

// 批次 122：注册 右列 + 顶条 两个面板（宿主进程内调用——DockedWidgetData
// 指针不经跨进程 marshal，因此 e2e 必须由插件命令在宿主侧触发）。
static void sidePanels(void) {
    HWND r = CreateDockDialog(g_inst);
    if (!r) return;
    DockedWidgetData rd{};
    rd.hClient = r;
    rd.pszName = L"side-right";
    rd.dlgID = 2001;
    rd.uMask = 0x10000000u;   // DWS_DF_CONT_RIGHT（容器 1 << 28；契约值）
    if (::SendMessageW(g_data.npp, kNPPM_DMMREGASDCKDLG, 0, (LPARAM)&rd))
        g_hwndRight = r;
    HWND t = CreateDockDialog(g_inst);
    if (!t) return;
    DockedWidgetData td{};
    td.hClient = t;
    td.pszName = L"side-top";
    td.dlgID = 2002;
    td.uMask = 0x20000000u;   // DWS_DF_CONT_TOP（容器 2 << 28；契约值）
    if (::SendMessageW(g_data.npp, kNPPM_DMMREGASDCKDLG, 0, (LPARAM)&td))
        g_hwndTop = t;
}

extern "C" __declspec(dllexport) HWND test_npp_dock_rightHwnd(void) { return g_hwndRight; }
extern "C" __declspec(dllexport) HWND test_npp_dock_topHwnd(void) { return g_hwndTop; }

static void dockPanel(void) {
    if (g_hwnd && ::IsWindow(g_hwnd)) {        // 已注册过：重新显示
        ::ShowWindow(g_hwnd, SW_SHOW);
        return;
    }
    fprintf(stderr, "[dockdll] creating dialog\n"); fflush(stderr);
    g_hwnd = CreateDockDialog(g_inst);
    fprintf(stderr, "[dockdll] dialog=%p\n", (void*)g_hwnd); fflush(stderr);
    if (!g_hwnd) return;

    DockedWidgetData data{};
    data.hClient = g_hwnd;
    data.pszName = L"npp-dock";
    data.dlgID = items[0].cmdID;               // 打开本面板的 FuncItem 命令 id
    data.uMask = 0;                            // 默认底部容器
    data.hIconTab = ::LoadIconW(nullptr, IDI_APPLICATION);   // 批次 125：图标上屏
    g_regResult = (BOOL)::SendMessageW(g_data.npp, kNPPM_DMMREGASDCKDLG,
                                       0, (LPARAM)&data);
    ++g_regCount;
}

// ---- NPP 六导出 -------------------------------------------------------------
extern "C" __declspec(dllexport) void setInfo(NPData* d) {
    if (d) g_data = *d;
}
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return wName; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }

extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 2;
    wcscpy_s(items[0].itemName, wA);          // 内联名 buffer（现代 SDK）
    items[0].func = dockPanel;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    wcscpy_s(items[1].itemName, wB);          // 批次 122：右列 + 顶条
    items[1].func = sidePanels;
    items[1].cmdID = 0;
    items[1].initCheck = false;
    items[1].shortcut = nullptr;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void* scn) { (void)scn; }
extern "C" __declspec(dllexport) LRESULT messageProc(UINT m, WPARAM wp, LPARAM lp) {
    (void)m; (void)wp; (void)lp; return 0;
}
