// test_sci_bridge.cpp — SCI_* **入参指针**族的布局表守卫（批次 113，批次 116 修订）。
//
// 为什么单开一个测试目标：这张表是"入参串在哪个参数槽、长度从哪来"的**唯一**
// 判据，两侧（代理读、编辑器还原）共用它。折进别的目标里，一旦"忘了注册进
// CMakeLists"，缺测会伪装成 100% 通过 —— 而这张表恰恰是"分错槽 = 静默把文本
// 送到错误的参数"或"当长度用 = 静默截断"的那类缺陷的唯一防线。
//
// 覆盖：
//   [1] 规模：逐条重数与 .inc 自报的 kSciInCount / 各布局计数对账
//   [2] 有序性：msg 严格升序、无重复（SciInLayoutOf 二分查找的前提）
//   [3] 二分 ↔ 线性：两种独立实现对撞（全表 + 表外边界值）
//   [4] 代表项：把每族的"决策"钉住，而不是描述现象
//   [5] 布局 ↔ 形状一致：入参族 ⟺ 形状表的 kInStr + kInBytes（两个方向都查）
//   [6] 三张派生判据的全枚举不变式（指针槽 / 长度来源 / 段数）
//   [7] ★ SciHostArgs 全枚举对撞：wire 值 + 本地缓冲 → 宿主 wp/lp
//   [8] ★ 长度桶的**精确**名单（含 AutoCShow 这个反例）
//   [9] 边界：表外与区间内空洞一律 kNone（= 不过桥）
//   [10] ★★ **真 Scintilla 反证**：把串过桥送到真控件，再用真控件读回来
//
// ★ 本守卫的边界（写进输出，避免下一个读它的人高估它）：
//   它证明「表内部自洽 + 与形状表一致 + 派生判据自洽 + **对真 Scintilla 成立**」，
//   **不**证明「表 ↔ Scintilla.iface 的名字映射」。后者是
//   scripts/gen-sci-marshal.py --check 的职责。两条守卫都必须跑，缺一不可。
//
// ★★ 为什么非要有 [10]（批次 116 的教训，值得单独记一笔）：
//   批次 113 的解析把 iface 的**空参数槽**压掉了，于是 `fun void SetText=2181(,
//   string text)` 这种（wParam 未用、串在 lParam）被判成"串在 wParam"—— 12 条
//   一起错，跨进程 SETTEXT/REPLACESEL/SETWORDCHARS… **恒返回 0、静默不生效**。
//   而**当时的全部守卫都是绿的**：
//     * 桶计数守卫数的是"有多少条"，不是"是哪几条" ⇒ 19 条自洽；
//     * e2e 夹具是照着同一张错表写的（把串塞进 wParam）⇒ 与假 Scintilla 自洽；
//     * 生成器的 A/B 对撞只比"带不带指针"，不比"在哪个槽"。
//   ⇒ 只有"把串真的送进**厂商编译过的**控件，再读回来"能证伪它。[10] 就是这条。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Scintilla.h>   // 真 Scintilla 的 SCI_* 编号（**不手抄**）

#include "../src/plugin/oop/SciBridge.h"
#include "../src/plugin/oop/SciMarshal.h"

namespace oop = xfs::oop;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int CountLayout(oop::SciInLayout lay) {
    int n = 0;
    for (unsigned i = 0; i < oop::kSciInCount; ++i)
        if (oop::kSciInTable[i].layout == lay) ++n;
    return n;
}

// 与 .inc 自报的计数逐项对账。写成表驱动，加布局时只改这里一处。
struct LayoutCount { oop::SciInLayout lay; unsigned declared; const char* name; };

static const LayoutCount kDeclared[] = {
    { oop::SciInLayout::kStrInLp,      oop::kSciInCountStrInLp,      "kStrInLp"      },
    { oop::SciInLayout::kStrInLpLen,   oop::kSciInCountStrInLpLen,   "kStrInLpLen"   },
    { oop::SciInLayout::kStrInWp,      oop::kSciInCountStrInWp,      "kStrInWp"      },
    { oop::SciInLayout::kStrBoth,      oop::kSciInCountStrBoth,      "kStrBoth"      },
    { oop::SciInLayout::kBytesInLpLen, oop::kSciInCountBytesInLpLen, "kBytesInLpLen" },
};

