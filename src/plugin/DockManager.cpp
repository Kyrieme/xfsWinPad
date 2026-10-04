#include "DockManager.h"
#include "npp/NppDockMask.h"          // 批次 119：uMask 解码 + 标题口径（纯函数）
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
// 批次 127：拖动阈值 / 拖出容差（均 96 dpi 逻辑像素，调用方按 dpi 折算）
constexpr int kDragThreshold = 4;   // 手抖容忍：位移不足此值不算拖动
constexpr int kDragOutSlack = 4;    // 拖出容差：光标须离开槽位这么多像素

int Scale(int v, int dpi) { return ::MulDiv(v, dpi, 96); }


} // namespace

// 能不能把这个 hClient 当成停靠客户端。
// 判据有两条，差别正是批次 117 的核心：
//   * 本进程窗口 —— 进程内插件，一切照旧（批次 117 之前只有这一条）。
//   * **受信任的代理承载窗口** —— 进程外插件的对话框。判据由 remote_ 提供，
//     它查的是"DMM 注册时登记过的 hClient"（注册请求只能来自本编辑器自己
//     启动的代理进程，且代理侧已验过 hClient 是它自己的窗口）。
// ⚠ 仍然拒绝陌生跨进程窗口：dock 那三步会向目标线程发消息，插件线程不泵就
//   把编辑器 UI 线程拖死；而且通知链也没法送到（见上面那段实测结论）。
bool DockManager::IsAcceptableClient(HWND hClient) const {
    if (!hClient || !::IsWindow(hClient)) return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hClient, &pid);
    if (pid == ::GetCurrentProcessId()) return true;
    return remote_ && remote_->IsTrustedClient(hClient);
}

LRESULT DockManager::SendNotifyToClient(HWND hClient, UINT_PTR idFrom, int code) {
    if (!hClient || !::IsWindow(hClient)) return 0;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hClient, &pid);
    if (pid != ::GetCurrentProcessId()) {
        // 进程外：由代理在它自己的进程里发（编辑器直发会被系统拒）。
        // 未登记（陌生窗口）⇒ 什么都不发，返回 0 = "未 veto"。
        if (remote_ && remote_->IsTrustedClient(hClient))
            return remote_->SendNotify(hClient, idFrom, code);
        return 0;
    }
    // 同进程：NPP 惯例是 WM_NOTIFY 到 hClient。
    // hwndFrom 必须填主框架窗口句柄：NppExec 等插件只认
    // hwndFrom == npp 主窗口 的 DMN_CLOSE/DMN_DOCK/DMN_FLOAT。
    NMHDR nm{};
    nm.hwndFrom = mainWnd_;
    nm.idFrom = idFrom;
    nm.code = code;
    return ::SendMessageW(hClient, WM_NOTIFY, (WPARAM)idFrom, (LPARAM)&nm);
}

void DockManager::SendActionToClient(HWND hClient, UINT action) {
    if (!hClient || !::IsWindow(hClient)) return;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hClient, &pid);
    if (pid != ::GetCurrentProcessId()) {
        if (remote_ && remote_->IsTrustedClient(hClient))
            remote_->SendAction(hClient, action);
        return;
    }
    ::SendMessageW(hClient, action, 0, 0);
}

// ---- 每个注册面板的宿主状态 --------------------------------------------------
struct DockManager::Panel {
    DockManager* owner = nullptr;     // 回指宿主（窗口过程转发用）
    npp::DockedWidgetData data;   // 注册时拷贝（pszName/pszModuleName 深拷贝）
    std::wstring name;            // 深拷贝：pszName 指向插件静态区，需复制
    std::wstring moduleName;      // 深拷贝：同上
    std::wstring addInfo;         // 深拷贝：同上（DWS_ADDINFO 时显示）
    HWND wrapper = nullptr;
    HWND label = nullptr;
    HWND iconCtrl = nullptr;      // 批次 125：kDwsIconTab 图标（SS_ICON）
    HWND closeBtn = nullptr;
    HICON icon = nullptr;         // 我们的副本（CopyIcon；插件可随时销毁原件）
    bool iconOwned = false;       // true = 析构时须 DestroyIcon
    bool visible = false;
    int heightLogical = 220;      // 96 dpi 逻辑像素（与宿主底部面板同口径）
    int widthLogical = 260;       // 右容器列宽（96 dpi 逻辑像素）
    npp::DockRequest container = npp::DockRequest::Bottom;  // 注册时定档（浮动降级）
    // 批次 126：浮动面板标题条拖拽（移动 + 拖回宿主重停靠）
    bool dragging = false;        // 标题条按住（已 SetCapture）
    bool dragMoved = false;       // 超过阈值，进入移动
    int grabX = 0, grabY = 0;     // 按下光标相对 wrapper 左上角的偏移
    int downX = 0, downY = 0;     // 按下光标屏幕位（拖动阈值基准）
    npp::DockRequest snapTarget = npp::DockRequest::Floating;  // 拖动中的吸附预判
    WNDPROC labelOrig = nullptr;  // 标签原子类过程（浮动面板子类化时保存）
};

