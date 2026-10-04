// test_sci_out.cpp — SCI_* **出参指针**族的容量/NUL 分类表守卫 + **真 Scintilla
// 实证**（批次 114；批次 115 起同时覆盖出入参族 kInOutStr 的**分类**）。
//
// ⚠ 批次 115 起 `SciOutKindOf` 查**两张**表：kSciOutTable（出参族 30 条）与
//   kSciInOutTable（出入参族 5 条）。两者共用同一套 SciOutKind 桶空间（出参侧
//   容量规则完全一样），所以本文件的桶守卫对 35 条一体成立。
//   出入参族的**入参串搬运**由 tests/test_sci_inout.cpp 负责，本文件只钉
//   "它在哪张表里、属于哪个桶、要不要带入参串"。
//
// 为什么单开一个测试目标：这张表是"宿主该分配多大缓冲、回传给插件多少字节"的
// **唯一**判据，两侧（宿主探长度+真调用、代理拷回插件缓冲）共用它。折进别的
// 目标里，一旦"忘了注册进 CMakeLists"，缺测会伪装成 100% 通过 —— 而这张表恰恰
// 是"越界写 1 字节（堆破坏且多半不崩）"这类缺陷的唯一防线。
//
// ★ 本文件的独特之处：它带**判据 B**（真 Scintilla 实证）。
//   表是从 Scintilla 源码读出来冻结的（判据 A），而"读源码"会看漏分支 ——
//   本批就实证了一次：Editor::GetTag 的写入量取决于有没有配 regex
//   （有 ⇒ need+1；无 ⇒ **只写 1 字节**），而返回值恒是那个先写死的 length。
//   静态读只看到 memcpy 那一支，会把它错判成 kNul。见 TestRealScintillaAll。
//
// 为什么"没被拒答"就是分类正确的判据（本批的核心机制）：
//   SciBridgeOutCall 真调用时给的缓冲是 copied + 哨兵余量，调用后做两个检查：
//     ① [copied, end) 必须仍是哨兵 ⇒ 写多了会踩哨兵；
//     ② 带 NUL 的桶必须 after[copied-1] == 0 ⇒ NUL 语义搞反会被抓。
//   于是**分类错了就必然被拒答**：
//     · kNul 错标成 kNoNul ⇒ copied 少 1 ⇒ Scintilla 写的那个 NUL 落在哨兵区 ⇒ ① 红
//     · kNoNul 错标成 kNul ⇒ copied 多 1 ⇒ 最后一格是哨兵不是 NUL ⇒ ② 红
//     · kClampWpNul 错标成 kNul（wp < need 时）⇒ 写停在 wp+1，after[need] 是哨兵 ⇒ ② 红
//   ⇒ "对每条可桥接消息真过桥都必须成功"这一条断言，就是分类的正确性判据。
//
// 覆盖：
//   [1] 规模：逐条重数与 .inc 自报的 kSciOutCount / 各桶计数对账
//   [2] 有序性：msg 严格升序、无重复（SciOutKindOf 二分查找的前提）
//   [2b]★ 两张表**互不相交**（合并查找的前提；重叠会让顺序静默决定胜负）
//   [3] 二分 ↔ 线性：两种独立实现对撞（**两张表的并集** + 表外边界值）
//   [4] 出参族 ⟺ 形状表的 kOutStr（两个方向都查）
//   [5] ★ SciOutBytesToCopy 全枚举 + 不变式（含 need 越界 → 0）
//   [6] ★ SciOutWroteOnlyExpected 正反例（把两个检查各自单独钉住）
//   [7] ★ 拒答桶的精确名单（TargetAsUTF8 / GetTag，附各自的反例证据）
//   [8] ★★ 真 Scintilla：30 条逐一真过桥，分类错必然拒答
//   [9] ★★ GetCurLine 的返回值是**光标列号**，不是长度（负控②的守门人）
//   [10] 查长度路径（writeBack=0）不写任何字节
//   [11] 边界：表外与区间内空洞一律 kNone（= 不过桥）
//
// ★ 本守卫的边界（写进输出，避免下一个读它的人高估它）：
//   它证明「表内部自洽 + 与形状表一致 + 派生判据自洽 + **对真 Scintilla 成立**」，
//   **不**证明「表 ↔ Scintilla.iface 的名字映射」。后者是
//   scripts/gen-sci-marshal.py --check 的职责。两条守卫都必须跑，缺一不可。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include <Scintilla.h>   // 真 Scintilla 的 SCI_* 编号（**不手抄**，来自厂商头文件）

#include "../src/plugin/oop/SciBridge.h"
#include "../src/plugin/oop/SciMarshal.h"

namespace oop = xfs::oop;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int CountKind(oop::SciOutKind k) {
    int n = 0;
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        if (oop::kSciOutTable[i].kind == k) ++n;
    return n;
}

// 与 .inc 自报的计数逐项对账。写成表驱动，加桶时只改这里一处。
struct KindCount { oop::SciOutKind kind; unsigned declared; const char* name; };

