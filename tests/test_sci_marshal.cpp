// test_sci_marshal.cpp — SCI_* 通道形状表的守卫（批次 112）。
//
// 为什么单开一个测试目标：这张表是"哪些 SCI_* 可以跨进程直发"的**唯一**判据。
// 折进别的目标里，一旦"忘了注册进 CMakeLists"，缺测会伪装成 100% 通过 ——
// 而这张表恰恰是"漏一条 = 宿主被外来指针写坏"的那类缺陷的唯一防线。
//
// 覆盖：
//   [1] 规模：逐条重数与 .inc 自报的 kSciCount / 各形状计数对账
//   [2] 有序性：msg 严格升序、无重复（SciShapeOf 二分查找的前提）
//   [3] 二分 ↔ 线性：两种独立实现对撞（全表 + 表外边界值）
//   [4] 代表项：把每族的"决策"钉住，而不是描述现象
//   [5] 区间：表内每条都在 [kSciMsgMin, kSciMsgMax] 且 > WM_USER
//   [6] 边界：表外编号（含 0 / WM_USER / min-1 / max+1）一律不可直发
//   [7] 不变式：kValue 的 why 为 nullptr，其余形状的 why 非空；名字全非空
//   [8] 直发集合 == kValue 集合，且带指针条数为 110（与生成器的 A 判据一致）
//
// ★ 本守卫的边界（写进输出，避免下一个读它的人高估它）：
//   它证明「表内部自洽 + 与生成器自报一致」，**不**证明「表 ↔ Scintilla.iface」。
//   后者是 scripts/gen-sci-marshal.py --check 的职责（它读 iface / ScintillaCall.h /
//   ScintillaMessages.h 三份文件对撞）。两条守卫都必须跑，缺一不可。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "../src/plugin/oop/SciMarshal.h"

namespace oop = xfs::oop;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int CountShape(oop::SciShape sh) {
    int n = 0;
    for (unsigned i = 0; i < oop::kSciCount; ++i)
        if (oop::kSciTable[i].shape == sh) ++n;
    return n;
}

// 与 .inc 自报的计数逐项对账。写成表驱动，加形状时只改这里一处。
struct ShapeCount { oop::SciShape sh; unsigned declared; const char* name; };

static const ShapeCount kDeclared[] = {
    { oop::SciShape::kValue,    oop::kSciCountValue,    "kValue"    },
    { oop::SciShape::kInStr,    oop::kSciCountInStr,    "kInStr"    },
    { oop::SciShape::kOutStr,   oop::kSciCountOutStr,   "kOutStr"   },
    { oop::SciShape::kInOutStr, oop::kSciCountInOutStr, "kInOutStr" },
    { oop::SciShape::kStruct,   oop::kSciCountStruct,   "kStruct"   },
    { oop::SciShape::kInBytes,  oop::kSciCountInBytes,  "kInBytes"  },
    { oop::SciShape::kRawPtr,   oop::kSciCountRawPtr,   "kRawPtr"   },
    { oop::SciShape::kPtrRet,   oop::kSciCountPtrRet,   "kPtrRet"   },
};

// [1] 规模 + 自报计数
static void TestScale() {
    CHECK(oop::kSciCount == 826);
    unsigned sum = 0;
    for (const ShapeCount& s : kDeclared) {
        const int actual = CountShape(s.sh);
        if (actual != (int)s.declared) {
            ++g_fail;
            std::printf("FAIL shape %s: table has %d, .inc declares %u\n",
                        s.name, actual, s.declared);
        }
        sum += (unsigned)actual;
    }
    // 八种形状之和必须**正好**等于表长：漏掉任何一种形状都会在这里红
    CHECK(sum == oop::kSciCount);
    std::printf("[1] scale: %u entries, 8 shape buckets sum to %u\n",
                oop::kSciCount, sum);
}

// [2] 严格升序 + 无重复
static void TestOrdered() {
    for (unsigned i = 1; i < oop::kSciCount; ++i) {
        if (oop::kSciTable[i].msg <= oop::kSciTable[i - 1].msg) {
            ++g_fail;
            std::printf("FAIL not strictly ascending at index %u: %u then %u\n",
                        i, oop::kSciTable[i - 1].msg, oop::kSciTable[i].msg);
            break;
        }
    }
    CHECK(oop::kSciTable[0].msg == oop::kSciMsgMin);
    CHECK(oop::kSciTable[oop::kSciCount - 1].msg == oop::kSciMsgMax);
    std::printf("[2] ordered: strictly ascending, %u..%u\n",
                oop::kSciMsgMin, oop::kSciMsgMax);
}