// 通知通道（SendNotifyToClient / SendActionToClient）的说明见 DockManager.h。
// ★ 这里把批次 117 的实测结论钉在代码旁边（它是"为什么要有 remote_"的判据）：
//   · 跨进程 `WM_NOTIFY` 被系统拒绝（err=5 ACCESS_DENIED，`lParam=0` 照样拒
//     ⇒ 与指针无关，也不是 UIPI）——而 DMN_* 全走 WM_NOTIFY；
//   · 直发 `DMM_*`（`WM_USER` 段）能送达，但会让**编辑器 UI 线程**去等一个
//     可能不泵消息的插件线程（挂起风险）。
// 批次 126：面板标题条拖拽——浮动态：移动窗口 + 光标落进主框架吸附带内松手
// 即重停靠（判据 = npp::SnapEdgeTo 纯函数，DockManager.h 声明静态成员）。
// 批次 127：停靠态**同样可拖**——光标离开面板所占槽位即转浮动（判据 =
// npp::DragOutOfSlot 纯函数），转换后本次拖动余下的行程由浮动态路径接管；
// 因此"拖出 → 拖回边缘松手"构成闭环，两条路径共用同一套抓取偏移。
LRESULT CALLBACK DockManager::LabelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<Panel*>(
        ::GetWindowLongPtrW(::GetParent(h), GWLP_USERDATA));
    auto* orig = reinterpret_cast<WNDPROC>(
        ::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p) {
        // 注意：本值在 FloatOut 之后即过期；FloatOut 分支一律 break 出去，
        // 下一次 WM_MOUSEMOVE 会重新取值（届时已是浮动）。
        const bool floating = (p->container == npp::DockRequest::Floating);
        switch (msg) {
        case WM_LBUTTONDOWN: {
            ::SetCapture(h);
            p->dragging = true;
            p->dragMoved = false;
            RECT wr{}; ::GetWindowRect(p->wrapper, &wr);
            RECT lr{}; ::GetWindowRect(h, &lr);
            const POINT scr{ lr.left + (short)LOWORD(lp),
                             lr.top + (short)HIWORD(lp) };
            p->grabX = scr.x - wr.left;   // 按下光标相对 wrapper 左上角的偏移
            p->grabY = scr.y - wr.top;
            p->downX = scr.x;             // 拖动阈值基准（屏幕坐标）
            p->downY = scr.y;
            break;
        }
        case WM_MOUSEMOVE: {
            if (!p->dragging) break;
            POINT scr{ (short)LOWORD(lp), (short)HIWORD(lp) };
            ::MapWindowPoints(h, nullptr, &scr, 1);
            const int dpi = ::GetDpiForWindow(h);
            if (!p->dragMoved) {
                if (abs(scr.x - p->downX) + abs(scr.y - p->downY) <
                    Scale(kDragThreshold, dpi)) break;
                p->dragMoved = true;
            }
            if (!floating) {
                // 停靠态：wrapper 不跟随光标（仍占着容器槽位），故槽位矩形
                // 在整个拖动过程中稳定 —— 光标离开它即判为拖出。
                RECT wr{}; ::GetWindowRect(p->wrapper, &wr);
                if (!npp::DragOutOfSlot(wr, scr, Scale(kDragOutSlack, dpi)))
                    break;
                p->owner->FloatOut(p, scr);
                break;
            }
            ::SetWindowPos(p->wrapper, nullptr, scr.x - p->grabX, scr.y - p->grabY,
                           0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT fr{}; ::GetWindowRect(p->owner->mainWnd_, &fr);
            npp::DockRequest t;
            p->snapTarget =
                npp::SnapEdgeTo(fr, scr,
                                ::MulDiv(48, dpi, 96), &t)
                    ? t : npp::DockRequest::Floating;
            break;
        }
        case WM_LBUTTONUP: {
            if (!p->dragging) break;
            ::ReleaseCapture();
            p->dragging = false;
            if (p->dragMoved && p->snapTarget != npp::DockRequest::Floating)
                p->owner->FloatToDock(p, p->snapTarget);
            break;
        }
        case WM_CAPTURECHANGED:
            p->dragging = false;
            break;
        }
    }
    return orig ? ::CallWindowProcW(orig, h, msg, wp, lp)
                : ::DefWindowProcW(h, msg, wp, lp);
}

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
        SendNotifyToClient(p->data.hClient, (UINT_PTR)p->data.dlgID,
                           npp::DMN_SWITCHIN);
    } else {
        ::ShowWindow(p->wrapper, SW_HIDE);
    }
    NotifyLayout();
}

