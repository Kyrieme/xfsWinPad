#pragma once
// SciBridge.h — SCI_* **指针参数**族的过桥契约（v2.7，批次 113–116）。
//
// 形状表（SciMarshal.h）把 826 条 SCI_* 分成「716 条直发 + 110 条拒答」。本文件
// 覆盖其中四族：
//   * **入参族**（kInStr 53 + kInBytes 1 = 54 条，批次 113）：中转窗在**自己
//     进程里**把插件给的串读出来随 wire 内联，编辑器在**自己的地址空间里**
//     重建指针再发给 Scintilla。
//   * **出参族**（kOutStr 30 条，批次 114）：方向相反 —— 编辑器先按 Scintilla
//     自带的「lParam==0 查长度」协议问出容量，在自己的缓冲里真调用一次，再把
//     结果字节随 wire 回带，由中转窗拷进插件给的缓冲。
//   * **出入参族**（kInOutStr 5 条，批次 115）：= 入参族的一半 + 出参族的一半
//     （形状 (string, stringresult)），出参容量规则与出参族**完全一样**。
//   * **结构族**（kStruct 8 条，批次 116）：指针指向**结构体**、结构体里**还有
//     指针** ⇒ 要**展平**字段 + **内联**串；其中 FindText 的 chrgText 是**条件
//     回写**（只在找到时才写）。
// 四族共用同一个 .cpp，与 NPPM_*（批次 111）是同一套路，只是参数形态更杂。
//
// 为什么布局也必须生成（见 SciBridgeTable.inc 头）
// ------------------------------------------------
//   * **19 条的串在 wParam**（单参数消息，如 SetText / ReplaceSel）—— 只看
//     lp 会静默丢掉整串，插件拿到的行为是"文本没改"。
//   * **2 条 wp 与 lp 都是串**（SetProperty / SetRepresentation）—— 要内联两段。
//   * **AutoCShow 的首参叫 lengthEntered**（已输入字符数，不是缓冲长度）——
//     当长度用会**截断补全列表**：不崩、不报错，只是功能坏掉。所以长度判据是
//     "首参名**精确等于** length"，名单冻在生成器里（EXPECT_LEN_NAMED）。
//
// 为什么出参容量也必须生成（见 SciOutTable.inc 头）
// -------------------------------------------------
//   * 容量**不在消息里**（与入参族相反：入参的长度至少有时由 wp 给出）。唯一的
//     来源是 Scintilla 出参协议自带的 lParam==0 调用 —— 它不写缓冲、只返回需要
//     的字节数（记 need）。
//   * 回传多少字节由**实现的 NUL 语义**决定，而 `StringResult()` 写 len+1、
//     `BytesResult()` 写 len —— **只差一个字节**。差一字节 = 越界写 1 字节
//     （堆破坏，而且多半不崩）⇒ 这份分类不能手写。
//   * `TargetAsUTF8` 是唯一**必须拒答**的一条：NUL 语义随运行期文档编码变化
//     （Unicode 模式不写 NUL / 非 Unicode 模式写），静态表分不对。
//
// 分工（分类只有一份，不会漂移）
// ------------------------------
//   代理侧 SciReadInbound()   —— 按布局把插件的 wp/lp 读成两段字节
//   编辑器侧 SciBridgeCall()  —— 按布局把两段字节还原成宿主本地指针
//   编辑器侧 SciBridgeOutCall() —— 探长度 → 真调用 → 哨兵校验 → 回带字节
//   两侧共用 kSciInTable / kSciOutTable（都是生成物）
//
// 已知边界（诚实声明 —— 别把"返回 0"当"成功"）
// --------------------------------------------
//   * 仍拒答：kRawPtr(4) / kPtrRet(9) 语义上不可桥接（返回文档内部指针）；
//     以及三张表里各自点名的拒答桶 —— kOutStr 的 TargetAsUTF8 / GetTag、
//     kInOutStr 的 EncodedFromUTF8 / GetRepresentation、kStruct 的
//     FormatRange / FormatRangeFull（hdc/hdcTarget 是**进程私有** GDI 句柄）。
//   * 长度未知的串按 NUL 扫描，上限 kSciPayloadMax；**超限一律拒答而不是截断**
//     —— 截断会静默改变语义（例如 SearchInTarget 的 pattern 被砍掉一半，
//     搜索结果会"看起来正常"但答案是错的）。出参同理：need 超限也拒答。
//   * **出参族的"调用方必须给够缓冲"这条契约被原样保留**：Scintilla 自己也不
//     检查调用方缓冲大小（`GetLine` 的注释就写着 "Risk of overwriting the end
//     of the buffer"）。桥接能做的是把"垃圾指针"变成拒答（LocalWritable），
//     做不到比进程内调用更安全 —— 别把这条写成"已防护"。
//   * 目标 Scintilla 句柄取自代理启动时的活动文档，**不随文档切换更新**
//     （与批次 112 的值类型直发同一条边界）。
//   * 本表只回答"这条消息的参数怎么搬"，**不**回答"宿主的 Scintilla 是否
//     实现了它"。未实现的消息也返回 0，与"被拒答"同形。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <limits>
#include <vector>

