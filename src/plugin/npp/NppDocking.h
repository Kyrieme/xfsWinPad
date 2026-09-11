#pragma once
// NppDocking.h — Notepad++ 可停靠对话框二进制契约（xfsWinPad 原创声明）。
//
// 数值是从公开事实源人工核对后抄录的【接口数值】（接口数值属于事实，
// 不构成表达）；实现均为原创。来源与抓取日期：
//   上游: github.com/notepad-plus-plus/notepad-plus
//         PowerEditor/src/WinControls/DockingWnd/Docking.h
//         PowerEditor/src/WinControls/DockingWnd/dockingResource.h
//   实际取数通道(2026-08-27): gitee.com/mirrors/notepad-plus-plus @ master
//   （raw.githubusercontent 直连在当时的网络环境不可达）
//
// 语义要点（对应 NPP 插件的通用惯例，分歧以上游原文为准）：
//   * 插件用 CreateDialog 建一个 modeless 对话框，得到 hClient；随后向宿主
//     SendMessage(NPPM_DMMREGASDCKDLG, 0, &tTbData) 把 hClient 交给宿主 dock。
//     宿主把 hClient 重挂到 dock 面板里（SetParent），之后由宿主控制尺寸。
//   * DWS_DF_CONT_* 编码在 uMask 的高 4 位（容器 << 28）；DWS_DF_FLOATING 置位
//     表示默认浮动。我们的 dock 体系只实现底部容器 → 浮动一律按底部处理。
//   * DMN_*：宿主以 WM_NOTIFY(idFrom=dlgID, lParam=&NMHDR{code=DMN_*}) 发给
//     插件对话框。DMN_CLOSE 里插件可 veto（把消息结果置 TRUE 保持打开）。
//   * DMM_*：宿主直接 SendMessage 给插件对话框的动作请求（关闭/移动等）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {
namespace npp {

// ---- 容器边（DockedWidgetData 的 rcFloat 无关；默认 dock 位置用）------------
constexpr int kContLeft = 0;
constexpr int kContRight = 1;
constexpr int kContTop = 2;
constexpr int kContBottom = 3;
constexpr int kContMax = 4;

// 标题位置（上游 CAPTION_TOP/BOTTOM；我们恒置顶）
constexpr BOOL kCaptionTop = TRUE;
constexpr BOOL kCaptionBottom = FALSE;

// ---- DWS_* 掩码（uMask 低 4 位是特性位，高 4 位是默认容器）-----------------
constexpr UINT kDwsIconTab       = 0x00000001u;  // 标签页显示图标（未启用）
constexpr UINT kDwsIconBar       = 0x00000002u;  // 图标条（上游已不支持）
constexpr UINT kDwsAddInfo       = 0x00000004u;  // 使用附加信息文本
constexpr UINT kDwsUseOwnDarkMode = 0x00000008u; // 插件自带深色模式
constexpr UINT kDwsParamsAll     = kDwsIconTab | kDwsIconBar | kDwsAddInfo;

// 默认 dock 位置（容器编号左移 28 位；与 kDwsParamsAll 位域无冲突）
constexpr UINT kDwsDfContLeft    = (kContLeft   << 28);
constexpr UINT kDwsDfContRight   = (kContRight  << 28);
constexpr UINT kDwsDfContTop     = (kContTop    << 28);
constexpr UINT kDwsDfContBottom  = (kContBottom << 28);
constexpr UINT kDwsDfFloating    = 0x80000000u; // 默认浮动（我们按底部处理）

// ---- DockedWidgetData（tTbData）：字段顺序/宽度是二进制契约，不可改动 -------
// 命名与注释为原创；成员顺序与官方一致（接口事实）。官方还有尾部新成员
// pszModuleName（2025 新增，用于重启后恢复 dock 位置）——一并保留。
struct DockedWidgetData {
    HWND hClient = nullptr;            // 插件对话框句柄（必填）
    const wchar_t* pszName = nullptr;  // 面板标题（显示在 dock 标题条）
    int dlgID = 0;                     // 打开该对话框的 FuncItem 命令 id
    UINT uMask = 0;                    // 特性位 + 默认容器位（见上）
    HICON hIconTab = nullptr;          // 标签页图标（未启用）
    const wchar_t* pszAddInfo = nullptr; // 附加信息文本（kDwsAddInfo 时显示）
    RECT rcFloat = {};                 // 浮动位置（内部数据，我们不使用）
    int iPrevCont = 0;                 // 上次容器（内部数据，我们不使用）
    const wchar_t* pszModuleName = nullptr; // 插件 DLL 文件名（重启恢复用）
};

// ---- DMN_* 通知码（宿主 → 插件对话框的 WM_NOTIFY code）-----------------------
constexpr int kDmnFirst = 1050;        // 上游宏 DMN_FIRST
constexpr int DMN_CLOSE       = kDmnFirst + 1; // 用户点关闭；可 veto
constexpr int DMN_DOCK        = kDmnFirst + 2; // 面板已 dock
constexpr int DMN_FLOAT       = kDmnFirst + 3; // 面板已浮动
constexpr int DMN_SWITCHIN    = kDmnFirst + 4; // 标签页切入
constexpr int DMN_SWITCHOFF   = kDmnFirst + 5; // 标签页切出
constexpr int DMN_FLOATDROPPED = kDmnFirst + 6; // 浮动拖放完成

// ---- DMM_* 消息（宿主 → 插件对话框的直接 SendMessage）------------------------
constexpr UINT kDmmMsg = 0x5000;       // 上游宏 DMM_MSG
constexpr UINT DMM_CLOSE          = kDmmMsg + 1;  // 请求关闭
constexpr UINT DMM_DOCK           = kDmmMsg + 2;  // 请求 dock
constexpr UINT DMM_FLOAT          = kDmmMsg + 3;  // 请求浮动
constexpr UINT DMM_DOCKALL        = kDmmMsg + 4;  // 全部 dock
constexpr UINT DMM_FLOATALL       = kDmmMsg + 5;  // 全部浮动
constexpr UINT DMM_MOVE           = kDmmMsg + 6;  // 移动
constexpr UINT DMM_UPDATEDISPINFO = kDmmMsg + 7;  // 刷新显示信息
constexpr UINT DMM_DROPDATA       = kDmmMsg + 10; // 拖放数据
constexpr UINT DMM_MOVE_SPLITTER  = kDmmMsg + 11; // 分隔条移动
constexpr UINT DMM_CANCEL_MOVE    = kDmmMsg + 12; // 取消移动
constexpr UINT DMM_LBUTTONUP      = kDmmMsg + 13; // 左键抬起

} // namespace npp
} // namespace xfs