// 用户点关闭：先问插件（DMN_CLOSE，可 veto），不 veto 才隐藏。
// hwndFrom = mainWnd_（同进程）/ 代理中转窗（进程外）：NppExec 等插件只处理
// npp 主窗口发来的 DMN_CLOSE，所以进程外那条必须由代理把 hwndFrom 填成
// **插件眼里的编辑器窗口**（= 代理中转窗），见 PluginHostMain.cpp。
void DockManager::AskClose(Panel* p) {
    if (!p) return;
    HWND h = p->data.hClient;
    if (h && ::IsWindow(h)) {
        const LRESULT r = SendNotifyToClient(h, (UINT_PTR)p->data.dlgID,
                                             npp::DMN_CLOSE);
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
        // 批次 138：图标控件也要重排。此前它只在创建时定位一次，而它的边长
        // （hh - 6）与偏移都是 dpi 派生的 —— 跨屏换 dpi 后图标会保持旧尺寸，
        // 而标签已经按新 hh 让过位，两者会错位。位置只与 hh 有关，所以普通
        // 改宽度的重排下这次 MoveWindow 是幂等的。
        if (p->iconCtrl) {
            const int isz = hh - 6;
            ::MoveWindow(p->iconCtrl, Scale(4, dpi), 3, isz, isz, TRUE);
        }
        // 标签 x 随图标控件让位（与创建路径同一口径）
        int lx = Scale(8, dpi);
        if (p->iconCtrl) lx = Scale(4, dpi) + (hh - 6) + Scale(4, dpi);
        if (p->label)
            ::MoveWindow(p->label, lx, 3, std::max(0, w - lx - Scale(60, dpi)), hh - 6, TRUE);
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

// 批次 138b：跨屏换 dpi 后让每个 wrapper 按新 dpi 重排标题条。标题条高、
// 图标边长、标签/关闭钮的位置与宽度全是 Scale(..., dpi) 折出来的，同样一次
// 成型；而尺寸没变时 MoveWindow 不会自发 WM_SIZE，所以这里主动补一条**同样
// 尺寸**的 WM_SIZE，把上面那条既有重排路径原样跑一遍（dpi 由它自己现取）。
void DockManager::OnDpiChanged(int dpi) {
    (void)dpi;
    for (auto& p : panels_) {
        if (!p->wrapper || !::IsWindow(p->wrapper)) continue;
        RECT rc{};
        ::GetClientRect(p->wrapper, &rc);
        ::SendMessageW(p->wrapper, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    }
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
    // 跨进程守卫：hClient 属于其他进程时，**只有**受信任的代理承载才放行。
    //
    // ★ 理由**不是**"SetParent 跨进程做不到" —— 本文件原来就是这么写的，批次 117
    //   的两进程实测证明那是**假的**：两侧都泵消息时正向/反向 `SetParent` 都成功
    //   （err=0、`GetAncestor(GA_PARENT)` 真的变了、窗口存活），把下面这三步
    //   （`GWL_STYLE` → `SetParent` → `SetWindowPos`）原样搬到外来窗口上逐步 err 全 0。
    //   真正的理由是**挂起风险**：这三步里 `SetWindowLongPtr(GWL_STYLE)` 与
    //   `SetWindowPos` **会向目标窗口的线程发消息**；插件线程若不泵消息，编辑器
    //   UI 线程就被拖死（批次 117 的探针两次都栽在这上面：对端不泵 ⇒ 调用方卡住，
    //   还被误判成"窗口被销毁 + err 87"）。
    //   还有一条独立理由：通知链（`DMN_*`/`DMM_*`）跨进程走不通 —— `WM_NOTIFY`
    //   被系统拒绝（err=5 ACCESS_DENIED，且 `lParam=0` 照样拒 ⇒ 与指针无关），
    //   必须由代理侧中继（v2.8 已实现，见 SendNotifyToClient）。
    // ⇒ 判据落在"**是不是我们自己代理承载的窗口**"上（IsAcceptableClient），
    //   而不是"是不是本进程"。
    if (!IsAcceptableClient(data.hClient)) {
        Logger::Info("DockManager: 拒绝 hClient（既非本进程窗口，也不是受信任代理承载）");
        return false;
    }
    // 幂等：同一对话框重复注册 = 更新元数据
    if (Panel* exist = FindByHwnd(data.hClient)) {
        const bool wasCoerced = npp::IsDockRequestCoerced(exist->data.uMask);
        exist->data.dlgID = data.dlgID;
        exist->data.uMask = data.uMask;
        exist->data.hIconTab = data.hIconTab;
        if (data.pszName) exist->name = data.pszName;
        if (data.pszModuleName) exist->moduleName = data.pszModuleName;
        // ★ 附加信息保持指向**我们的深拷贝**（首次注册时就是这样）。旧代码这里写
        //   `exist->data.pszAddInfo = data.pszAddInfo`，即指回插件自己的内存 ——
        //   插件释放那块串之后就是悬垂指针，而且与首次注册那条路径口径不一致。
        if (data.pszAddInfo) exist->addInfo = data.pszAddInfo;
        exist->data.pszAddInfo = exist->addInfo.c_str();
        // 批次 125：幂等更新带上图标——有控件则换图（收编副本，销毁旧副本）
        if (exist->iconCtrl) {
            if (exist->iconOwned && exist->icon)
                ::DestroyIcon(exist->icon);
            exist->icon = data.hIconTab ? ::CopyIcon(data.hIconTab) : nullptr;
            exist->iconOwned = exist->icon != nullptr;
            if (!exist->icon && data.hIconTab) {
                exist->icon = data.hIconTab;   // 复制失败：直用原件
                exist->iconOwned = false;
            }
            ::SendMessageW(exist->iconCtrl, STM_SETICON,
                           (WPARAM)exist->icon, 0);
        }
        if (exist->label)
            ::SetWindowTextW(exist->label,
                             npp::DockPanelTitle(exist->name.c_str(),
                                                 exist->addInfo.c_str(),
                                                 exist->data.uMask).c_str());
        // 账本随"降级状态"变化增减——批次 123 起谓词恒假，此段成为空转的
        // 历史机制（保留：一旦未来出现新降级场景，机制即刻生效）。
        const bool nowCoerced = npp::IsDockRequestCoerced(exist->data.uMask);
        if (nowCoerced != wasCoerced) coercedContainers_ += nowCoerced ? 1 : -1;
        return true;
    }

    auto panel = std::make_unique<Panel>();
    panel->owner = this;
    panel->data = data;                       // 结构整体拷贝
    panel->container = npp::RequestedDock(data.uMask);
    if (panel->container == npp::DockRequest::Left)
        panel->container = npp::DockRequest::Bottom;   // 编码重合（非降级）
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
    // 批次 123：浮动容器 = 独立的**有主顶级窗口**（owner = 主框架）——
    // 不参与 LayoutChildren 几何；位置取上游 rcFloat（屏幕坐标，上游契约），
    // 无效则级联缺省。docked 容器照旧 WS_CHILD。
    const bool floating = (panel->container == npp::DockRequest::Floating);
    DWORD wStyle = WS_CLIPSIBLINGS | WS_VISIBLE;
    int px = 0, py = 0, pw = Scale(600, dpi), ph = Scale(panel->heightLogical, dpi);
    if (floating) {
        wStyle |= WS_POPUPWINDOW | WS_THICKFRAME;
        const RECT& rf = data.rcFloat;
        if (rf.right > rf.left && rf.bottom > rf.top) {
            px = rf.left; py = rf.top;
            pw = rf.right - rf.left; ph = rf.bottom - rf.top;
        } else {
            const int n = (int)panels_.size() % 8;
            px = Scale(120 + 40 * n, dpi);
            py = Scale(120 + 30 * n, dpi);
            pw = Scale(400, dpi); ph = Scale(300, dpi);
        }
    } else {
        wStyle |= WS_CHILD;
    }
    // 有主顶级窗口：CreateWindowEx 的 hwndParent 参数对顶级窗口即 owner
    panel->wrapper = ::CreateWindowExW(
        0, kDockClass, nullptr,
        wStyle, px, py, pw, ph,
        mainWnd_, nullptr, inst_, panel.get());
    if (!panel->wrapper) return false;
    ::SetWindowLongPtrW(panel->wrapper, GWLP_USERDATA, (LONG_PTR)panel.get());

    const int hh = Scale(kHeaderH, dpi);
    // 标题条文本：kDwsAddInfo 置位且附加信息非空时把附加信息接在后面。
    // 口径在 npp/NppDockMask.h 的 DockPanelTitle（纯函数、有单测）——
    // 在此之前 pszAddInfo 只被深拷贝、从未上屏（与 Panel::addInfo 上的注释不符）。
    const std::wstring title = npp::DockPanelTitle(panel->name.c_str(),
                                                   panel->addInfo.c_str(),
                                                   panel->data.uMask);
    // 批次 125：kDwsIconTab 带图标 ⇒ CopyIcon 收编副本（插件可随时销毁原件）
    // 并在标题行最左建 SS_ICON 静态控件上屏；标签右移让位。
    int labelX = Scale(8, dpi);
    if (data.hIconTab) {
        panel->icon = ::CopyIcon(data.hIconTab);
        panel->iconOwned = panel->icon != nullptr;
        if (!panel->icon) panel->icon = data.hIconTab;   // 复制失败：直用原件
        const int isz = hh - 6;
        panel->iconCtrl = ::CreateWindowExW(0, L"STATIC", nullptr,
                                            WS_CHILD | WS_VISIBLE | SS_ICON,
                                            Scale(4, dpi), 3, isz, isz,
                                            panel->wrapper,
                                            (HMENU)(INT_PTR)(kLabelId - 1),
                                            inst_, nullptr);
        if (panel->iconCtrl)
            ::SendMessageW(panel->iconCtrl, STM_SETICON,
                           (WPARAM)panel->icon, 0);
        labelX = Scale(4, dpi) + isz + Scale(4, dpi);
    }
    panel->label = ::CreateWindowExW(0, L"STATIC", title.c_str(),
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                     labelX, 3, Scale(520, dpi), hh - 6,
                                     panel->wrapper, (HMENU)(INT_PTR)kLabelId,
                                     inst_, nullptr);
    // 批次 126/127：标题条子类化——浮动态：拖动移动 + 拖回重停靠；
    // 停靠态：拖出槽位转浮动。两个方向共用同一套过程，故所有面板一律装上
    // （批次 126 只给浮动面板装，拖回后即失去拖拽能力）。
    if (panel->label) {
        panel->labelOrig = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(panel->label, GWLP_WNDPROC,
                                (LONG_PTR)DockManager::LabelProc));
        ::SetWindowLongPtrW(panel->label, GWLP_USERDATA,
                            (LONG_PTR)panel->labelOrig);
    }
    panel->closeBtn = ::CreateWindowExW(0, L"BUTTON", L"✕",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        0, 1, Scale(48, dpi), hh - 2,
                                        panel->wrapper, (HMENU)(INT_PTR)kCloseId,
                                        inst_, nullptr);

    // 把插件对话框重挂进 wrapper：清弹窗/边框风格、加子窗口位。
    // 浮动容器：client 尺寸跟随 wrapper 客户区（THICKFRAME 可调大小）。
    LONG_PTR style = ::GetWindowLongPtrW(data.hClient, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    style |= WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE;
    ::SetWindowLongPtrW(data.hClient, GWL_STYLE, style);
    ::SetParent(data.hClient, panel->wrapper);
    RECT wrc{};
    ::GetClientRect(panel->wrapper, &wrc);
    ::SetWindowPos(data.hClient, nullptr, 0, hh, wrc.right,
                   wrc.bottom - hh,
                   SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    panel->visible = true;
    panels_.push_back(std::move(panel));

    SendNotifyToClient(data.hClient, (UINT_PTR)data.dlgID, npp::DMN_DOCK);
    if (floating) {
        // 上游语义：面板进浮动容器 → DMN_FLOAT
        SendNotifyToClient(data.hClient, (UINT_PTR)data.dlgID, npp::DMN_FLOAT);
        Logger::Info("DockManager: floating plugin dialog '" +
                     WideToUtf8(data.pszName ? data.pszName : L"") + "'");
    } else {
        Logger::Info("DockManager: docked plugin dialog '" +
                     WideToUtf8(data.pszName ? data.pszName : L"") + "'");
    }
    if (!floating) NotifyLayout();   // 浮动面板不参与宿主布局几何
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
    // 插件发 NPPM_DMMUPDATEDISPINFO = "我的显示信息变了，刷新一下" ⇒ 标题条按当前
    // 名字/附加信息重算（口径与注册路径同一个纯函数）。
    if (p->label)
        ::SetWindowTextW(p->label,
                         npp::DockPanelTitle(p->name.c_str(), p->addInfo.c_str(),
                                             p->data.uMask).c_str());
    ::InvalidateRect(p->wrapper, nullptr, TRUE);
    if (p->data.hClient && ::IsWindow(p->data.hClient))
        SendActionToClient(p->data.hClient, npp::DMM_UPDATEDISPINFO);
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
        if (p->visible && p->container == npp::DockRequest::Bottom)
            h += Scale(p->heightLogical, dpi);
    return h;
}

int DockManager::Layout(int x, int y, int width, int dpi) {
    for (auto& p : panels_) {
        if (!p->visible || p->container != npp::DockRequest::Bottom) continue;
        const int h = Scale(p->heightLogical, dpi);
        ::MoveWindow(p->wrapper, x, y, width, h, TRUE);
        y += h;
    }
    return y;
}

// ---- 右列 / 顶条（批次 122：真实侧容器）--------------------------------------

int DockManager::TotalWidth(int dpi) const {
    int w = 0;
    for (auto& p : panels_) {
        if (!p->visible || p->container != npp::DockRequest::Right) continue;
        w = (std::max)(w, Scale(p->widthLogical, dpi));
    }
    return w;
}

int DockManager::LayoutRight(int x, int y, int height, int dpi) {
    const int w = TotalWidth(dpi);
    if (w == 0) return y;
    for (auto& p : panels_) {
        if (!p->visible || p->container != npp::DockRequest::Right) continue;
        const int h = Scale(p->heightLogical, dpi);
        ::MoveWindow(p->wrapper, x, y, w, h, TRUE);
        y += h;
    }
    (void)height;   // 列高由调用方的几何约束（MoveWindow 受父窗口裁剪）兜底
    return y;
}

int DockManager::TopTotalHeight(int dpi) const {
    int h = 0;
    for (auto& p : panels_)
        if (p->visible && p->container == npp::DockRequest::Top)
            h += Scale(p->heightLogical, dpi);
    return h;
}

int DockManager::LayoutTop(int x, int y, int width, int dpi) {
    for (auto& p : panels_) {
        if (!p->visible || p->container != npp::DockRequest::Top) continue;
        const int h = Scale(p->heightLogical, dpi);
        ::MoveWindow(p->wrapper, x, y, width, h, TRUE);
        y += h;
    }
    return y;
}

// 批次 126：把浮动面板转为指定停靠容器（拖回吸附的落点执行）。
// wrapper 窗口**原地转换**：弹窗样式 → 子窗口样式，重挂回主框架——
// 图标/标题/关闭钮子控件全部存活，Layout 随后按新容器布置。
// 批次 127：不再解掉标题条子类化 —— 停靠态也要能拖（拖出），
// 子类化在 wrapper 创建时一次性装上，两个方向共用。
void DockManager::FloatToDock(Panel* p, npp::DockRequest target) {
    if (!p || p->container != npp::DockRequest::Floating) return;
    if (target == npp::DockRequest::Floating ||
        target == npp::DockRequest::Left)
        target = npp::DockRequest::Bottom;   // 无浮动目标 / 编码重合
    p->container = target;
    LONG_PTR st = ::GetWindowLongPtrW(p->wrapper, GWL_STYLE);
    st &= ~(WS_POPUP | WS_POPUPWINDOW | WS_THICKFRAME | WS_DLGFRAME | WS_SYSMENU);
    st |= WS_CHILD | WS_CLIPSIBLINGS;
    ::SetWindowLongPtrW(p->wrapper, GWL_STYLE, st);
    ::SetParent(p->wrapper, mainWnd_);
    ::SetWindowPos(p->wrapper, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SendNotifyToClient(p->data.hClient, (UINT_PTR)p->data.dlgID, npp::DMN_DOCK);
    Logger::Info("DockManager: floating panel '" +
                 WideToUtf8(p->name.c_str()) + "' re-docked to " +
                 npp::DockRequestName(target));
    NotifyLayout();
}

// 批次 127：把停靠面板转为浮动（FloatToDock 的逆操作；标题条拖出槽位时执行）。
// wrapper 同样**原地转换**：子窗口 → 有主顶级弹窗（owner = 主框架）。
// 位置取"光标仍在按下时的抓取点"——拖出瞬间不跳位，grabX/grabY 对后续
// 浮动态移动继续有效。尺寸沿用停靠时的矩形（用户拖前看到的即最终大小）。
// 转换后由 LabelProc 的浮动态路径接管，松手若落进宿主吸附带即 FloatToDock 回去。
void DockManager::FloatOut(Panel* p, POINT cursorScreen) {
    if (!p || !p->wrapper) return;
    if (p->container == npp::DockRequest::Floating) return;
    RECT wr{};
    ::GetWindowRect(p->wrapper, &wr);          // 槽位矩形 = 转换前的停靠尺寸
    const int keepW = wr.right - wr.left;
    const int keepH = wr.bottom - wr.top;
    p->container = npp::DockRequest::Floating;
    p->snapTarget = npp::DockRequest::Floating;
    LONG_PTR st = ::GetWindowLongPtrW(p->wrapper, GWL_STYLE);
    st &= ~WS_CHILD;
    st |= WS_POPUPWINDOW | WS_THICKFRAME | WS_VISIBLE;
    ::SetWindowLongPtrW(p->wrapper, GWL_STYLE, st);
    // 顶级窗口：SetParent(nullptr) 后再把 mainWnd_ 记成 **owner**
    // （对顶层窗口 GWLP_HWNDPARENT 语义即 owner，窗口关闭时一并收起）。
    ::SetParent(p->wrapper, nullptr);
    ::SetWindowLongPtrW(p->wrapper, GWLP_HWNDPARENT, (LONG_PTR)mainWnd_);
    ::SetWindowPos(p->wrapper, HWND_TOP, cursorScreen.x - p->grabX,
                   cursorScreen.y - p->grabY, keepW, keepH,
                   SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    // 上游语义：面板进浮动容器 → DMN_FLOAT（与注册即浮动那条口径一致）
    SendNotifyToClient(p->data.hClient, (UINT_PTR)p->data.dlgID, npp::DMN_FLOAT);
    Logger::Info("DockManager: panel '" + WideToUtf8(p->name.c_str()) +
                 "' floated out");
    NotifyLayout();   // 容器收回原槽位（TotalHeight/TotalWidth 随之减少）
}

void DockManager::Destroy() {
    for (auto& p : panels_) {
        if (p->data.hClient && ::IsWindow(p->data.hClient))
            SendActionToClient(p->data.hClient, npp::DMM_CLOSE);
        if (p->wrapper) ::DestroyWindow(p->wrapper);
        p->wrapper = nullptr;
        p->label = nullptr;
        p->iconCtrl = nullptr;
        p->closeBtn = nullptr;
        if (p->iconOwned && p->icon) { ::DestroyIcon(p->icon); }
        p->icon = nullptr;
        p->iconOwned = false;
    }
    panels_.clear();
    // 账本描述的是"当前注册集合"，面板都没了就必须归零 ——
    // 否则它会跨 Destroy 累加，变成一个只涨不落的计数器（e2e 里钉住）。
    coercedContainers_ = 0;
}

} // namespace xfs