#include "OopProtocol.h"

namespace xfs {
namespace oop {

// 入参族的参数布局。kNone = 不在入参族（本批不过桥）。
enum class SciInLayout : unsigned char {
    kNone = 0,      // 不是入参族
    kStrInLp,       // lp 是串，wp 是值；串 NUL 结尾（长度未知）
    kStrInLpLen,    // lp 是串，**wp 就是字节数**（可省 NUL 扫描）
    kStrInWp,       // wp 是串，lp 是值（单参数消息都落这里）
    kStrBoth,       // wp 与 lp 都是串（内联两段）
    kBytesInLpLen,  // lp 是裸字节缓冲，wp 是字节数（cells）
};

struct SciInEntry {
    unsigned msg;
    SciInLayout layout;
};

#include "SciBridgeTable.inc"

// 表按 msg 严格升序 ⇒ 二分。表外返回 kNone（= 不过桥，安全方向）。
inline SciInLayout SciInLayoutOf(unsigned msg) {
    std::size_t lo = 0;
    std::size_t hi = kSciInCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kSciInTable[mid].msg < msg)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < kSciInCount && kSciInTable[lo].msg == msg)
        return kSciInTable[lo].layout;
    return SciInLayout::kNone;
}

inline bool SciBridgeableIn(unsigned msg) {
    return SciInLayoutOf(msg) != SciInLayout::kNone;
}

inline const char* SciInLayoutName(SciInLayout lay) {
    switch (lay) {
    case SciInLayout::kNone:         return "none";
    case SciInLayout::kStrInLp:      return "str-in-lp";
    case SciInLayout::kStrInLpLen:   return "str-in-lp-len";
    case SciInLayout::kStrInWp:      return "str-in-wp";
    case SciInLayout::kStrBoth:      return "str-both";
    case SciInLayout::kBytesInLpLen: return "bytes-in-lp-len";
    }
    return "?";
}

// 哪些槽装的是指针（其余槽是值，原样透传）。
inline void SciInPtrSlots(SciInLayout lay, bool& wpIsPtr, bool& lpIsPtr) {
    switch (lay) {
    case SciInLayout::kStrInLp:
    case SciInLayout::kStrInLpLen:
    case SciInLayout::kBytesInLpLen:
        wpIsPtr = false; lpIsPtr = true;  return;
    case SciInLayout::kStrInWp:
        wpIsPtr = true;  lpIsPtr = false; return;
    case SciInLayout::kStrBoth:
        wpIsPtr = true;  lpIsPtr = true;  return;
    default:
        wpIsPtr = false; lpIsPtr = false; return;
    }
}

