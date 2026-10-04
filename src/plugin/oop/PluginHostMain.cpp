// PluginHostMain.cpp — xfsWinPadPluginHost.exe 进程外插件代理（v2 多槽位）。
//
// 职责：一个代理进程承载多个 NPP 兼容插件 DLL。启动后仅建 slot0 控制窗并
// 发 OOPM_READY 报到；编辑器对 slot0 发 OOPM_ADD（DLL 路径 + cookie），
// 代理在 ADD 处理里同步完成 LoadLibrary → setInfo/getFuncsArray → 每插件
// 一个消息窗 → HANDSHAKE 交命令表 → 等编辑器回填 CMDIDS。此后 EXEC/NOTIFY
// 按各自插件窗寻址（路由与 v1 单插件形态完全一致）。
// 插件 setInfo 里 exit()/卡死（看门狗自杀）带走整个代理——编辑器把死亡
// 归因于正在装载的那个，幸存者重进新代理。运行期某插件把代理搞崩则同车
// 连坐（内存换隔离粒度的既定取舍）。
//
// 命令行（v2 不再有 --plugin）：
//   --parent <hwnd>   OopHost 接收窗（READY/HANDSHAKE/REJECT 通道落地端）
//   --npp <hwnd>      编辑器主窗口。⚠ v2.2 起**不再直接交给插件**：插件的
//                     NppData.npp 是本进程的**中转窗**（见下），--npp 只作为
//                     中转窗的转发目标（WM_COPYDATA 等系统封送消息）。
//   --sci <hwnd>      活动文档 Scintilla 句柄。⚠ v2.3 起**不再直接交给插件**：
//                     插件的 NppData.scintillaMain 是本进程的 **SCI 中转窗**
//                     （见下），--sci 只作为它的转发目标（值类型 SCI_* 直发端；
//                     入参/出参指针族走 wire，由编辑器侧转给它）。
//   --deadline <ms>   单次装载看门狗时限（默认 15000）
//
// v2.2（批次 111）中转窗：NPPM_* 编号在 WM_USER 之上 ⇒ Windows 不封送参数。
// 插件若把 NPPM_* 直发编辑器主窗口，lp 是**本进程**的地址，编辑器解引用即
// 访问违例（批次 110 已把"崩"改成"拒答"）。本批把"拒答"改成"过桥"：
// 插件看到的 npp 是本进程的 RelayWndProc 窗，它按 NppmMarshal.h 的分类把
// 调用（含入参串）封送成 OOPM_NPPMCALL，编辑器在**自己的**地址空间里准备
// 缓冲、走既有 ForwardNppMessage，再把结果与出参经 OOPM_NPPMREPLY 送回。
//
// v2.3（批次 112）SCI 通道：scintillaMain 同样换成中转窗。SCI_* 与 NPPM_* 的
// 差别是它**大部分消息根本不带指针**（826 条里 716 条）⇒ 不需要新 wire：
// kValue 直接 SendMessage 给宿主真 Scintilla（没有指针要封送），带指针的按
// SciMarshal.h 的形状表**拒答**（返回 0 + 计数）。形状表由
// scripts/gen-sci-marshal.py 从 Scintilla 自己的 Scintilla.iface 生成，
// 表外编号一律拒答 ⇒ 升级 Scintilla 只会"安全地不可用"。
//
// v2.4（批次 113）SCI 入参族：110 条带指针的里，**入参**那 54 条
// （kInStr 53 + kInBytes 1）从"拒答"升级为"过桥"：中转窗在**本进程**把插件
// 给的串读出来（有界：未终止/越界/不可读一律拒答），内联进 OOPM_SCICALL；
// 编辑器在自己的地址空间里重建指针再发给 Scintilla。参数布局（指针在 wp
// 还是 lp、长度从哪来）也是生成的（SciBridgeTable.inc）—— 54 条里有 19 条的
// 串在 **wParam**、2 条 wp 与 lp 都是串、1 条的首参名 lengthEntered 不是缓冲
// 长度（当长度用会静默截断补全列表）。出参族/结构体族**当时**仍拒答（v2.5–v2.7
// 接上，见 SciBridge.h）；kRawPtr / kPtrRet 语义上不可桥接（**永久**）。
//
// v2.5（批次 114）SCI 出参族：kOutStr 30 条也接上（OOPM_SCIOUTCALL/REPLY）。
// 方向与入参族相反 —— 插件给的是**接收缓冲**，Scintilla 往里写 —— 难点也从
// "搬内容"变成"搬多少"：**容量不在消息里**。唯一的来源是 Scintilla 出参协议
// 自带的 lParam==0 调用（不写缓冲、只返回需要的字节数）。所以中转窗**不做**
// 长度判断，只把 writeBack 标志送过去；编辑器探出 need、在自己的缓冲里真调用
// 一次、校验哨兵、把字节回带，中转窗再用 LocalWritable 验一次插件缓冲再拷。
// 回传字节数由实现的 NUL 语义决定（StringResult 写 len+1 / BytesResult 写 len，
// **只差一个字节**）⇒ 由 SciOutTable.inc 统一给出，两侧不各判一次。
// kInOutStr(5) / kStruct(8) 此时仍拒答（v2.6/v2.7 接上）；TargetAsUTF8 因 NUL 语义
// 随运行期文档编码变化而**必须**拒答。
// ★ 各族划分与"谁拒答"的**唯一来源**是 SciBridge.h 头注释 —— 本文件与
//   OopProtocol.h 只做索引、不复述计数（三处各抄一遍正是它俩曾自相矛盾的原因）。
//
// FuncItem 布局契约（现代 SDK，步长 152）：
//   [0..127]  itemName  wchar_t[64]（内联，偏移 0 即文本）
//   [128..135] func      void(*)()
//   [136..139] cmdID     int（宿主回填）
//   [140]      initCheck bool（+3 填充）
//   [144..151] shortcut  ShortcutKey*（{bool ctrl,alt,shift,uchar key}）
// ★ 批次 121：下面这些偏移**不再手写**，改由 xfs::npp::FuncItem 派生。
//   理由：同一份布局在本文件里原本抄了四遍（步长 + func/cmdID/shortcut 三个
//   偏移），而 NppCompat.h 的结构体已经被 static_assert 钉死、并且由
//   scripts/check-npp-abi-contract.py 对着上游 PluginInterface.h 核对过。
//   从"抄一份"改成"引一份"，这样四处不可能各自漂移。
//
// 法律边界：布局 = 公开契约（插件系统设计笔记 §5.1），实现为 xfsWinPad
// 原创，不含 NPP 源码/头文件。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "OopProtocol.h"
#include "NppmMarshal.h"
#include "SciMarshal.h"
#include "SciBridge.h"
#include "DmmMarshal.h"
#include "../npp/NppPointerGuard.h"
#include "../npp/NppDocking.h"
#include "../npp/NppCompat.h"

// NppData 布局契约镜像（三个连续指针尺寸成员）
struct NppDataWire {
    HWND npp;
    HWND scintillaMain;
    HWND scintillaSecond;
};

typedef void (*VoidFn)(void);
// FuncItem 步长：由已钉死的结构体派生（= 152），不再手写。
static const size_t kStride = sizeof(xfs::npp::FuncItem);

// ---- 每插件槽位状态 ----------------------------------------------------------
struct Slot {
    HMODULE mod = nullptr;
    void (*setInfo)(void*) = nullptr;
    const wchar_t* (*getName)(void) = nullptr;
    void* (*getFuncsArray)(int*) = nullptr;   // 实为 FuncItem* (*)(int*)
    void (*beNotified)(void*) = nullptr;
    LRESULT (*messageProc)(UINT, WPARAM, LPARAM) = nullptr;  // 可选（v2.1 OOPM_MSG 桥）
    void* items = nullptr;                    // FuncItem 数组（152B 步长寻址）
    int itemCount = 0;
    HWND wnd = nullptr;
    std::vector<unsigned char> cmdIds;        // CMDIDS 载荷副本（同步回填）
    bool cmdIdsDone = false;
};

static std::vector<Slot*> g_slots;     // [0]=控制槽（无插件）；vector 存指针保地址稳定
static HWND  g_parent = nullptr;       // OopHost 接收窗（OOP 协议通道）
static HWND  g_npp = nullptr;          // 编辑器主窗口（中转窗的转发目标）
static HWND  g_sci = nullptr;          // NppData.scintillaMain
static DWORD g_deadlineMs = 15000;

// ---- 中转窗：插件看到的 npp（v2.2，批次 111）--------------------------------
// 插件 setInfo 拿到的 nppHandle 从"编辑器主窗口"改成"本进程的中转窗"。
// 插件的 NPPM_* 因此落在**本进程**（指针合法），由 RelayNppm 按
// NppmMarshal.h 的分类封送过桥；编辑器在它自己的地址空间里准备缓冲 ——
// 批次 110 那条"外来指针判据"在过桥路径上永远不会被触发（本批核心断言）。
static HWND     g_relay = nullptr;
static UINT_PTR g_nppmSeq = 0;
static LONG     g_relayBusy = 0;        // 在途调用互斥（Interlocked 单飞）
static unsigned g_relayRefused = 0;     // 拒答计数（不支持/无回包/并发/超长）
static unsigned g_relayBridged = 0;     // 成功过桥计数

