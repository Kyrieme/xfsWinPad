#pragma once
// NppmMarshal.h — 进程外插件的 NPPM_* 指针参数「可封送性」分类（两侧唯一事实源）。
//
// 背景（批次 110）：NPPM_* 编号全在 WM_USER 之上，而 Windows **只对 WM_USER
// 以下的部分消息、WM_COPYDATA、WM_GETTEXT/WM_SETTEXT 等做参数封送** ⇒ 超过
// WM_USER 的消息，wp/lp 按位原样传递。进程外插件若把 NPPM_* 直接发给宿主主
// 窗口，lp 里装的是**代理进程地址空间**的地址，宿主解引用 = 访问违例（那些
// 分支外面没有 SEH）⇒ 宿主当场死亡。批次 110 的对策是「拒答」：能崩的变成
// 拿不到数据（返回契约失败值）。本文件服务于批次 111 的「过桥」：插件看到的
// npp 变成**代理进程里的中转窗**，中转窗按本表把调用封送过桥，宿主在**自己**
// 的地址空间里准备缓冲 —— 于是既不崩，也真的拿到数据。
//
// 本表是代理侧（中转窗）与宿主侧（OopHost::HandleNppmCall）**共用的唯一分类**。
// 两侧各写一份必然漂移，而漂移的症状是「某些消息静默返回 0」—— 所以用
// 「表 + 完备性守卫」钉死：NppMessages.h 里声明的每一条 NPPM_* 都必须在本表
// 出现（受支持的给形状，不受支持的给形状 + 原因）。守卫在
// tests/test_nppm_marshal.cpp，缺一条就红。
//
// ★ 新增 NPPM_* 时的固定动作（三步，缺一不可）：
//     ① NppMessages.h 的 NppMsg 枚举加值；
//     ② NppMessages.h 的 kNppKnownMsgs 加同一条（就在枚举下方，守卫的对照清单）；
//     ③ 本文件的 kNppmTable 给出形状（或 kUnsupported + 原因）。
//   漏 ③ ⇒ 守卫红（"declared but unclassified"）；漏 ② ⇒ 守卫也红
//   （"table entry not in kNppKnownMsgs"），两条互为对照。
//
// 形状与「中转窗要做什么」的对应：
//   kValue        wp/lp 都是值 ⇒ **两个槽都要原样过桥**（宿主按老路径处理）
//   kInWideStr    lp 是入参宽串 ⇒ 代理读出来随载荷过桥，宿主拷进本地缓冲
//   kOutInt       lp 是 int* 出参（4 字节）
//   kOutStruct4   lp 是 4 字节出参结构（ShortcutKey 布局）
//   kOutWideBuf   两段式：lp==0 查询所需长度；否则 wp=容量（wchar_t 计）
//   kDmmReg       lp = DockedWidgetData* ⇒ **展平**进载荷（值字段 + 3 个宽串），
//                 宿主侧重建结构体再走老路径（v2.8）
//   kDmmTwoStr    wp = 窗口名串、lp = 模块名串 ⇒ 两个串都内联（v2.8）
//   kUnsupported  明确拒答：中转窗返回 0，**不产生跨进程调用**
//
// ★ 两个槽的"默认语义"相反，判据也各一条（都写在下面）：
//   · lp 默认是**指针** ⇒ `NppmLpIsValue` 只有 kValue 例外
//   · wp 默认是**值**   ⇒ `NppmWpIsPointer` 只有 kDmmTwoStr 例外
//   写反任何一条都是**静默错**（117a 修的正是 lp 那一侧）。
//
// ⚠ kValue 的 lp **也是值**（批次 117 修的静默缺陷）：NppmCallWire 原来只有
//   wParam 一格，于是 kValue 消息的 lp 被静默换成 0 —— 而 kValue 里真有 4 条
//   读 lp（SETMENUITEMCHECK 的勾选位、GETNBOPENFILES 的视图类型、
//   RELOADBUFFERID 的强制位、GETBUFFERIDFROMPOS 的视图）。
//   ⇒ 判据：**"值槽"与"指针槽"必须由形状推出来**（见下面的 NppmLpIsValue），
//   代理侧按它决定"原样透传"还是"填 0"，不靠人记。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>

#include "OopProtocol.h"
#include "../npp/NppMessages.h"