// 长度是否由 wp 直接给出（true ⇒ 不必 NUL 扫描，也不必担心未终止缓冲）。
inline bool SciInLenFromWp(SciInLayout lay) {
    return lay == SciInLayout::kStrInLpLen || lay == SciInLayout::kBytesInLpLen;
}

// 需要几段载荷（kStrBoth 两段，其余一段；kNone 零段）。
inline unsigned SciInSegments(SciInLayout lay) {
    if (lay == SciInLayout::kNone) return 0;
    return lay == SciInLayout::kStrBoth ? 2u : 1u;
}

// ---- 编辑器侧：wire 值 + 本地缓冲 → 发给宿主 Scintilla 的 wp/lp -------------
// 纯函数（不碰 Win32 API、不读全局）⇒ 单测可以直接把 6 种布局全枚举对撞。
// argWp/argLp 是 wire 里的**值语义**部分；指针槽在 wire 里是 0，这里被换成
// buf1/buf2。两段缓冲都必须以 NUL 结尾（调用方保证），因为 Scintilla 对
// kStrInLp / kStrInWp 形态是按 C 串读的。
inline void SciHostArgs(SciInLayout lay, UINT_PTR argWp, UINT_PTR argLp,
                        void* buf1, void* buf2,
                        WPARAM& hostWp, LPARAM& hostLp) {
    bool wpPtr = false, lpPtr = false;
    SciInPtrSlots(lay, wpPtr, lpPtr);
    hostWp = wpPtr ? reinterpret_cast<WPARAM>(buf1)
                   : static_cast<WPARAM>(argWp);
    hostLp = lpPtr ? reinterpret_cast<LPARAM>(lay == SciInLayout::kStrBoth ? buf2 : buf1)
                   : static_cast<LPARAM>(argLp);
}

// ---- 代理侧：把插件的 wp/lp 读成两段字节 ------------------------------------
// 成功时：b1/b2 是两段字节（各多留一个 NUL 结尾），argWp/argLp 是要随 wire
// 透传的值语义部分（指针槽为 0）。失败返回 false ⇒ 调用方必须拒答。
// 指针在本进程里合法，但仍**有界**读取：插件传了未终止的串、越界的长度、
// 或指向未映射页的指针，都不能把代理进程拖死（那是隔离的意义所在）。
bool SciReadInbound(SciInLayout lay, UINT_PTR wp, UINT_PTR lp,
                    std::vector<unsigned char>& b1,
                    std::vector<unsigned char>& b2,
                    UINT_PTR& argWp, UINT_PTR& argLp);

// ---- 编辑器侧：一次入参过桥 -------------------------------------------------
// 把 wire 里内联的两段字节还原成**宿主本地**指针，再发给宿主真 Scintilla。
// 返回 false = 拒答（编号不在入参族 / 长度越界 / 没有目标窗）。
// 独立成自由函数（而不是塞进 OopHost 的 WndProc 分支）是为了能被测试直接
// 调用 —— 否则 e2e 夹具就得自己实现一遍编辑器侧，那条断言只能证明"夹具
// 会搬指针"，证明不了产品会。
bool SciBridgeCall(HWND sciTarget, const SciCallWire& cw, DWORD cbData,
                   LRESULT& out);