static const KindCount kDeclared[] = {
    { oop::SciOutKind::kNul,           oop::kSciOutCountNul,           "kNul"           },
    { oop::SciOutKind::kNoNul,         oop::kSciOutCountNoNul,         "kNoNul"         },
    { oop::SciOutKind::kClampWpNul,    oop::kSciOutCountClampWpNul,    "kClampWpNul"    },
    { oop::SciOutKind::kRefuseRuntime, oop::kSciOutCountRefuseRuntime, "kRefuseRuntime" },
};

// 线性查找（与表里的二分实现**独立**）。批次 115 起必须扫**两张**表，否则
// 出入参族那 5 条会让"二分 ↔ 线性对撞"变成假绿（线性侧全说 kNone）。
static oop::SciOutKind LinearLookup(unsigned msg) {
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i)
        if (oop::kSciInOutTable[i].msg == msg) return oop::kSciInOutTable[i].kind;
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        if (oop::kSciOutTable[i].msg == msg) return oop::kSciOutTable[i].kind;
    return oop::SciOutKind::kNone;
}

// [1] 规模 + 自报计数
static void TestScale() {
    CHECK(oop::kSciOutCount == 30);
    unsigned sum = 0;
    for (const KindCount& d : kDeclared) {
        const int actual = CountKind(d.kind);
        if (static_cast<unsigned>(actual) != d.declared) {
            ++g_fail;
            std::printf("FAIL 桶 %s：实际 %d 条，.inc 自报 %u 条\n",
                        d.name, actual, d.declared);
        }
        sum += static_cast<unsigned>(actual);
    }
    // 桶和 == 表长：多一个漏归类的条目就会在这里露出来
    CHECK(sum == oop::kSciOutCount);
    // 出参族 = 形状表的 kOutStr（两个生成物必须自洽）
    CHECK(oop::kSciOutCount == oop::kSciCountOutStr);
    std::printf("  [1] 30 条：kNul 14 / kNoNul 12 / kClampWpNul 2 / kRefuseRuntime 2\n");
}

// [2] 有序性 + [4] 与形状表一致
static void TestOrderAndShapeAgreement() {
    for (unsigned i = 0; i < oop::kSciOutCount; ++i) {
        CHECK(oop::kSciOutTable[i].kind != oop::SciOutKind::kNone);
        if (i) CHECK(oop::kSciOutTable[i - 1].msg < oop::kSciOutTable[i].msg);
        CHECK(oop::kSciOutTable[i].msg >= oop::kSciMsgMin);
        CHECK(oop::kSciOutTable[i].msg <= oop::kSciMsgMax);
        CHECK(oop::kSciOutTable[i].msg > WM_USER);
        // 表里的每一条，形状表必须判成 kOutStr
        CHECK(oop::SciShapeOf(oop::kSciOutTable[i].msg) == oop::SciShape::kOutStr);
    }
    // 反方向：形状表里每一条 kOutStr 都必须有分类（没有"漏进表"的）
    int outs = 0, withKind = 0;
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        if (oop::kSciTable[i].shape != oop::SciShape::kOutStr) continue;
        ++outs;
        if (oop::SciOutKindOf(oop::kSciTable[i].msg) != oop::SciOutKind::kNone)
            ++withKind;
    }
    CHECK(outs == 30);
    CHECK(withKind == outs);
    // 三族互不相交（同一编号不可能既在入参族又在出参族）
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        CHECK(!oop::SciBridgeableIn(oop::kSciOutTable[i].msg));
    // 反方向（批次 115）：形状表里每一条 kInOutStr 都必须在出入参表里
    int inouts = 0, inoutWithKind = 0;
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        if (oop::kSciTable[i].shape != oop::SciShape::kInOutStr) continue;
        ++inouts;
        if (oop::SciOutKindOf(oop::kSciTable[i].msg) != oop::SciOutKind::kNone)
            ++inoutWithKind;
        // 出入参族的**入参串**必须被判成"要带串"
        CHECK(oop::SciOutNeedsInStr(oop::kSciTable[i].msg));
    }
    CHECK(inouts == oop::kSciInOutCount);
    CHECK(inoutWithKind == inouts);
}

// [2b] ★ 两张表互不相交 —— 合并查找（先 kSciInOutTable 再 kSciOutTable）的前提。
// 重叠的话"哪张表赢"由查找顺序静默决定：同一条消息会同时有出参桶和入参串语义，
// 那是一个设计错误，不是可以靠顺序掩盖的细节。
static void TestTablesDisjoint() {
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i) {
        const unsigned m = oop::kSciInOutTable[i].msg;
        for (unsigned j = 0; j < oop::kSciOutCount; ++j) {
            if (oop::kSciOutTable[j].msg == m) {
                ++g_fail;
                std::printf("FAIL 编号 %u 同时出现在两张出参表里\n", m);
            }
        }
    }
    // 逐条重数：两表各自的规模之和 == SciOutKindOf 认得的条数
    int known = 0;
    for (unsigned m = oop::kSciMsgMin; m <= oop::kSciMsgMax; ++m)
        if (oop::SciOutKindOf(m) != oop::SciOutKind::kNone) ++known;
    CHECK(static_cast<unsigned>(known) == oop::kSciOutCount + oop::kSciInOutCount);
    std::printf("  [2b] 两表互不相交：%u + %u = %d 条\n",
                oop::kSciOutCount, oop::kSciInOutCount, known);
}

