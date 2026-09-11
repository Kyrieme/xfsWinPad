#pragma once
// xfsWinPad - 插件管理对话框（Plugins Admin）。
//
// 与 Notepad++ 保持一致的四标签结构：可用 / 更新 / 已安装 / 不兼容。
//   * 可用(Available)：清单中有而尚未安装的插件，勾选后点「安装」。
//   * 更新(Updates) ：已安装且清单里有更高版本的插件，勾选后点「更新」。
//   * 已安装(Installed)：当前已安装的插件，选中后点「移除」。
//   * 不兼容(Incompatible)：最近一次启动中加载失败的插件 DLL——按失败原因
//     （架构不匹配/ABI 不符/缺导出/ANSI 被拒/无法加载）列出，只读展示。
// 实时搜索框：输入关键词即时定位（选中 + 滚动到）当前页匹配的插件行。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {

class PluginManager;

class PluginAdminDialog {
public:
    // 模态运行；parent 禁用到关闭期间。mgr 用于「不兼容」页读取加载失败
    // 清单（可为 null——该页显示为空）。
    static void Run(HWND parent, HINSTANCE hInst, PluginManager* mgr = nullptr);
};

} // namespace xfs