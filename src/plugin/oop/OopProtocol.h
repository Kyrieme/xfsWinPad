#pragma once
// OopProtocol.h — 进程外插件桥协议 v2（原创实现，无 NPP 源码依赖）。
//
// 背景：个别 NPP 兼容插件会在 setInfo 里直接 exit()（实测 ComparePlus）或
// 在 beNotified/命令回调里访问违例——进程内加载时这些都会杀死宿主编辑器，
// SEH 无法拦截 exit()。本协议把插件 DLL 装进 xfsWinPadPluginHost.exe 代理
// 进程：插件死，编辑器活着。
//
// v2（批次 66）：一个代理进程承载多个插件（多槽位）。代理启动不带插件，
// 编辑器对 slot0 控制窗发 OOPM_ADD 动态装载；每个插件获得自己的消息窗，
// EXEC/NOTIFY/CMDIDS 按窗寻址（路由与 v1 完全一致）。装载失败经
// OOPM_REJECT 回执（进程不陪葬）；插件在 setInfo 里 exit() 仍会带走整个
// 代理进程——编辑器把死亡归因于正在装载的那个，幸存者撤销命令后重进新
// 代理。共享代理是内存换隔离粒度的取舍：运行期某插件把代理搞崩，同车
// 插件连坐记账（oop-died）。
//
// 传输：WM_COPYDATA（SendMessage 同步语义，跨进程内核拷贝，无共享内存）。
//   编辑器 → 插件宿主进程：OOPM_ADD / OOPM_EXEC / OOPM_NOTIFY / OOPM_SHUTDOWN
//                             OOPM_MSG（messageProc 桥，v2.1）
//   插件宿主进程 → 编辑器：  OOPM_HANDSHAKE（插件名 + FuncItem 表转录）
//                             OOPM_REJECT（装载拒绝回执）
//                             OOPM_MSGREPLY（messageProc 返回值，v2.1）
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
    OOPM_ADD       = 6,   // editor→host(slot0)：动态装载一个插件 DLL（v2 尾部追加）
    OOPM_REJECT    = 7,   // host→editor：装载拒绝回执（v2 尾部追加；代理进程无恙，
                          //   reason 取 kExit* 约定值）
    OOPM_READY     = 8,   // host→editor：代理进程就绪（v2 尾部追加；wp=slot0 窗，
                          //   载荷仅 magic+msg）
    OOPM_MSG       = 9,   // editor→host：调用槽插件 messageProc(msg,wp,lp)，
                          //   结果经 OOPM_MSGREPLY 回带（v2.1 尾部追加）
    OOPM_MSGREPLY  = 10,  // host→editor：messageProc 的 LRESULT（v2.1 尾部追加）
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

// 握手载荷（host→editor）。cookie = 编辑器 ADD 请求携带的对账值，原样回带；
// v2 起必填（代理与编辑器同包发布，无跨版本兼容负担）。
struct HandshakeWire {
    UINT_PTR magic;
    UINT_PTR msg;
    int      itemCount;
    wchar_t  pluginName[64];             // getName() 结果（NUL 结尾）
    ItemWire items[128];                 // FuncItem 上限（NppExec 实测 24）
    UINT_PTR cookie;                     // 尾部追加（v2）
    UINT_PTR slotWnd;                    // 尾部追加（v2）：插件槽窗（CMDIDS/EXEC 目标）
};
constexpr int kHandshakeItemsMax = 128;

// OOPM_ADD 载荷（editor→host slot0）：装载指定 DLL 并握手。
// path 为 NUL 结尾绝对路径（DLL 最大路径 260 内，含结尾符）。
constexpr int kAddPathMax = 260;
struct AddWire {
    UINT_PTR   magic;
    UINT_PTR   msg;
    UINT_PTR   cookie;
    wchar_t    path[kAddPathMax];
    UINT_PTR   deadlineMs;               // 本次装载的看门狗时限（0=代理默认）
};

// OOPM_REJECT 载荷（host→editor）：装载失败回执，reason 取 kExit* 约定值。
struct RejectWire {
    UINT_PTR magic;
    UINT_PTR msg;
    UINT_PTR cookie;
    UINT_PTR reason;
};

// OOPM_MSG 载荷（editor→host 槽窗）：messageProc 桥（v2.1）。安全红线：
// 只允许 wp/lp 均为值类型（整数/HWND/HMENU）的消息过桥——编辑器侧白名单
// 把关，任何指针参数（如 WM_SETTINGCHANGE 的 LPWSTR）一律不过桥。
// reqId = 编辑器请求序号，回带对账；超时的回包按 reqId 失配丢弃。
struct MsgWire {
    UINT_PTR magic;
    UINT_PTR msg;                        // 恒 OOPM_MSG（wire[1] 惯例不变）
    UINT_PTR reqId;
    UINT_PTR wndMsg;                     // 转发给 messageProc 的窗口消息（WM_*）
    UINT_PTR wParam;
    LONG_PTR lParam;
};

// OOPM_MSGREPLY 载荷（host→editor）：插件 messageProc 的返回值。
// slotWnd = 应答槽窗（编辑器对账归属）；插件未导出 messageProc 时
// 代理以 result=0 应答（NPP 语义：未处理）。
struct MsgReplyWire {
    UINT_PTR magic;
    UINT_PTR msg;                        // 恒 OOPM_MSGREPLY
    UINT_PTR reqId;
    UINT_PTR wndMsg;                     // 被应答的窗口消息（诊断）
    LONG_PTR result;
    UINT_PTR slotWnd;
};

// 代理进程退出码约定（编辑器日志 / 不兼容页展示用）：
//   0  正常（收到 OOPM_SHUTDOWN 后退出）
//   2  LoadLibrary 失败（坏 DLL / 非 PE）
//   3  isUnicode() != TRUE（ANSI 插件拒绝，与进程内策略一致）
//   4  缺关键导出（setInfo / getName / getFuncsArray / isUnicode；
//      messageProc 可选，v2.1 起经 OOPM_MSG 桥接）
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
