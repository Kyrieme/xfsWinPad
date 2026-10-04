#pragma once
// DmmMarshal.h — 停靠族（NPPM_DMM*）两条"结构体/双串"形状的载荷布局
// （两侧唯一事实源，v2.8 / 批次 117）。
//
// 为什么需要它：`NPPM_DMMREGASDCKDLG` 的参数是 `DockedWidgetData*`（结构体里
// 还有 3 个宽串），`NPPM_DMMGETPLUGINHWNDBYNAME` 的两个参数都是串（一个在 wp、
// 一个在 lp）。这两条都**不能**把插件进程的地址放上 wire（批次 110 的宿主崩溃
// 就是这么来的）⇒ 代理侧把它们**展平**成「值 + 内联串」，宿主侧重建。
// 布局写在**这一处**、两侧 include 同一个头 —— 各写一份必然漂移，而漂移的
// 症状是**静默错**（117a 修的 lp 缺陷即此类）。
//
// 统一规则：
//   * 布局 =「定长头 + 尾随串」；串一律以 NUL 结尾，长度按 **wchar_t 计**。
//   * `chars == 0` ⇒ 该槽是 **NULL**；`chars >= 1` ⇒ 该串有 chars 个 wchar_t
//     且最后一个必须是 NUL。
//     ★ NULL 与"空串"必须可分：`DMMGETPLUGINHWNDBYNAME` 的契约里
//       `windowName == NULL` 是"按模块名取首个"，与"空串"语义不同
//       （批次 115 的教训：wire 扩字段要让"不带串/空串"可分）。
//   * 长度在**解析侧**全部重验（wire 是外来数据）：总字节数、每段 chars、
//     末字节是不是 NUL。任一条不满足 ⇒ `ok = false`，调用方拒答。
//     ★ "拒答"与"真返回 0"必须可分 —— 所以视图带 `ok` 位，而不是用返回值
//       兼表两义（批次 112 的教训：判据落在"对方是否收到"上）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <vector>

#include "../npp/NppDocking.h"