namespace xfs {
namespace oop {

enum class NppmShape {
    kUnsupported,
    kValue,
    kInWideStr,
    kOutInt,
    kOutStruct4,
    kOutWideBuf,
    // ---- v2.8（批次 117）：停靠族（NPPM_DMM*）------------------------------
    // 载荷布局（两侧共用）见 DmmMarshal.h。两条形状都**只有一个指针槽**，
    // 所以它们进不了上面任何一档：lp 在 kDmmReg 里指向一个结构体、
    // 在 kDmmTwoStr 里指向第二个串（第一个串在 wp —— 本表**唯一**把指针
    // 放在 wp 的形状，见下面的 NppmWpIsPointer）。
    kDmmReg,      // lp = DockedWidgetData*（展平成 值字段 + 3 个宽串）
    kDmmTwoStr,   // wp = 窗口名串、lp = 模块名串（DMMGETPLUGINHWNDBYNAME）
};

struct NppmEntry {
    unsigned msg;
    NppmShape shape;
    // 仅 kUnsupported 用：ASCII 原因（会出现在诊断日志与守卫输出里）。
    // 受支持项恒 nullptr —— 守卫会检查这个不变式。
    const char* why;
};

// ---- 分类表 -----------------------------------------------------------------
// 顺序 = 先按「过桥代价」分组（值 → 入参串 → 定长出参 → 变长出参 → 不支持），
// 便于人读；守卫只要求 msg 唯一，不依赖顺序。
inline constexpr NppmEntry kNppmTable[] = {
    // ---- 值类型：wp/lp 都不含指针，原样过桥 --------------------------------
    { npp::NPPM_GETNBOPENFILES,     NppmShape::kValue, nullptr },
    { npp::NPPM_SAVECURRENTFILE,    NppmShape::kValue, nullptr },
    { npp::NPPM_SAVEALLFILES,       NppmShape::kValue, nullptr },
    { npp::NPPM_GETNPPVERSION,      NppmShape::kValue, nullptr },
    { npp::NPPM_SETMENUITEMCHECK,   NppmShape::kValue, nullptr },
    { npp::NPPM_GETPOSFROMBUFFERID, NppmShape::kValue, nullptr },
    { npp::NPPM_GETBUFFERIDFROMPOS, NppmShape::kValue, nullptr },
    { npp::NPPM_GETCURRENTBUFFERID, NppmShape::kValue, nullptr },
    { npp::NPPM_RELOADBUFFERID,     NppmShape::kValue, nullptr },
    // 停靠族里 lp 是 **HWND 值** 的三条（句柄是内核对象值，跨进程传安全）。
    // ⚠ 它们属于 kValue 是**判断**，不是巧合：形状的唯一作用是决定"哪个槽要
    //   封送"。这三条的 wp 恒 0、lp 是值 ⇒ 与 kValue 的定义完全一致。
    { npp::NPPM_DMMSHOW,            NppmShape::kValue, nullptr },
    { npp::NPPM_DMMHIDE,            NppmShape::kValue, nullptr },
    { npp::NPPM_DMMUPDATEDISPINFO,  NppmShape::kValue, nullptr },

    // ---- 入参宽字符串（lp 是插件自己地址空间里的合法串）--------------------
    { npp::NPPM_SWITCHTOFILE, NppmShape::kInWideStr, nullptr },
    { npp::NPPM_DOOPEN,       NppmShape::kInWideStr, nullptr },
    { npp::NPPM_RELOADFILE,   NppmShape::kInWideStr, nullptr },
    { npp::NPPM_DMMVIEWOTHERTAB, NppmShape::kInWideStr, nullptr },

    // ---- 定长出参 ----------------------------------------------------------
    { npp::NPPM_GETCURRENTSCINTILLA, NppmShape::kOutInt, nullptr },
    { npp::NPPM_ALLOCATECMDID,       NppmShape::kOutInt, nullptr },
    { npp::NPPM_GETSHORTCUTBYCMDID,  NppmShape::kOutStruct4, nullptr },

    // ---- 两段式宽字符串出参 -------------------------------------------------
    { npp::NPPM_GETPLUGINSCONFIGDIR,     NppmShape::kOutWideBuf, nullptr },
    { npp::NPPM_GETFULLPATHFROMBUFFERID, NppmShape::kOutWideBuf, nullptr },
    { npp::NPPM_GETFULLCURRENTPATH,      NppmShape::kOutWideBuf, nullptr },
    { npp::NPPM_GETCURRENTDIRECTORY,     NppmShape::kOutWideBuf, nullptr },
    { npp::NPPM_GETFILENAME,             NppmShape::kOutWideBuf, nullptr },
    { npp::NPPM_GETNAMEPART,             NppmShape::kOutWideBuf, nullptr },
    { npp::NPPM_GETEXTPART,              NppmShape::kOutWideBuf, nullptr },
    { npp::NPPM_GETCURRENTWORD,          NppmShape::kOutWideBuf, nullptr },

    // ---- 停靠族的结构体/双串两条（v2.8）--------------------------------------
    // 与上面各族的关键差别：**宿主侧要重建一个结构体**，而结构体里的字段是
    // 值 + 串的混合 ⇒ 展平进载荷（布局见 DmmMarshal.h），wire 里**没有**外来
    // 地址。返回的 HWND（DMMGETPLUGINHWNDBYNAME）也是值，走回包的 result。
    //
    // ★ 这 6 条原来是 kUnsupported，理由写的是 "needs relay + DockManager
    //   whitelist" —— 批次 117 的两进程实测把那两半都推翻了：
    //     ① "SetParent 跨进程被系统禁止"**是假的**（两侧都泵消息时正向/反向都
    //        成功，err=0、GA_PARENT 真的变了、窗口存活）；
    //     ② 真正的拦路虎是 **`WM_NOTIFY` 这一个消息 ID**（跨进程 err=5
    //        ACCESS_DENIED，`lParam=0` 照样拒 ⇒ 与指针无关，也不是 UIPI）。
    //   白名单**不是**缺的那块 ⇒ 本批把 6 条全部升级为过桥，并用
    //   OOPM_DMMNOTIFY/DMMACTION 把**通知链**也搬进代理进程（见 OopProtocol.h）。
    { npp::NPPM_DMMREGASDCKDLG,        NppmShape::kDmmReg,    nullptr },
    { npp::NPPM_DMMGETPLUGINHWNDBYNAME, NppmShape::kDmmTwoStr, nullptr },

    // ---- 明确不支持（每条都要给原因；原因不是装饰，是"为什么不做"的判据）----
    { npp::NPPM_GETOPENFILENAMES_DEPRECATED, NppmShape::kUnsupported,
      "out array of caller buffers: needs per-element marshalling" },
    { npp::NPPM_GETOPENFILENAMESPRIMARY_DEPRECATED, NppmShape::kUnsupported,
      "out array of caller buffers: needs per-element marshalling" },
    { npp::NPPM_GETOPENFILENAMESSECOND_DEPRECATED, NppmShape::kUnsupported,
      "out array of caller buffers: needs per-element marshalling" },
    { npp::NPPM_GETMENUHANDLE, NppmShape::kUnsupported,
      "HMENU is a user object handle, not portable across processes" },
    // ★ 批次 120：原名误作 `NPPM_GETMENUBAR`（上游没有这个宏）。NPPMSG+52 是真实的
    //   `NPPM_ISTABBARHIDDEN`（BOOL，无指针）⇒ 形状从 kUnsupported 改为 kValue。
    //   ⚠ 不变式要求 `why` 只属于 kUnsupported（守卫 [4] 段），所以这里**不带** why；
    //   改名与错答的来龙去脉写在 NppMessages.h 该行上方。
    { npp::NPPM_ISTABBARHIDDEN, NppmShape::kValue },
};

inline constexpr std::size_t kNppmTableCount =
    sizeof(kNppmTable) / sizeof(kNppmTable[0]);

// NppMessages.h 里声明的每一条 NPPM_*（守卫的对照清单，见文件头 ★）。
// 清单本体在 NppMessages.h（紧邻枚举），这里只做别名，避免两处维护。
inline constexpr const unsigned* kNppmKnownMsgs = npp::kNppKnownMsgs;
inline constexpr std::size_t kNppmKnownCount = npp::kNppKnownCount;

// ---- 查询助手 ---------------------------------------------------------------

// 未知编号也返回 kUnsupported（"不在表里" = 不做过桥），与宿主侧
// ForwardNppMessage 的 default 分支语义一致：明确拒答，不猜。
inline NppmShape NppmShapeOf(unsigned msg) {
    for (std::size_t i = 0; i < kNppmTableCount; ++i)
        if (kNppmTable[i].msg == msg) return kNppmTable[i].shape;
    return NppmShape::kUnsupported;
}

inline bool NppmSupported(unsigned msg) {
    return NppmShapeOf(msg) != NppmShape::kUnsupported;
}

// 不支持的原因；受支持或表外编号返回 nullptr。
inline const char* NppmUnsupportedWhy(unsigned msg) {
    for (std::size_t i = 0; i < kNppmTableCount; ++i)
        if (kNppmTable[i].msg == msg)
            return kNppmTable[i].shape == NppmShape::kUnsupported
                       ? kNppmTable[i].why : nullptr;
    return nullptr;
}

inline const char* NppmShapeName(NppmShape sh) {
    switch (sh) {
    case NppmShape::kUnsupported: return "unsupported";
    case NppmShape::kValue:       return "value";
    case NppmShape::kInWideStr:   return "in-wide-str";
    case NppmShape::kOutInt:      return "out-int";
    case NppmShape::kOutStruct4:  return "out-struct4";
    case NppmShape::kOutWideBuf:  return "out-wide-buf";
    case NppmShape::kDmmReg:      return "dmm-reg";
    case NppmShape::kDmmTwoStr:   return "dmm-two-str";
    }
    return "?";
}

// 编号是否落在宿主拦截的两个 NPPM 区间内（中转窗据此决定是否接管）。
// 区间与 PluginManager::ForwardNppMessage 的拦截区间**同口径**：多一个
// 少一个都会让"中转窗接管了但宿主不认"或反之。
inline bool NppmInRanges(unsigned msg) {
    return (msg >= npp::kNppMsgBase && msg <= npp::kNppMsgBase + 128) ||
           (msg >= npp::kRunCmdBase && msg <= npp::kRunCmdBase + 64);
}

// 单次过桥的载荷上限（字节）。入参与出参各自不得超过它 —— 代理侧按此
// 拒绝超长入参串，宿主侧按此钳制 outCap（wire 是外来数据，不可信）。
// 与 OopProtocol.h 的 kNppmPayloadMax 同值，这里给出"为什么是这个量级"：
// 路径/词/配置目录这类串远小于 8K wchar_t，而 16K 字节的载荷在
// WM_COPYDATA 上仍是单次内核拷贝的量级。
inline constexpr unsigned kNppmInCharsMax = 8192;    // 入参串上限（wchar_t 计）
inline constexpr unsigned kNppmOutWCharsMax = 8192;  // 出参缓冲上限（wchar_t 计）

// lp 是不是「值槽」（而不是指针槽）。
// ★ 这个判据是**契约的一部分**，不是实现细节：代理侧按它决定"原样透传"还是
//   "填 0"，宿主侧按它决定"直接用"还是"换成自己重建的指针"。两侧各写一份必然
//   漂移，而漂移的症状是**静默错**（见文件头 kValue 那段）。
//   ★ 新增形状时**必须**同步在这里表态：默认分支是"不是值槽"，所以漏表态会
//     表现为"新形状的 lp 被填 0"（静默），而不是崩 —— 这正是本批修的那类缺陷。
inline bool NppmLpIsValue(NppmShape sh) {
    return sh == NppmShape::kValue;          // 其余形状的 lp 都是指针/缓冲
}

// wp 是不是「指针槽」。
// ★ 与 NppmLpIsValue 的**默认方向相反**，这不是笔误：lp 在 NPPM 里绝大多数
//   是**指针**（所以默认填 0、只有 kValue 例外），wp 绝大多数是**值**
//   （所以默认透传、只有下面这一条例外）：
//     · kValue        —— wp 是值本身（或 0）
//     · kOutWideBuf   —— wp 是**调用方声明的容量**（会被用！填 0 会让插件拿不到数据）
//     · kOutInt/kOutStruct4 —— wp 是入参（如 GETCURRENTSCINTILLA 的 which、
//                              GETSHORTCUTBYCMDID 的 cmdID）
//     · kInWideStr/kDmmReg/kUnsupported —— wp 恒 0
//     · kDmmTwoStr    —— **唯一**把指针放在 wp 的形状（wp=窗口名串、lp=模块名串）
//   ⇒ 代理侧：`cw.wParam = NppmWpIsPointer(shape) ? 0 : wp;`
//   ⚠ 批次 117a 曾误写成 NppmLpIsValue 的镜像（只认 kValue），那会把
//     kOutWideBuf 的容量填 0 —— 无用且有害，故删。这里保留的是"指针"判据。
inline bool NppmWpIsPointer(NppmShape sh) {
    return sh == NppmShape::kDmmTwoStr;
}

// 中转窗按 shape + 调用方参数决定「向宿主索要多少出参字节」。
//   0 = 只要返回值（lp==0 的查询调用、或本来就无出参的值类型）
// 注意 kOutWideBuf 的 wp 是**调用方声明**的容量：钳到 kNppmOutWCharsMax
// 之后再传给宿主，宿主便不可能写出超过本地上限的内容（见 OopHost 侧注释）。
inline unsigned NppmOutCapBytes(NppmShape sh, WPARAM wp, LPARAM lp) {
    switch (sh) {
    case NppmShape::kOutInt:
    case NppmShape::kOutStruct4:
        return lp ? 4u : 0u;
    case NppmShape::kOutWideBuf: {
        if (!lp) return 0u;
        const unsigned long long cap = static_cast<unsigned long long>(wp);
        const unsigned long long lim = kNppmOutWCharsMax;
        return static_cast<unsigned>((cap < lim ? cap : lim) * sizeof(wchar_t));
    }
    case NppmShape::kDmmReg:
    case NppmShape::kDmmTwoStr:
        // 停靠族只要返回值（BOOL / HWND）。★ 显式列出而不是靠 default：
        // 新增形状时 default 会**静默**给它 0 出参容量，而"该有出参却拿到 0"
        // 的症状是插件拿到空数据（与 117a 修的 lp 缺陷同一类）——显式列表让
        // 新形状必须在这里表态（编译器 -Wswitch 也会盯着）。
        return 0u;
    default:
        return 0u;
    }
}

} // namespace oop
} // namespace xfs