// [3] 二分 ↔ 线性对撞
static void TestLookupAgreement() {
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        CHECK(oop::SciOutKindOf(oop::kSciOutTable[i].msg) ==
              LinearLookup(oop::kSciOutTable[i].msg));
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i)
        CHECK(oop::SciOutKindOf(oop::kSciInOutTable[i].msg) ==
              LinearLookup(oop::kSciInOutTable[i].msg));
    const unsigned probes[] = { 0, 1, WM_USER, 1999, 2000,
                                oop::kSciMsgMin - 1, oop::kSciMsgMin,
                                oop::kSciMsgMax, oop::kSciMsgMax + 1, 65535 };
    for (unsigned p : probes)
        CHECK(oop::SciOutKindOf(p) == LinearLookup(p));
    // 区间内的空洞（有形状但不是出参族/出入参族）也必须是 kNone
    const unsigned span = oop::kSciMsgMax - oop::kSciMsgMin + 1;
    int gaps = 0;
    for (unsigned m = oop::kSciMsgMin; m <= oop::kSciMsgMax; ++m)
        if (oop::SciOutKindOf(m) == oop::SciOutKind::kNone) ++gaps;
    CHECK(static_cast<unsigned>(gaps) ==
          span - oop::kSciOutCount - oop::kSciInOutCount);
    std::printf("  [3] 区间 %u..%u 共 %u 个编号，出参族 %u + 出入参族 %u，其余 %d 个不过桥\n",
                oop::kSciMsgMin, oop::kSciMsgMax, span, oop::kSciOutCount,
                oop::kSciInOutCount, gaps);
}

// [5] ★ SciOutBytesToCopy 全枚举 + 不变式
static void TestBytesToCopy() {
    const oop::SciOutKind kAll[] = {
        oop::SciOutKind::kNone, oop::SciOutKind::kNul, oop::SciOutKind::kNoNul,
        oop::SciOutKind::kClampWpNul, oop::SciOutKind::kRefuseRuntime,
    };
    const unsigned long kNeeds[] = { 0, 1, 2, 7, 8, 31, 32768 };
    const unsigned long kWps[] = { 0, 1, 3, 7, 8, 64, 32768, 0xFFFFFFFFul };
    for (oop::SciOutKind k : kAll) {
        for (unsigned long need : kNeeds) {
            for (unsigned long wp : kWps) {
                const unsigned long c = oop::SciOutBytesToCopy(k, need, wp);
                switch (k) {
                case oop::SciOutKind::kNul:
                    CHECK(c == need + 1); break;
                case oop::SciOutKind::kNoNul:
                    CHECK(c == need); break;
                case oop::SciOutKind::kClampWpNul:
                    CHECK(c == (need < wp ? need : wp) + 1); break;
                default:
                    CHECK(c == 0); break;      // kNone / kRefuseRuntime 一律 0
                }
            }
        }
    }
    // 不变式：可桥接桶永远至少 1 字节（NUL 桶）或恰为 need；拒答桶恒 0
    for (oop::SciOutKind k : { oop::SciOutKind::kNul, oop::SciOutKind::kClampWpNul })
        for (unsigned long need : kNeeds)
            CHECK(oop::SciOutBytesToCopy(k, need, 64) >= 1);
    CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kNul, 0, 0) == 1);
    CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kNoNul, 0, 0) == 0);
    // ★ need 越界（上游漏了钳制）必须变成 0（拒答），而不是整数回绕
    CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kNul, oop::kSciOutNeedMax + 1, 0) == 0);
    CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kNoNul, 0xFFFFFFFFul, 0) == 0);
    CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kClampWpNul, 0xFFFFFFFFul, 64) == 0);
    std::printf("  [5] SciOutBytesToCopy：5 桶 × %d need × %d wp 全枚举；越界 → 0\n",
                static_cast<int>(sizeof(kNeeds) / sizeof(kNeeds[0])),
                static_cast<int>(sizeof(kWps) / sizeof(kWps[0])));
}

