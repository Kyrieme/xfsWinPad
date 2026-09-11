#include "DockManager.h"
#include "../core/Log.h"
#include "../core/Util.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>

namespace xfs {

namespace {
constexpr wchar_t kDockClass[] = L"xfsWinPadPluginDock";
constexpr int kHeaderH = 24;        // 标题条高度（96 dpi 逻辑像素）
constexpr int kCloseId = 2401;      // wrapper 内关闭钮控制 id
constexpr int kLabelId = 2400;      // wrapper 内标题 STATIC 控制 id

int Scale(int v, int dpi) { return ::MulDiv(v, dpi, 96); }

// 把 NMHDR 打头的通知发给插件对话框（NPP 惯例：WM_NOTIFY 到 hClient）。
// hwndFrom 必须填主框架窗口句柄：NppExec 等插件只认
// hwndFrom == npp 主窗口 的 DMN_CLOSE/DMN_DOCK/DMN_FLOAT。
void NotifyDialog(HWND hClient, UINT_PTR idFrom, int code, HWND hwndFrom) {
    if (!hClient || !::IsWindow(hClient)) return;
    NMHDR nm{};
    nm.hwndFrom = hwndFrom;
    nm.idFrom = idFrom;
    nm.code = code;
    ::SendMessageW(hClient, WM_NOTIFY, (WPARAM)idFrom, (LPARAM)&nm);
}
} // namespace

// ---- 每个注册面板的宿主状态 --------------------------------------------------
struct DockManager::Panel {
    DockManager* owner = nullptr;     // 回指宿主（窗口过程转发用）
    npp::DockedWidgetData data;   // 注册时拷贝（pszName/pszModuleName 深拷贝）
    std::wstring name;            // 深拷贝：pszName 指向插件静态区，需复制
    std::wstring moduleName;      // 深拷贝：同上
    std::wstring addInfo;         // 深拷贝：同上（DWS_ADDINFO 时显示）
    HWND wrapper = nullptr;
    HWND label = nullptr;
    HWND closeBtn = nullptr;
    bool visible = false;
    int heightLogical = 220;      // 96 dpi 逻辑像素（与宿主底部面板同口径）
};

void DockManager::Init(HWND mainWnd, HINSTANCE inst) {
    mainWnd_ = mainWnd;
    inst_ = inst;
}

DockManager::DockManager() = default;

DockManager::~DockManager() {
    // Panel 在此处是完整类型（仅布局数据，无资源需释放）；wrapper 由 Destroy 负责
    Destroy();
}

void DockManager::NotifyLayout() {
    if (onLayoutChanged) onLayoutChanged();
}

size_t DockManager::PanelCount() const { return panels_.size(); }
bool DockManager::Empty() const { return panels_.empty(); }

void DockManager::SetVisible(Panel* p, bool show) {
    if (!p || !p->wrapper) return;
    if (p->visible == show) {
        if (show) ::ShowWindow(p->wrapper, SW_SHOW);
        return;
    }
    p->visible = show;
    if (show) {
        ::ShowWindow(p->wrapper, SW_SHOW);
        if (p->data.hClient) ::ShowWindow(p->data.hClient, SW_SHOW);
        NotifyDialog(p->data.hClient, (UINT_PTR)p->data.dlgID, npp::DMN_SWITCHIN,
                     mainWnd_);
    } else {
        ::ShowWindow(p->wrapper, SW_HIDE);
    }
    NotifyLayout();
}

// 用户点关闭：先问插件（DMN_CLOSE，可 veto），不 veto 才隐藏。
// hwndFrom = mainWnd_：NppExec 等插件只处理 npp 主窗口发来的 DMN_CLOSE。
void DockManager::AskClose(Panel* p) {
    if (!p) return;
    HWND h = p->data.hClient;
    if (h && ::IsWindow(h)) {
        NMHDR nm{};
        nm.hwndFrom = mainWnd_;
        nm.idFrom = (UINT_PTR)p->data.dlgID;
        nm.code = npp::DMN_CLOSE;
        const LRESULT r = ::SendMessageW(h, WM_NOTIFY, (WPARAM)p->data.dlgID,
                                         (LPARAM)&nm);
        if (r == TRUE) return;   // 插件 veto：保持打开
    }
    SetVisible(p, false);
}

LRESULT CALLBACK DockManager::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<Panel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        p = reinterpret_cast<Panel*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
    }
    // 面板对象由 DockManager 持有；经 owner 转发到成员 WndProc。
    return (p && p->owner)
               ? p->owner->WndProc(hwnd, msg, wp, lp, p)
               : ::DefWindowProcW(hwnd, msg, wp, lp);
}