// 线性查找（与表里的二分实现**独立**）
static oop::SciInLayout LinearLookup(unsigned msg) {
    for (unsigned i = 0; i < oop::kSciInCount; ++i)
        if (oop::kSciInTable[i].msg == msg) return oop::kSciInTable[i].layout;
    return oop::SciInLayout::kNone;
}

// [1] 规模 + 自报计数
static void TestScale() {
    CHECK(oop::kSciInCount == 54);
    unsigned sum = 0;
    for (const LayoutCount& d : kDeclared) {
        const int actual = CountLayout(d.lay);
        if (static_cast<unsigned>(actual) != d.declared) {
            ++g_fail;
            std::printf("FAIL 桶 %s：实际 %d 条，.inc 自报 %u 条\n",
                        d.name, actual, d.declared);
        }
        sum += static_cast<unsigned>(actual);
    }
    // 桶和 == 表长：多一个漏归类的条目就会在这里露出来
    CHECK(sum == oop::kSciInCount);
    // 入参族 = 形状表的 kInStr + kInBytes（两个生成物必须自洽）
    CHECK(oop::kSciInCount == oop::kSciCountInStr + oop::kSciCountInBytes);
    std::printf("  [1] 54 条：%s\n",
                "kStrInLp 33 / kStrInLpLen 11 / kStrInWp 7 / kStrBoth 2 / kBytesInLpLen 1");
}

// [2] 有序性 + [5] 与形状表一致
static void TestOrderAndShapeAgreement() {
    for (unsigned i = 0; i < oop::kSciInCount; ++i) {
        CHECK(oop::kSciInTable[i].layout != oop::SciInLayout::kNone);
        if (i) CHECK(oop::kSciInTable[i - 1].msg < oop::kSciInTable[i].msg);
        CHECK(oop::kSciInTable[i].msg >= oop::kSciMsgMin);
        CHECK(oop::kSciInTable[i].msg <= oop::kSciMsgMax);
        CHECK(oop::kSciInTable[i].msg > WM_USER);
        // 布局表里的每一条，形状表必须判成"入参族"
        const oop::SciShape sh = oop::SciShapeOf(oop::kSciInTable[i].msg);
        CHECK(sh == oop::SciShape::kInStr || sh == oop::SciShape::kInBytes);
    }
    // 反方向：形状表里每一条入参族都必须有布局（没有"漏进表"的）
    int inb = 0, withLayout = 0;
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        const oop::SciShape sh = oop::kSciTable[i].shape;
        if (sh != oop::SciShape::kInStr && sh != oop::SciShape::kInBytes) continue;
        ++inb;
        if (oop::SciInLayoutOf(oop::kSciTable[i].msg) != oop::SciInLayout::kNone)
            ++withLayout;
    }
    CHECK(inb == 54);
    CHECK(withLayout == inb);
}

// [3] 二分 ↔ 线性对撞
static void TestLookupAgreement() {
    for (unsigned i = 0; i < oop::kSciInCount; ++i)
        CHECK(oop::SciInLayoutOf(oop::kSciInTable[i].msg) ==
              LinearLookup(oop::kSciInTable[i].msg));
    const unsigned probes[] = { 0, 1, WM_USER, 1999, 2000,
                                oop::kSciMsgMin - 1, oop::kSciMsgMin,
                                oop::kSciMsgMax, oop::kSciMsgMax + 1, 65535 };
    for (unsigned p : probes)
        CHECK(oop::SciInLayoutOf(p) == LinearLookup(p));
    // 区间内的空洞（有形状但不是入参族）也必须是 kNone。
    // 注意别拿 826 当区间长度：区间 [2001,4033] 有 2033 个编号，形状表只覆盖
    // 826 个（其余是 Scintilla 从未用过的号段），入参族只占 54 个。
    const unsigned span = oop::kSciMsgMax - oop::kSciMsgMin + 1;
    int gaps = 0;
    for (unsigned m = oop::kSciMsgMin; m <= oop::kSciMsgMax; ++m)
        if (oop::SciInLayoutOf(m) == oop::SciInLayout::kNone) ++gaps;
    CHECK(static_cast<unsigned>(gaps) == span - oop::kSciInCount);
    std::printf("  [3] 区间 %u..%u 共 %u 个编号，入参族 %u 个，其余 %d 个不过桥\n",
                oop::kSciMsgMin, oop::kSciMsgMax, span, oop::kSciInCount, gaps);
}