// ============================================================================
// 出参族（kOutStr 30 条）—— 批次 114
// + 出入参族（kInOutStr 5 条）—— 批次 115
// ============================================================================
// 方向与入参族相反：插件给的是**接收缓冲**，Scintilla 往里写。跨进程的困难
// 不在"搬内容"，而在**搬多少** —— 容量不在消息里。
//
// 出入参族（批次 115）就是"入参族的一半 + 出参族的一半"：形状 (string,
// stringresult) ⇒ **入参串在 wParam、出参缓冲在 lParam**。出参侧的容量规则与
// 出参族**完全一样**（所以两表共用下面这套 SciOutKind 桶空间），多的只是
// "要不要把 wParam 当串读进来"（SciOutNeedsInStr）。这个 wire 约定是**推**出来
// 的：生成器 build_inout() 断言本族参数恰好是 (string, stringresult) 且顺序如此。
//
// 协议（Scintilla 自带，不是我们发明的）
// --------------------------------------
//   同一条消息用 lParam == 0 调一次 = "查长度"：不写任何缓冲，返回需要的字节数。
//   所有可桥接条目都支持（实现里分别是 `if (lParam == 0) return ...`、
//   `if (buffer)`、`if (tagValue)`、`if ((lParam) && ...)` 这几种保护形态）。
//   于是：容量 = 这次调用的返回值（need）。
//
// 回传多少字节（这才是"差一个字节"的地方）
// ----------------------------------------
//   kNul           need + 1          StringResult 写 len+1（含结尾 NUL）
//   kNoNul         need              BytesResult 写 len（**不含** NUL）
//   kClampWpNul    min(need, wp) + 1 GetText / GetCurLine 先被 wParam 钳制
//   kRefuseRuntime 拒答              TargetAsUTF8 的 NUL 语义随运行期编码变
//
// ⚠ 回传字节数**绝不能**用消息的返回值算：`GetCurLine` 返回的是**光标列号**
//   （sel.MainCaret() - lineStart），`GetText` 返回的是**实际拷入量**而不是
//   需要的量。只能用 probe 得到的 need（本批负控②专门钉这条）。
//
// ⚠ 出入参族里的 `GetRepresentation`(2666) 也是拒答桶，理由不同：probe **分不了
//   两种 0** —— repr 不存在时直接 return 0（**0 字节**），repr 存在但内容为空串时
//   走 StringResult（**1 个 NUL**）。两种 probe 都返回 0 ⇒ 容量不可知。
//   `EncodedFromUTF8`(2449) 则是因为入参长度来自伴生消息 SetLengthForEncode(2448)、
//   且 NUL 语义随 IsUnicodeMode() 变。两条都写在 SciInOutTable.inc 的头注释里。
enum class SciOutKind : unsigned char {
    kNone = 0,       // 不在出参族（本批不过桥）
    kNul,            // 回传 need+1（含结尾 NUL）
    kNoNul,          // 回传 need（不含 NUL）
    kClampWpNul,     // 回传 min(need, wp)+1
    kRefuseRuntime,  // 拒答：NUL 语义随运行期状态变化
};

struct SciOutEntry {
    unsigned msg;
    SciOutKind kind;
};

#include "SciOutTable.inc"
#include "SciInOutTable.inc"

// 表按 msg 严格升序 ⇒ 二分。表外返回 kNone（= 不过桥，安全方向）。
inline bool FindSciOutEntryIn(const SciOutEntry* tab, unsigned count,
                              unsigned msg, SciOutKind& out) {
    std::size_t lo = 0;
    std::size_t hi = count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (tab[mid].msg < msg)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < count && tab[lo].msg == msg) {
        out = tab[lo].kind;
        return true;
    }
    return false;
}

// 两张表**共用**同一套 SciOutKind 桶空间（出参侧的容量规则一样），所以合并成
// 一次判定：先出入参族（kInOutStr），再出参族（kOutStr）。两表互斥，顺序只影响
// 可读性。合并的意义是**分类只有一份** —— 不会出现"代理以为不带入参串、
// 宿主以为带"这种漂移。
inline SciOutKind SciOutKindOf(unsigned msg) {
    SciOutKind k = SciOutKind::kNone;
    if (FindSciOutEntryIn(kSciInOutTable, kSciInOutCount, msg, k)) return k;
    if (FindSciOutEntryIn(kSciOutTable, kSciOutCount, msg, k)) return k;
    return SciOutKind::kNone;
}