// wrapper 窗口过程：标题条里放标题 + 关闭钮；客户端区整块给插件对话框。
// 只处理尺寸布局与关闭钮；其余交给 DefWindowProc。
LRESULT DockManager::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, Panel* p) {
    switch (msg) {
    case WM_SIZE: {
        const int w = GET_X_LPARAM(lp), h = GET_Y_LPARAM(lp);
        const int dpi = ::GetDpiForWindow(hwnd);
        const int hh = Scale(kHeaderH, dpi);
        if (p->label)
            ::MoveWindow(p->label, Scale(8, dpi), 3, std::max(0, w - Scale(60, dpi)), hh - 6, TRUE);
        if (p->closeBtn)
            ::MoveWindow(p->closeBtn, std::max(0, w - Scale(52, dpi)), 1, Scale(48, dpi), hh - 2, TRUE);
        if (p->data.hClient && ::IsWindow(p->data.hClient))
            ::MoveWindow(p->data.hClient, 0, hh, w, std::max(0, h - hh), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kCloseId) {
            // 从 wrapper 反查面板（thunk 里已把 Panel* 放 USERDATA）
            AskClose(p);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;   // 用类背景刷（COLOR_BTNFACE），避免闪烁
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

DockManager::Panel* DockManager::FindByHwnd(HWND hDlg) {
    for (auto& p : panels_)
        if (p->data.hClient == hDlg) return p.get();
    return nullptr;
}

DockManager::Panel* DockManager::FindByName(const wchar_t* name) {
    if (!name) return nullptr;
    for (auto& p : panels_)
        if (p->name == name) return p.get();
    return nullptr;
}

bool DockManager::DockWidget(const npp::DockedWidgetData& data) {
    if (!mainWnd_ || !inst_) return false;
    if (!data.hClient || !::IsWindow(data.hClient)) {
        Logger::Error("DockManager: DockWidget 收到无效 hClient");
        return false;
    }
    // 跨进程守卫：hClient 属于其他进程（进程外插件代理）时拒绝 dock ——
    // v1 代理进程无宿主窗口承载（SetParent 跨进程被系统禁止）。
    // 进程外插件的 DMM 注册请求不该走到这里（NppCompat 未桥接 DMM*），
    // 此处是最后防线，防未来桥接时误把代理窗口挂进本进程。
    DWORD clientPid = 0;
    ::GetWindowThreadProcessId(data.hClient, &clientPid);
    if (clientPid != ::GetCurrentProcessId()) {
        Logger::Info("DockManager: 拒绝跨进程 hClient（进程外插件无停靠，v1）");
        return false;
    }
    // 幂等：同一对话框重复注册 = 更新元数据
    if (Panel* exist = FindByHwnd(data.hClient)) {
        exist->data.dlgID = data.dlgID;
        exist->data.uMask = data.uMask;
        exist->data.hIconTab = data.hIconTab;
        exist->data.pszAddInfo = data.pszAddInfo;
        if (data.pszName) exist->name = data.pszName;
        if (data.pszModuleName) exist->moduleName = data.pszModuleName;
        if (exist->label) ::SetWindowTextW(exist->label, exist->name.c_str());
        return true;
    }

    // 只实现底部容器：DWS_DF_FLOATING/左右上按底部处理（兼容报告口径）
    if (data.uMask & npp::kDwsDfFloating)
        Logger::Info("DockManager: 插件要求浮动，按底部 dock 处理 '" +
                     WideToUtf8(data.pszName ? data.pszName : L"") + "'");

    auto panel = std::make_unique<Panel>();
    panel->owner = this;
    panel->data = data;                       // 结构整体拷贝
    if (data.pszName) panel->name = data.pszName;
    if (data.pszModuleName) panel->moduleName = data.pszModuleName;
    if (data.pszAddInfo) panel->addInfo = data.pszAddInfo;
    panel->data.pszName = panel->name.c_str();      // 指针改指深拷贝
    panel->data.pszModuleName = panel->moduleName.c_str();
    panel->data.pszAddInfo = panel->addInfo.c_str();

    // 惰性注册 wrapper 类（跨多个面板共享）
    static bool sClassReady = false;
    if (!sClassReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = inst_;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDockClass;
        sClassReady = ::RegisterClassExW(&wc) ||
                      ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        if (!sClassReady) return false;
    }

    const int dpi = ::GetDpiForWindow(mainWnd_);
    panel->wrapper = ::CreateWindowExW(
        0, kDockClass, nullptr,
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        0, 0, Scale(600, dpi), Scale(panel->heightLogical, dpi),
        mainWnd_, nullptr, inst_, panel.get());
    if (!panel->wrapper) return false;
    ::SetWindowLongPtrW(panel->wrapper, GWLP_USERDATA, (LONG_PTR)panel.get());

    const int hh = Scale(kHeaderH, dpi);
    panel->label = ::CreateWindowExW(0, L"STATIC", panel->name.c_str(),
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                     Scale(8, dpi), 3, Scale(520, dpi), hh - 6,
                                     panel->wrapper, (HMENU)(INT_PTR)kLabelId,
                                     inst_, nullptr);
    panel->closeBtn = ::CreateWindowExW(0, L"BUTTON", L"✕",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        0, 1, Scale(48, dpi), hh - 2,
                                        panel->wrapper, (HMENU)(INT_PTR)kCloseId,
                                        inst_, nullptr);

    // 把插件对话框重挂进 wrapper：清弹窗/边框风格、加子窗口位。
    LONG_PTR style = ::GetWindowLongPtrW(data.hClient, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    style |= WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE;
    ::SetWindowLongPtrW(data.hClient, GWL_STYLE, style);
    ::SetParent(data.hClient, panel->wrapper);
    ::SetWindowPos(data.hClient, nullptr, 0, hh, Scale(600, dpi),
                   Scale(panel->heightLogical, dpi) - hh,
                   SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    panel->visible = true;
    panels_.push_back(std::move(panel));

    NotifyDialog(data.hClient, (UINT_PTR)data.dlgID, npp::DMN_DOCK, mainWnd_);
    Logger::Info("DockManager: docked plugin dialog '" +
                 WideToUtf8(data.pszName ? data.pszName : L"") + "'");
    NotifyLayout();
    return true;
}

bool DockManager::Show(HWND hDlg) {
    Panel* p = FindByHwnd(hDlg);
    if (!p) return false;
    SetVisible(p, true);
    return true;
}

bool DockManager::Hide(HWND hDlg) {
    Panel* p = FindByHwnd(hDlg);
    if (!p) return false;
    SetVisible(p, false);
    return true;
}

void DockManager::UpdateDisplayInfo(HWND hDlg) {
    Panel* p = FindByHwnd(hDlg);
    if (!p || !p->wrapper) return;
    ::InvalidateRect(p->wrapper, nullptr, TRUE);
    if (p->data.hClient && ::IsWindow(p->data.hClient))
        ::SendMessageW(p->data.hClient, npp::DMM_UPDATEDISPINFO, 0, 0);
}

bool DockManager::ShowByName(const wchar_t* name) {
    Panel* p = FindByName(name);
    if (!p) return false;
    SetVisible(p, true);
    return true;
}

HWND DockManager::FindHwndByName(const wchar_t* windowName,
                                 const wchar_t* moduleName) {
    for (auto& p : panels_) {
        if (windowName && p->name != windowName) continue;
        if (moduleName && p->moduleName != moduleName) continue;
        return p->data.hClient;
    }
    return nullptr;
}

int DockManager::TotalHeight(int dpi) const {
    int h = 0;
    for (auto& p : panels_)
        if (p->visible) h += Scale(p->heightLogical, dpi);
    return h;
}

int DockManager::Layout(int x, int y, int width, int dpi) {
    for (auto& p : panels_) {
        if (!p->visible) continue;
        const int h = Scale(p->heightLogical, dpi);
        ::MoveWindow(p->wrapper, x, y, width, h, TRUE);
        y += h;
    }
    return y;
}

void DockManager::Destroy() {
    for (auto& p : panels_) {
        if (p->data.hClient && ::IsWindow(p->data.hClient))
            ::SendMessageW(p->data.hClient, npp::DMM_CLOSE, 0, 0);
        if (p->wrapper) ::DestroyWindow(p->wrapper);
        p->wrapper = nullptr;
        p->label = nullptr;
        p->closeBtn = nullptr;
    }
    panels_.clear();
}

} // namespace xfs