// [3] 二分 ↔ 线性对撞：两种独立实现，全表 + 表外边界值都必须一致
static oop::SciShape LinearShapeOf(unsigned msg) {
    for (unsigned i = 0; i < oop::kSciCount; ++i)
        if (oop::kSciTable[i].msg == msg) return oop::kSciTable[i].shape;
    return oop::SciShape::kUnknown;
}

static void TestBinaryVsLinear() {
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        const unsigned msg = oop::kSciTable[i].msg;
        if (oop::SciShapeOf(msg) != LinearShapeOf(msg)) {
            ++g_fail;
            std::printf("FAIL binary != linear for msg %u\n", msg);
            break;
        }
    }
    // 表外/边界值：二分最容易在 lo/hi 收敛处写错，这里逐点钉住
    const unsigned probes[] = { 0u, 1u, WM_USER, 1999u, 2000u,
                                oop::kSciMsgMin - 1, oop::kSciMsgMin,
                                oop::kSciMsgMax, oop::kSciMsgMax + 1, 65535u };
    for (unsigned p : probes) {
        const oop::SciShape got = oop::SciShapeOf(p);
        const oop::SciShape want = LinearShapeOf(p);
        if (got != want) {
            ++g_fail;
            std::printf("FAIL probe %u: binary %d vs linear %d\n",
                        p, (int)got, (int)want);
        }
    }
    std::printf("[3] binary search matches linear scan on %u entries + 10 probes\n",
                oop::kSciCount);
}

// [4] 代表项：每族的决策钉死（改了判定就会红）
static void TestRepresentatives() {
    struct Rep { unsigned msg; oop::SciShape want; const char* why; };
    static const Rep kReps[] = {
        // 纯值 —— 必须直发
        { 2006, oop::SciShape::kValue, "SCI_GETLENGTH" },
        { 2007, oop::SciShape::kValue, "SCI_GETCHARAT" },
        { 2008, oop::SciShape::kValue, "SCI_GETCURRENTPOS" },
        { 2009, oop::SciShape::kValue, "SCI_GETANCHOR" },
        { 2025, oop::SciShape::kValue, "SCI_GOTOPOS" },
        { 2154, oop::SciShape::kValue, "SCI_GETLINECOUNT" },
        { 2183, oop::SciShape::kValue, "SCI_GETTEXTLENGTH" },
        // 带指针 —— 必须拒答，且族分类正确
        { 2001, oop::SciShape::kInStr,    "SCI_ADDTEXT" },
        { 2170, oop::SciShape::kInStr,    "SCI_REPLACESEL" },
        { 2197, oop::SciShape::kInStr,    "SCI_SEARCHINTARGET" },
        { 2153, oop::SciShape::kOutStr,   "SCI_GETLINE" },
        { 2161, oop::SciShape::kOutStr,   "SCI_GETSELTEXT" },
        { 2182, oop::SciShape::kOutStr,   "SCI_GETTEXT" },
        { 2646, oop::SciShape::kOutStr,   "SCI_GETWORDCHARS" },
        { 4008, oop::SciShape::kInOutStr, "SCI_GETPROPERTY" },
        { 2015, oop::SciShape::kStruct,   "SCI_GETSTYLEDTEXT" },
        { 2150, oop::SciShape::kStruct,   "SCI_FINDTEXT" },
        { 2162, oop::SciShape::kStruct,   "SCI_GETTEXTRANGE" },
        { 2002, oop::SciShape::kInBytes,  "SCI_ADDSTYLEDTEXT" },
        { 2358, oop::SciShape::kRawPtr,   "SCI_SETDOCPOINTER" },
        { 4033, oop::SciShape::kRawPtr,   "SCI_SETILEXER" },
        { 2357, oop::SciShape::kPtrRet,   "SCI_GETDOCPOINTER" },
        { 2520, oop::SciShape::kPtrRet,   "SCI_GETCHARACTERPOINTER" },
    };
    for (const Rep& r : kReps) {
        const oop::SciShape got = oop::SciShapeOf(r.msg);
        if (got != r.want) {
            ++g_fail;
            std::printf("FAIL %s (%u): shape %s, expected %s\n", r.why, r.msg,
                        oop::SciShapeName(got), oop::SciShapeName(r.want));
        }
    }
    std::printf("[4] representatives: %zu pinned\n", sizeof(kReps) / sizeof(kReps[0]));
}