// 这条消息要不要把 wParam 当**入参串**读进来（即 kInOutStr 族）。
// 判据就是"在 kSciInOutTable 里"—— 与 SciOutKindOf 同一个来源，不另立判据。
inline bool SciOutNeedsInStr(unsigned msg) {
    SciOutKind k = SciOutKind::kNone;
    return FindSciOutEntryIn(kSciInOutTable, kSciInOutCount, msg, k);
}

// 表里且**不是**拒答桶 ⇒ 真能过桥。代理侧用它把"过桥"与"拒答"分开记账。
inline bool SciOutRelayable(unsigned msg) {
    const SciOutKind k = SciOutKindOf(msg);
    return k != SciOutKind::kNone && k != SciOutKind::kRefuseRuntime;
}

inline const char* SciOutKindName(SciOutKind k) {
    switch (k) {
    case SciOutKind::kNone:          return "none";
    case SciOutKind::kNul:           return "nul";
    case SciOutKind::kNoNul:         return "no-nul";
    case SciOutKind::kClampWpNul:    return "clamp-wp-nul";
    case SciOutKind::kRefuseRuntime: return "refuse-runtime";
    }
    return "?";
}

// need 的合理上限（1 MiB）。上游必须先把 need 钳到 kSciPayloadMax 再进来；
// 这里再兜一道，把"忘了钳"变成 0（拒答）而不是整数回绕。
constexpr unsigned long kSciOutNeedMax = 1u << 20;

// 回传字节数（纯函数）。0 = 拒答（拒答桶 / 表外 / need 越界）。
// ⚠ need/wp 是 unsigned long 而不是 size_t：避免 x64 上 size_t / LONG_PTR
//   之间的重载歧义（本工程 Release 下踩过类似的静默选错重载）。
inline unsigned long SciOutBytesToCopy(SciOutKind k, unsigned long need,
                                       unsigned long wp) {
    if (need > kSciOutNeedMax) return 0;
    switch (k) {
    case SciOutKind::kNul:         return need + 1;
    case SciOutKind::kNoNul:       return need;
    case SciOutKind::kClampWpNul:  return (need < wp ? need : wp) + 1;
    default:                       return 0;      // kNone / kRefuseRuntime
    }
}

// 写后白名单（纯函数）：确认 Scintilla 只写了我们允许它写的那一段。
//   * [copied, totalLen) 必须仍是哨兵字节 ⇒ 抓**越界写**（写多了）
//   * 带 NUL 的桶：after[copied-1] 必须是 '\0' ⇒ 抓**NUL 语义搞反**
// 注意它证明不了 [0, copied) 里的内容"对" —— 那一区没有独立的真值来源，
// 它的作用是把"写多了"和"少写/多写一个 NUL"这两种错变成可读的失败。
inline bool SciOutWroteOnlyExpected(SciOutKind k, std::size_t copied,
                                    const unsigned char* after,
                                    std::size_t totalLen,
                                    unsigned char canary) {
    if (!after) return false;
    if (copied > totalLen) return false;
    if (k != SciOutKind::kNul && k != SciOutKind::kNoNul &&
        k != SciOutKind::kClampWpNul)
        return false;                              // 拒答桶/表外：不该走到这
    for (std::size_t i = copied; i < totalLen; ++i)
        if (after[i] != canary) return false;      // 越界写
    if (copied == 0) return true;                  // 什么都没写：平凡成立
    if (k == SciOutKind::kNul || k == SciOutKind::kClampWpNul)
        return after[copied - 1] == 0;             // NUL 语义
    return true;
}

// 哨兵：真调用前把整块缓冲填成它，调用后检查 [copied, totalLen) 没被动过。
// 余量给足 16 字节 —— 越界写 1 字节（本批最危险的那个错误方向）必然落在里面。
constexpr std::size_t kSciOutCanaryMargin = 16;
constexpr unsigned char kSciOutCanaryByte = 0xA5;

