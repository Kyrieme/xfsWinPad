#pragma once
// NppCompat.h — Notepad++ 插件兼容通道（原创实现）。
//
// 法律边界（见 插件系统设计笔记 §3）：本文件只声明从公开文档研究得到的
// 二进制契约（调用序列 / 消息路由 / 结构偏移是事实，不构成表达），所有声明、
// 命名与实现均为 xfsWinPad 原创；不复制 Notepad++ 源码或其 SDK 头文件。
//
// 一个 NPP 形态的插件是普通 DLL，导出六个 C 入口：
//   setInfo(NppData*)              宿主把句柄三件套交给插件（插件抄进自己静态区）
//   getName() -> const wchar_t*    插件显示名（Plugins 菜单分组名）
//   getFuncsArray(int*) -> FuncItem*  返回命令数组；宿主随后逐项回填 _cmdID
//   beNotified(SCNotification*)    通知泵（4c 起才由我们调用）
//   messageProc(UINT,WPARAM,LPARAM)消息钩子（4b 起）
//   isUnicode() -> BOOL            必须 TRUE（否则拒绝加载；与 N++ 现代行为一致）
//
// 结构字段名是我们自己的——二进制兼容只取决于成员顺序与宽度，与命名无关。
// 字段顺序 = 公开契约，不可改动；新增字段也只能追加在尾部。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace xfs {
namespace npp {

typedef void (*VoidFn)(void);

// 宿主传给插件的句柄三元组（布局契约：连续三个指针大小成员）。
struct NppData {
    HWND npp;
    HWND scintillaMain;
    HWND scintillaSecond;
};

// 快捷键描述（4a 仅保存不接线；4b 统一做加速表注入）。
// 字段顺序 = 公开契约：现代 NPP 插件 SDK（PluginInterface.h）为
// {bool ctrl, bool alt, bool shift, UCHAR key} —— 插件以指针传本结构，
// 顺序错位会把 key 误读成 modifier（实测 NppExec 即此布局）。
struct ShortcutKey {
    bool ctrl;
    bool alt;
    bool shift;
    unsigned char key;
};

// 单条插件菜单命令（布局契约 = 现代 SDK：内联名 buffer 128B + 指针 +
// int + bool + 指针，总 152B、步长 152）。⚠ 早期 SDK 的 _pName 是指针，
// 现代插件（NppExec 等）把名字放进内联 wchar_t[64]，偏移 0 直接是文本；
// 若宿主仍按指针解引用，会把 UTF-16 文本字节误当地址而崩溃（0xC0000005）。
struct FuncItem {
    wchar_t itemName[64];      // 内联命令名（128B，NUL 结尾）
    VoidFn func;               // 插件命令回调（无参）
    int cmdID;                 // ★ getFuncsArray 之后由宿主写回，永不复改
    bool initCheck;            // 初始勾选态
    ShortcutKey* shortcut;     // 可空
};

class NppAdapter {
public:
    // 无参 NPP 回调 → 统一命令表 (cb,user) 的桥；user 必须是 Slots() 元素。
    struct FuncSlot { VoidFn fn; };
    static void Thunk(void* user);

    // 解析六导出、校验 isUnicode()==TRUE 并拷贝插件名。
    // 失败返回 false（原因已写日志；FailReason 给出令牌 export/ansi 供
    // 不兼容页展示）。调用序列的编排由 PluginManager 完成：
    // Resolve → CallSetInfo → FetchItems → （manager 回填 cmdID / 建命令）。
    bool Resolve(HMODULE dll);
    const std::string& FailReason() const { return failReason_; }
    const std::wstring& Name() const;

    // 调用序列前半（公开契约）：把宿主句柄三元组交给插件静态区。
    void CallSetInfo(const NppData& data);

    // 调用 getFuncsArray，返回 FuncItem 表（count 个），并按表建立槽位映射。
    FuncItem* FetchItems(int& count);

    // 通知泵桥（4c）：把宿主合成或编辑器转发的事件以 SCNotification 布局
    // 递给插件的 beNotified。scn 必须指向有效的 SCNotification（布局见
    // Scintilla.h：nmhdr 兼容头打头，含 hwndFrom/idFrom/code）；nullptr 安全。
    void Notify(const void* scn);

private:
    HMODULE dll_ = nullptr;
    // 六导出函数指针（GetProcAddress 结果原样保存）
    void (*setInfo_)(NppData*) = nullptr;
    const wchar_t* (*getName_)(void) = nullptr;
    FuncItem* (*getFuncsArray_)(int*) = nullptr;
    void (*beNotified_)(void*) = nullptr;
    LRESULT (*messageProc_)(UINT, WPARAM, LPARAM) = nullptr;
    BOOL (*isUnicode_)(void) = nullptr;

    std::wstring name_;
    std::string failReason_;      // Resolve 失败令牌：export / ansi
    bool crashed_ = false;        // setInfo/beNotified 抛异常后置位，停发通知
    int itemCount_ = 0;
    FuncItem* items_ = nullptr;
    std::vector<FuncSlot> slots_;

public:
    const std::vector<FuncSlot>& Slots() const { return slots_; }
};

} // namespace npp
} // namespace xfs
