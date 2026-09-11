#pragma once
// xfsWinPad - 管理快捷键（Shortcut Mapper，docs/settings-plan.md 阶段 3）。
//
// 三页列表：主菜单命令 / 内部命令（标签切换等）/ 插件命令，Name+Shortcut
// 双列 + 实时过滤；选中行可 修改…（捕获式录入：勾选修饰键 + 按键即捕获）、
// 清除（解绑）、恢复默认。所有改动即时写入 ShortcutTable 并落盘
// shortcuts.json；宿主经 IShortcutChange 在每次变更后重建加速器表。
// 冲突检测：SetShortcut 已内建，撞键时提示占用者。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace xfs {

struct ShortcutEntry {
    unsigned int id = 0;
    std::wstring name;        // 显示名（主菜单=菜单文本，插件=插件名::项）
    int group = 0;            // 0=主菜单 1=内部命令 2=插件命令
};

struct IShortcutChange {
    virtual ~IShortcutChange() = default;
    virtual void OnShortcutChanged() = 0;   // 重建 ACCEL 表 + 保存 json
};

class ShortcutMapperDialog {
public:
    static void Run(HWND parent, HINSTANCE hInst,
                    const std::vector<ShortcutEntry>& entries,
                    IShortcutChange* change);
};

} // namespace xfs
