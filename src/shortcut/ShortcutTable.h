#pragma once
// xfsWinPad - ShortcutTable: 快捷键单一数据源（设置系统设计笔记 阶段 0）。
//
// 设计要点：
//   * 默认表 = C++ 编译期常量（对齐历史 BuildAccelerators 硬编码数组）。
//   * 用户改动（覆盖 / 显式删除）序列化到 %APPDATA%\xfsWinPad\shortcuts.json，
//     **只写被改过的项** —— 与 NPP shortcuts.xml 同策略，天然兼容新增命令。
//   * Get() = 覆盖优先，其次默认；显式删除存为 vk=0 的覆盖项。
//   * 组合串格式 "Ctrl+Shift+F3"（人类可读，便于手改与测试）。
//   * 线程约定：仅在 UI 线程访问（与加速器构建同一前提）。

#include <string>
#include <utility>
#include <vector>

namespace xfs {

struct ShortcutInfo {
    unsigned int vk = 0;    // Windows 虚拟键码；0 = 显式删除/无效
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    bool Valid() const { return vk != 0; }
    bool SameAs(const ShortcutInfo& o) const {
        return vk == o.vk && ctrl == o.ctrl && alt == o.alt && shift == o.shift;
    }
    // "Ctrl+Shift+F3"；无效组合返回空串。
    std::wstring ToString() const;
    // 解析失败返回 vk=0。
    static ShortcutInfo FromString(const std::wstring& combo);
};

class ShortcutTable {
public:
    ShortcutInfo Get(unsigned cmdId) const;                    // override > 默认
    // 覆盖（vk=0 = 显式删除）。conflictCmd 非空时返回撞键的命令 id。
    bool SetShortcut(unsigned cmdId, const ShortcutInfo& info,
                     unsigned* conflictCmd = nullptr);
    // 撤销覆盖（回到默认项；无覆盖返回 false）。
    bool ClearOverride(unsigned cmdId);

    // 供加速器构建：合并后的 (cmdId, shortcut) 列表（仅含 Valid 项，过滤仅展示项）。
    std::vector<std::pair<unsigned int, ShortcutInfo>> All() const;
    // 供 Shortcut Mapper 列表：全量条目（含仅展示项，默认+覆盖合并）。
    std::vector<std::pair<unsigned int, ShortcutInfo>> Everything() const;

    // 冲突查询：该组合当前被哪个命令占用（0 = 无）。
    unsigned FindByCombo(const ShortcutInfo& info) const;

    // 仅展示型条目（Scintilla 自处理的编辑键）：进菜单尾注/tooltip，
    // 不进 ACCEL 表（避免终端/对话框聚焦时误触编辑器命令）。
    static bool DisplayOnly(unsigned cmdId);

    size_t OverrideCount() const { return overrides_.size(); }

    static const wchar_t* FilePath();   // %APPDATA%\xfsWinPad\shortcuts.json
    bool Load(const std::wstring& path);    // 失败/缺失 → 仅默认表，返回 false
    bool Save(const std::wstring& path) const;

private:
    // cmdId → 覆盖项（Valid=改键；!Valid=删除默认键）。
    std::vector<std::pair<unsigned int, ShortcutInfo>> overrides_;

    static const std::vector<std::pair<unsigned int, ShortcutInfo>>& Defaults();
};

// 进程级共享实例（MainWindow 启动时 Load；BuildAccelerators/CmdShortcut 查询）。
ShortcutTable& GlobalShortcuts();

} // namespace xfs
