#pragma once
// NppDockMask.h — DockedWidgetData.uMask 的解码（纯函数，批次 119）。
//
// 【为什么单独抽出来】
//   本项目的 dock 体系**只实现底部容器**，其余请求一律降级。批次 119 之前这件事
//   只写在注释里，而且注释与实现**不一致**：头文件与设计文档都写
//   "浮动 / 左 / 右 / 上一律按底部处理**并记日志**"，实现里却只有
//   kDwsDfFloating 打了日志，左/右/上一行都没有 —— 用户遇到"我要右边、它给我底部"
//   时没有任何证据可查。
//   解码规则本身与窗口系统无关（只是位运算），抽成纯函数才能把下面两个位编码的坑
//   钉进测试；顺带让"面板标题条显示什么"也成为可单测的口径。
//
// 【位编码的两个坑】（都在 tests/test_dock_mask.cpp 里钉住）
//   1. `kDwsDfFloating`(bit31) 与容器位(bits28-29) 是**互不重叠的两个字段**，
//      可以同时置位。同时置 = 矛盾请求 ⇒ 本实现取**更强**的"浮动"。
//   2. `kDwsDfContLeft == (0 << 28) == 0`，与"**完全没指定**"在编码上**重合**
//      ⇒ 分不出"我要左边"和"我什么都没说"。本实现把"没指定"按**底部**处理
//      （与既有夹具 uMask = 0 的口径一致），因此**显式请求左容器在编码上不可表达**。
//      这是上游位编码的固有性质，不是本实现的取舍。

#include <string>

#include "NppDocking.h"

