#pragma once
// Dpi.h — per-monitor DPI 的换算与钳制（纯函数，批次 138a）。
//
// 【为什么单独抽出来】
//   本程序从第一天起就是 PerMonitorV2 感知（resources/app.manifest），但全仓
//   **没有一处处理 WM_DPICHANGED**：跨屏拖动后，顶层窗口不按系统给出的建议矩形
//   重排，一次性资源（字体 / 工具栏图标位图 / 标题条子控件尺寸）全部停留在旧 dpi。
//
//   布局那一半其实已经"自愈"——LayoutChildren 与各面板的 WM_SIZE 每次都现取
//   GetDpiForWindow 重算，所以本批次真正要补的是"触发一次重排 + 重建一次性资源"。
//   触发路径上需要判断的东西有两类，都与窗口系统无关，因此抽成纯函数：
//     1. WM_DPICHANGED 的**建议矩形**不能照单全收（系统给的是它算的，不是我们
//        能接受的：会小于我们的最小尺寸，也可能被推到工作区外），要先钳制；
//     2. "要不要重建"——dpi 没变就没有任何理由重建字体与图标位图。
//
// 【与 MulDiv 的口径】ScalePx 刻意复刻 MulDiv(v, dpi, 96) 的舍入口径（四舍五入、
//   半数远离零），这样本文件替换到既有调用点上不会产生 1px 的静默漂移。
//   既有代码里的 MulDiv 调用点本轮**不动**（它们本来就正确），只有需要纯函数
//   可测性的新代码走这里。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {

// 逻辑像素的基准 dpi。全仓的比例常量（kHeaderH=24、iconPx 16 等）都是这个口径。
inline constexpr int kBaseDpi = 96;

// 逻辑像素 → 物理像素（96 dpi 基准）。
// dpi <= 0（GetDpiForWindow 对无效窗口会返回 0）时按基准处理，绝不除零。
inline int ScalePx(int logical, int dpi) {
    if (dpi <= 0 || dpi == kBaseDpi) return logical;
    const long long num = static_cast<long long>(logical) * dpi;
    const long long half = (num >= 0) ? kBaseDpi / 2 : -(kBaseDpi / 2);
    return static_cast<int>((num + half) / kBaseDpi);
}

// WM_DPICHANGED 的 wParam：LOWORD = 新 dpiX，HIWORD = 新 dpiY。
// 单独钉住是因为这里有个经典写错法——只取 HIWORD 当 dpi（那是 Y 分量，
// 竖屏 / 非等比缩放时两者不等）。
inline int DpiXFromWParam(WPARAM wp) {
    return static_cast<int>(LOWORD(static_cast<DWORD_PTR>(wp)));
}
inline int DpiYFromWParam(WPARAM wp) {
    return static_cast<int>(HIWORD(static_cast<DWORD_PTR>(wp)));
}

// 只有 dpi 真的变了才值得重建字体 / 图像列表。0 表示"未知"（窗口还没建好），
// 也一律不重建——避免启动路径上被误触发。
inline bool NeedsResourceRebuild(int oldDpi, int newDpi) {
    return oldDpi > 0 && newDpi > 0 && oldDpi != newDpi;
}

// 钳制 WM_DPICHANGED 的建议矩形：不小于最小尺寸，且完全落在工作区内。
//   * 尺寸：先抬到 minW/minH；若工作区本身就比最小值还小，工作区说了算
//     （否则会把窗口撑出屏幕）。
//   * 位置：按"右下优先可见"把左上角拉回来——这是既有 WM_DISPLAYCHANGE 处理
//     的同一口径（宁可遮住左边，也不让窗口跑到看不见的地方）。
// 纯函数 ⇒ 可以直接喂边界数据单测，不必真的跨屏拖窗口。
inline RECT ClampSuggestedRect(const RECT& suggested, int minW, int minH,
                               const RECT& workArea) {
    const int areaW = workArea.right - workArea.left;
    const int areaH = workArea.bottom - workArea.top;

    int w = suggested.right - suggested.left;
    int h = suggested.bottom - suggested.top;
    if (w < minW) w = minW;
    if (h < minH) h = minH;
    if (areaW > 0 && w > areaW) w = areaW;   // 工作区更小：以工作区为准
    if (areaH > 0 && h > areaH) h = areaH;

    int x = suggested.left;
    int y = suggested.top;
    const int maxX = workArea.right - w;
    const int maxY = workArea.bottom - h;
    if (x > maxX) x = maxX;                  // 右下优先：先保证右边可见
    if (y > maxY) y = maxY;
    if (x < workArea.left) x = workArea.left; // 再保证左边不越界
    if (y < workArea.top) y = workArea.top;

    RECT out{};
    out.left = x;
    out.top = y;
    out.right = x + w;
    out.bottom = y + h;
    return out;
}

} // namespace xfs
