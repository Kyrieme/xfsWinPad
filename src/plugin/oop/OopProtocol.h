#pragma once
// OopProtocol.h — 进程外插件桥协议 v1（原创实现，无 NPP 源码依赖）。
//
// 背景：个别 NPP 兼容插件会在 setInfo 里直接 exit()（实测 ComparePlus）或
// 在 beNotified/命令回调里访问违例——进程内加载时这些都会杀死宿主编辑器，
// SEH 无法拦截 exit()。本协议把插件 DLL 装进 xfsWinPadPluginHost.exe 代理
// 进程：插件死，编辑器活着。
//
// 传输：WM_COPYDATA（SendMessage 同步语义，跨进程内核拷贝，无共享内存）。
//   编辑器 → 插件宿主进程：OOPM_EXEC / OOPM_NOTIFY / OOPM_SHUTDOWN
//   插件宿主进程 → 编辑器：  OOPM_HANDSHAKE（插件名 + FuncItem 表转录）
//   编辑器应答：            OOPM_CMDIDS（宿主分配的命令 id 回填）
// 插件 → 编辑器的 NPPM_* 消息不经本协议：setInfo 拿到的 nppHandle 就是编辑器
// 主窗口 HWND，SendMessage 跨进程直达（复用 §5.6 消息垫片）。
//
// 载荷布局 = 公开契约（与 docs/plugin-system.md §5.1 同规则：字段顺序即契约，
// 只能尾部追加）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {
namespace oop {

// WM_COPYDATA.dwData 魔数（与单实例转发的 'XFWP'=0x58465750 无冲突）
constexpr UINT_PTR kMagic = 0x4F4F5031;   // 'OOP1'

enum OopMsg : UINT_PTR {
    OOPM_HANDSHAKE = 1,   // host→editor：插件名 + FuncItem 表（ItemWire 数组）
    OOPM_CMDIDS    = 2,   // editor→host：逐项命令 id（宿主分配，回填 FuncItem.cmdID）
    OOPM_EXEC      = 3,   // editor→host：执行 items[index].func（SEH 包裹）
    OOPM_NOTIFY    = 4,   // editor→host：beNotified({code,idFrom})（SEH 包裹）
    OOPM_SHUTDOWN  = 5,   // editor→host：退出（SHUTDOWN 通知已由 Notify 广播送达，
                          //   本消息只令代理进程退出，绝不二次回调 beNotified）
};

// FuncItem 跨进程转录：内联名 + 快捷键。函数指针留在代理进程内，
// 编辑器侧命令以 index 寻址执行。
struct ItemWire {
    wchar_t      name[64];               // 与 FuncItem.itemName 同宽（128B）
    unsigned char ctrl, alt, shift, key; // ShortcutKey 展开；key==0 = 无快捷键
};

// OOPM_EXEC / OOPM_NOTIFY / OOPM_SHUTDOWN 的定长载荷（进入 OOPM_SHUTDOWN 时
// 仅 magic/msg 有效）
struct ExecWire {
    UINT_PTR   magic;
    UINT_PTR   msg;
    int        index;                    // EXEC：FuncItem 下标
    unsigned int code;                   // NOTIFY：NPPN_* 通知码
    UINT_PTR   idFrom;                   // NOTIFY：BufferID（编辑器侧 Document* 空间，
                                         //   对插件而言是不透明句柄，与 N++ 语义一致）
};

// OOPM_CMDIDS 载荷：头 + count 个 int 尾随数组
struct CmdIdsWire {
    UINT_PTR magic;
    UINT_PTR msg;
    int      count;
    // int ids[count]; —— 跟随在结构体之后
};

// 握手载荷（host→editor）
struct HandshakeWire {
    UINT_PTR magic;
    UINT_PTR msg;
    int      itemCount;
    wchar_t  pluginName[64];             // getName() 结果（NUL 结尾）
    ItemWire items[128];                 // FuncItem 上限（NppExec 实测 24）
};
constexpr int kHandshakeItemsMax = 128;

// 代理进程退出码约定（编辑器日志 / 不兼容页展示用）：
//   0  正常（收到 OOPM_SHUTDOWN 后退出）
//   2  LoadLibrary 失败（坏 DLL / 非 PE）
//   3  isUnicode() != TRUE（ANSI 插件拒绝，与进程内策略一致）
//   4  缺关键导出（setInfo / getFuncsArray / isUnicode / messageProc）
//   5  启动看门狗超时（setInfo/getFuncsArray 卡死，主动自杀）
//   6  插件代码访问违例（SEH 捕获后退出，绝不让 WER 弹窗拖累后台）
//   其他任意值 = 插件自身调用 exit(n)（如 ComparePlus 的 exit(1)）——
//   编辑器原样记录，这正是进程外隔离的核心价值。
constexpr int kExitClean = 0;
constexpr int kExitLoadFail = 2;
constexpr int kExitAnsi = 3;
constexpr int kExitExport = 4;
constexpr int kExitWatchdog = 5;
constexpr int kExitFault = 6;

} // namespace oop
} // namespace xfs