// [5] 区间不变式
static void TestRanges() {
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        const unsigned msg = oop::kSciTable[i].msg;
        if (!oop::SciInRange(msg)) {
            ++g_fail;
            std::printf("FAIL msg %u outside [%u,%u]\n", msg,
                        oop::kSciMsgMin, oop::kSciMsgMax);
            break;
        }
        if (msg <= WM_USER) {          // 区间之上的消息才"不封送参数"
            ++g_fail;
            std::printf("FAIL msg %u is not above WM_USER (%u)\n", msg, WM_USER);
            break;
        }
    }
    std::printf("[5] ranges: every entry in [%u,%u] and above WM_USER\n",
                oop::kSciMsgMin, oop::kSciMsgMax);
}

// [6] 边界：表外编号一律不可直发（"安全方向"的不变式）
static void TestBoundaries() {
    const unsigned outside[] = { 0u, 1u, WM_USER, 1999u, 2000u,
                                 oop::kSciMsgMin - 1, oop::kSciMsgMax + 1, 65535u };
    for (unsigned m : outside) {
        if (oop::SciForwardable(m)) {
            ++g_fail;
            std::printf("FAIL msg %u is outside the table but forwardable\n", m);
        }
    }
    // 区间内但表外的编号（表不是连续区间）也必须不可直发
    unsigned inRangeUnknown = 0;
    for (unsigned m = oop::kSciMsgMin; m <= oop::kSciMsgMax; ++m) {
        if (oop::SciShapeOf(m) == oop::SciShape::kUnknown) {
            if (oop::SciForwardable(m)) {
                ++g_fail;
                std::printf("FAIL in-range unknown msg %u is forwardable\n", m);
            }
            ++inRangeUnknown;
        }
    }
    std::printf("[6] boundaries: 8 outside values refused; %u in-range gaps refused\n",
                inRangeUnknown);
}

// [7] 不变式：why / name
static void TestWhyInvariant() {
    const oop::SciShape all[] = {
        oop::SciShape::kValue, oop::SciShape::kInStr, oop::SciShape::kOutStr,
        oop::SciShape::kInOutStr, oop::SciShape::kStruct, oop::SciShape::kInBytes,
        oop::SciShape::kRawPtr, oop::SciShape::kPtrRet, oop::SciShape::kUnknown,
    };
    for (oop::SciShape sh : all) {
        const char* nm = oop::SciShapeName(sh);
        CHECK(nm && nm[0]);
        const char* why = oop::SciRefusalWhy(sh);
        if (sh == oop::SciShape::kValue)
            CHECK(why == nullptr);          // 能直发的没有"原因"
        else
            CHECK(why && why[0]);           // 拒答的必须说清为什么
    }
    std::printf("[7] invariants: names non-empty; why null iff kValue\n");
}

// [8] 直发集合 == kValue 集合；带指针条数 == 生成器的 A 判据
static void TestForwardSet() {
    unsigned fwd = 0, ptr = 0;
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        const oop::SciEntry& e = oop::kSciTable[i];
        const bool forwardable = oop::SciForwardable(e.msg);
        if (forwardable) ++fwd;
        if (e.shape != oop::SciShape::kValue) ++ptr;
        if (forwardable != (e.shape == oop::SciShape::kValue)) {
            ++g_fail;
            std::printf("FAIL msg %u: forwardable=%d but shape=%s\n",
                        e.msg, (int)forwardable, oop::SciShapeName(e.shape));
            break;
        }
    }
    CHECK(fwd == 716);
    CHECK(ptr == 110);                     // 与 scripts/gen-sci-marshal.py 的 A 判据一致
    std::printf("[8] forward set: %u forwardable / %u pointer-carrying\n", fwd, ptr);
}

int main() {
    std::printf("== test_sci_marshal: SCI_* relay shape-table guard ==\n");
    TestScale();
    TestOrdered();
    TestBinaryVsLinear();
    TestRepresentatives();
    TestRanges();
    TestBoundaries();
    TestWhyInvariant();
    TestForwardSet();
    std::printf("NOTE: this guard proves the TABLE is self-consistent and matches the\n");
    std::printf("      counts the generator declared. It does NOT prove table <-> iface;\n");
    std::printf("      that is scripts/gen-sci-marshal.py --check (three-file cross-check).\n");
    std::printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
