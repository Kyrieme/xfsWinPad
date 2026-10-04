#pragma once
// UiFont.h — UI 控件字体的唯一来源（批次 145）。
//
// 【要解决的问题】
//   本程序从各面板到标签页、对话框，UI 字体原来一律手写成：
//       ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, ...,
//                     CLEARTYPE_QUALITY, ..., L"Segoe UI")
//   它把 9 当成「96 dpi 下的 9 **像素**」——100% 缩放时字高只有 9px，远低于
//   系统 UI 基准（9pt ≈ 12px），在 1080p / 100% 屏上又小又发虚；而且字号与字面
//   写死在代码里，换台设备或改一下缩放就得回来改代码。
//
// 【做法：完全自适应，零写死常量】
//   按**目标监视器的 dpi** 向系统要「消息字体」（SPI_GETNONCLIENTMETRICS 的
//   lfMessageFont，走 SystemParametersInfoForDpi 变体），再原样 CreateFontIndirectW。
//   于是字号 / 字面 / 粗细 / 斜体全部跟着「设置 → 系统 → 显示 → 缩放」与
//   「辅助功能 → 文本大小」自动走。跨屏拖动时各面板在 OnDpiChanged 里传新 dpi
//   重取即可，不需要任何比例常量。
//
//   老系统没有 ForDpi 变体时回退 SystemParametersInfoW 的当前 dpi 版本；再失败
//   返回 nullptr，调用方**保持原字体不变**（绝不因此把界面变成无字体）。
//
// 返回的 HFONT 归调用方所有，须自行 DeleteObject。

#include <windows.h>

namespace xfs {

// dpi <= 0 时按 96 处理（GetDpiForWindow 对未就绪窗口会返回 0）。
HFONT CreateUiFont(int dpi);

} // namespace xfs