namespace xfs {
namespace npp {

// 插件在 uMask 里请求的默认容器。
enum class DockRequest { Bottom, Left, Right, Top, Floating };

// 从 uMask 解出请求的容器。纯函数。
inline DockRequest RequestedDock(UINT uMask) {
    if (uMask & kDwsDfFloating) return DockRequest::Floating;
    const int cont = static_cast<int>((uMask >> 28) & 0x3u);
    switch (cont) {
    case kContRight:  return DockRequest::Right;
    case kContTop:    return DockRequest::Top;
    case kContBottom: return DockRequest::Bottom;
    default:          return DockRequest::Bottom;   // kContLeft(0) 与"未指定"重合
    }
}

// 本实现提供的容器：底部（默认/未指定/编码上重合的左）、右、上（批次 122）、
// 浮动（批次 123：有主顶级窗口，见 DockManager::DockWidget）。
// 五种请求至此全部有真实容器 ⇒ **不再有任何降级**；谓词恒为假，
// 与 CoercedContainers 账本一并保留供兼容报告口径。
inline bool IsDockRequestCoerced(UINT uMask) {
    (void)uMask;
    return false;
}

// 日志 / 守卫输出用的 ASCII 名字（不要进 i18n：这是诊断文本）。
inline const char* DockRequestName(DockRequest r) {
    switch (r) {
    case DockRequest::Left:     return "left";
    case DockRequest::Right:    return "right";
    case DockRequest::Top:      return "top";
    case DockRequest::Floating: return "floating";
    case DockRequest::Bottom:   return "bottom";
    }
    return "?";
}

// 面板标题条文本：`kDwsAddInfo` 置位且附加信息非空时，把附加信息接在名字后面。
// 纯函数 ⇒ 标题口径可单测，不必建窗口。
inline std::wstring DockPanelTitle(const wchar_t* name, const wchar_t* addInfo,
                                   UINT uMask) {
    std::wstring t = name ? name : L"";
    if ((uMask & kDwsAddInfo) && addInfo && *addInfo) {
        t += L" — ";
        t += addInfo;
    }
    return t;
}

// 批次 126：拖回吸附判据（纯函数）。浮动面板的标题条被拖动时，光标落进
// 主框架某条边的吸附带（zonePx）内 ⇒ 松手即停靠进该容器。
//   * 四条边按距离取最近；垂直边要求光标在框架纵向跨度内（含 zone 容差），
//     水平边同理 —— 角落同时近两条边时取更近者。
//   * Left 在编码上与"未指定"重合 ⇒ 吸附结果改判 Bottom（与既有口径一致）。
//   * frame 为主框架的**屏幕坐标**矩形；zone 为 96dpi 逻辑像素（调用方折算）。
// 返回 true 并写出目标容器；光标不在任何吸附带内返回 false（继续浮动）。
inline bool SnapEdgeTo(const RECT& frameScreen, POINT pt, int zonePx,
                       DockRequest* out) {
    if (zonePx <= 0 || !out) return false;
    const bool inX = pt.x >= frameScreen.left - zonePx &&
                     pt.x <= frameScreen.right + zonePx;
    const bool inY = pt.y >= frameScreen.top - zonePx &&
                     pt.y <= frameScreen.bottom + zonePx;
    if (!inX || !inY) return false;
    const int dL = pt.x >= frameScreen.left ? pt.x - frameScreen.left
                                            : frameScreen.left - pt.x;
    const int dR = pt.x >= frameScreen.right ? pt.x - frameScreen.right
                                             : frameScreen.right - pt.x;
    const int dT = pt.y >= frameScreen.top ? pt.y - frameScreen.top
                                           : frameScreen.top - pt.y;
    const int dB = pt.y >= frameScreen.bottom ? pt.y - frameScreen.bottom
                                              : frameScreen.bottom - pt.y;
    DockRequest best = DockRequest::Floating;
    int bestD = zonePx + 1;
    if (inY && dL < bestD) { bestD = dL; best = DockRequest::Left; }
    if (inY && dR < bestD) { bestD = dR; best = DockRequest::Right; }
    if (inX && dT < bestD) { bestD = dT; best = DockRequest::Top; }
    if (inX && dB < bestD) { bestD = dB; best = DockRequest::Bottom; }
    if (bestD > zonePx) return false;
    if (best == DockRequest::Left) best = DockRequest::Bottom;
    *out = best;
    return true;
}

// 批次 127：拖出判据（纯函数）——SnapEdgeTo 的反方向。
// 停靠面板的标题条被拖动时，光标离开**面板自身所占的槽位**（= 停靠态的
// wrapper 屏幕矩形）slackPx 之外 ⇒ 判为拖出，转浮动容器。
//   * 停靠态 wrapper 不随光标移动，所以这个矩形在整个拖动过程中是稳定的。
//   * slackPx 是**容差**：光标须再往外走这么多像素才算数。拖动阈值（LabelProc
//     的 4px）只防手抖，不足以防"贴着边缘的微调"，故两者叠加使用。
//   * slackPx < 0 视为非法入参 ⇒ 返回 false（宁可不转浮动，也不误判）。
inline bool DragOutOfSlot(const RECT& slotScreen, POINT pt, int slackPx) {
    if (slackPx < 0) return false;
    return pt.x < slotScreen.left - slackPx || pt.x > slotScreen.right + slackPx ||
           pt.y < slotScreen.top - slackPx || pt.y > slotScreen.bottom + slackPx;
}

// 批次 139：OOP 停靠面板的键盘焦点桥判据（纯函数）。
//
// 【为什么需要】焦点在 Win32 里是"每输入队列"的：键盘只发给**前台线程**队列的
//   焦点窗口（实测见 DockManager.cpp 顶部批次 139 段）。
//   · 进程内面板与宿主同队列 —— 插件控件调 SetFocus 就等于设了宿主（前台线程）
//     队列的焦点 ⇒ 键盘天然可达，宿主不必插手。
//   · 跨进程（OOP）面板的 hClient 属于**代理进程**的队列 —— 插件控件调 SetFocus
//     只改它那条队列；宿主队列焦点不动 ⇒ 不桥接就永远收不到键盘（鼠标仍正常，
//     因为鼠标按光标下的窗口路由，与焦点无关）。
//   宿主居中补一次 SetFocus(hClient) 即可打通：两进程实测里它返回 err=0，两侧
//   队列焦点都变成 hClient，且插件进程真的收到 WM_SETFOCUS。
//
// 【触发点】WM_PARENTNOTIFY 的鼠标按下事件。系统在子窗口（= 插件 hClient）收到
//   点击时把该消息发给**父窗口**（= 我们的 wrapper），载荷是坐标不是指针 ⇒ 跨进程
//   安全；且它先于子窗口处理点击到达，随后插件自身控件的 SetFocus 仍能覆盖，
//   不会抢走插件内部焦点。
//
// 【为什么限定"仅跨进程"】进程内面板若也在每次鼠标按下时把焦点设到 hClient，
//   就会**夺走**插件控件自己的焦点（回归）。故 clientIsForeign == false 一律不桥。
inline bool ShouldBridgeKeyboardFocus(UINT parentNotifyEvent, bool clientIsWindow,
                                      bool clientIsForeign) {
    if (!clientIsWindow || !clientIsForeign) return false;
    return parentNotifyEvent == WM_LBUTTONDOWN ||
           parentNotifyEvent == WM_MBUTTONDOWN ||
           parentNotifyEvent == WM_RBUTTONDOWN;
}

} // namespace npp
} // namespace xfs
