#pragma once
// xfsWinPad - 语言样式配置器（Style Configurator，docs/settings-plan.md 阶段 2b）。
//
// N++ 三区布局：顶部主题下拉（实时切换）+ 左侧语言列表 + 右侧样式（角色）列表，
// 下方该样式的 前景/背景 颜色（启用开关+取色按钮）与 Bold/Italic/Underline。
// 改动经 IStyleApplier 即时作用于全部编辑器；「保存并关闭」落盘 stylers.json，
// 「取消/X」用会话快照还原。
// （语言级整行字体、userExt、主题本身的颜色编辑属于阶段 4。）

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {

// 对话框 → 宿主的实时应用通道
struct IStyleApplier {
    virtual ~IStyleApplier() = default;
    virtual void RestyleAll() = 0;                            // 全部编辑器重刷样式
    virtual bool SwitchThemeByName(const wchar_t* name) = 0;  // 主题下拉实时切换
    virtual const wchar_t* CurrentThemeName() = 0;            // 取消还原用
};

class StyleConfiguratorDialog {
public:
    static void Run(HWND parent, HINSTANCE hInst, IStyleApplier* applier);
};

} // namespace xfs