// [4] 代表项：把"决策"钉住（不是描述现象）
// ★ 批次 116 修正：SetText / ReplaceSel 的串在 **lParam**，不是 wParam。
//   iface 写的是 `fun void SetText=2181(, string text)` —— 首参为空（wParam 未用），
//   串在槽 1。批次 113 的解析把空槽压掉了 ⇒ 这两条（连同另外 10 条）被错判成
//   kStrInWp ⇒ 跨进程调用**恒返回 0、静默不生效**。见 [9] 的真机反证。
static void TestRepresentative() {
    struct Rep { unsigned msg; oop::SciInLayout lay; const char* name; };
    static const Rep kRep[] = {
        { 2001, oop::SciInLayout::kStrInLpLen,   "AddText(len,text)"            },
        { 2002, oop::SciInLayout::kBytesInLpLen, "AddStyledText(len,cells)"     },
        { 2003, oop::SciInLayout::kStrInLp,      "InsertText(pos,text)"         },
        { 2100, oop::SciInLayout::kStrInLp,      "AutoCShow(lenEntered,list)"   },
        { 2181, oop::SciInLayout::kStrInLp,      "SetText(,text) 串在 lp"       },
        { 2170, oop::SciInLayout::kStrInLp,      "ReplaceSel(,text) 串在 lp"    },
        { 2197, oop::SciInLayout::kStrInLpLen,   "SearchInTarget(len,pattern)"  },
        { 4004, oop::SciInLayout::kStrBoth,      "SetProperty(key,value)"       },
        { 2665, oop::SciInLayout::kStrBoth,      "SetRepresentation(a,b)"       },
        { 4005, oop::SciInLayout::kStrInLp,      "SetKeyWords(set,words)"       },
        { 2766, oop::SciInLayout::kStrInWp,      "SetRepresentationAppearance"  },
        { 2667, oop::SciInLayout::kStrInWp,      "ClearRepresentation(str,)"    },
        { 4010, oop::SciInLayout::kStrInWp,      "GetPropertyInt(key,default)"  },
    };
    for (const Rep& r : kRep) {
        const oop::SciInLayout got = oop::SciInLayoutOf(r.msg);
        if (got != r.lay) {
            ++g_fail;
            std::printf("FAIL 代表项 %s(%u)：布局 %s，期望 %s\n",
                        r.name, r.msg, oop::SciInLayoutName(got),
                        oop::SciInLayoutName(r.lay));
        }
    }
}

// [6] 三张派生判据的全枚举不变式
static void TestDerivedPredicates() {
    const oop::SciInLayout kAll[] = {
        oop::SciInLayout::kNone, oop::SciInLayout::kStrInLp,
        oop::SciInLayout::kStrInLpLen, oop::SciInLayout::kStrInWp,
        oop::SciInLayout::kStrBoth, oop::SciInLayout::kBytesInLpLen,
    };
    for (oop::SciInLayout lay : kAll) {
        bool wpPtr = true, lpPtr = true;
        oop::SciInPtrSlots(lay, wpPtr, lpPtr);
        if (lay == oop::SciInLayout::kNone) {
            CHECK(!wpPtr && !lpPtr);
            CHECK(!oop::SciInLenFromWp(lay));
            CHECK(oop::SciInSegments(lay) == 0);
            CHECK(std::strcmp(oop::SciInLayoutName(lay), "?") != 0);
            continue;
        }
        // 每种可桥接布局至少有一个指针槽（否则没有东西要搬）
        CHECK(wpPtr || lpPtr);
        // 长度只可能来自 wp（且只在两种"长度桶"布局上）
        const bool lenFromWp = oop::SciInLenFromWp(lay);
        CHECK(lenFromWp == (lay == oop::SciInLayout::kStrInLpLen ||
                            lay == oop::SciInLayout::kBytesInLpLen));
        if (lenFromWp) CHECK(!wpPtr);        // 长度桶：wp 是长度 ⇒ 不可能是指针
        // 段数：双指针两段，其余一段
        CHECK(oop::SciInSegments(lay) == (lay == oop::SciInLayout::kStrBoth ? 2u : 1u));
        CHECK(std::strcmp(oop::SciInLayoutName(lay), "?") != 0);
    }
    // 全表：每种布局的指针槽与段数都与布局语义一致
    for (unsigned i = 0; i < oop::kSciInCount; ++i) {
        const oop::SciInLayout lay = oop::kSciInTable[i].layout;
        bool wpPtr = false, lpPtr = false;
        oop::SciInPtrSlots(lay, wpPtr, lpPtr);
        CHECK(wpPtr || lpPtr);
    }
}