namespace xfs {
namespace oop {

// 单个宽串的字符数上限（含 NUL）。与 NppmMarshal.h 的 kNppmInCharsMax 同量级：
// DockedWidgetData 的三个串都是标题/模块名这类短串，4K 足够，同时挡住"插件给了
// 未终止串"把代理拖死（代理侧一律**有界扫描**，见 DmmBuild*）。
inline constexpr unsigned kDmmStrCharsMax = 4096;

// ---- kDmmReg 的定长头 --------------------------------------------------------
// 字段顺序 = 布局契约。刻意排成 8/8/4/4/4/4 ⇒ 恰好 32 字节、**无填充**。
// 两侧编译器与架构相同，但"无填充"让下面的 static_assert 有实际约束力
// （否则填充会随对齐规则漂，而漂移的症状同样是静默错）。
//
// 结构体里**不带**的两样（都是**判断**，不是遗漏）：
//   * `rcFloat` / `iPrevCont` —— NppDocking.h 明说"内部数据，我们不使用"。
// 将来真要用了，必须按"值槽显式表态"的规则在这里加字段并同步两侧。
//   * `hIconTab` **不带值**（HICON 是用户对象句柄，传值无意义）—— 但它不是
//     "不用"，而是**转码成图像块**挂在尾部（见下面的图标段，批次 137）。
struct DmmRegHead {
    UINT_PTR hClient;       // DockedWidgetData.hClient（值：HWND 是内核句柄）
    UINT_PTR uMask;
    int      dlgID;
    unsigned nameChars;     // 0 = pszName 为 NULL
    unsigned addInfoChars;  // 0 = pszAddInfo 为 NULL
    unsigned moduleChars;   // 0 = pszModuleName 为 NULL
};
static_assert(sizeof(DmmRegHead) == 32, "DmmRegHead 必须无填充（布局契约）");

// ---- kDmmReg 尾部的**图标段**（批次 137）-------------------------------------
// 为什么要有它：`hIconTab` 是 HICON —— 一个**用户对象句柄**，跨进程传值毫无
// 意义（宿主进程里的同一个数值指向别的对象或什么都不是）。但"传句柄"做不到，
// 不等于"传图标"做不到：代理进程与插件**同进程**、能解引用 HICON ⇒ 把它转码
// 成图像块上 wire，宿主侧 CreateIconIndirect **重建**本进程的 HICON。于是进程外
// 插件的停靠面板也能有标签图标（与批次 125 的进程内 iconCtrl 对齐）。
//
// 布局：紧跟 DmmRegHead + 三个宽串之后，追加
//     [DmmIconHead(16)][colorBytes(32bpp BGRA)][maskBytes(1bpp)]
// ★ 刻意**不**往 DmmRegHead 里塞图标字段：它现在是 32 字节、靠 8/8/4/4/4/4 的
//   排布做到**无填充**；插一个指针/多一个 int 都会被对齐撑成 40（一有洞，那份
//   static_assert 的约束力就没了）。所以图标自成一节、挂在尾部。
// ★ 图标段**恒在**（没图标也给 16 字节的 width==0 头）：解析侧的边界于是是
//   "恰好用完"，而不是"可以少一节"—— "少一节"与"载荷被截断"同形，分不开。
struct DmmIconHead {
    unsigned width;        // 0 = 无图标（此时 height/colorBytes/maskBytes 必须为 0）
    unsigned height;
    unsigned colorBytes;   // 恒 == width*4*height（32bpp BGRA，top-down）
    unsigned maskBytes;    // 恒 == ((width+31)/32)*4*height（1bpp，top-down）
};
static_assert(sizeof(DmmIconHead) == 16, "DmmIconHead 必须无填充（布局契约）");

// 图标两段字节数之和的上限（≈40×40 的 32bpp 图标）。超限**退化为无图标**，
// 不让注册失败 —— 一个过大的图标不该把面板挡在门外。
// 与 kNppmPayloadMax 的关系：DmmBuildReg 还会做一次"总量不超载荷上限"的检查
// （三个 4096 字符的大串 + 图标会顶穿代理侧的整单闸，那样**整个**注册请求会被
// 拒掉）⇒ 丢图标保注册是这里的取舍。
inline constexpr unsigned kDmmIconBytesMax = 8192;

// ---- kDmmTwoStr 的定长头 -----------------------------------------------------
struct DmmTwoStrHead {
    unsigned wpChars;       // 0 = wp 为 NULL；否则含 NUL 的 wchar_t 数
    unsigned lpChars;       // 同上（lp = 模块名）
};
static_assert(sizeof(DmmTwoStrHead) == 8, "DmmTwoStrHead 必须无填充（布局契约）");

// ---- 组装（代理侧：读**插件**内存，一律有界扫描）-----------------------------
// 返回 false = 某个串不可读或超长 ⇒ 调用方**拒答**（不产生跨进程调用）。
// out 里是「头 + 串 + 图标段」，可直接当 OOPM_NPPMCALL 的载荷尾随部分。
// ★ 图标**不参与**成败判断：编码不出来/太大就退化成 width==0 的头（见 AppendIcon）。
bool DmmBuildReg(const npp::DockedWidgetData& d, std::vector<unsigned char>& out);
bool DmmBuildTwoStr(const wchar_t* wp, const wchar_t* lp,
                    std::vector<unsigned char>& out);

// ---- 解析（宿主侧：读**自己的** wire 缓冲，长度全部重验）---------------------
// 视图里的指针指向**调用方给的缓冲内部**（本函数不分配、不拷贝）。
struct DmmRegView {
    bool ok = false;              // false = 布局不过（调用方拒答）
    UINT_PTR hClient = 0;
    UINT_PTR uMask = 0;
    int      dlgID = 0;
    const wchar_t* name = nullptr;    // NULL 表示插件传的就是 NULL
    const wchar_t* addInfo = nullptr;
    const wchar_t* module = nullptr;
    // 图标段（批次 137）：指针同样指向**调用方缓冲内部**；无图标时 width==0。
    unsigned iconWidth = 0;
    unsigned iconHeight = 0;
    unsigned iconColorBytes = 0;
    unsigned iconMaskBytes = 0;
    const unsigned char* iconColor = nullptr;   // 32bpp BGRA，top-down
    const unsigned char* iconMask = nullptr;    // 1bpp，top-down
};
DmmRegView DmmParseReg(const unsigned char* p, unsigned bytes);

// 解码（宿主侧）：把视图里的图标字节**重建**成本进程的 HICON。
// 无图标 / 布局不过 / GDI 失败 ⇒ 返回 nullptr ⇒ 调用方照常注册，只是没图标。
// ★ 返回的 HICON 归**调用方**所有（用完自行 DestroyIcon）。
HICON DmmIconFromView(const DmmRegView& v);

struct DmmTwoStrView {
    bool ok = false;
    const wchar_t* wp = nullptr;
    const wchar_t* lp = nullptr;
};
DmmTwoStrView DmmParseTwoStr(const unsigned char* p, unsigned bytes);

} // namespace oop
} // namespace xfs