// [6] ★ SciOutWroteOnlyExpected 正反例 —— 把两个检查各自单独钉住
static void TestWroteOnlyExpected() {
    const unsigned char kCanary = oop::kSciOutCanaryByte;

    // 正例：kNul，need=5 ⇒ copied=6，6 字节内容 + 第 6 格 NUL，其余是哨兵
    {
        std::vector<unsigned char> b(6 + 8, kCanary);
        b[0] = 'h'; b[1] = 'e'; b[2] = 'l'; b[3] = 'l'; b[4] = 'o'; b[5] = 0;
        CHECK(oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNul, 6, b.data(), b.size(), kCanary));
    }
    // 反例①（越界写）：kNul 但最后一格不是 NUL（= 实际少写了一个字节）
    {
        std::vector<unsigned char> b(6 + 8, kCanary);
        b[0] = 'h'; b[5] = 'o';          // 第 6 格留哨兵
        CHECK(!oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNul, 6, b.data(), b.size(), kCanary));
    }
    // 反例②（写多了，踩哨兵）：kNoNul，copied=5，但第 5 格被写成 NUL
    {
        std::vector<unsigned char> b(5 + 8, kCanary);
        b[0] = 'h'; b[1] = 'e'; b[2] = 'l'; b[3] = 'l'; b[4] = 'o';
        b[5] = 0;                        // ← Scintilla 多写了 1 字节
        CHECK(!oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNoNul, 5, b.data(), b.size(), kCanary));
    }
    // 正例：kNoNul 不含 NUL，哨兵区原封不动
    {
        std::vector<unsigned char> b(5 + 8, kCanary);
        b[0] = 'h'; b[1] = 'e'; b[2] = 'l'; b[3] = 'l'; b[4] = 'o';
        CHECK(oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNoNul, 5, b.data(), b.size(), kCanary));
    }
    // 正例：kClampWpNul，copied=4（= min(need,wp)+1）
    {
        std::vector<unsigned char> b(4 + 8, kCanary);
        b[0] = 'h'; b[1] = 'e'; b[2] = 'l'; b[3] = 0;
        CHECK(oop::SciOutWroteOnlyExpected(oop::SciOutKind::kClampWpNul, 4, b.data(), b.size(), kCanary));
    }
    // copied == 0：什么都没写，哨兵区完整 ⇒ 平凡成立
    {
        std::vector<unsigned char> b(0 + 8, kCanary);
        CHECK(oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNoNul, 0, b.data(), b.size(), kCanary));
    }
    // 拒答桶 / 表外：不该走到这里 ⇒ 一律 false（fail-closed）
    {
        std::vector<unsigned char> b(4 + 8, kCanary);
        CHECK(!oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNone, 4, b.data(), b.size(), kCanary));
        CHECK(!oop::SciOutWroteOnlyExpected(oop::SciOutKind::kRefuseRuntime, 4, b.data(), b.size(), kCanary));
    }
    // 参数自洽：copied > totalLen / 空指针 ⇒ false
    {
        std::vector<unsigned char> b(4 + 8, kCanary);
        CHECK(!oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNoNul, 99, b.data(), b.size(), kCanary));
        CHECK(!oop::SciOutWroteOnlyExpected(oop::SciOutKind::kNoNul, 4, nullptr, 12, kCanary));
    }
    std::printf("  [6] SciOutWroteOnlyExpected：越界写 / NUL 语义搞反 各有独立反例\n");
}

// [7] ★ 拒答桶的精确名单（两条，各有各的反例证据）
static void TestRefuseRoster() {
    struct Rep { unsigned msg; const char* name; const char* why; };
    static const Rep kRefuse[] = {
        { SCI_TARGETASUTF8, "TargetAsUTF8",
          "NUL 语义随运行期文档编码变化（Unicode 不写 / 非 Unicode 写）" },
        { SCI_GETTAG, "GetTag",
          "写入量随有没有配 regex 变化（有 ⇒ need+1；无 ⇒ 只写 1 字节），"
          "返回值恒是那个先写死的 length ⇒ probe 的返回值不是容量" },
    };
    int n = 0;
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        if (oop::kSciOutTable[i].kind == oop::SciOutKind::kRefuseRuntime) ++n;
    CHECK(n == static_cast<int>(sizeof(kRefuse) / sizeof(kRefuse[0])));
    for (const Rep& r : kRefuse) {
        if (oop::SciOutKindOf(r.msg) != oop::SciOutKind::kRefuseRuntime) {
            ++g_fail;
            std::printf("FAIL %s(%u) 必须在拒答桶里：%s\n", r.name, r.msg, r.why);
        }
        // 拒答桶不可桥接（代理侧据此记账）
        CHECK(!oop::SciOutRelayable(r.msg));
        // 拒答桶的 copied 必须是 0（不可能有人拿它去算容量）
        CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kRefuseRuntime, 7, 64) == 0);
    }
    // 三条可桥接桶至少各有一条（否则说明分类退化成了"全拒答"）
    CHECK(oop::SciOutRelayable(SCI_GETSELTEXT));
    CHECK(oop::SciOutRelayable(SCI_GETLINE));
    CHECK(oop::SciOutRelayable(SCI_GETTEXT));
    std::printf("  [7] 拒答桶 2 条：TargetAsUTF8（编码）/ GetTag（regex）\n");
}

// ============================================================================
// 判据 B：真 Scintilla
// ============================================================================
// Scintilla.dll 的 DllMain 在 DLL_PROCESS_ATTACH 里注册 "Scintilla" 窗口类
// （third_party/scintilla/win32/ScintillaDLL.cxx）⇒ LoadLibrary 之后就能直接
// CreateWindowEx。本 exe 与 Scintilla.dll 同在 build/bin/Release/，而 exe 所在
// 目录是 DLL 搜索顺序的第一位 ⇒ 不需要额外的拷贝步骤。
static HWND g_sci = nullptr;

