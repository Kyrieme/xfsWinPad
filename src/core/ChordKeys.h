#pragma once
// xfsWinPad - ChordKeys: VS 风格双键和弦判定（批次 154）。
//
// 需求：仿 Visual Studio 的 Ctrl+K,C（加行注释）与 Ctrl+K,U（取消行注释）。
// ShortcutTable 是**单和弦**模型（ShortcutInfo{vk,ctrl,alt,shift}），结构上装不下
// 双键序列，硬塞会波及 ACCEL 构建与 Shortcut Mapper 的冲突检测。因此和弦改由消息
// 循环**独立拦截**；这一层做成 header-only 纯函数：零 Windows 依赖、可直接单测。
//
// 为什么放 core：tests/CMakeLists.txt 各测试**直接编译指定源文件**、不链接 app，
// 纯 inline 头 ⇒ 零 CMake 改动即可单测（与 Dpi.h / NavHistory.h 同路数）。

namespace xfs {

enum class ChordAction {
    None,       // 不处理：交回正常流程（加速器 / Scintilla）
    Arm,        // 消费该键并进入前缀态（按下 Ctrl+K）
    Comment,    // 消费该键并触发「加行注释」
    Uncomment,  // 消费该键并触发「取消行注释」
};

// pending = 是否已按下前缀（Ctrl+K）。
// 规则：
//   pending==true ：仅 Ctrl+C → Comment；仅 Ctrl+U → Uncomment；其余 → None。
//   pending==false：仅 Ctrl+K → Arm；其余 → None。
// 「仅 Ctrl」= ctrl 为真且 alt/shift 都为假。这是刻意收窄：Ctrl+Shift+K / Ctrl+Alt+C
// 这类组合不属于和弦，必须放行给正常流程，否则会静默吞掉用户真正的快捷键。
inline ChordAction DecideChord(unsigned vk, bool ctrl, bool alt, bool shift,
                               bool pending) {
    if (!(ctrl && !alt && !shift)) return ChordAction::None;
    if (pending) {
        if (vk == 'C') return ChordAction::Comment;
        if (vk == 'U') return ChordAction::Uncomment;
        return ChordAction::None;
    }
    if (vk == 'K') return ChordAction::Arm;
    return ChordAction::None;
}

} // namespace xfs