// [7] ★ SciHostArgs 全枚举对撞 —— 编辑器侧重建指针的**唯一**判据
static void TestHostArgs() {
    unsigned char bufA[8] = { 'A', 0, 0, 0, 0, 0, 0, 0 };
    unsigned char bufB[8] = { 'B', 0, 0, 0, 0, 0, 0, 0 };
    const UINT_PTR kArgWp = 0x1111;
    const UINT_PTR kArgLp = 0x2222;

    struct Case {
        oop::SciInLayout lay;
        UINT_PTR wantWp;   // 0 = 应为 bufA，1 = 应为 bufB，2 = 应为 kArgWp
        UINT_PTR wantLp;   // 同上（2 = 应为 kArgLp）
        const char* name;
    };
    static const Case kCases[] = {
        { oop::SciInLayout::kStrInLp,      2, 0, "lp=缓冲 / wp=值"        },
        { oop::SciInLayout::kStrInLpLen,   2, 0, "lp=缓冲 / wp=长度"      },
        { oop::SciInLayout::kBytesInLpLen, 2, 0, "lp=缓冲 / wp=长度(cells)" },
        { oop::SciInLayout::kStrInWp,      0, 2, "wp=缓冲 / lp=值"        },
        { oop::SciInLayout::kStrBoth,      0, 1, "wp=buf1 / lp=buf2"      },
    };
    auto resolve = [&](UINT_PTR want, WPARAM hostWp, LPARAM hostLp,
                       bool isWp) -> bool {
        if (want == 0) return isWp ? (reinterpret_cast<void*>(hostWp) == bufA)
                                   : (reinterpret_cast<void*>(hostLp) == bufA);
        if (want == 1) return isWp ? (reinterpret_cast<void*>(hostWp) == bufB)
                                   : (reinterpret_cast<void*>(hostLp) == bufB);
        return isWp ? (static_cast<UINT_PTR>(hostWp) == kArgWp)
                    : (static_cast<UINT_PTR>(hostLp) == kArgLp);
    };
    for (const Case& c : kCases) {
        WPARAM hostWp = 0;
        LPARAM hostLp = 0;
        oop::SciHostArgs(c.lay, kArgWp, kArgLp, bufA, bufB, hostWp, hostLp);
        if (!resolve(c.wantWp, hostWp, hostLp, true) ||
            !resolve(c.wantLp, hostWp, hostLp, false)) {
            ++g_fail;
            std::printf("FAIL SciHostArgs %s：wp=%p lp=%p\n", c.name,
                        reinterpret_cast<void*>(hostWp),
                        reinterpret_cast<void*>(hostLp));
        }
    }
    std::printf("  [7] SciHostArgs：5 种布局的 wp/lp 落点全部对撞通过\n");
}