// ---- 编辑器侧：一次出参过桥 -------------------------------------------------
// ① 用 lParam==0 探长度得 need（超 kSciPayloadMax ⇒ 拒答）；
// ② writeBack 为假就到此为止（插件在查长度），out = need；
// ③ 否则分配 copied + 哨兵余量、填哨兵、真调用一次；
// ④ SciOutWroteOnlyExpected 校验；outBytes 只带前 copied 字节。
// 返回 false = 拒答（不在出参族 / 拒答桶 / need 越界 / 没目标窗 / 校验不过）。
// 独立成自由函数（与 SciBridgeCall 同理）：e2e 夹具直接调**产品代码**，
// 而不是自己实现一遍编辑器侧 —— 否则那条断言只能证明"夹具会搬字节"。
bool SciBridgeOutCall(HWND sciTarget, const SciOutCallWire& cw, DWORD cbData,
                      std::vector<unsigned char>& outBytes,
                      unsigned long& need, unsigned long& copied,
                      LRESULT& out);

// ============================================================================
// 结构体指针族（kStruct 8 条）—— 批次 116
// ============================================================================
// 形状是「指针指向**结构体**，结构体里**还有指针**」⇒ 要**展平**结构体字段、
// **内联**结构体里的串，并处理**条件回写**。四类：
//
//   kRangeOut      GetTextRange(2162) / GetTextRangeFull(2039)
//   kStyledOut     GetStyledText(2015) / GetStyledTextFull(2778)
//   kFindInOut     FindText(2150) / FindTextFull(2196)
//   kRefuseHandle  FormatRange(2151) / FormatRangeFull(2777)   ← **永久**拒答
//
// ★ 本族是本项目第一个**真正的 in-out 族**：同一条消息里既有入参侧（chrg /
//   needle）又有出参侧（出缓冲 / chrgText）。
// ★ 容量**算得出**（与 kOutStr **相反**：那里容量不在消息里、必须探长度）。
//   kRangeOut 写 len+1、kStyledOut 写 2*len+2，len 由 cpMin/cpMax（+docLen）算出。
// ★ 代理侧**不 include Scintilla 头**（见 src/CMakeLists.txt「自持契约镜像」），
//   所以这里自带一份公开 ABI 的**局部镜像**（abi::*）；尺寸/偏移由
//   tests/test_sci_struct.cpp 与真 Scintilla.h **对撞**（static_assert 两两相等）。
enum class SciStructKind : unsigned char {
    kNone = 0,        // 不在结构族
    kRangeOut,        // 只读结构 + 出缓冲，容量 = len + 1
    kStyledOut,       // 只读结构 + 出缓冲，容量 = 2*len + 2
    kFindInOut,       // 入串（needle）+ **条件**回写 chrgText
    kRefuseHandle,    // 拒答：hdc/hdcTarget 是进程私有 GDI 句柄（且 FormatRange 是绘制）
};

struct SciStructEntry {
    unsigned msg;
    SciStructKind kind;
    bool full;        // true ⇒ 结构体用 64 位 Sci_Position（*Full 变体）
};

#include "SciStructTable.inc"

// 表按 msg 严格升序 ⇒ 二分。表外返回 false（= 不过桥，安全方向）。
inline bool FindSciStructEntry(unsigned msg, SciStructKind& out, bool& full) {
    std::size_t lo = 0;
    std::size_t hi = kSciStructCount;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (kSciStructTable[mid].msg < msg)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < kSciStructCount && kSciStructTable[lo].msg == msg) {
        out = kSciStructTable[lo].kind;
        full = kSciStructTable[lo].full;
        return true;
    }
    return false;
}

inline SciStructKind SciStructKindOf(unsigned msg) {
    SciStructKind k = SciStructKind::kNone;
    bool full = false;
    FindSciStructEntry(msg, k, full);
    return k;
}

