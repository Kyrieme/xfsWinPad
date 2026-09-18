#pragma once
// xfsWinPad - 首选项对话框（设置系统设计笔记 阶段 1）。
//
// Notepad++ 风格：TabControl 分页（常规/编辑/新建文档/备份/终端），
// 改动**实时应用**（经 IPrefsApplier 作用于全部已开编辑器 + UI），
// 「确定」提交并落盘、「取消/X」用会话快照还原。
//
// 宿主侧（MainWindow）实现 IPrefsApplier：把 AppSettings 灌进
// Editor::ApplyPrefs / 主题切换 / 自动保存定时器等。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../settings/Settings.h"

namespace xfs {

// 对话框 → 宿主的实时应用通道（非拥有）。
struct IPrefsApplier {
    virtual ~IPrefsApplier() = default;
    // 用给定设置刷新整个应用（编辑器+主题+定时器）。宿主自行决定
    // 是否把它写盘（首选项「确定」后由 MainWindow 落盘）。
    virtual void ApplyAll(const AppSettings& s) = 0;
};

class PreferencesDialog {
public:
    // 模态运行。current 为宿主持有的设置对象：确定时被写回；
    // 取消时宿主经 ApplyAll(original) 还原。
    static void Run(HWND parent, HINSTANCE hInst, AppSettings* current,
                    IPrefsApplier* applier);
};

} // namespace xfs