// 回包落地槽。g_relayBusy 保证同一时刻只有一次在途调用 ⇒ 单槽足够。
// 回包由 HostWndProc 在 OOPM_NPPMREPLY 里填：那时发起线程正阻塞在自己的
// SendMessage 里，Windows 把 sent message 投递到该线程栈上（CMDIDS 走的
// 也是这条语义，见文件头）。回包落在 slot0 控制窗 —— 它属于代理主线程，
// 而插件命令回调就跑在代理主线程上，两边是同一个线程，不会错配。
struct NppmReplySlot {
    UINT_PTR reqId = 0;
    bool     got = false;
    LONG_PTR result = 0;
    unsigned bytes = 0;
    unsigned char payload[xfs::oop::kNppmPayloadMax];
};
static NppmReplySlot g_reply;

// 插件可见的 npp：中转窗优先。建窗失败则回落到编辑器主窗口 —— 那是
// 批次 110 的行为（不崩，但拿不到数据），比"没有 npp"好。
static HWND NppHandleForPlugins() {
    if (g_relay) return g_relay;
    return g_npp ? g_npp : g_parent;
}

// h 是不是**本进程**的窗口。
// 停靠族（v2.8）的两个方向都要它，理由各不相同但同源：我们一旦把一个
// **本进程的指针**交给某个窗口（WM_NOTIFY 的 NMHDR，或 SetParent 的目标），
// 就必须先确认那个窗口归本进程 —— 否则就是批次 110 修掉的那类跨进程解引用。
//   * 正向（kDmmReg）：hClient 由插件给出 ⇒ 验它，别让插件把别的进程的窗口
//     交给宿主去 SetParent。
//   * 反向（DMMNOTIFY/DMMACTION）：hClient 由编辑器给出 ⇒ 验它，别让编辑器
//     （或伪造的 wire）让我们把栈上 NMHDR 指针交给陌生进程。
static bool IsOwnedWindow(HWND h) {
    if (!h || !::IsWindow(h)) return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    return pid == ::GetCurrentProcessId();
}

// 转发清单：**只收 Windows 自身会封送参数的消息**。
// 理由：转发一个带外来指针的普通消息 = 把批次 110 修掉的崩溃重新引进来。
// WM_COPYDATA 是其中最重要的一条 —— 插件（与我们的测试夹具）用
// SendMessage(npp, WM_COPYDATA, …) 给编辑器送数据，这条今天就是直发宿主
// 窗口的，中转窗必须接住转过去，否则行为回归。
static bool ForwardableToHost(UINT msg) {
    switch (msg) {
    case WM_COPYDATA:        // 系统封送 COPYDATASTRUCT 及其 lpData 缓冲
    case WM_GETTEXT:         // 系统封送接收缓冲
    case WM_SETTEXT:         // 系统封送发送串
    case WM_GETTEXTLENGTH:
        return true;
    default:
        return false;
    }
}

static LRESULT RelayNppm(UINT msg, WPARAM wp, LPARAM lp) {
    namespace oo = xfs::oop;

    const oo::NppmShape shape = oo::NppmShapeOf(msg);
    if (shape == oo::NppmShape::kUnsupported) {
        ++g_relayRefused;      // 表外编号也走这里（"不在表里 = 不过桥"）
        return 0;              // 契约失败值：这一族上游都用 0/FALSE
    }
    // 单飞：并发调用不做队列（要按 reqId 排队出参，收益小、错配风险大），
    // 直接明确拒答。插件命令回调跑在代理主线程 ⇒ 正常路径天然串行。
    if (::InterlockedExchange(&g_relayBusy, 1) != 0) {
        ++g_relayRefused;
        return 0;
    }

    // 入参：把插件**本进程**里的参数读出来内联进载荷。指针在本进程里合法，
    // 但仍一律**有界扫描** —— 插件传了未终止/越界的串不能把代理拖死。
    //   kInWideStr   一个 lp 串（SWITCHTOFILE / DMMVIEWOTHERTAB …）
    //   kDmmReg      lp 指向 DockedWidgetData ⇒ **展平**成 值 + 3 个宽串（v2.8）
    //   kDmmTwoStr   wp 与 lp 各一个串（DMMGETPLUGINHWNDBYNAME，v2.8）
    std::vector<unsigned char> in;
    if (shape == oo::NppmShape::kInWideStr) {
        const wchar_t* s = reinterpret_cast<const wchar_t*>(lp);
        const std::size_t n =
            s ? xfs::npp::LocalWideStrChars(s, oo::kNppmInCharsMax) : 0;
        if (n == 0 || n * sizeof(wchar_t) > oo::kNppmPayloadMax) {
            ++g_relayRefused;
            ::InterlockedExchange(&g_relayBusy, 0);
            return 0;
        }
        in.resize(n * sizeof(wchar_t));
        memcpy(in.data(), s, in.size());
    } else if (shape == oo::NppmShape::kDmmReg) {
        const auto* d = reinterpret_cast<const xfs::npp::DockedWidgetData*>(lp);
        // 先验结构体本身可读，再让 DmmBuildReg 逐个串做有界扫描。
        // ★ 还要验 hClient 归**本进程**：宿主拿到它就会 `SetParent` 进自己的
        //   dock 面板（跨进程 SetParent 实测可行）—— 不验的话，插件能把**别的
        //   应用**的窗口搬进编辑器。这不是"插件是恶意的"，而是"别让一次笔误
        //   变成跨应用窗口绑架"（判据与反向中继那条同一个函数）。
        if (!d || !xfs::npp::LocalReadable(d, sizeof(*d)) ||
            !IsOwnedWindow(d->hClient) || !oo::DmmBuildReg(*d, in)) {
            ++g_relayRefused;
            ::InterlockedExchange(&g_relayBusy, 0);
            return 0;
        }
    } else if (shape == oo::NppmShape::kDmmTwoStr) {
        if (!oo::DmmBuildTwoStr(reinterpret_cast<const wchar_t*>(wp),
                                reinterpret_cast<const wchar_t*>(lp), in)) {
            ++g_relayRefused;
            ::InterlockedExchange(&g_relayBusy, 0);
            return 0;
        }
    }
    // 载荷总上限：DmmReg 的定长部分最坏 32 + 3×4096×2 = 24608；图标段（批次
    // 137）由 DmmBuildReg 按 kDmmIconBytesMax 与"加上后仍 ≤ kNppmPayloadMax"
    // 双重钳制 —— 顶不穿时它**退化为无图标**而不是把整单顶红。所以正常传不
    // 进来；但这是 wire 前的最后一道闸，将来调大上述上限时会先在这里红。
    if (in.size() > oo::kNppmPayloadMax) {
        ++g_relayRefused;
        ::InterlockedExchange(&g_relayBusy, 0);
        return 0;
    }

    const unsigned outCap = oo::NppmOutCapBytes(shape, wp, lp);

    std::vector<unsigned char> call(sizeof(oo::NppmCallWire) + in.size());
    auto* cw = reinterpret_cast<oo::NppmCallWire*>(call.data());
    // ⚠ 协议契约：magic 必须是 kMagic、msg 必须是本消息 id —— 两侧 WndProc
    //   都先校验 wire[0]==kMagic 再按 wire[1] 分派，NPPM_* 编号在 nppm 字段。
    cw->magic = xfs::oop::kMagic;
    cw->msg = xfs::oop::OOPM_NPPMCALL;
    cw->nppm = msg;
    // ★ 值语义（批次 117b）：wp **默认是值**，只有 kDmmTwoStr 把它当指针槽
    //   （wp=窗口名串）⇒ 那一条必须填 0，否则外来地址上 wire。判据由形状推出
    //   （NppmWpIsPointer），与下面 lp 的 NppmLpIsValue 是**相反方向**的默认 ——
    //   原因见 NppmMarshal.h 里那两条判据的注释。
    cw->wParam = oo::NppmWpIsPointer(shape) ? 0 : static_cast<UINT_PTR>(wp);
    cw->reqId = ++g_nppmSeq;
    cw->replyTo = reinterpret_cast<UINT_PTR>(g_slots[0]->wnd);
    cw->inBytes = static_cast<unsigned>(in.size());
    cw->outCap = outCap;
    // ★ 值语义（批次 117）：指针槽一律填 0 —— **外来地址绝不上 wire**；
    //   值槽必须原样透传。kValue 的 lp 也是值，而 wire 原来只有 wParam 一格，
    //   于是 kValue 消息的 lp 被静默换成 0（SETMENUITEMCHECK 恒取消勾选、
    //   GETNBOPENFILES 副视图查询答成主视图…）。判据由形状推出，不靠人记。
    cw->lParam = oo::NppmLpIsValue(shape) ? static_cast<UINT_PTR>(lp) : 0;
    if (!in.empty())
        memcpy(call.data() + sizeof(oo::NppmCallWire), in.data(), in.size());

    g_reply.reqId = cw->reqId;
    g_reply.got = false;
    g_reply.bytes = 0;

    COPYDATASTRUCT cds{};
    cds.dwData = xfs::oop::kMagic;
    cds.cbData = static_cast<DWORD>(call.size());
    cds.lpData = call.data();
    ::SendMessageW(g_parent, WM_COPYDATA,
                   reinterpret_cast<WPARAM>(g_slots[0]->wnd),
                   reinterpret_cast<LPARAM>(&cds));

    if (!g_reply.got || g_reply.reqId != cw->reqId) {
        ++g_relayRefused;      // 编辑器没答（拒绝/忙/已死）：拒答，绝不猜
        ::InterlockedExchange(&g_relayBusy, 0);
        return 0;
    }

    // 出参回填：只写插件给的 lp，长度按**插件声明的容量**再钳一次
    if (lp && g_reply.bytes) {
        const unsigned n = g_reply.bytes < outCap ? g_reply.bytes : outCap;
        unsigned char* dst = reinterpret_cast<unsigned char*>(lp);
        if (n && xfs::npp::LocalWritable(dst, n))
            memcpy(dst, g_reply.payload, n);
        else
            ++g_relayRefused;
    }
    ++g_relayBridged;
    const LRESULT res = static_cast<LRESULT>(g_reply.result);
    ::InterlockedExchange(&g_relayBusy, 0);
    return res;
}