static bool MakeScintilla() {
    HMODULE mod = ::LoadLibraryW(L"Scintilla.dll");
    if (!mod) {
        ++g_fail;
        std::printf("FAIL 加载 Scintilla.dll 失败（err=%lu）。\n"
                    "  ⚠ 这里**不能跳过**：判据 B 缺了，分类表就只剩「读源码」一路判据，\n"
                    "     而 GetTag 那个反例正是被这条路抓出来的。\n",
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

struct OutRun {
    bool ok = false;                       // 未被拒答
    unsigned long need = 0;
    unsigned long copied = 0;
    LRESULT result = 0;
    std::vector<unsigned char> bytes;
};

// ⚠ 比较/取字节前**必须先查长度**。负控（分类错 ⇒ 拒答）下 bytes 是空的，
//   直接 std::memcmp(b.data(), ...) 或 b[i] 会读空缓冲而**崩** —— 那样失败就
//   不可读了（本批实测踩过：一条负控把整个测试进程打成段错误，诊断全丢）。
//   纪律：负控的产物必须是**可读的失败**，不是"进程没了"。
static bool BytesAre(const std::vector<unsigned char>& b, const char* expect,
                     std::size_t n) {
    if (b.size() < n) return false;
    return std::memcmp(b.data(), expect, n) == 0;
}

static bool ByteAt(const std::vector<unsigned char>& b, std::size_t i,
                   unsigned char v) {
    return i < b.size() && b[i] == v;
}

// 走一次**产品代码**的出参过桥（不是夹具自己实现的编辑器侧）
static OutRun RunOut(unsigned sciMsg, UINT_PTR argWp, bool writeBack = true) {
    OutRun r;
    oop::SciOutCallWire cw{};
    cw.magic = oop::kMagic;
    cw.msg = oop::OOPM_SCIOUTCALL;
    cw.sciMsg = sciMsg;
    cw.argWp = argWp;
    cw.reqId = 1;
    cw.replyTo = 0;
    cw.writeBack = writeBack ? 1u : 0u;
    r.ok = oop::SciBridgeOutCall(g_sci, cw, sizeof(cw),
                                 r.bytes, r.need, r.copied, r.result);
    return r;
}

// 通用不变式：按桶验"回传字节数 + NUL 位置"
static void CheckInvariant(const char* name, unsigned msg, const OutRun& r,
                           UINT_PTR wp) {
    const oop::SciOutKind k = oop::SciOutKindOf(msg);
    const unsigned long want =
        oop::SciOutBytesToCopy(k, r.need, static_cast<unsigned long>(wp));
    if (r.copied != want || r.bytes.size() != want) {
        ++g_fail;
        std::printf("FAIL %s：need=%lu copied=%lu 期望 %lu（桶 %s）\n",
                    name, r.need, r.copied, want, oop::SciOutKindName(k));
        return;
    }
    if (want == 0) return;
    if (k == oop::SciOutKind::kNul || k == oop::SciOutKind::kClampWpNul) {
        if (!ByteAt(r.bytes, static_cast<std::size_t>(want) - 1, 0)) {
            ++g_fail;
            std::printf("FAIL %s：最后一格不是 NUL（桶 %s）\n", name, oop::SciOutKindName(k));
        }
    }
}

// [8] ★★ 30 条逐一真过桥 —— 分类错就必然拒答（见文件头）
static void TestRealScintillaAll() {
    if (!g_sci) return;

    // 造一份有内容的文档：两行（第一行 13 字节含 CRLF）
    static const char kText[] = "hello world\r\nsecond line here\r\n";
    ::SendMessageW(g_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(kText));
    // 先造一条撤销历史（让 GetUndoActionText 有东西可读），再设选区 ——
    // 顺序不能反：SCI_UNDO 会把选区重置掉，于是 GetSelText 会读到 0 字节。
    ::SendMessageW(g_sci, SCI_APPENDTEXT, 1, reinterpret_cast<LPARAM>("x"));
    ::SendMessageW(g_sci, SCI_UNDO, 0, 0);
    ::SendMessageW(g_sci, SCI_SETSEL, 0, 5);      // 选中 [0,5)，光标停在第 5 列

    struct Case {
        unsigned msg;
        const char* name;
        UINT_PTR wp;
    };
    // 覆盖全部 30 条；GetText / GetCurLine 各测两种 wp（wp ≥ need 与 wp < need）
    static const Case kCases[] = {
        { SCI_GETCURLINE,                 "GetCurLine",                 1024 },
        { SCI_GETCURLINE,                 "GetCurLine(wp<need)",        3    },
        { SCI_GETLINE,                    "GetLine",                    0    },
        { SCI_GETSELTEXT,                 "GetSelText",                 0    },
        { SCI_GETTEXT,                    "GetText",                    1024 },
        { SCI_GETTEXT,                    "GetText(wp<need)",           4    },
        { SCI_STYLEGETINVISIBLEREPRESENTATION, "StyleGetInvisibleRepresentation", 0 },
        { SCI_STYLEGETFONT,               "StyleGetFont",               0    },
        { SCI_MARGINGETTEXT,              "MarginGetText",              0    },
        { SCI_MARGINGETSTYLES,            "MarginGetStyles",            0    },
        { SCI_ANNOTATIONGETTEXT,          "AnnotationGetText",          0    },
        { SCI_ANNOTATIONGETSTYLES,        "AnnotationGetStyles",        0    },
        { SCI_AUTOCGETCURRENTTEXT,        "AutoCGetCurrentText",        0    },
        { SCI_GETWORDCHARS,               "GetWordChars",               0    },
        { SCI_GETWHITESPACECHARS,         "GetWhitespaceChars",         0    },
        { SCI_GETPUNCTUATIONCHARS,        "GetPunctuationChars",        0    },
        { SCI_GETTARGETTEXT,              "GetTargetText",              0    },
        { SCI_GETDEFAULTFOLDDISPLAYTEXT,  "GetDefaultFoldDisplayText",  0    },
        { SCI_EOLANNOTATIONGETTEXT,       "EOLAnnotationGetText",       0    },
        { SCI_GETFONTLOCALE,              "GetFontLocale",              0    },
        { SCI_GETSELECTIONSERIALIZED,     "GetSelectionSerialized",     0    },
        { SCI_GETUNDOACTIONTEXT,          "GetUndoActionText",          0    },
        { SCI_GETCOPYSEPARATOR,           "GetCopySeparator",           0    },
        { SCI_GETLEXERLANGUAGE,           "GetLexerLanguage",           0    },
        { SCI_PROPERTYNAMES,              "PropertyNames",              0    },
        { SCI_DESCRIBEKEYWORDSETS,        "DescribeKeyWordSets",        0    },
        { SCI_GETSUBSTYLEBASES,           "GetSubStyleBases",           0    },
        { SCI_NAMEOFSTYLE,                "NameOfStyle",                0    },
        { SCI_TAGSOFSTYLE,                "TagsOfStyle",                0    },
        { SCI_DESCRIPTIONOFSTYLE,         "DescriptionOfStyle",         0    },
    };
    int bridged = 0;
    for (const Case& c : kCases) {
        const oop::SciOutKind k = oop::SciOutKindOf(c.msg);
        if (!oop::SciOutRelayable(c.msg)) {
            ++g_fail;
            std::printf("FAIL %s(%u)：表说不可桥接（桶 %s），但它不在拒答名单里\n",
                        c.name, c.msg, oop::SciOutKindName(k));
            continue;
        }
        const OutRun r = RunOut(c.msg, c.wp);
        if (!r.ok) {
            ++g_fail;
            std::printf("FAIL %s(%u)：真过桥被**拒答**（桶 %s）—— "
                        "分类与真 Scintilla 的写入量不一致\n",
                        c.name, c.msg, oop::SciOutKindName(k));
            continue;
        }
        ++bridged;
        CheckInvariant(c.name, c.msg, r, c.wp);
    }
    // 覆盖面自检：列了 30 条用例（GetText/GetCurLine 各多一条 wp<need 的变体）
    CHECK(bridged == static_cast<int>(sizeof(kCases) / sizeof(kCases[0])));
    CHECK(static_cast<int>(sizeof(kCases) / sizeof(kCases[0])) == 30);

    // ---- 逐条定性：把"决策"钉住，而不是只验不变式 ----
    {
        // GetLine：线长 13（"hello world\r\n"），kNoNul ⇒ **不含** NUL，
        // 且最后一格是 '\n' 而不是 0（若错标成 kNul，这里会变成 NUL）
        const OutRun r = RunOut(SCI_GETLINE, 0);
        CHECK(r.ok && r.need == 13 && r.copied == 13);
        CHECK(r.bytes.size() == 13);
        CHECK(BytesAre(r.bytes, "hello world\r\n", 13));
        CHECK(ByteAt(r.bytes, 12, '\n'));
        // GetSelText：选中 [0,5) ⇒ need=5，kNul ⇒ 6 字节、末格 NUL
        const OutRun s = RunOut(SCI_GETSELTEXT, 0);
        CHECK(s.ok && s.need == 5 && s.copied == 6);
        CHECK(s.bytes.size() == 6);
        CHECK(BytesAre(s.bytes, "hello", 5));
        CHECK(ByteAt(s.bytes, 5, 0));
        // GetText（wp ≥ need）：kClampWpNul ⇒ need+1；wp < need ⇒ wp+1
        const OutRun t1 = RunOut(SCI_GETTEXT, 1024);
        const OutRun t2 = RunOut(SCI_GETTEXT, 4);
        CHECK(t1.ok && t2.ok);
        CHECK(t1.need == t2.need);                 // need 与 wp 无关（协议如此）
        CHECK(t1.copied == t1.need + 1);
        CHECK(t2.copied == 5);                     // min(need,4)+1
        CHECK(t2.bytes.size() == 5);
        CHECK(BytesAre(t2.bytes, "hell", 4));
        CHECK(ByteAt(t2.bytes, 4, 0));
        // GetWordChars：kNoNul ⇒ copied == need（无 NUL），need 是字符类计数
        const OutRun w = RunOut(SCI_GETWORDCHARS, 0);
        CHECK(w.ok && w.need > 0 && w.copied == w.need);
        CHECK(w.bytes.size() == w.copied);
    }
    std::printf("  [8] 真 Scintilla：%d 条全部真过桥成功（分类错 ⇒ 必然拒答）\n", bridged);

    // 拒答桶：**即使**代理把请求送过来，宿主侧也必须拒答（纵深防御）。
    // 代理侧现在就在中转窗把它们拒掉了（不产生跨进程往返），但宿主侧的同一道闸
    // 必须独立成立 —— 否则一旦代理漏判，写下去就是"运行时语义不对"的静默错。
    {
        const OutRun u = RunOut(SCI_TARGETASUTF8, 0);
        const OutRun g = RunOut(SCI_GETTAG, 1);
        CHECK(!u.ok && u.bytes.empty() && u.copied == 0);
        CHECK(!g.ok && g.bytes.empty() && g.copied == 0);
        // 出入参族（kInOutStr，批次 115 起**已可过桥**）在**不带入参串**时必须
        // 拒答：inBytes==0 而表说要带串 ⇒ 契约不成立，宁可拒答也不拿 0 当串指针。
        // ⚠ 这条**不能**再解释成"编号不在表里"（那是批次 114 的旧事实）。
        //   带上入参串的正向路径由 tests/test_sci_inout.cpp 覆盖。
        const OutRun p = RunOut(SCI_GETPROPERTY, 0);
        CHECK(!p.ok && p.bytes.empty() && p.copied == 0);
        CHECK(oop::SciOutNeedsInStr(SCI_GETPROPERTY));
    }
}

// [9] ★★ GetCurLine 的返回值是**光标列号**，不是长度（负控②的守门人）
// 若有人把 copied 按"宿主返回值"算，这里会得到 5 而不是 min(need,wp)+1。
static void TestGetCurLineReturnsCaretColumn() {
    if (!g_sci) return;
    ::SendMessageW(g_sci, SCI_SETSEL, 0, 5);       // 光标停在第 0 行的第 5 列
    const OutRun a = RunOut(SCI_GETCURLINE, 1024);
    const OutRun b = RunOut(SCI_GETCURLINE, 3);
    CHECK(a.ok && b.ok);
    CHECK(a.need == 13 && b.need == 13);           // need = 整行长度（含 CRLF）
    CHECK(a.result == 5 && b.result == 5);         // 返回值 = 光标列号
    CHECK(a.copied == 14 && b.copied == 4);        // = min(need,wp)+1
    // ★ 这两条断言就是负控②的守门人：返回值(5) 与 copied(14/4) **不相等**
    CHECK(a.result != static_cast<LRESULT>(a.copied));
    CHECK(b.result != static_cast<LRESULT>(b.copied));
    // 内容与"按 wp 截断的行首"一致
    CHECK(b.bytes.size() == 4);
    CHECK(BytesAre(b.bytes, "hel", 3));
    CHECK(ByteAt(b.bytes, 3, 0));
    std::printf("  [9] GetCurLine：返回值 %ld 是光标列号，copied=%lu/%lu —— "
                "两者必须不同（负控②）\n",
                static_cast<long>(a.result), a.copied, b.copied);
}

// [10] 查长度路径（writeBack=0）：只回 need，一个字节都不写
// 这是 Scintilla 出参协议自带的那次调用 —— 插件自己探容量时走的就是它。
// 断言"它不写缓冲"很重要：若这里真去写了，插件给 lp=0 的探长度调用就会崩。
static void TestProbeOnly() {
    if (!g_sci) return;
    static const unsigned kProbe[] = {
        SCI_GETTEXT, SCI_GETSELTEXT, SCI_GETLINE, SCI_GETCURLINE,
        SCI_GETWORDCHARS, SCI_GETFONTLOCALE, SCI_GETCOPYSEPARATOR,
        SCI_GETLEXERLANGUAGE, SCI_MARGINGETTEXT, SCI_GETTARGETTEXT,
    };
    int n = 0;
    for (unsigned msg : kProbe) {
        if (!oop::SciOutRelayable(msg)) { ++g_fail;
            std::printf("FAIL 探长度用例 %u 不在可桥接桶里\n", msg); continue; }
        const OutRun r = RunOut(msg, 0, /*writeBack=*/false);
        if (!r.ok) { ++g_fail;
            std::printf("FAIL %u 的查长度调用被拒答\n", msg); continue; }
        if (!r.bytes.empty() || r.copied != 0) { ++g_fail;
            std::printf("FAIL %u 的查长度调用写了 %zu 字节（copied=%lu）—— 必须为 0\n",
                        msg, r.bytes.size(), r.copied); continue; }
        ++n;
        // 与写模式对照：写模式的 need 必须与探长度一致（同一份协议）。
        // ⚠ 两次必须用**同一个 wp**：出参族的 wp 不都是容量 —— GetLine 的 wp 是
        //   **行号**，GetCurLine/GetText 的 wp 才是容量。拿 wp=1024 去调 GetLine
        //   会问"第 1024 行"，need 自然是 0（本批实测踩过，正是这一条抓出来的）。
        const OutRun w = RunOut(msg, 0);
        if (!w.ok || w.need != r.need) {
            ++g_fail;
            std::printf("FAIL %u：探长度 need=%lu（ok=%d），写模式 need=%lu（ok=%d）"
                        "—— 应当一致且都成功\n",
                        msg, r.need, r.ok ? 1 : 0, w.need, w.ok ? 1 : 0);
        }
    }
    CHECK(n == static_cast<int>(sizeof(kProbe) / sizeof(kProbe[0])));
    std::printf("  [10] 查长度路径 %d 条：只回 need、不写任何字节\n", n);
}

// [11] 边界：表外与区间内空洞一律不过桥
static void TestBoundaries() {
    const unsigned outside[] = { 0, 1, WM_USER, 1999, 2000,
                                 oop::kSciMsgMax + 1, 5000, 65535 };
    for (unsigned m : outside) {
        CHECK(oop::SciOutKindOf(m) == oop::SciOutKind::kNone);
        CHECK(!oop::SciOutRelayable(m));
    }
    CHECK(oop::SciOutKindOf(3001) == oop::SciOutKind::kNone);   // 区间内空洞
    // 表里第一条/最后一条必须可桥接（否则首尾探测没意义）
    CHECK(oop::SciOutRelayable(oop::kSciOutTable[0].msg));
    CHECK(oop::SciOutRelayable(oop::kSciOutTable[oop::kSciOutCount - 1].msg));
    // 出入参族（kInOutStr，批次 115）：**已分类** —— 3 条 kNul、2 条拒答桶。
    // ⚠ 这里只钉"分到哪个桶"；"要不要带入参串"是 SciOutNeedsInStr 的事。
    CHECK(oop::SciOutKindOf(SCI_GETPROPERTY) == oop::SciOutKind::kNul);
    CHECK(oop::SciOutKindOf(SCI_GETPROPERTYEXPANDED) == oop::SciOutKind::kNul);
    CHECK(oop::SciOutKindOf(SCI_DESCRIBEPROPERTY) == oop::SciOutKind::kNul);
    CHECK(oop::SciOutKindOf(SCI_GETREPRESENTATION) == oop::SciOutKind::kRefuseRuntime);
    CHECK(oop::SciOutKindOf(SCI_ENCODEDFROMUTF8) == oop::SciOutKind::kRefuseRuntime);
    CHECK(!oop::SciOutRelayable(SCI_GETREPRESENTATION));
    CHECK(!oop::SciOutRelayable(SCI_ENCODEDFROMUTF8));
    // 不在**出参族**桶空间里的（形状表明确分出去的四类）：kStruct / kRawPtr /
    // kPtrRet / 表外。★ 它们不属于本表**不等于**不过桥：kStruct 自批次 116 起有
    // 自己的 SciStructTable（见 test_sci_struct）；kRawPtr/kPtrRet 才是永久不桥接。
    CHECK(oop::SciOutKindOf(SCI_GETSTYLEDTEXT) == oop::SciOutKind::kNone);   // kStruct（另表）
    CHECK(oop::SciOutKindOf(SCI_SETDOCPOINTER) == oop::SciOutKind::kNone);   // kRawPtr
    CHECK(oop::SciOutKindOf(SCI_GETDOCPOINTER) == oop::SciOutKind::kNone);   // kPtrRet
    CHECK(!oop::SciOutNeedsInStr(SCI_GETSTYLEDTEXT));
    std::printf("  [11] 表外 %d 个 + 区间内空洞 + kStruct/kRawPtr/kPtrRet：都不在**出参表**里\n",
                static_cast<int>(sizeof(outside) / sizeof(outside[0])));
}

int main() {
    std::printf("== test_sci_out (batch 114 + 115 分类面) ==\n");
    TestScale();
    TestOrderAndShapeAgreement();
    TestTablesDisjoint();
    TestLookupAgreement();
    TestBytesToCopy();
    TestWroteOnlyExpected();
    TestRefuseRoster();
    if (MakeScintilla()) {
        TestRealScintillaAll();
        TestGetCurLineReturnsCaretColumn();
        TestProbeOnly();
    }
    TestBoundaries();
    std::printf("== %s (%d failure%s) ==\n", g_fail ? "FAILED" : "ALL PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    std::printf("NOTE: 本守卫证明表内部自洽 + 与形状表一致 + 对**真 Scintilla** 成立；\n"
                "      表 ↔ Scintilla.iface 的名字映射由 scripts/gen-sci-marshal.py "
                "--check 负责。两条都必须跑。\n");
    if (g_sci) ::DestroyWindow(g_sci);
    return g_fail ? 1 : 0;
}