// [8] ★ 长度桶的精确名单（含 AutoCShow 这个反例）
// 长度桶 = wp 就是缓冲字节数。判据是"首参名精确等于 length"（生成器冻结）。
// 这里用 **id 名单**独立再钉一次：Scintilla 升级导致名单变化时，生成器与
// 本守卫会各自报错（两个独立判据，不是同一处维护）。
static void TestLengthBucketRoster() {
    static const unsigned kLenFromWp[] = {
        2001,  // AddText
        2002,  // AddStyledText（cells）
        2073,  // SetStylingEx
        2194,  // ReplaceTarget
        2195,  // ReplaceTargetRE
        2197,  // SearchInTarget
        2282,  // AppendText
        2420,  // CopyText
        2672,  // ChangeInsertion
        2771,  // ReplaceRectangular
        2779,  // ReplaceTargetMinimal
        2801,  // ChangeLastUndoActionText
    };
    int n = 0;
    for (unsigned i = 0; i < oop::kSciInCount; ++i)
        if (oop::SciInLenFromWp(oop::kSciInTable[i].layout)) ++n;
    CHECK(n == static_cast<int>(sizeof(kLenFromWp) / sizeof(kLenFromWp[0])));
    for (unsigned msg : kLenFromWp) {
        if (!oop::SciInLenFromWp(oop::SciInLayoutOf(msg))) {
            ++g_fail;
            std::printf("FAIL %u 应在长度桶里（wp 是缓冲字节数）\n", msg);
        }
    }
    // ★ 反例：AutoCShow 的首参叫 lengthEntered（已输入字符数），**不是**缓冲
    //   长度。它必须走 NUL 扫描 —— 若落进长度桶，补全列表会被截断，而且
    //   不崩、不报错（静默功能缺陷）。这条断言就是那个陷阱的守门人。
    CHECK(oop::SciInLayoutOf(2100) == oop::SciInLayout::kStrInLp);
    CHECK(!oop::SciInLenFromWp(oop::SciInLayoutOf(2100)));
    std::printf("  [8] 长度桶 12 条；AutoCShow(2100) 走 NUL 扫描（lengthEntered 不是长度）\n");
}

// [9] 边界：表外与区间内空洞一律不过桥
static void TestBoundaries() {
    const unsigned outside[] = { 0, 1, WM_USER, 1999, 2000,
                                 oop::kSciMsgMax + 1, 5000, 65535 };
    for (unsigned m : outside)
        CHECK(!oop::SciBridgeableIn(m));
    // 区间内但不在表里（3000 是形状表里的空洞；用 3001 更稳）
    CHECK(!oop::SciBridgeableIn(3001));
    // 表里第一条/最后一条必须可桥接（否则首尾探测没意义）
    CHECK(oop::SciBridgeableIn(oop::kSciInTable[0].msg));
    CHECK(oop::SciBridgeableIn(oop::kSciInTable[oop::kSciInCount - 1].msg));
    std::printf("  [9] 表外 %d 个 + 区间内空洞：一律不过桥\n",
                static_cast<int>(sizeof(outside) / sizeof(outside[0])));
}

// ============================================================================
// [10] ★★ 真 Scintilla 反证：串真的落在 Scintilla **读的那个槽**里
// ============================================================================
// 手法：走**产品代码**（SciBridgeCall）把串过桥送进真控件，再用真控件读回来。
// 关键是每条都配一个**反证**：把同一个串塞进**另一个槽**，真控件必须**不认**。
// 只验"过桥后值对了"是不够的 —— 那证明不了"槽"是对的（碰巧两个槽都能生效、
// 或控件恰好容忍了错槽，都会绿）。
//
// ⚠ 需要 Scintilla.dll（exe 与它同在 build/bin/Release/，按 exe 目录找到）。
//   找不到时**报失败而不是跳过** —— 跳过等于本节静默消失，而本节是本批唯一
//   能证伪"串在哪个参数槽"的判据（见文件头那段教训）。
static HWND g_sci = nullptr;