// 表里且**不是**拒答桶 ⇒ 真能过桥。代理侧用它把"过桥"与"拒答"分开记账，
// 并在**中转窗**就把 kRefuseHandle 拒掉（不产生一次注定被拒的跨进程往返）。
inline bool SciStructRelayable(unsigned msg) {
    const SciStructKind k = SciStructKindOf(msg);
    return k != SciStructKind::kNone && k != SciStructKind::kRefuseHandle;
}

// 这条消息要不要把 needle 读进来（即 kFindInOut 族）。判据就是"在表里且是该桶"
// —— 与 SciStructKindOf 同一个来源，不另立判据。
inline bool SciStructNeedsInStr(unsigned msg) {
    return SciStructKindOf(msg) == SciStructKind::kFindInOut;
}

inline bool SciStructIsFull(unsigned msg) {
    SciStructKind k = SciStructKind::kNone;
    bool full = false;
    FindSciStructEntry(msg, k, full);
    return full;
}

inline const char* SciStructKindName(SciStructKind k) {
    switch (k) {
    case SciStructKind::kNone:         return "none";
    case SciStructKind::kRangeOut:     return "range-out";
    case SciStructKind::kStyledOut:    return "styled-out";
    case SciStructKind::kFindInOut:    return "find-in-out";
    case SciStructKind::kRefuseHandle: return "refuse-handle";
    }
    return "?";
}

// ---- Scintilla 公开结构体的**局部镜像** ------------------------------------
// 代理侧不 include Scintilla 头（CMakeLists 的「自持契约镜像」），所以这里自己
// 定义一份。**尺寸与字段偏移由测试与真 Scintilla.h 对撞** —— 只写 static_assert
// 在这里是不够的（那只是在核对我自己写的常量），test_sci_struct.cpp 里会
// `#include <Scintilla.h>` 后逐条比对 sizeof 与 offsetof。
//   Sci_PositionCR = long（Sci_Position.h:21）        → 非 Full 变体
//   Sci_Position   = intptr_t（ScintillaTypes.h:709） → Full 变体
namespace abi {

struct CharRange     { long    cpMin; long    cpMax; };
struct CharRangeFull { INT_PTR cpMin; INT_PTR cpMax; };

struct TextRange     { CharRange     chrg; char*       lpstrText; };
struct TextRangeFull { CharRangeFull chrg; char*       lpstrText; };

struct TextToFind     { CharRange     chrg; const char* lpstrText; CharRange     chrgText; };
struct TextToFindFull { CharRangeFull chrg; const char* lpstrText; CharRangeFull chrgText; };

} // namespace abi

static_assert(sizeof(abi::CharRange) == 8, "Sci_CharacterRange 是 2 × long");
static_assert(sizeof(abi::CharRangeFull) == 16, "Sci_CharacterRangeFull 是 2 × intptr_t");
static_assert(sizeof(abi::TextRange) == 16, "Sci_TextRange");
static_assert(sizeof(abi::TextRangeFull) == 24, "Sci_TextRangeFull");
static_assert(sizeof(abi::TextToFind) == 24, "Sci_TextToFind");
static_assert(sizeof(abi::TextToFindFull) == 40, "Sci_TextToFindFull");

// 代理侧按它给 LocalReadable / LocalWritable 定界。0 = 不在可桥接族。
inline std::size_t SciStructSize(SciStructKind k, bool full) {
    switch (k) {
    case SciStructKind::kRangeOut:
    case SciStructKind::kStyledOut:
        return full ? sizeof(abi::TextRangeFull) : sizeof(abi::TextRange);
    case SciStructKind::kFindInOut:
        return full ? sizeof(abi::TextToFindFull) : sizeof(abi::TextToFind);
    default:
        return 0;
    }
}

// SCI_GETTEXTLENGTH(2183)：kValue 消息，用来拿 docLen（kRangeOut 的 cpMax==-1
// 语义与越界判定都要它）。编号由生成器的形状表核对（`--show GetTextLength`）。
constexpr unsigned kSciGetTextLength = 2183;