static LRESULT CALLBACK RelayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (xfs::oop::NppmInRanges(msg))
        return RelayNppm(msg, wp, lp);
    if (ForwardableToHost(msg) && g_npp)
        return ::SendMessageW(g_npp, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- SCI 中转窗：插件看到的 scintillaMain（v2.3，批次 112）-------------------
// 与 npp 中转窗**分开一个窗口**：现实里 scintillaMain 与 npp 本来就是两个不同
// 的 HWND，插件可能比较它们、也可能用 sci 当 CreateDialog 的父窗；给同一个
// 句柄会让这类代码走上一条现实中不存在的分支。
//
// 与 NPPM 通道的**关键差别**：SCI_* 有 826 条，其中 716 条**不带指针**，跨进程
// 直发是安全的。所以这里不需要新 wire —— kValue 直接 SendMessage 给宿主真
// Scintilla（没有指针要封送）；带指针的按 SciMarshal.h 的形状表分流：入参族
// （v2.4）与出参族（v2.5）过桥，其余**拒答**。
static HWND     g_sciRelay = nullptr;
static unsigned g_sciForwarded = 0;   // 直发成功计数（"机制被走到过"）
static unsigned g_sciRefused = 0;     // 拒答计数（不可桥接族/表外/指针不可读/无回包）
static unsigned g_sciBridged = 0;     // 入参族成功过桥计数（v2.4）
static unsigned g_sciOutBridged = 0;  // 出参族成功过桥计数（v2.5）
static unsigned g_sciOutProbes = 0;   // 其中"插件自己查长度"（writeBack==0）的次数

// 入参族过桥的接收槽与单飞标志。与 NPPM 的 g_relayBusy/g_reply **分开**：
// 两条通道各自独立，互不阻塞（插件可能在一次 NPPM 回调里再发 SCI_*）。
// 单飞的取舍与 NPPM 一致：并发不做队列，直接明确拒答。
static UINT_PTR g_sciSeq = 0;
static LONG     g_sciBusy = 0;

struct SciReplySlot {
    UINT_PTR reqId = 0;
    bool     got = false;
    LONG_PTR result = 0;
    bool     handled = false;   // 宿主是否真的转给了 Scintilla（与 result 无关）
};
static SciReplySlot g_sciReply;

// 出参族（v2.5）的接收槽与单飞标志，与入参族**各自独立**（同一个插件完全
// 可能在一次回调里先写后读）。回带字节放在定长数组里而不是 vector：回包在
// 窗口过程里被填充，生命周期跨函数；上限定为 kSciPayloadMax + 1（kNul 比
// need 多一个结尾 NUL）。
static UINT_PTR g_sciOutSeq = 0;
static LONG     g_sciOutBusy = 0;

struct SciOutReplySlot {
    UINT_PTR reqId = 0;
    bool     got = false;
    LONG_PTR result = 0;
    bool     handled = false;
    unsigned need = 0;
    unsigned copied = 0;
    unsigned char payload[xfs::oop::kSciPayloadMax + 1];
};
static SciOutReplySlot g_sciOutReply;

// 结构族（v2.7，批次 116）的接收槽与单飞标志，同样**各自独立**（同一个插件
// 完全可能在一次回调里既读文本范围又做查找）。回带字节上限定为 kSciPayloadMax
// （SciStructBytesToWrite 保证 cap ≤ 该值），chrgText 是两路可选回带里的一路。
static unsigned g_sciStructBridged = 0;  // 结构族成功过桥计数（v2.7）
static UINT_PTR g_sciStructSeq = 0;
static LONG     g_sciStructBusy = 0;

struct SciStructReplySlot {
    UINT_PTR reqId = 0;
    bool     got = false;
    LONG_PTR result = 0;
    bool     handled = false;
    unsigned copied = 0;
    bool     hasChrg = false;
    INT_PTR  chrgMin = 0;
    INT_PTR  chrgMax = 0;
    unsigned char payload[xfs::oop::kSciPayloadMax];
};
static SciStructReplySlot g_sciStructReply;

// 插件可见的 scintillaMain：中转窗优先；建窗失败回落宿主真句柄
// （那是批次 110 的行为 —— 不崩，但带指针的 SCI_* 会被宿主拒绝）。
static HWND SciHandleForPlugins() {
    if (g_sciRelay) return g_sciRelay;
    return g_sci ? g_sci : g_npp;
}

// 入参族过桥（v2.4，批次 113）：把插件给的串读出来内联进 OOPM_SCICALL。
// 指针在本进程里合法，但**有界**读取（SciReadInbound 负责）：插件传了未终止
// 的串、越界的长度、指向未映射页的指针，都只能换来"拒答"，不能把代理拖死
// —— 隔离的意义就在这里。
static LRESULT RelaySciBridge(UINT msg, xfs::oop::SciInLayout lay,
                              WPARAM wp, LPARAM lp) {
    namespace oo = xfs::oop;

    std::vector<unsigned char> b1, b2;
    UINT_PTR argWp = 0, argLp = 0;
    if (!oo::SciReadInbound(lay, static_cast<UINT_PTR>(wp),
                            static_cast<UINT_PTR>(lp), b1, b2, argWp, argLp)) {
        ++g_sciRefused;      // 指针不可读 / 界内未终止 / 超长：拒答，绝不猜
        return 0;
    }
    if (!g_parent) {
        ++g_sciRefused;
        return 0;
    }
    if (::InterlockedExchange(&g_sciBusy, 1) != 0) {
        ++g_sciRefused;      // 并发调用不做队列（同 NPPM 的取舍）
        return 0;
    }

    // b1/b2 各带一个尾部 NUL（SciReadInbound 的约定）；wire 里只装**内容**
    // 字节，宿主侧再各自补一个 NUL ⇒ 长度桶的调用方没给终止符也安全。
    const unsigned n1 = b1.empty() ? 0u : static_cast<unsigned>(b1.size() - 1);
    const unsigned n2 = b2.empty() ? 0u : static_cast<unsigned>(b2.size() - 1);

    std::vector<unsigned char> call(sizeof(oo::SciCallWire) + n1 + n2);
    auto* cw = reinterpret_cast<oo::SciCallWire*>(call.data());
    // ⚠ 协议契约：magic/msg 打头，SCI_* 编号在 sciMsg 字段（见 OopProtocol.h）。
    cw->magic = oo::kMagic;
    cw->msg = oo::OOPM_SCICALL;
    cw->sciMsg = msg;
    cw->argWp = argWp;
    cw->argLp = argLp;
    cw->reqId = ++g_sciSeq;
    cw->replyTo = reinterpret_cast<UINT_PTR>(g_slots[0]->wnd);
    cw->inBytes = n1;
    cw->inBytes2 = n2;
    unsigned char* body = call.data() + sizeof(oo::SciCallWire);
    if (n1) memcpy(body, b1.data(), n1);
    if (n2) memcpy(body + n1, b2.data(), n2);

    g_sciReply.reqId = cw->reqId;
    g_sciReply.got = false;
    g_sciReply.result = 0;
    g_sciReply.handled = false;

    COPYDATASTRUCT cds{};
    cds.dwData = oo::kMagic;
    cds.cbData = static_cast<DWORD>(call.size());
    cds.lpData = call.data();
    ::SendMessageW(g_parent, WM_COPYDATA,
                   reinterpret_cast<WPARAM>(g_slots[0]->wnd),
                   reinterpret_cast<LPARAM>(&cds));

    if (!g_sciReply.got || g_sciReply.reqId != cw->reqId) {
        ++g_sciRefused;      // 编辑器没答（忙/已死）：拒答，绝不猜
        ::InterlockedExchange(&g_sciBusy, 0);
        return 0;
    }
    if (!g_sciReply.handled) {
        // 宿主明确拒答（编号不在入参族 / 长度越界 / 没有目标 Scintilla）。
        // 与"Scintilla 真返回 0"必须分开记账 —— 否则两者同形。
        ++g_sciRefused;
        ::InterlockedExchange(&g_sciBusy, 0);
        return 0;
    }
    ++g_sciBridged;
    const LRESULT res = static_cast<LRESULT>(g_sciReply.result);
    ::InterlockedExchange(&g_sciBusy, 0);
    return res;
}

// 出参族过桥（v2.5，批次 114）+ 出入参族（批次 115）：插件给的是**接收缓冲**，
// Scintilla 往里写。容量**不在消息里** ⇒ 中转窗不猜：只把 writeBack 送过去，
// 由编辑器按 Scintilla 自带的 lParam==0 协议探出 need、真调用一次、校验哨兵，
// 再把字节回带。
//
// 出入参族（kInOutStr，批次 115）在此基础上多一件事：wParam 本身是**入参串**
// （形状 (string, stringresult)），所以要把它搬进 wire 载荷、由宿主侧重建。
// 判据是表里的 SciOutNeedsInStr(msg)，**与 lp 是否为 0 无关** —— lParam==0 的
// "查长度"调用同样需要那个属性名（探长度正是插件拿不到容量的唯一出路）。
//
// 拷回插件缓冲前用 LocalWritable 验一次，把"垃圾/空指针"变成拒答而不是访问
// 违例。⚠ 这条检查是**页粒度**的（VirtualQuery 只能回答"这一页可写"），所以
// 它挡不住"缓冲比结果小" —— 那一条 Scintilla 自己也挡不住（GetLine 的注释就
// 写着 Risk of overwriting the end of the buffer）。本桥接**原样保留**那条
// 调用方契约，不假装已经防护。
//
// lp == 0 时走"查长度"分支：插件自己在按协议探容量，返回值就是 need —— 与
// 进程内调用完全同形，一个字节都不写。
static LRESULT RelaySciOut(UINT msg, WPARAM wp, LPARAM lp) {
    namespace oo = xfs::oop;
    const bool writeBack = (lp != 0);

    // ★ 本地就能判定的失败**不留跨进程工作**（与拒答桶同一原则）：接收缓冲根本
    //   不可写时立刻拒答，不产生一次注定被拒的往返。这里只验 1 字节 —— 够判
    //   "这个地址能不能写"；**权威判定**仍是往返之后按回包 copied 精确到字节的
    //   那一次（1 字节可写 ≠ 整个 copied 区间可写）。
    //   ⚠ 这条 early gate 同时是计数口径的一部分：没有它，只读缓冲那两条会各自
    //   产生一次"查长度 + 真写"的往返，把场景 13 的承重计数从 7/65/7 抬成 9/92/9。
    if (writeBack && !xfs::npp::LocalWritable(reinterpret_cast<void*>(lp), 1)) {
        ++g_sciRefused;      // 指针不可写：本地拒答，绝不猜
        return 0;
    }

    // ---- 出入参族（kInOutStr，批次 115）：把插件的入参串搬进 wire 载荷 -------
    // 要不要带串**由表决定**（SciOutNeedsInStr），不是"看 lp 是不是 0"：lParam==0
    // 的查长度调用同样需要 wParam 里的那个属性名。
    //
    // 载荷**含结尾 NUL**（与 SciCallWire 的"只装内容字节、宿主补 NUL"相反，
    // 见 OopProtocol.h 的说明）：这样 inBytes==0 能干净地表示"不带串"，空串则是
    // 有 NUL 的 1 字节，宿主侧还能多验一道"最后一个字节必须是 NUL"。
    // 扫描有界（kSciPayloadMax）：未终止的缓冲在这里变成拒答而不是读穿。
    std::vector<unsigned char> inStr;
    if (oo::SciOutNeedsInStr(msg)) {
        const std::size_t nb = xfs::npp::LocalNarrowStrBytes(
            reinterpret_cast<const char*>(wp), oo::kSciPayloadMax);
        if (nb == 0) {
            ++g_sciRefused;  // 指针不可读 / 界内未终止 / 超长：拒答，绝不猜
            return 0;
        }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(wp);
        inStr.assign(p, p + nb);
    }

    if (!g_parent) {
        ++g_sciRefused;
        return 0;
    }
    if (::InterlockedExchange(&g_sciOutBusy, 1) != 0) {
        ++g_sciRefused;      // 并发不做队列（同入参族/NPPM 的取舍）
        return 0;
    }

    std::vector<unsigned char> call(sizeof(oo::SciOutCallWire) + inStr.size());
    auto* cw = reinterpret_cast<oo::SciOutCallWire*>(call.data());
    // ⚠ 协议契约：magic/msg 打头，SCI_* 编号在 sciMsg 字段（见 OopProtocol.h）。
    cw->magic = oo::kMagic;
    cw->msg = oo::OOPM_SCIOUTCALL;
    cw->sciMsg = msg;
    // 带串时 wParam 由宿主用载荷重建 ⇒ argWp 必须是 0（宿主侧硬校验这条）。
    // 不带串的族维持老语义：argWp 就是那个值语义 wParam。
    cw->argWp = inStr.empty() ? static_cast<UINT_PTR>(wp) : 0;
    cw->reqId = ++g_sciOutSeq;
    cw->replyTo = reinterpret_cast<UINT_PTR>(g_slots[0]->wnd);
    cw->writeBack = writeBack ? 1u : 0u;
    cw->inBytes = static_cast<unsigned>(inStr.size());
    if (!inStr.empty())
        memcpy(call.data() + sizeof(oo::SciOutCallWire), inStr.data(), inStr.size());

    g_sciOutReply.reqId = cw->reqId;
    g_sciOutReply.got = false;
    g_sciOutReply.result = 0;
    g_sciOutReply.handled = false;
    g_sciOutReply.need = 0;
    g_sciOutReply.copied = 0;

    COPYDATASTRUCT cds{};
    cds.dwData = oo::kMagic;
    cds.cbData = static_cast<DWORD>(call.size());
    cds.lpData = call.data();
    ::SendMessageW(g_parent, WM_COPYDATA,
                   reinterpret_cast<WPARAM>(g_slots[0]->wnd),
                   reinterpret_cast<LPARAM>(&cds));

    if (!g_sciOutReply.got || g_sciOutReply.reqId != cw->reqId) {
        ++g_sciRefused;      // 编辑器没答（忙/已死）：拒答，绝不猜
        ::InterlockedExchange(&g_sciOutBusy, 0);
        return 0;
    }
    if (!g_sciOutReply.handled) {
        // 宿主明确拒答（不在出参族 / 拒答桶 TargetAsUTF8 / need 越界 /
        // 哨兵校验不过）。与"Scintilla 真返回 0"必须分开记账 —— 否则同形。
        ++g_sciRefused;
        ::InterlockedExchange(&g_sciOutBusy, 0);
        return 0;
    }

    const LRESULT res = static_cast<LRESULT>(g_sciOutReply.result);
    if (!writeBack) {
        ++g_sciOutBridged;
        ++g_sciOutProbes;    // "插件自己在查长度"也走桥：它证明协议被走到过
        ::InterlockedExchange(&g_sciOutBusy, 0);
        return res;          // = need，与 Scintilla 原语义一致
    }
    const unsigned n = g_sciOutReply.copied;
    if (n == 0) {
        // 空结果（只有 kNoNul 且 need==0）：Scintilla 一个字节都没写，
        // 这里也**不触碰**插件缓冲（空串语义：调用方看到的是原样缓冲）。
        ++g_sciOutBridged;
        ::InterlockedExchange(&g_sciOutBusy, 0);
        return res;
    }
    if (!xfs::npp::LocalWritable(reinterpret_cast<void*>(lp), n)) {
        ++g_sciRefused;      // 指针不可写：拒答，绝不猜
        ::InterlockedExchange(&g_sciOutBusy, 0);
        return 0;
    }
    memcpy(reinterpret_cast<void*>(lp), g_sciOutReply.payload, n);
    ++g_sciOutBridged;
    ::InterlockedExchange(&g_sciOutBusy, 0);
    return res;
}

// 结构族过桥（v2.7，批次 116）：插件给的 lp 是一个**结构体**（TextRange /
// TextToFind …），结构体里**还带指针**。中转窗做两件事：
//   ① 按 sciMsg 的 full 标志把结构体**展平**成 cpMin/cpMax（+ FindText 的 needle）；
//   ② 回包到达后，把字节拷进结构体里的出缓冲，或把 chrgText **条件**回写进结构体。
//
// ★ 代理侧**不 include Scintilla 头**（见 src/CMakeLists.txt「自持契约镜像」），
//   结构体布局取自 SciBridge.h 的 abi::* 局部镜像（尺寸与真 Scintilla.h 对撞过）。
//
// 所有指针访问都**有界**：结构体整体先验可读（Find 族还要验可写）、needle 有界
// 扫描、出缓冲拷回前按 copied 精确验可写。拿不到证明就拒答 —— 原生在这些位置上
// 会直接解引用崩溃（`SCI_FINDTEXT` 传 lp==0 就是 `strlen(nullptr)`），桥接**严格更安全**。
static LRESULT RelaySciStruct(UINT msg, WPARAM wp, LPARAM lp) {
    namespace oo = xfs::oop;

    oo::SciStructKind kind = oo::SciStructKind::kNone;
    bool full = false;
    if (!oo::FindSciStructEntry(msg, kind, full)) {
        ++g_sciRefused;      // 表外：不过桥（安全方向）
        return 0;
    }
    // 拒答桶不该走到这里（RelaySci 已在中转窗拒掉，省一次注定被拒的往返）；
    // 留着作纵深防御。
    if (kind == oo::SciStructKind::kRefuseHandle) {
        ++g_sciRefused;
        return 0;
    }

    // lp 是插件那个结构体：先验"整个结构体可读"（界 = 该族的镜像尺寸）。
    const std::size_t structSize = oo::SciStructSize(kind, full);
    if (structSize == 0 || lp == 0 ||
        !xfs::npp::LocalReadable(reinterpret_cast<const void*>(lp), structSize)) {
        ++g_sciRefused;
        return 0;
    }

    INT_PTR cpMin = 0, cpMax = 0;
    void* outBuf = nullptr;              // 出缓冲（kRangeOut / kStyledOut）
    std::vector<unsigned char> needle;   // kFindInOut 的 needle（含结尾 NUL）
    auto* base = reinterpret_cast<unsigned char*>(lp);

    if (kind == oo::SciStructKind::kFindInOut) {
        // 结构体会被**条件回写**（chrgText）⇒ 还要可写。整结构体的范围是**调用前
        // 就已知**的（= structSize），所以这一次检查就是权威判定，不必等回包
        // —— 与出缓冲不同（那里 copied 要等回包才知道）。
        if (!xfs::npp::LocalWritable(reinterpret_cast<void*>(lp), structSize)) {
            ++g_sciRefused;
            return 0;
        }
        const char* needlePtr = nullptr;
        if (full) {
            auto* ft = reinterpret_cast<const oo::abi::TextToFindFull*>(lp);
            cpMin = ft->chrg.cpMin; cpMax = ft->chrg.cpMax; needlePtr = ft->lpstrText;
        } else {
            auto* ft = reinterpret_cast<const oo::abi::TextToFind*>(lp);
            cpMin = ft->chrg.cpMin; cpMax = ft->chrg.cpMax; needlePtr = ft->lpstrText;
        }
        if (!needlePtr) {
            ++g_sciRefused;      // 原生 strlen(nullptr) 会崩；桥接拒答
            return 0;
        }
        const std::size_t nb =
            xfs::npp::LocalNarrowStrBytes(needlePtr, oo::kSciPayloadMax);
        if (nb == 0) {
            ++g_sciRefused;      // 指针不可读 / 界内未终止 / 超长：拒答，绝不猜
            return 0;
        }
        const auto* p = reinterpret_cast<const unsigned char*>(needlePtr);
        needle.assign(p, p + nb);
    } else {
        if (full) {
            auto* tr = reinterpret_cast<const oo::abi::TextRangeFull*>(lp);
            cpMin = tr->chrg.cpMin; cpMax = tr->chrg.cpMax;
            outBuf = tr->lpstrText;
        } else {
            auto* tr = reinterpret_cast<const oo::abi::TextRange*>(lp);
            cpMin = tr->chrg.cpMin; cpMax = tr->chrg.cpMax;
            outBuf = tr->lpstrText;
        }
        if (!outBuf) {
            ++g_sciRefused;      // 原生会崩；桥接拒答
            return 0;
        }
        // ★ 本地就能判定的失败**不留跨进程工作**（同出参族的 early gate）：出缓冲
        //   根本不可写时立刻拒答。这里只验 1 字节 —— 权威判定仍是回包后按 copied
        //   精确到字节的那一次（1 字节可写 ≠ 整个 copied 区间可写）。
        if (!xfs::npp::LocalWritable(outBuf, 1)) {
            ++g_sciRefused;
            return 0;
        }
    }

    if (!g_parent) {
        ++g_sciRefused;
        return 0;
    }
    if (::InterlockedExchange(&g_sciStructBusy, 1) != 0) {
        ++g_sciRefused;      // 并发不做队列（同入参/出参/NPPM 的取舍）
        return 0;
    }

    std::vector<unsigned char> call(sizeof(oo::SciStructCallWire) + needle.size());
    auto* cw = reinterpret_cast<oo::SciStructCallWire*>(call.data());
    // ⚠ 协议契约：magic/msg 打头，SCI_* 编号在 sciMsg 字段（见 OopProtocol.h）。
    cw->magic = oo::kMagic;
    cw->msg = oo::OOPM_SCISTRUCTCALL;
    cw->sciMsg = msg;
    cw->argWp = static_cast<UINT_PTR>(wp);  // FindText 的 searchFlags；其余族原生也忽略
    cw->reqId = ++g_sciStructSeq;
    cw->replyTo = reinterpret_cast<UINT_PTR>(g_slots[0]->wnd);
    cw->cpMin = cpMin;
    cw->cpMax = cpMax;
    cw->inBytes = static_cast<unsigned>(needle.size());
    if (!needle.empty())
        memcpy(call.data() + sizeof(oo::SciStructCallWire), needle.data(),
               needle.size());

    g_sciStructReply.reqId = cw->reqId;
    g_sciStructReply.got = false;
    g_sciStructReply.result = 0;
    g_sciStructReply.handled = false;
    g_sciStructReply.copied = 0;
    g_sciStructReply.hasChrg = false;
    g_sciStructReply.chrgMin = 0;
    g_sciStructReply.chrgMax = 0;

    COPYDATASTRUCT cds{};
    cds.dwData = oo::kMagic;
    cds.cbData = static_cast<DWORD>(call.size());
    cds.lpData = call.data();
    ::SendMessageW(g_parent, WM_COPYDATA,
                   reinterpret_cast<WPARAM>(g_slots[0]->wnd),
                   reinterpret_cast<LPARAM>(&cds));

    if (!g_sciStructReply.got || g_sciStructReply.reqId != cw->reqId) {
        ++g_sciRefused;      // 编辑器没答（忙/已死）：拒答，绝不猜
        ::InterlockedExchange(&g_sciStructBusy, 0);
        return 0;
    }
    if (!g_sciStructReply.handled) {
        // 宿主明确拒答（表外 / 拒答桶 / wire 契约不过 / 容量算不出 / 写后校验不过 /
        // 没有目标窗）。与"Scintilla 真返回 0"必须分开记账 —— 否则同形。
        ++g_sciRefused;
        ::InterlockedExchange(&g_sciStructBusy, 0);
        return 0;
    }

    const LRESULT res = static_cast<LRESULT>(g_sciStructReply.result);

    // 回带①：出缓冲内容（kRangeOut / kStyledOut）。copied 是**回包才知道**的量，
    // 所以这里才是权威的可写判定（early gate 只验了 1 字节）。
    const unsigned n = g_sciStructReply.copied;
    if (n) {
        if (!outBuf || !xfs::npp::LocalWritable(outBuf, n)) {
            ++g_sciRefused;      // 指针不可写：拒答，绝不猜
            ::InterlockedExchange(&g_sciStructBusy, 0);
            return 0;
        }
        memcpy(outBuf, g_sciStructReply.payload, n);
    }

    // 回带②：chrgText（kFindInOut，**只在宿主说"真的写了"时才回写**）。
    // ★ hasChrg 是必要的区分位：没找到时 Scintilla 一个字节都不写，代理侧若照写
    //   就会把插件结构体里**原有的** chrgText 覆盖成 (0,0) —— "没找到"与"找到了
    //   空匹配"就分不开了。
    if (g_sciStructReply.hasChrg && kind == oo::SciStructKind::kFindInOut) {
        if (full) {
            auto* cr = reinterpret_cast<oo::abi::CharRangeFull*>(
                base + offsetof(oo::abi::TextToFindFull, chrgText));
            cr->cpMin = g_sciStructReply.chrgMin;
            cr->cpMax = g_sciStructReply.chrgMax;
        } else {
            auto* cr = reinterpret_cast<oo::abi::CharRange*>(
                base + offsetof(oo::abi::TextToFind, chrgText));
            cr->cpMin = static_cast<long>(g_sciStructReply.chrgMin);
            cr->cpMax = static_cast<long>(g_sciStructReply.chrgMax);
        }
    }

    ++g_sciStructBridged;
    ::InterlockedExchange(&g_sciStructBusy, 0);
    return res;
}

static LRESULT RelaySci(UINT msg, WPARAM wp, LPARAM lp) {
    // 结构族优先判（v2.7）：它与前三族 / kValue 不相交（形状表保证），先判只是
    // 为了可读性。
    // ⚠ 这里用 SciStructRelayable（而不是 KindOf != kNone）来分流：拒答桶
    //   （FormatRange / FormatRangeFull）在**中转窗**就拒掉，不产生一次注定被拒的
    //   跨进程往返。判据仍是**两侧共用的那张表** ⇒ 不是"两处各判一次"。宿主侧
    //   同一道闸保留作纵深防御（SciBridgeStructCall 里再判一次）。
    if (xfs::oop::SciStructRelayable(msg))
        return RelaySciStruct(msg, wp, lp);
    if (xfs::oop::SciStructKindOf(msg) != xfs::oop::SciStructKind::kNone) {
        ++g_sciRefused;      // 拒答桶：进程私有 HDC，语义上不可桥接
        return 0;
    }

    // 出参族优先判（v2.5）：它们与入参族 / kValue 不相交（形状表保证），
    // 先判只是为了可读性。
    // ⚠ 这里用 SciOutRelayable（而不是 KindOf != kNone）来分流：拒答桶
    //   （TargetAsUTF8 / GetTag）在**中转窗**就拒掉，不产生一次注定被拒的跨进程
    //   往返。判据仍是**两侧共用的那张表**（SciOutRelayable 读的就是 kSciOutTable）
    //   ⇒ 不是"两处各判一次"。宿主侧同一道闸保留作纵深防御（SciBridgeOutCall
    //   里再判一次），并有独立单测钉它（test_sci_out.cpp）。
    if (xfs::oop::SciOutRelayable(msg))
        return RelaySciOut(msg, wp, lp);
    if (xfs::oop::SciOutKindOf(msg) != xfs::oop::SciOutKind::kNone) {
        ++g_sciRefused;      // 拒答桶：表里明确不可桥接
        return 0;
    }

    // 入参族优先判：它们与 kValue 不相交（形状表保证），但先判可读性更好。
    const xfs::oop::SciInLayout lay = xfs::oop::SciInLayoutOf(msg);
    if (lay != xfs::oop::SciInLayout::kNone)
        return RelaySciBridge(msg, lay, wp, lp);

    // 表外编号也走这里：不在表里 = 不过桥（安全方向）。
    if (!xfs::oop::SciForwardable(msg)) {
        ++g_sciRefused;
        return 0;      // 契约失败值：SCI_* 上游统一按 0 处理
    }
    if (!g_sci) {      // 启动时没有活动文档（--sci 0）
        ++g_sciRefused;
        return 0;
    }
    ++g_sciForwarded;
    return ::SendMessageW(g_sci, msg, wp, lp);
}

static LRESULT CALLBACK SciRelayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (xfs::oop::SciInRange(msg))
        return RelaySci(msg, wp, lp);
    // 与 npp 中转窗同一条转发清单，但目标换成 Scintilla：WM_GETTEXT /
    // WM_GETTEXTLENGTH 是读编辑器文本的常用路，系统会封送接收缓冲。
    if (ForwardableToHost(msg) && g_sci)
        return ::SendMessageW(g_sci, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- SEH 包裹（无 C++ 对象，规避 MSVC C2712）--------------------------------
static int CallSetInfoSeh(Slot* s, void* data) {
    __try {
        s->setInfo(data);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static void* CallGetFuncsArraySeh(Slot* s, int* count) {
    __try {
        return s->getFuncsArray(count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool CallFuncSeh(Slot* s, int index) {
    void* fn = *reinterpret_cast<void**>(reinterpret_cast<char*>(s->items) +
                                         (size_t)index * kStride +
                                         offsetof(xfs::npp::FuncItem, func));
    __try {
        reinterpret_cast<VoidFn>(fn)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallNotifySeh(Slot* s, void* scn) {
    __try {
        s->beNotified(scn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// messageProc（v2.1 桥）：崩溃按 NPP「未处理」语义回 0，代理不自杀。
static LRESULT CallMessageProcSeh(Slot* s, UINT m, WPARAM w, LPARAM l) {
    __try {
        return s->messageProc(m, w, l);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- 装载看门狗：单次 ADD 的 setInfo/getFuncsArray/握手卡死则代理自杀 -------
// ADD 全程串行（代理主线程同步处理），单事件即可；时限由 AddWire 逐次携带。
static HANDLE g_addStopEvent = nullptr;

static DWORD WINAPI WatchdogThread(LPVOID param) {
    DWORD ms = static_cast<DWORD>(reinterpret_cast<uintptr_t>(param));
    if (::WaitForSingleObject(g_addStopEvent, ms) == WAIT_OBJECT_0)
        return 0;                                  // 装载完成，正常撤销
    ::ExitProcess(xfs::oop::kExitWatchdog);
}

// ---- 协议回带小工具 -----------------------------------------------------------
static void SendToParent(UINT_PTR msgId, const void* data, DWORD size) {
    COPYDATASTRUCT cds{};
    cds.dwData = xfs::oop::kMagic;
    cds.cbData = size;
    cds.lpData = const_cast<void*>(data);
    ::SendMessageW(g_parent, WM_COPYDATA, reinterpret_cast<WPARAM>(g_slots[0]->wnd),
                   reinterpret_cast<LPARAM>(&cds));
}

static void SendReject(UINT_PTR cookie, UINT_PTR reason) {
    xfs::oop::RejectWire rj{};
    rj.magic = xfs::oop::kMagic;
    rj.msg = xfs::oop::OOPM_REJECT;
    rj.cookie = cookie;
    rj.reason = reason;
    SendToParent(xfs::oop::OOPM_REJECT, &rj, sizeof(rj));
}

// ---- 槽位消息窗（slot0 与插件槽共用同一个 WndProc，按 GWLP_USERDATA 分流）----
static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// ADD 处理：在 slot0 的 wndproc 内同步完成一个插件的装载+握手。
// 期间 HANDSHAKE 的 SendMessage 阻塞等待编辑器处理，而编辑器会在同一窗口期
// 对插件槽窗回发 CMDIDS——阻塞方照常被投递 incoming sent messages（v1 启动
// 序列即依赖此语义），故无死锁。
static BOOL HandleAdd(xfs::oop::AddWire* aw) {
    UINT_PTR cookie = aw->cookie;
    auto* s = new Slot();

    s->mod = ::LoadLibraryW(aw->path);
    if (!s->mod) { SendReject(cookie, xfs::oop::kExitLoadFail); delete s; return TRUE; }

    s->setInfo       = reinterpret_cast<void(*)(void*)>(::GetProcAddress(s->mod, "setInfo"));
    s->getName       = reinterpret_cast<const wchar_t* (*)()>(::GetProcAddress(s->mod, "getName"));
    s->getFuncsArray = reinterpret_cast<void* (*)(int*)>(::GetProcAddress(s->mod, "getFuncsArray"));
    s->beNotified    = reinterpret_cast<void(*)(void*)>(::GetProcAddress(s->mod, "beNotified"));
    auto isUnicode   = reinterpret_cast<BOOL(*)()>(::GetProcAddress(s->mod, "isUnicode"));
    s->messageProc   = reinterpret_cast<LRESULT(*)(UINT, WPARAM, LPARAM)>(
                           ::GetProcAddress(s->mod, "messageProc"));
    // messageProc 可选（v2.1 起经 OOPM_MSG 桥接；缺省按未处理回 0），不要求导出。
    if (!s->setInfo || !s->getName || !s->getFuncsArray || !isUnicode) {
        ::FreeLibrary(s->mod);
        SendReject(cookie, xfs::oop::kExitExport);
        delete s;
        return TRUE;
    }
    if (isUnicode() != TRUE) {
        ::FreeLibrary(s->mod);
        SendReject(cookie, xfs::oop::kExitAnsi);
        delete s;
        return TRUE;
    }

    // 槽窗先建：HANDSHAKE 阻塞期间编辑器要按此 HWND 回发 CMDIDS
    s->wnd = ::CreateWindowExW(0, L"xfsWinPadPluginHostWnd", L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr,
                               ::GetModuleHandleW(nullptr), s);
    if (!s->wnd) {
        ::FreeLibrary(s->mod);
        SendReject(cookie, xfs::oop::kExitLoadFail);
        delete s;
        return TRUE;
    }
    g_slots.push_back(s);

    // 看门狗覆盖 setInfo + getFuncsArray + 握手全程（卡死 = 自杀，
    // 编辑器死亡归因会把本插件隔离、幸存者重进新代理）
    const DWORD addMs =
        aw->deadlineMs ? static_cast<DWORD>(aw->deadlineMs) : g_deadlineMs;
    HANDLE watchdog = nullptr;
    g_addStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_addStopEvent)
        watchdog = ::CreateThread(
            nullptr, 0, WatchdogThread,
            reinterpret_cast<LPVOID>(static_cast<uintptr_t>(addMs)), 0, nullptr);

    NppDataWire data{ NppHandleForPlugins(), SciHandleForPlugins(), nullptr };
    if (CallSetInfoSeh(s, &data) != 0) {
        // setInfo 抛异常：进程状态存疑，整进程陪葬（编辑器归因装载者）
        if (watchdog) ::WaitForSingleObject(watchdog, 500);
        ::ExitProcess(xfs::oop::kExitFault);
    }
    int n = 0;
    void* items = CallGetFuncsArraySeh(s, &n);
    if (!items || n < 0 || n > xfs::oop::kHandshakeItemsMax) {
        if (g_addStopEvent) ::SetEvent(g_addStopEvent);
        if (watchdog) { ::WaitForSingleObject(watchdog, 500); ::CloseHandle(watchdog); }
        if (g_addStopEvent) { ::CloseHandle(g_addStopEvent); g_addStopEvent = nullptr; }
        // 表无效但进程未坏：撤槽（不 FreeLibrary——DllMain 可能已生线程）
        ::DestroyWindow(s->wnd);
        s->wnd = nullptr;
        std::vector<Slot*> kept;
        for (auto* p : g_slots) if (p != s) kept.push_back(p);
        g_slots.swap(kept);
        delete s;
        SendReject(cookie, xfs::oop::kExitExport);
        return TRUE;
    }
    s->items = items;
    s->itemCount = n;

    xfs::oop::HandshakeWire hs{};
    hs.magic = xfs::oop::kMagic;
    hs.msg = xfs::oop::OOPM_HANDSHAKE;
    hs.itemCount = n;
    hs.cookie = cookie;
    hs.slotWnd = reinterpret_cast<UINT_PTR>(s->wnd);
    const wchar_t* pname = s->getName();
    wcsncpy_s(hs.pluginName, pname && pname[0] ? pname : L"(unnamed)", _TRUNCATE);
    for (int i = 0; i < n; ++i) {
        const unsigned char* base =
            reinterpret_cast<const unsigned char*>(items) + (size_t)i * kStride;
        wcsncpy_s(hs.items[i].name, reinterpret_cast<const wchar_t*>(base), 63);
        const void* skPtr =
            *reinterpret_cast<void* const*>(
                base + offsetof(xfs::npp::FuncItem, shortcut));   // shortcut 指针
        unsigned char sk[4] = {};
        if (skPtr) memcpy(sk, skPtr, 4);                   // {ctrl, alt, shift, key}
        hs.items[i].ctrl = sk[0];
        hs.items[i].alt = sk[1];
        hs.items[i].shift = sk[2];
        hs.items[i].key = sk[3];
    }
    SendToParent(xfs::oop::OOPM_HANDSHAKE, &hs, sizeof(hs));

    // 回填 cmdID（编辑器处理握手时同步回发 CMDIDS，已投递到本槽窗）
    if (s->cmdIdsDone && s->cmdIds.size() >= sizeof(xfs::oop::CmdIdsWire)) {
        auto* ids = reinterpret_cast<const xfs::oop::CmdIdsWire*>(s->cmdIds.data());
        const int* arr = reinterpret_cast<const int*>(ids + 1);
        int cnt = ids->count < s->itemCount ? ids->count : s->itemCount;
        for (int i = 0; i < cnt; ++i)
            *reinterpret_cast<int*>(reinterpret_cast<char*>(s->items) +
                                    (size_t)i * kStride +
                                    offsetof(xfs::npp::FuncItem, cmdID)) = arr[i];
    }

    if (g_addStopEvent) ::SetEvent(g_addStopEvent);
    if (watchdog) { ::WaitForSingleObject(watchdog, 2000); ::CloseHandle(watchdog); }
    if (g_addStopEvent) { ::CloseHandle(g_addStopEvent); g_addStopEvent = nullptr; }
    return TRUE;
}

// ---- 停靠族反向中继（v2.8，批次 117）---------------------------------------
// 编辑器 → 代理：请代理在**它自己的进程里**把 WM_NOTIFY / DMM_* 发给插件对话框。
//
// ★ 为什么必须绕这一圈（实测，展开见 OopProtocol.h 的枚举注释）：
//   · 跨进程 `WM_NOTIFY` 被系统拒绝（err=5 ACCESS_DENIED，且 `lParam=0` 照样拒
//     ⇒ 与指针无关）——而 DockManager 的停靠通知全走 `WM_NOTIFY`；
//   · 直发 `DMM_*`（`WM_USER` 段）虽能送达，但会让编辑器 UI 线程去等一个可能
//     不泵消息的插件线程（挂起风险）。
// 归属判据（IsOwnedWindow）见上面的注释。
static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* slot = reinterpret_cast<Slot*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_COPYDATA) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds || cds->dwData != xfs::oop::kMagic || !cds->lpData) return FALSE;
        if (cds->cbData < sizeof(UINT_PTR) * 2) return FALSE;
        auto* wire = reinterpret_cast<const UINT_PTR*>(cds->lpData);
        if (wire[0] != xfs::oop::kMagic) return FALSE;
        switch (wire[1]) {
            case xfs::oop::OOPM_ADD: {
                if (cds->cbData < sizeof(xfs::oop::AddWire) || !slot ||
                    slot->itemCount != 0 || slot->mod)   // 仅 slot0 受理
                    return FALSE;
                auto* aw = reinterpret_cast<xfs::oop::AddWire*>(cds->lpData);
                aw->path[xfs::oop::kAddPathMax - 1] = L'\0';   // 信任边界钳制
                return HandleAdd(aw);
            }
            case xfs::oop::OOPM_CMDIDS: {
                if (!slot || cds->cbData < sizeof(xfs::oop::CmdIdsWire)) return FALSE;
                slot->cmdIds.assign(reinterpret_cast<const unsigned char*>(cds->lpData),
                                    reinterpret_cast<const unsigned char*>(cds->lpData) +
                                        cds->cbData);
                slot->cmdIdsDone = true;
                return TRUE;
            }
            case xfs::oop::OOPM_EXEC: {
                if (!slot || cds->cbData < sizeof(xfs::oop::ExecWire)) return FALSE;
                auto* ex = reinterpret_cast<const xfs::oop::ExecWire*>(cds->lpData);
                if (ex->index < 0 || ex->index >= slot->itemCount) return FALSE;
                return CallFuncSeh(slot, ex->index) ? TRUE : FALSE;
            }
            case xfs::oop::OOPM_NOTIFY: {
                if (!slot || !slot->beNotified) return TRUE;
                if (cds->cbData < sizeof(xfs::oop::ExecWire)) return FALSE;
                auto* nt = reinterpret_cast<const xfs::oop::ExecWire*>(cds->lpData);
                // SCNotification 布局：nmhdr 三件套打头，尾部整体置零——插件
                // 读到的额外字段全部为 0（与宿主进程内合成路径一致）。
                // 尾部 160B ≥ 真实 SCNotification 全长。
                static const size_t kScnSize = 24 + 160;
                unsigned char scn[kScnSize] = {};
                void** h = reinterpret_cast<void**>(scn);
                // hwndFrom = 插件眼里的"编辑器窗口" = 中转窗（插件会拿它与
                // setInfo 收到的 npp 比对，两者必须同值）
                h[0] = reinterpret_cast<void*>(NppHandleForPlugins());
                h[1] = reinterpret_cast<void*>(nt->idFrom);        // idFrom
                *reinterpret_cast<unsigned int*>(scn + 16) = nt->code;
                return CallNotifySeh(slot, scn) ? TRUE : FALSE;
            }
            case xfs::oop::OOPM_SHUTDOWN:
                // 注意：SHUTDOWN 通知已随 Notify 广播送达插件，这里只退出，
                // 不二次回调 beNotified。
                ::PostQuitMessage(0);
                return TRUE;
            case xfs::oop::OOPM_MSG: {
                // messageProc 桥（v2.1）：仅编辑器白名单（值类型参数）可达此处。
                // 插件卡死 → 本消息处理不返回 → 编辑器侧 SMTO 超时自行放弃。
                if (!slot || !slot->mod ||
                    cds->cbData < sizeof(xfs::oop::MsgWire)) return FALSE;
                auto* mw = reinterpret_cast<const xfs::oop::MsgWire*>(cds->lpData);
                LRESULT res = 0;
                if (slot->messageProc)
                    res = CallMessageProcSeh(slot, static_cast<UINT>(mw->wndMsg),
                                             static_cast<WPARAM>(mw->wParam),
                                             static_cast<LPARAM>(mw->lParam));
                xfs::oop::MsgReplyWire rp{};
                rp.magic = xfs::oop::kMagic;
                rp.msg = xfs::oop::OOPM_MSGREPLY;
                rp.reqId = mw->reqId;
                rp.wndMsg = mw->wndMsg;
                rp.result = res;
                rp.slotWnd = reinterpret_cast<UINT_PTR>(slot->wnd);
                SendToParent(xfs::oop::OOPM_MSGREPLY, &rp, sizeof(rp));
                return TRUE;
            }
            case xfs::oop::OOPM_NPPMREPLY: {
                // NPPM 过桥回包（v2.2）：编辑器对中转窗发起的调用给出结果。
                // 落点是本窗（slot0），但**处理它的线程未必是发起调用的线程**
                // ——所以这里只把结果放进全局槽，由 RelayNppm 在 SendMessage
                // 返回后自取；reqId 对账把"过期/串包"挡在外面。
                if (cds->cbData < sizeof(xfs::oop::NppmReplyWire)) return FALSE;
                auto* rp = reinterpret_cast<const xfs::oop::NppmReplyWire*>(cds->lpData);
                if (rp->magic != xfs::oop::kMagic) return FALSE;
                if (rp->reqId != g_reply.reqId || g_reply.got)
                    return TRUE;                   // 过期 / 重复：静默丢弃
                const unsigned maxOut =
                    static_cast<unsigned>(cds->cbData - sizeof(*rp));
                unsigned n = rp->outBytes < maxOut ? rp->outBytes : maxOut;
                if (n > xfs::oop::kNppmPayloadMax) n = xfs::oop::kNppmPayloadMax;
                if (n)
                    memcpy(g_reply.payload,
                           reinterpret_cast<const unsigned char*>(rp + 1), n);
                g_reply.bytes = n;
                g_reply.result = rp->result;
                g_reply.got = true;
                return TRUE;
            }
            case xfs::oop::OOPM_DMMRELAY: {
                // 停靠中继（v2.8，批次 117）：编辑器请代理在**本进程内**把一条
                // 消息发给插件对话框。两种 kind 合并走一条 wire，见 OopProtocol.h。
                //
                // hwndFrom 恒 = 插件眼里的"编辑器窗口" = 本进程中转窗
                // （与 OOPM_NOTIFY 的 SCNotification.nmhdr 同口径 —— 插件会拿它
                //  与 setInfo 收到的 npp 比对，两者必须同值）。
                //
                // ★ delivered 必须回带：DMN_CLOSE 的 **veto** 语义是"插件把
                //   WM_NOTIFY 的结果置 TRUE ⇒ 别关"，而"归属验不过（什么都没发）"
                //   时 result 也是 0 —— 两者同形，只有 delivered 能分开
                //   （批次 112 的教训：判据要落在"对方是否收到"上）。
                if (cds->cbData < sizeof(xfs::oop::DmmRelayWire)) return FALSE;
                auto* rw = reinterpret_cast<const xfs::oop::DmmRelayWire*>(cds->lpData);
                HWND target = reinterpret_cast<HWND>(rw->hClient);
                const bool delivered = IsOwnedWindow(target);
                LRESULT res = 0;
                if (delivered) {
                    if (rw->kind == xfs::oop::kDmmRelayNotify) {
                        NMHDR nm{};
                        nm.hwndFrom = NppHandleForPlugins();
                        nm.idFrom = rw->idFrom;
                        nm.code = rw->code;
                        res = ::SendMessageW(target, WM_NOTIFY,
                                             static_cast<WPARAM>(rw->idFrom),
                                             reinterpret_cast<LPARAM>(&nm));
                    } else {
                        // DMM_* 动作请求：wp/lp 恒 0（见 DmmRelayWire 注释）
                        ::SendMessageW(target, static_cast<UINT>(rw->action), 0, 0);
                    }
                }
                xfs::oop::DmmRelayReplyWire rp{};
                rp.magic = xfs::oop::kMagic;
                rp.msg = xfs::oop::OOPM_DMMRELAYREPLY;
                rp.reqId = rw->reqId;
                rp.delivered = delivered ? 1u : 0u;
                rp.result = res;
                SendToParent(xfs::oop::OOPM_DMMRELAYREPLY, &rp, sizeof(rp));
                return TRUE;
            }
            case xfs::oop::OOPM_SCIREPLY: {
                // SCI 入参过桥回包（v2.4）：与 NPPMREPLY 同构 —— 落点是本窗
                // （slot0），但处理它的线程未必是发起调用的线程，所以只把结果
                // 放进全局槽，由 RelaySciBridge 在 SendMessage 返回后自取；
                // reqId 对账把"过期/串包"挡在外面。
                if (cds->cbData < sizeof(xfs::oop::SciReplyWire)) return FALSE;
                auto* rp = reinterpret_cast<const xfs::oop::SciReplyWire*>(cds->lpData);
                if (rp->magic != xfs::oop::kMagic) return FALSE;
                if (rp->reqId != g_sciReply.reqId || g_sciReply.got)
                    return TRUE;                   // 过期 / 重复：静默丢弃
                g_sciReply.result = rp->result;
                g_sciReply.handled = (rp->handled != 0);
                g_sciReply.got = true;
                return TRUE;
            }
            case xfs::oop::OOPM_SCIOUTREPLY: {
                // SCI 出参过桥回包（v2.5）：与 SCIReply 同构，多一段**回带
                // 字节**的载荷。同样只落全局槽，由 RelaySciOut 在 SendMessage
                // 返回后自取（处理本回包的线程未必是发起调用的线程）。
                if (cds->cbData < sizeof(xfs::oop::SciOutReplyWire)) return FALSE;
                auto* rp = reinterpret_cast<const xfs::oop::SciOutReplyWire*>(cds->lpData);
                if (rp->magic != xfs::oop::kMagic) return FALSE;
                if (rp->reqId != g_sciOutReply.reqId || g_sciOutReply.got)
                    return TRUE;                   // 过期 / 重复：静默丢弃
                // 载荷是外来数据：按实际长度、按上限、再按 need+1 三道钳。
                const unsigned maxOut =
                    static_cast<unsigned>(cds->cbData - sizeof(*rp));
                unsigned n = rp->copied < maxOut ? rp->copied : maxOut;
                if (n > xfs::oop::kSciPayloadMax + 1) n = xfs::oop::kSciPayloadMax + 1;
                if (n)
                    memcpy(g_sciOutReply.payload,
                           reinterpret_cast<const unsigned char*>(rp + 1), n);
                g_sciOutReply.copied = n;
                g_sciOutReply.need = rp->need;
                g_sciOutReply.result = rp->result;
                g_sciOutReply.handled = (rp->handled != 0);
                g_sciOutReply.got = true;
                return TRUE;
            }
            case xfs::oop::OOPM_SCISTRUCTREPLY: {
                // 结构族过桥回包（v2.7）：与 SCIOUTREPLY 同构，多两路**可选**回带
                // （copied 字节 / chrgText）。同样只落全局槽，由 RelaySciStruct 在
                // SendMessage 返回后自取（处理本回包的线程未必是发起调用的线程）。
                if (cds->cbData < sizeof(xfs::oop::SciStructReplyWire)) return FALSE;
                auto* rp = reinterpret_cast<const xfs::oop::SciStructReplyWire*>(
                    cds->lpData);
                if (rp->magic != xfs::oop::kMagic) return FALSE;
                if (rp->reqId != g_sciStructReply.reqId || g_sciStructReply.got)
                    return TRUE;                   // 过期 / 重复：静默丢弃
                // 载荷是外来数据：按实际长度、再按上限两道钳。
                const unsigned maxOut =
                    static_cast<unsigned>(cds->cbData - sizeof(*rp));
                unsigned n = rp->copied < maxOut ? rp->copied : maxOut;
                if (n > xfs::oop::kSciPayloadMax) n = xfs::oop::kSciPayloadMax;
                if (n)
                    memcpy(g_sciStructReply.payload,
                           reinterpret_cast<const unsigned char*>(rp + 1), n);
                g_sciStructReply.copied = n;
                g_sciStructReply.hasChrg = (rp->hasChrg != 0);
                g_sciStructReply.chrgMin = rp->chrgMin;
                g_sciStructReply.chrgMax = rp->chrgMax;
                g_sciStructReply.result = rp->result;
                g_sciStructReply.handled = (rp->handled != 0);
                g_sciStructReply.got = true;
                return TRUE;
            }
            default:
                return FALSE;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 参数解析 ---------------------------------------------------------------
static bool ArgValue(const wchar_t* const* argv, int argc, int& i,
                     const wchar_t* name, std::wstring& out) {
    if (wcscmp(argv[i], name) != 0) return false;
    if (i + 1 >= argc) ::ExitProcess(xfs::oop::kExitExport);
    out = argv[++i];
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    const wchar_t* const* argv =
        reinterpret_cast<const wchar_t* const*>(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (!argv) return xfs::oop::kExitExport;

    std::wstring parentStr, nppStr, sciStr, deadlineStr;
    for (int i = 1; i < argc; ++i) {
        if (ArgValue(argv, argc, i, L"--parent", parentStr)) continue;
        if (ArgValue(argv, argc, i, L"--npp", nppStr)) continue;
        if (ArgValue(argv, argc, i, L"--sci", sciStr)) continue;
        if (ArgValue(argv, argc, i, L"--deadline", deadlineStr)) continue;
    }
    if (parentStr.empty())
        return xfs::oop::kExitExport;
    g_parent = reinterpret_cast<HWND>(wcstoull(parentStr.c_str(), nullptr, 16));
    g_npp = reinterpret_cast<HWND>(wcstoull(nppStr.c_str(), nullptr, 16));
    g_sci = reinterpret_cast<HWND>(wcstoull(sciStr.c_str(), nullptr, 16));
    if (!deadlineStr.empty())
        g_deadlineMs = static_cast<DWORD>(wcstoul(deadlineStr.c_str(), nullptr, 10));

    // 坏 DLL 不得弹「损坏的映像」硬错误框拖住后台
    ::SetErrorMode(SEM_FAILCRITICALERRORS);

    // slot0 控制窗：编辑器 ADD/SHUTDOWN 的落地端；HWND 经 OOPM_READY 报到
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"xfsWinPadPluginHostWnd";
    ::RegisterClassExW(&wc);
    auto* ctl = new Slot();               // slot0 无插件：仅占位承载 GWLP_USERDATA
    HWND self = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, ctl);
    if (!self) return xfs::oop::kExitLoadFail;
    ctl->wnd = self;
    g_slots.push_back(ctl);

    // ---- 中转窗（v2.2）：插件 setInfo 拿到的 nppHandle -----------------------
    // 不用 HWND_MESSAGE —— message-only 窗不能当可见对话框的 owner，插件
    // 用 npp 做 CreateDialog/MessageBox 的父窗时对话框会跟着"不可见"。
    // 这里用 0x0 的**可见** toolwindow：不渲染像素、不进任务栏
    // （WS_EX_TOOLWINDOW）、不抢焦点（WS_EX_NOACTIVATE），但作为 owner 是
    // 可见的，插件的模态对话框照常显示。
    WNDCLASSEXW rwc{};
    rwc.cbSize = sizeof(rwc);
    rwc.lpfnWndProc = RelayWndProc;
    rwc.hInstance = ::GetModuleHandleW(nullptr);
    rwc.lpszClassName = L"xfsWinPadNppRelayWnd";
    ::RegisterClassExW(&rwc);
    g_relay = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                rwc.lpszClassName, L"", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, rwc.hInstance, nullptr);
    if (g_relay) ::ShowWindow(g_relay, SW_SHOWNOACTIVATE);

    // ---- SCI 中转窗（v2.3，批次 112）：插件 setInfo 拿到的 scintillaMain ----
    // 同样的 0x0 可见 toolwindow 形态（插件会拿它当对话框 owner）。
    WNDCLASSEXW swc{};
    swc.cbSize = sizeof(swc);
    swc.lpfnWndProc = SciRelayWndProc;
    swc.hInstance = ::GetModuleHandleW(nullptr);
    swc.lpszClassName = L"xfsWinPadSciRelayWnd";
    ::RegisterClassExW(&swc);
    g_sciRelay = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   swc.lpszClassName, L"", WS_POPUP,
                                   0, 0, 0, 0, nullptr, nullptr, swc.hInstance, nullptr);
    if (g_sciRelay) ::ShowWindow(g_sciRelay, SW_SHOWNOACTIVATE);

    UINT_PTR ready[2] = { xfs::oop::kMagic, xfs::oop::OOPM_READY };
    SendToParent(xfs::oop::OOPM_READY, ready, sizeof(ready));

    // ---- 消息泵：ADD/EXEC/NOTIFY/SHUTDOWN 由 HostWndProc 按槽位处理 ----------
    MSG m;
    while (::GetMessageW(&m, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
    return xfs::oop::kExitClean;
}