static bool MakeScintilla() {
    if (!::LoadLibraryW(L"Scintilla.dll")) {
        ++g_fail;
        std::printf("FAIL 加载 Scintilla.dll 失败（err=%lu）。\n"
                    "  ⚠ [10] 是唯一能证伪「串在哪个参数槽」的判据，不能跳过。\n",
                    static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    g_sci = ::CreateWindowExW(0, L"Scintilla", L"", WS_OVERLAPPEDWINDOW,
                              0, 0, 400, 300, nullptr, nullptr, nullptr, nullptr);
    if (!g_sci) {
        ++g_fail;
        std::printf("FAIL CreateWindowEx(\"Scintilla\") 失败（err=%lu）\n",
                    static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    return true;
}

static std::string SciText() {
    char b[256] = { 0 };
    ::SendMessageW(g_sci, SCI_GETTEXT, sizeof(b), reinterpret_cast<LPARAM>(b));
    return std::string(b);
}

// 走**产品代码**把一段串过桥送进去（wire 的值槽由调用方给）。返回是否过桥成功。
static bool BridgeStr(unsigned sciMsg, UINT_PTR argWp, UINT_PTR argLp, const char* s) {
    const std::size_t n = std::strlen(s) + 1;
    std::vector<unsigned char> w(sizeof(oop::SciCallWire) + n, 0);
    auto* cw = reinterpret_cast<oop::SciCallWire*>(w.data());
    cw->magic = oop::kMagic;
    cw->msg = oop::OOPM_SCICALL;
    cw->sciMsg = sciMsg;
    cw->argWp = argWp;
    cw->argLp = argLp;
    cw->reqId = 1;
    cw->replyTo = 0;
    cw->inBytes = static_cast<unsigned>(n);
    cw->inBytes2 = 0;
    std::memcpy(w.data() + sizeof(oop::SciCallWire), s, n);
    LRESULT out = 0;
    return oop::SciBridgeCall(g_sci, *cw, static_cast<DWORD>(w.size()), out);
}

static void TestRealScintillaSlot() {
    if (!g_sci) return;

    // ---- SetText(2181)：串在 **lParam**（iface：`(, string text)`）----------
    ::SendMessageW(g_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(""));
    CHECK(BridgeStr(SCI_SETTEXT, 0, 0, "alpha"));
    CHECK(SciText() == "alpha");
    // ★ 反证：把**同一个串**塞进 wp（错槽）⇒ 真控件**不认** —— lParam==0 时
    //   Editor.cxx:6317 直接 `return 0`，文档一个字节都不动。
    //   "错槽"的危害正在这里：**静默什么都不做**，不崩、不报错。
    ::SendMessageW(g_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(""));
    ::SendMessageW(g_sci, SCI_SETTEXT, reinterpret_cast<WPARAM>("gamma"), 0);
    CHECK(SciText().empty());

    // ---- ReplaceSel(2170)：同样在 **lParam** -------------------------------
    ::SendMessageW(g_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>("hello"));
    ::SendMessageW(g_sci, SCI_SETSEL, 0, 5);
    CHECK(BridgeStr(SCI_REPLACESEL, 0, 0, "world"));
    CHECK(SciText() == "world");
    ::SendMessageW(g_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>("hello"));
    ::SendMessageW(g_sci, SCI_SETSEL, 0, 5);
    ::SendMessageW(g_sci, SCI_REPLACESEL, reinterpret_cast<WPARAM>("nope"), 0);
    CHECK(SciText() == "hello");          // 错槽：没被替换

    // ---- SetWordChars(2077)：在 **lParam**；用出参族的 GetWordChars 读回来 --
    // （GetWordChars(2646) 是 kOutStr/kNoNul，直接进程内调用即可，不必过桥。）
    //
    // ⚠ 回读是**降序**的：厂商实现 CharClassify.cxx:50 是
    //   `for (int ch = maxChar - 1; ch >= 0; --ch)` ⇒ 送进去 "QZ" 读回来是 "ZQ"。
    //   这不是桥接的错，是厂商的枚举顺序 ⇒ 期望值按**厂商实现**写。
    CHECK(BridgeStr(SCI_SETWORDCHARS, 0, 0, "QZ"));
    {
        char b[64] = { 0 };
        const LRESULT gw = ::SendMessageW(g_sci, SCI_GETWORDCHARS, 0,
                                          reinterpret_cast<LPARAM>(b));
        // 若表把这条判成 kStrInWp，串就会落到 wp ⇒ 真控件读到 lParam==0 ⇒
        // SetDefaultCharClasses(false) ⇒ 这里会是**空串**（或那一长串默认字符类），
        // 绝不是恰好 2 个字符 —— 所以 `gw == 2` 这条对"错槽"是有区分力的。
        CHECK(gw == 2);
        CHECK(std::strcmp(b, "ZQ") == 0);
    }
    // ★ 正对照：换一个串，"降序"这条性质仍然成立。
    //   这一条是必需的 —— 否则"期望写成 ZQ"与"桥接把串反着送进去了"这两种
    //   解释都能让上面那条通过。换个串就能把两者分开：若桥接真的反转了串，
    //   送 "Mz" 会变成 "zM"，再降序回读就又是 "Mz" ⇒ 下面这条会红。
    //   ⚠ 对照串必须挑**升序**的（'M'=0x4D < 'z'=0x7A）；若挑成 "aM"
    //   （'M' < 'a'，本身已是降序），输入就等于输出，这条对照会退化成空断言。
    CHECK(BridgeStr(SCI_SETWORDCHARS, 0, 0, "Mz"));
    {
        char b[64] = { 0 };
        const LRESULT gw = ::SendMessageW(g_sci, SCI_GETWORDCHARS, 0,
                                          reinterpret_cast<LPARAM>(b));
        CHECK(gw == 2 && std::strcmp(b, "zM") == 0);
    }

    // ---- 反向：**首参非空**的那 7 条确实在 wp ---------------------------------
    // 取 ClearRepresentation(2667) = `(string encodedCharacter,)` 当代表：
    // 设 repr "a" -> "AA"，过桥清掉 "a"，再查它应当**消失**。
    // 若表把它判成 kStrInLp（批次 113 那版就是），串会落到 lp、wParam 变 0 ⇒
    // 厂商代码走 `std::string_view(nullptr)`（Editor.cxx:8634 →
    // PositionCache.cxx:716）⇒ **访问违例**。也就是说：wp 族的错槽后果是**崩**，
    // 而 lp 族（上面三条）的错槽后果是**静默不做**。两种都坏，但这一条不能在本
    // 进程里"演"错槽 —— 演出来就是整个测试进程死掉、连 stdout 一起丢。
    // 所以这里只有正例；"为什么不能有错槽反证"本身就是本节要传达的事实。
    ::SendMessageW(g_sci, SCI_SETREPRESENTATION, reinterpret_cast<WPARAM>("a"),
                   reinterpret_cast<LPARAM>("AA"));
    CHECK(BridgeStr(SCI_CLEARREPRESENTATION, 0, 0, "a"));
    {
        char b[16] = { 0 };
        const LRESULT n = ::SendMessageW(g_sci, SCI_GETREPRESENTATION,
                                         reinterpret_cast<WPARAM>("a"),
                                         reinterpret_cast<LPARAM>(b));
        CHECK(n == 0 && b[0] == 0);       // 已被清掉 ⇒ 0 字节
    }

    std::printf("  [10] 真 Scintilla：SetText/ReplaceSel/SetWordChars 的串在 **lp**"
                "（错槽 wp 静默不生效）；ClearRepresentation 的在 **wp**"
                "（错槽会崩，故只验正例）\n");
}

int main() {
    std::printf("== test_sci_bridge (batch 113 + 116 修订) ==\n");
    TestScale();
    TestOrderAndShapeAgreement();
    TestLookupAgreement();
    TestRepresentative();
    TestDerivedPredicates();
    TestHostArgs();
    TestLengthBucketRoster();
    TestBoundaries();
    if (MakeScintilla()) TestRealScintillaSlot();
    std::printf("== %s (%d failure%s) ==\n", g_fail ? "FAILED" : "ALL PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    std::printf("NOTE: 本守卫证明表内部自洽 + 与形状表一致 + **对真 Scintilla 成立**；\n"
                "      表 ↔ Scintilla.iface 的名字映射由 scripts/gen-sci-marshal.py "
                "--check 负责。两条都必须跑。\n");
    if (g_sci) ::DestroyWindow(g_sci);
    return g_fail ? 1 : 0;
}