// 出缓冲容量（纯函数）。0 = 拒答（表外 / 拒答桶 / 范围非法 / 超限）。
//   kRangeOut  : len = (cpMax == -1 ? docLen : cpMax) - cpMin；返回 len + 1
//   kStyledOut : len = cpMax - cpMin；返回 2*len + 2
//
// ★ **故意收窄**（见 SciStructTable.inc 头）：kRangeOut 越界（cpEnd > docLen）
//   一律拒答 —— 原生那条路会留下"前面全是垃圾、只有末尾一个 NUL"，桥接不把
//   垃圾回灌给插件。kStyledOut 不支持 cpMax == -1（原生只写两个 NUL 就返回 0）。
inline unsigned long SciStructBytesToWrite(SciStructKind k, INT_PTR cpMin,
                                           INT_PTR cpMax, INT_PTR docLen) {
    INT_PTR len = 0;
    if (k == SciStructKind::kRangeOut) {
        const INT_PTR cpEnd = (cpMax == -1) ? docLen : cpMax;
        if (cpMin < 0 || cpEnd < cpMin || cpEnd > docLen) return 0;
        len = cpEnd - cpMin;
    } else if (k == SciStructKind::kStyledOut) {
        if (cpMin < 0 || cpMax < cpMin) return 0;
        len = cpMax - cpMin;
    } else {
        return 0;
    }
    // 先按 len 的绝对上限挡住整数回绕，再算 cap，最后统一按 kSciPayloadMax 钳。
    if (len < 0 || len > static_cast<INT_PTR>(kSciPayloadMax)) return 0;
    const unsigned long cap = (k == SciStructKind::kStyledOut)
        ? static_cast<unsigned long>(len) * 2u + 2u
        : static_cast<unsigned long>(len) + 1u;
    return cap <= kSciPayloadMax ? cap : 0;
}

// chrgText 的 canary：宿主侧调用前把它填进**自己的**结构体，调用后"没变"= 没被写。
// 用 INT_PTR 的最小值 —— 一个真实文档位置永远不可能是它（被写时 cpMin = pos ≥ 0）。
constexpr INT_PTR kSciStructChrgCanary = std::numeric_limits<INT_PTR>::min();

// ★ 非 Full 变体（Sci_TextToFind）的 chrgText 字段是 **long**（Windows 上 4 字节），
//   它的 canary **必须单独给**：`static_cast<long>(kSciStructChrgCanary)` 是**截断**，
//   结果是 **0** —— 而 0 是一个完全合法的命中位置 ⇒ "命中在位置 0" 会被读成"没写"
//   （批次 116 负控③ 抓到的真缺陷：非 Full 的 FindText 在 0 处命中会静默回一个
//   "没找到"，插件侧的 chrgText 保持原值）。long 的最小值同样不可能是位置
//   （位置恒 ≥ 0），且与任何真实写入都能用 `!=` 分开。
constexpr long kSciStructChrgCanaryCR = std::numeric_limits<long>::min();

// ---- 编辑器侧：一次结构体过桥 -----------------------------------------------
// 按族展平/重建 → 真调用 → 写后白名单（复用出参族那套 canary）→ 回带。
// 返回 false = 拒答（表外 / 拒答桶 / wire 契约不过 / 容量算不出 / 校验不过 /
// 没有目标窗）。独立成自由函数（与 SciBridgeCall / SciBridgeOutCall 同理）：
// e2e 夹具直接调**产品代码**，而不是自己实现一遍编辑器侧。
bool SciBridgeStructCall(HWND sciTarget, const SciStructCallWire& cw, DWORD cbData,
                         std::vector<unsigned char>& outBytes,
                         unsigned long& copied, unsigned& hasChrg,
                         INT_PTR& chrgMin, INT_PTR& chrgMax,
                         LRESULT& out);

} // namespace oop
} // namespace xfs
