// test_sci_struct.cpp — SCI_* **结构体指针**族（kStruct，8 条）守卫 + ABI 镜像
// 对撞 + 真 Scintilla 实证（批次 116）。
//
// 为什么单开一个测试目标：本族是**第一个真正的 in-out 族**（同一条消息里既有
// 入参侧 chrg/needle，又有出参侧 出缓冲/chrgText），判据与前三族都不同 ——
// 折进 sci_bridge / sci_out / sci_inout 任何一处，一旦"忘了注册进 CMakeLists"，
// 缺测会伪装成 100% 通过。
//
// 覆盖：
//   [0] ★ ABI 镜像对撞：SciBridge.h 的 abi::* ↔ 真 Scintilla.h（sizeof + offsetof
//       + 字段类型），编译期 static_assert
//   [1] 规模：8 = kRangeOut 2 + kStyledOut 2 + kFindInOut 2 + kRefuseHandle 2
//   [2] 有序性：msg 严格升序（FindSciStructEntry 二分查找的前提）
//   [3] 二分 ↔ 线性对撞（全表 + 表外边界）
//   [4] ★ 派生判据：NeedsInStr ⟺ kFindInOut；IsFull ⟺ 表里的 full 位
//   [5] 与形状表一致：kStruct 8 条 ⟺ 本表 8 条；且**不**出现在入参/出参表里
//   [6] ★ Relayable 恰好是"表里且非拒答桶"（6 条），拒答桶只有 FormatRange 两支
//   [7] ★ SciStructBytesToWrite 纯函数：len+1 / 2*len+2 / 越界收窄 / 上限钳制
//   [8] ★ wire 契约：inBytes 五道校验 + 非 Full 的值域 + 表外/拒答桶/无目标窗
//   [9] ★★ 真 Scintilla：6 条逐条过桥 + 与**进程内直调**逐字节/逐字段对撞
//   [10] ★★ 拒答桶：桥接必拒答 + **当场演示**它为什么不可桥接（GDI 句柄）
//
// ★ 本守卫的边界（写进输出，避免下一个读它的人高估它）：
//   它证明「表内部自洽 + 与形状表一致 + abi 镜像与厂商头一致 + 对真 Scintilla
//   成立」，**不**证明「表 ↔ Scintilla.iface 的名字映射」。后者是
//   scripts/gen-sci-marshal.py --check 的职责。两条守卫都必须跑，缺一不可。
//
// ★ 为什么 [0] 是编译期的：代理侧**不 include Scintilla 头**（见
//   src/CMakeLists.txt 的「自持契约镜像」），所以 abi::* 是手写的镜像。只在
//   SciBridge.h 里写 static_assert 只是在核对我自己写的常量；必须与厂商头
//   逐条比。本文件是**唯一**同时看得见两者的地方。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

#include <Scintilla.h>   // 真 Scintilla 的结构体与 SCI_* 编号（**不手抄**）

#include "../src/plugin/oop/SciBridge.h"
#include "../src/plugin/oop/SciMarshal.h"

namespace oop = xfs::oop;

// ============================================================================
// [0] ABI 镜像对撞（编译期）
// ============================================================================
// 尺寸相等 ≠ 字段偏移相等；字段偏移相等 ≠ 字段类型相等。三条都查。
#define ABI_SAME_SIZE(a, b) \
    static_assert(sizeof(a) == sizeof(b), "abi 镜像的 sizeof 与厂商头不符：" #a)

ABI_SAME_SIZE(oop::abi::CharRange,     Sci_CharacterRange);
ABI_SAME_SIZE(oop::abi::CharRangeFull, Sci_CharacterRangeFull);
ABI_SAME_SIZE(oop::abi::TextRange,     Sci_TextRange);
ABI_SAME_SIZE(oop::abi::TextRangeFull, Sci_TextRangeFull);
ABI_SAME_SIZE(oop::abi::TextToFind,    Sci_TextToFind);
ABI_SAME_SIZE(oop::abi::TextToFindFull, Sci_TextToFindFull);

#define ABI_SAME_OFF(a, b, field) \
    static_assert(offsetof(a, field) == offsetof(b, field), \
                  "abi 镜像的字段偏移与厂商头不符：" #a "." #field)

ABI_SAME_OFF(oop::abi::TextRange,        Sci_TextRange,        chrg);
ABI_SAME_OFF(oop::abi::TextRange,        Sci_TextRange,        lpstrText);
ABI_SAME_OFF(oop::abi::TextRangeFull,    Sci_TextRangeFull,    chrg);
ABI_SAME_OFF(oop::abi::TextRangeFull,    Sci_TextRangeFull,    lpstrText);
ABI_SAME_OFF(oop::abi::TextToFind,       Sci_TextToFind,       chrg);
ABI_SAME_OFF(oop::abi::TextToFind,       Sci_TextToFind,       lpstrText);
ABI_SAME_OFF(oop::abi::TextToFind,       Sci_TextToFind,       chrgText);
ABI_SAME_OFF(oop::abi::TextToFindFull,   Sci_TextToFindFull,   chrg);
ABI_SAME_OFF(oop::abi::TextToFindFull,   Sci_TextToFindFull,   lpstrText);
ABI_SAME_OFF(oop::abi::TextToFindFull,   Sci_TextToFindFull,   chrgText);

// ★ 字段类型：非 Full 变体是 Sci_PositionCR（= long，4 字节），Full 变体是
//   Sci_Position（= intptr_t，8 字节）。**这是本族最容易错的地方** ——
//   把两者弄反会让偏移整体错位，而 sizeof 在某些组合下仍可能"看起来合理"。
static_assert(std::is_same<decltype(oop::abi::CharRange::cpMin),
                           Sci_PositionCR>::value,
              "非 Full 变体的位置字段必须是 Sci_PositionCR（long）");
static_assert(std::is_same<decltype(oop::abi::CharRangeFull::cpMin),
                           Sci_Position>::value,
              "Full 变体的位置字段必须是 Sci_Position（intptr_t）");
static_assert(sizeof(Sci_PositionCR) == 4, "Sci_PositionCR 是 long（Windows 上 4 字节）");
static_assert(sizeof(Sci_Position) == 8, "Sci_Position 是 intptr_t（x64 上 8 字节）");
// lpstrText 的类型（char* / const char*）也必须一致：WriteToFind 的入串是
// **const char\***（Scintilla 只读它），TextRange 的出缓冲是 **char\***。
static_assert(std::is_same<decltype(oop::abi::TextRange::lpstrText), char*>::value, "");
static_assert(std::is_same<decltype(oop::abi::TextToFind::lpstrText), const char*>::value, "");

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- 表查询助手（与表里的二分实现**独立**）---------------------------------
static int CountStructKind(oop::SciStructKind k) {
    int n = 0;
    for (unsigned i = 0; i < oop::kSciStructCount; ++i)
        if (oop::kSciStructTable[i].kind == k) ++n;
    return n;
}

static oop::SciStructKind LinearLookup(unsigned msg, bool* full) {
    for (unsigned i = 0; i < oop::kSciStructCount; ++i) {
        if (oop::kSciStructTable[i].msg == msg) {
            if (full) *full = oop::kSciStructTable[i].full;
            return oop::kSciStructTable[i].kind;
        }
    }
    if (full) *full = false;
    return oop::SciStructKind::kNone;
}

// [1] 规模 + 自报计数
static void TestScale() {
    CHECK(oop::kSciStructCount == 8);
    CHECK(oop::kSciStructCount == oop::kSciCountStruct);   // 与形状表自洽
    struct Row { oop::SciStructKind k; unsigned declared; const char* name; };
    static const Row kRows[] = {
        { oop::SciStructKind::kRangeOut,     oop::kSciStructCountRangeOut,     "kRangeOut"     },
        { oop::SciStructKind::kStyledOut,    oop::kSciStructCountStyledOut,    "kStyledOut"    },
        { oop::SciStructKind::kFindInOut,    oop::kSciStructCountFindInOut,    "kFindInOut"    },
        { oop::SciStructKind::kRefuseHandle, oop::kSciStructCountRefuseHandle, "kRefuseHandle" },
    };
    unsigned sum = 0;
    for (const Row& r : kRows) {
        const int actual = CountStructKind(r.k);
        if (static_cast<unsigned>(actual) != r.declared) {
            ++g_fail;
            std::printf("FAIL 桶 %s：实际 %d 条，.inc 自报 %u 条\n",
                        r.name, actual, r.declared);
        }
        sum += static_cast<unsigned>(actual);
    }
    // 桶和 == 表长：多一个漏归类的条目就会在这里露出来
    CHECK(sum == oop::kSciStructCount);
    // 本族只有这四个桶：别的桶出现在这里说明分类器变了，必须显式复核
    CHECK(CountStructKind(oop::SciStructKind::kNone) == 0);
    std::printf("  [1] 8 条：kRangeOut 2 / kStyledOut 2 / kFindInOut 2 / kRefuseHandle 2\n");
}

// [2] 有序性
static void TestOrder() {
    for (unsigned i = 0; i < oop::kSciStructCount; ++i) {
        CHECK(oop::kSciStructTable[i].kind != oop::SciStructKind::kNone);
        if (i) CHECK(oop::kSciStructTable[i - 1].msg < oop::kSciStructTable[i].msg);
        CHECK(oop::kSciStructTable[i].msg >= oop::kSciMsgMin);
        CHECK(oop::kSciStructTable[i].msg <= oop::kSciMsgMax);
        CHECK(oop::kSciStructTable[i].msg > WM_USER);
    }
}

// [3] 二分 ↔ 线性对撞
static void TestLookupAgreement() {
    for (unsigned i = 0; i < oop::kSciStructCount; ++i) {
        bool fullA = false, fullB = false;
        const oop::SciStructKind a =
            oop::SciStructKindOf(oop::kSciStructTable[i].msg);
        const oop::SciStructKind b = LinearLookup(oop::kSciStructTable[i].msg, &fullB);
        CHECK(a == b);
        CHECK(oop::SciStructIsFull(oop::kSciStructTable[i].msg) == fullB);
        (void)fullA;
    }
    const unsigned probes[] = { 0, 1, WM_USER, 1999, 2000, 2015, 2016,
                                oop::kSciMsgMin - 1, oop::kSciMsgMin,
                                oop::kSciMsgMax, oop::kSciMsgMax + 1, 65535 };
    for (unsigned p : probes) {
        bool fullA = false, fullB = false;
        CHECK(oop::SciStructKindOf(p) == LinearLookup(p, &fullB));
        CHECK(oop::SciStructIsFull(p) == fullB);
        (void)fullA;
    }
}

// [4] ★ 派生判据必须**同一个来源**（不另立判据）
static void TestDerivedPredicates() {
    // NeedsInStr ⟺ kFindInOut（两个方向）
    for (unsigned i = 0; i < oop::kSciStructCount; ++i) {
        const unsigned m = oop::kSciStructTable[i].msg;
        const bool isFind = oop::kSciStructTable[i].kind == oop::SciStructKind::kFindInOut;
        CHECK(oop::SciStructNeedsInStr(m) == isFind);
    }
    // 表外一律不带串
    for (unsigned m : { 0u, 1u, (unsigned)WM_USER, 2000u, 2014u, 3000u, 65535u })
        CHECK(!oop::SciStructNeedsInStr(m));
    // IsFull 只在表里有意义
    for (unsigned m : { 0u, 1u, (unsigned)WM_USER, 2000u, 3000u, 65535u })
        CHECK(!oop::SciStructIsFull(m));
    // 每族都有 full / 非 full 各一条（结构族的对称性）
    CHECK(oop::SciStructIsFull(2015) == false && oop::SciStructIsFull(2778) == true);   // styled
    CHECK(oop::SciStructIsFull(2162) == false && oop::SciStructIsFull(2039) == true);   // range
    CHECK(oop::SciStructIsFull(2150) == false && oop::SciStructIsFull(2196) == true);   // find
    CHECK(oop::SciStructIsFull(2151) == false && oop::SciStructIsFull(2777) == true);   // refuse
    // 名字表不能有 "?"
    for (unsigned i = 0; i < oop::kSciStructCount; ++i)
        CHECK(std::strcmp(oop::SciStructKindName(oop::kSciStructTable[i].kind), "?") != 0);
    std::printf("  [4] NeedsInStr ⟺ kFindInOut；IsFull ⟺ 表里的 full 位（三方向都查）\n");
}

// [5] 与形状表一致（两个方向）+ 与前三族互斥
static void TestShapeAgreement() {
    for (unsigned i = 0; i < oop::kSciStructCount; ++i)
        CHECK(oop::SciShapeOf(oop::kSciStructTable[i].msg) == oop::SciShape::kStruct);
    int shaped = 0, inTable = 0;
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        if (oop::kSciTable[i].shape != oop::SciShape::kStruct) continue;
        ++shaped;
        if (oop::SciStructKindOf(oop::kSciTable[i].msg) != oop::SciStructKind::kNone)
            ++inTable;
    }
    CHECK(shaped == 8);
    CHECK(inTable == shaped);
    // ★ 互斥：结构族的 8 条一条都不许出现在入参表 / 出参表里。
    //   （"归到哪一族"必须唯一 —— 否则中转窗会按两张表各判一次，行为取决于顺序。）
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        if (oop::kSciTable[i].shape != oop::SciShape::kStruct) continue;
        const unsigned m = oop::kSciTable[i].msg;
        CHECK(oop::SciInLayoutOf(m) == oop::SciInLayout::kNone);
        CHECK(oop::SciOutKindOf(m) == oop::SciOutKind::kNone);
    }
    std::printf("  [5] 形状表 kStruct 8 条 ⟺ 本表 8 条；且与入参/出参两表互斥\n");
}

// [6] ★ Relayable 恰好是"表里且非拒答桶"
static void TestRelayable() {
    static const unsigned kBridgeable[] = { 2015, 2039, 2150, 2162, 2196, 2778 };
    static const unsigned kRefused[] = { 2151, 2777 };
    for (unsigned m : kBridgeable) {
        CHECK(oop::SciStructKindOf(m) != oop::SciStructKind::kNone);
        CHECK(oop::SciStructRelayable(m));
    }
    for (unsigned m : kRefused) {
        CHECK(oop::SciStructKindOf(m) == oop::SciStructKind::kRefuseHandle);
        CHECK(!oop::SciStructRelayable(m));
    }
    // 表外一律不 Relayable（安全方向）
    for (unsigned m : { 0u, 1u, (unsigned)WM_USER, 2000u, 3000u, 65535u })
        CHECK(!oop::SciStructRelayable(m));
    CHECK(static_cast<unsigned>(sizeof(kBridgeable) / sizeof(kBridgeable[0])) +
          static_cast<unsigned>(sizeof(kRefused) / sizeof(kRefused[0])) ==
          oop::kSciStructCount);
    std::printf("  [6] Relayable 6 条（另 2 条是 FormatRange/Full：进程私有 HDC）\n");
}

// [7] ★ SciStructBytesToWrite（纯函数）
static void TestBytesToWrite() {
    using oop::SciStructKind;
    const INT_PTR maxLen = static_cast<INT_PTR>(oop::kSciPayloadMax);
    // kRangeOut：len + 1，其中 len = (cpMax == -1 ? docLen : cpMax) - cpMin
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 0, 5, 11) == 6);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 0, -1, 11) == 12);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 3, 11, 11) == 9);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 5, 5, 11) == 1);   // len==0 ⇒ 仍 1
    // ★ 故意收窄：cpEnd > docLen 一律拒答（原生会留下"前面全是垃圾 + 末尾 NUL"）
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 0, 12, 11) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 0, -1, -1) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, -1, 5, 11) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 5, 3, 11) == 0);
    // kStyledOut：2*len + 2；**不支持** cpMax == -1（原生只写两个 NUL 就返回 0）
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kStyledOut, 0, 3, 0) == 8);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kStyledOut, 0, 0, 0) == 2);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kStyledOut, 2, 1, 0) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kStyledOut, 0, -1, 0) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kStyledOut, -1, 2, 0) == 0);
    // 不在本函数职责里的桶一律 0
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kFindInOut, 0, 5, 11) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRefuseHandle, 0, 5, 11) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kNone, 0, 5, 11) == 0);
    // ★ 上限：kSciPayloadMax 是**回带载荷**上限 ⇒ len 一顶到它就装不下（+1 / +2）
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 0, maxLen, maxLen) == 0);
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kStyledOut, 0, maxLen, 0) == 0);
    // 刚好装得下：len = maxLen-1 ⇒ cap = maxLen（kRangeOut）
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kRangeOut, 0, maxLen - 1,
                                     maxLen - 1) == oop::kSciPayloadMax);
    // 整数回绕的守门：len 大到能撑爆 unsigned long 也必须是 0，不能回绕成小值
    CHECK(oop::SciStructBytesToWrite(SciStructKind::kStyledOut, 0,
                                     static_cast<INT_PTR>(maxLen) * 4, 0) == 0);
    // chrgText 的 canary 必须是一个**真实文档位置不可能取到**的值
    CHECK(oop::kSciStructChrgCanary == std::numeric_limits<INT_PTR>::min());
    CHECK(oop::kSciStructChrgCanaryCR == std::numeric_limits<long>::min());
    // ★★ 记录批次 116 负控③ 抓到的**真缺陷**（两个 canary 必须分开给）：
    //   非 Full 变体的 chrgText 是 long（4 字节），把它从 INT_PTR 的最小值**截断**
    //   过来会得到 **0** —— 而 0 是一个**完全合法的命中位置**。于是"命中在位置 0"
    //   会被读成"没写" ⇒ 非 Full 的 FindText 在 0 处命中时静默回一个"没找到"。
    //   下面两条断言把"陷阱长什么样"和"修完长什么样"一起钉住。
    {
        // 用变量而不是字面量：MSVC 对字面量截断会发 C4309/C4310 告警。
        INT_PTR naive = oop::kSciStructChrgCanary;
        CHECK(static_cast<long>(naive) == 0);        // 陷阱：截断成 0（合法位置！）
        CHECK(oop::kSciStructChrgCanaryCR < 0);      // 修法：long 自己的最小值
        CHECK(oop::kSciStructChrgCanaryCR != 0);
    }
    std::printf("  [7] BytesToWrite：len+1 / 2*len+2 / 越界收窄 / 上限钳制 / 不回绕\n");
}

// [8] SciStructSize（代理侧给 LocalReadable/LocalWritable 定界用）
static void TestSizeFn() {
    using oop::SciStructKind;
    CHECK(oop::SciStructSize(SciStructKind::kRangeOut,  false) == 16);
    CHECK(oop::SciStructSize(SciStructKind::kRangeOut,  true)  == 24);
    CHECK(oop::SciStructSize(SciStructKind::kStyledOut, false) == 16);
    CHECK(oop::SciStructSize(SciStructKind::kStyledOut, true)  == 24);
    CHECK(oop::SciStructSize(SciStructKind::kFindInOut, false) == 24);
    CHECK(oop::SciStructSize(SciStructKind::kFindInOut, true)  == 40);
    // 拒答桶与表外没有布局 ⇒ 0（调用方据此拒答）
    CHECK(oop::SciStructSize(SciStructKind::kRefuseHandle, false) == 0);
    CHECK(oop::SciStructSize(SciStructKind::kRefuseHandle, true)  == 0);
    CHECK(oop::SciStructSize(SciStructKind::kNone, false) == 0);
    // 与厂商头对撞（[0] 的 static_assert 已经查过，这里再钉一次"函数返回的就是它"）
    CHECK(oop::SciStructSize(SciStructKind::kRangeOut, false) == sizeof(Sci_TextRange));
    CHECK(oop::SciStructSize(SciStructKind::kRangeOut, true) == sizeof(Sci_TextRangeFull));
    CHECK(oop::SciStructSize(SciStructKind::kFindInOut, false) == sizeof(Sci_TextToFind));
    CHECK(oop::SciStructSize(SciStructKind::kFindInOut, true) == sizeof(Sci_TextToFindFull));
    std::printf("  [8] SciStructSize：16/24/24/40，拒答桶与表外为 0\n");
}

// ============================================================================
// 真 Scintilla（判据 B）
// ============================================================================
// Scintilla.dll 的 DllMain 在 DLL_PROCESS_ATTACH 里注册 "Scintilla" 窗口类 ⇒
// LoadLibrary 之后就能 CreateWindowEx。exe 与 Scintilla.dll 同在
// build/bin/Release/，而 exe 所在目录是 DLL 搜索顺序第一位 ⇒ 无需额外拷贝。
//
// ⚠ 本族**不需要**词法器：GetTextRange / GetStyledText / FindText 都只依赖文档
//   内容与样式字节。默认样式是 0 ⇒ styled 族的字节流是 (char, 0) 对。
//   ⚠ 找不到 DLL 时**报失败而不是跳过** —— 跳过等于判据 B 静默消失。
static HWND g_sci = nullptr;
static const char kDoc[] = "hello world";
static constexpr std::size_t kDocLen = sizeof(kDoc) - 1;

static bool MakeScintilla() {
    if (!::LoadLibraryW(L"Scintilla.dll")) {
        ++g_fail;
        std::printf("FAIL 加载 Scintilla.dll 失败（err=%lu）。\n"
                    "  ⚠ 这里**不能跳过**：判据 B 缺了，本文件就只剩「读表」一路判据。\n",
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
    // 自证：文档真的装进去了（否则后面每条断言都会在"空文档"上退化成平凡成立）
    ::SendMessageW(g_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(kDoc));
    char b[64] = { 0 };
    ::SendMessageW(g_sci, SCI_GETTEXT, sizeof(b), reinterpret_cast<LPARAM>(b));
    if (std::strcmp(b, kDoc) != 0) {
        ++g_fail;
        std::printf("FAIL 文档没装上：GETTEXT 得到 \"%s\"（应为 \"%s\"）\n", b, kDoc);
        return false;
    }
    // ★ 再自证一次：本测试**不能**装词法器，否则 styled 族的样式字节就不是 0，
    //   下面按 (char, 0) 算的期望值会全错（而那种错看起来像"桥接坏了"）。
    ::SendMessageW(g_sci, SCI_SETILEXER, 0, 0);      // 确保没有词法器
    return true;
}

// wire 组装：直接喂给**产品代码**（不是夹具自己重写一遍宿主侧）
// needle == nullptr ⇒ 不带载荷（inBytes = 0）；needle == "" ⇒ 载荷是 1 个 NUL。
static std::vector<unsigned char> MakeStructWire(unsigned sciMsg, INT_PTR cpMin,
                                                INT_PTR cpMax, const char* needle,
                                                UINT_PTR argWp) {
    const std::size_t nb = needle ? std::strlen(needle) + 1 : 0;
    std::vector<unsigned char> w(sizeof(oop::SciStructCallWire) + nb, 0);
    auto* cw = reinterpret_cast<oop::SciStructCallWire*>(w.data());
    cw->magic = oop::kMagic;
    cw->msg = oop::OOPM_SCISTRUCTCALL;
    cw->sciMsg = sciMsg;
    cw->argWp = argWp;
    cw->reqId = 1;
    cw->replyTo = 0;
    cw->cpMin = cpMin;
    cw->cpMax = cpMax;
    cw->inBytes = static_cast<unsigned>(nb);
    if (nb) std::memcpy(w.data() + sizeof(oop::SciStructCallWire), needle, nb);
    return w;
}

struct WireRun {
    bool ok = false;
    unsigned long copied = 0;
    unsigned hasChrg = 0;
    INT_PTR chrgMin = 0;
    INT_PTR chrgMax = 0;
    LRESULT result = 0;
    std::vector<unsigned char> bytes;
};

static WireRun CallWire(const std::vector<unsigned char>& w, DWORD cbData) {
    WireRun r;
    const auto* cw = reinterpret_cast<const oop::SciStructCallWire*>(w.data());
    r.ok = oop::SciBridgeStructCall(g_sci, *cw, cbData, r.bytes, r.copied,
                                    r.hasChrg, r.chrgMin, r.chrgMax, r.result);
    return r;
}

// ---- 进程内直调（**同一个**真 Scintilla）—— 对撞基准 ------------------------
static void DirectRange(unsigned msg, bool full, INT_PTR cpMin, INT_PTR cpMax,
                        std::size_t cap, std::vector<unsigned char>* out,
                        LRESULT* res) {
    out->assign(cap + 16, 0xA5);
    if (full) {
        Sci_TextRangeFull tr{ { cpMin, cpMax }, reinterpret_cast<char*>(out->data()) };
        *res = ::SendMessageW(g_sci, msg, 0, reinterpret_cast<LPARAM>(&tr));
    } else {
        Sci_TextRange tr{ { static_cast<Sci_PositionCR>(cpMin),
                            static_cast<Sci_PositionCR>(cpMax) },
                          reinterpret_cast<char*>(out->data()) };
        *res = ::SendMessageW(g_sci, msg, 0, reinterpret_cast<LPARAM>(&tr));
    }
}

static void DirectFind(unsigned msg, bool full, INT_PTR cpMin, INT_PTR cpMax,
                       const char* needle, LRESULT* res, bool* wroteChrg,
                       INT_PTR* cmin, INT_PTR* cmax) {
    if (full) {
        Sci_TextToFindFull ft{ { cpMin, cpMax }, needle,
                               { oop::kSciStructChrgCanary, oop::kSciStructChrgCanary } };
        *res = ::SendMessageW(g_sci, msg, 0, reinterpret_cast<LPARAM>(&ft));
        *wroteChrg = (ft.chrgText.cpMin != oop::kSciStructChrgCanary);
        *cmin = ft.chrgText.cpMin;
        *cmax = ft.chrgText.cpMax;
    } else {
        const Sci_PositionCR canary = oop::kSciStructChrgCanaryCR;
        Sci_TextToFind ft{ { static_cast<Sci_PositionCR>(cpMin),
                             static_cast<Sci_PositionCR>(cpMax) },
                           needle, { canary, canary } };
        *res = ::SendMessageW(g_sci, msg, 0, reinterpret_cast<LPARAM>(&ft));
        *wroteChrg = (ft.chrgText.cpMin != canary);
        *cmin = ft.chrgText.cpMin;
        *cmax = ft.chrgText.cpMax;
    }
}

// [8] ★ wire 契约：每一条都要能**单独**把请求挡下来
static void TestWireContract() {
    if (!g_sci) return;
    const std::size_t kHdr = sizeof(oop::SciStructCallWire);

    // 正例（后面所有断言的前提）
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_GETTEXTRANGE, 0, 5, nullptr, 0);
        const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
        CHECK(r.ok && r.copied == 6);
    }
    // ① kRangeOut 带了串 ⇒ 拒答（不该带串的族带了串 = 绕过 argWp 的旁路）
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_GETTEXTRANGE, 0, 5, "x", 0);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ② kFindInOut 缺串 ⇒ 拒答（绝不拿 argWp 的 0 当串指针）
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_FINDTEXT, 0, 11, nullptr, 0);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ③ needle 末字节不是 NUL ⇒ 拒答（宿主会按 C 串读它）
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_FINDTEXT, 0, 11, "ab", 0);
        CHECK(w.size() == kHdr + 3);
        w[kHdr + 2] = 'Z';
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ④ cbData 比 sizeof + inBytes 少 1 ⇒ 载荷没到齐 ⇒ 拒答
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_FINDTEXT, 0, 11, "ab", 0);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size() - 1)).ok);
    }
    // ⑤ cbData 比结构体还小 ⇒ 连 inBytes 都不该读
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_FINDTEXT, 0, 11, "ab", 0);
        CHECK(!CallWire(w, static_cast<DWORD>(kHdr - 1)).ok);
    }
    // ⑥ 空串 != 不带串：inBytes == 1（只有 NUL）必须是**合法**请求
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_FINDTEXT, 0, 11, "", 0);
        CHECK(w.size() == kHdr + 1);
        CHECK(reinterpret_cast<const oop::SciStructCallWire*>(w.data())->inBytes == 1);
        CHECK(CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ⑦ 非 Full 变体的位置字段是 long：超值域必须先拒答（截断会**静默换一个范围**）
    {
        const INT_PTR tooBig = static_cast<INT_PTR>(std::numeric_limits<long>::max()) + 1;
        std::vector<unsigned char> w = MakeStructWire(SCI_GETTEXTRANGE, tooBig, 5, nullptr, 0);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
        std::vector<unsigned char> w2 = MakeStructWire(SCI_GETTEXTRANGE, 0, tooBig, nullptr, 0);
        CHECK(!CallWire(w2, static_cast<DWORD>(w2.size())).ok);
        // Full 变体可以吃下整个 INT_PTR 值域（这里只验"不是被值域挡掉的"）
        std::vector<unsigned char> w3 = MakeStructWire(SCI_GETTEXTRANGEFULL, tooBig, 5, nullptr, 0);
        CHECK(!CallWire(w3, static_cast<DWORD>(w3.size())).ok);   // 拒答，但理由是"范围非法"
    }
    // ⑧ 表外编号 ⇒ 拒答
    {
        std::vector<unsigned char> w = MakeStructWire(3000, 0, 5, nullptr, 0);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ⑨ 拒答桶（纵深防御：中转窗已经拒过一次）⇒ 拒答
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_FORMATRANGE, 0, 5, nullptr, 0);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
        std::vector<unsigned char> w2 = MakeStructWire(SCI_FORMATRANGEFULL, 0, 5, nullptr, 0);
        CHECK(!CallWire(w2, static_cast<DWORD>(w2.size())).ok);
    }
    // ⑩ 没有目标窗 ⇒ 拒答（不是"崩"）
    {
        std::vector<unsigned char> w = MakeStructWire(SCI_GETTEXTRANGE, 0, 5, nullptr, 0);
        const auto* cw = reinterpret_cast<const oop::SciStructCallWire*>(w.data());
        std::vector<unsigned char> bytes;
        unsigned long copied = 0;
        unsigned hasChrg = 0;
        INT_PTR a = 0, b = 0;
        LRESULT out = 0;
        CHECK(!oop::SciBridgeStructCall(nullptr, *cw, static_cast<DWORD>(w.size()),
                                        bytes, copied, hasChrg, a, b, out));
    }
    std::printf("  [8] wire 契约：多串 / 缺串 / 末字节非 NUL / cbData 不足 / 超值域 / "
                "表外 / 拒答桶 / 无目标窗 各自单独拒答；空串合法\n");
}

// [9] ★★ 6 条可桥接的逐条对撞
static void CrossCheckRange(const char* name, unsigned msg, bool full,
                            INT_PTR cpMin, INT_PTR cpMax) {
    const INT_PTR cpEnd = (cpMax == -1) ? static_cast<INT_PTR>(kDocLen) : cpMax;
    const std::size_t wantLen = static_cast<std::size_t>(cpEnd - cpMin);
    const std::size_t kHdr = sizeof(oop::SciStructCallWire);

    std::vector<unsigned char> w = MakeStructWire(msg, cpMin, cpMax, nullptr, 0);
    CHECK(w.size() == kHdr);
    const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
    if (!r.ok) {
        ++g_fail;
        std::printf("FAIL %s：真过桥被**拒答**\n", name);
        return;
    }
    if (r.copied != wantLen + 1 || r.bytes.size() != wantLen + 1) {
        ++g_fail;
        std::printf("FAIL %s：copied=%lu bytes=%zu（期望 %zu = len+1）\n",
                    name, r.copied, r.bytes.size(), wantLen + 1);
        return;
    }
    if (r.result != static_cast<LRESULT>(wantLen)) {
        ++g_fail;
        std::printf("FAIL %s：返回值 %ld（期望 %zu = len，**不含** NUL）\n",
                    name, static_cast<long>(r.result), wantLen);
    }
    if (std::memcmp(r.bytes.data(), kDoc + cpMin, wantLen) != 0) {
        ++g_fail;
        std::printf("FAIL %s：搬回的内容不是文档的 [%ld,%ld)\n",
                    name, static_cast<long>(cpMin), static_cast<long>(cpEnd));
    }
    if (r.bytes[wantLen] != 0) {
        ++g_fail;
        std::printf("FAIL %s：第 len 格不是 NUL\n", name);
    }
    // ★ 对撞：与进程内直调**逐字节相等** + 返回值相等
    std::vector<unsigned char> d;
    LRESULT dres = 0;
    DirectRange(msg, full, cpMin, cpMax, wantLen + 1, &d, &dres);
    if (r.result != dres) {
        ++g_fail;
        std::printf("FAIL %s：桥接返回值 %ld != 进程内直调 %ld\n",
                    name, static_cast<long>(r.result), static_cast<long>(dres));
    }
    if (std::memcmp(r.bytes.data(), d.data(), wantLen + 1) != 0) {
        ++g_fail;
        std::printf("FAIL %s：桥接搬回的 %zu 字节与进程内直调**不一致**\n", name, wantLen + 1);
    }
}

static void CrossCheckStyled(const char* name, unsigned msg, bool full,
                             INT_PTR cpMin, INT_PTR cpMax) {
    const std::size_t wantLen = static_cast<std::size_t>(cpMax - cpMin);
    const std::size_t wantCopied = wantLen * 2 + 2;
    std::vector<unsigned char> w = MakeStructWire(msg, cpMin, cpMax, nullptr, 0);
    const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
    if (!r.ok) {
        ++g_fail;
        std::printf("FAIL %s：真过桥被**拒答**\n", name);
        return;
    }
    if (r.copied != wantCopied || r.bytes.size() != wantCopied) {
        ++g_fail;
        std::printf("FAIL %s：copied=%lu bytes=%zu（期望 %zu = 2*len+2）\n",
                    name, r.copied, r.bytes.size(), wantCopied);
        return;
    }
    if (r.result != static_cast<LRESULT>(wantLen * 2)) {
        ++g_fail;
        std::printf("FAIL %s：返回值 %ld（期望 %zu = 2*len，**不含**末尾两个 NUL）\n",
                    name, static_cast<long>(r.result), wantLen * 2);
    }
    // 内容 = (char, style) 对；本测试没装词法器 ⇒ style 恒 0
    for (std::size_t i = 0; i < wantLen; ++i) {
        if (r.bytes[i * 2] != static_cast<unsigned char>(kDoc[cpMin + i]) ||
            r.bytes[i * 2 + 1] != 0) {
            ++g_fail;
            std::printf("FAIL %s：第 %zu 对不是 (%c, 0)\n", name, i, kDoc[cpMin + i]);
            break;
        }
    }
    if (r.bytes[wantLen * 2] != 0 || r.bytes[wantLen * 2 + 1] != 0) {
        ++g_fail;
        std::printf("FAIL %s：末尾两个字节不是 NUL\n", name);
    }
    std::vector<unsigned char> d;
    LRESULT dres = 0;
    DirectRange(msg, full, cpMin, cpMax, wantCopied, &d, &dres);
    if (r.result != dres || std::memcmp(r.bytes.data(), d.data(), wantCopied) != 0) {
        ++g_fail;
        std::printf("FAIL %s：与进程内直调不一致（result %ld vs %ld）\n",
                    name, static_cast<long>(r.result), static_cast<long>(dres));
    }
}

static void CrossCheckFind(const char* name, unsigned msg, bool full,
                           const char* needle, LRESULT wantPos) {
    std::vector<unsigned char> w = MakeStructWire(msg, 0, static_cast<INT_PTR>(kDocLen),
                                                  needle, 0);
    const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
    if (!r.ok) {
        ++g_fail;
        std::printf("FAIL %s：真过桥被**拒答**\n", name);
        return;
    }
    if (r.result != wantPos) {
        ++g_fail;
        std::printf("FAIL %s：返回值 %ld（期望 %ld）\n",
                    name, static_cast<long>(r.result), static_cast<long>(wantPos));
    }
    // ★ 直接断言 hasChrg（**不看**基准）。理由：批次 116 负控③ 抓到"命中在位置 0
    //   却报 hasChrg=0"时，桥接与直调**同向出错**（两边都用了被截断成 0 的 canary），
    //   于是下面那条"与直调一致"的断言照样通过 —— 一致 ≠ 对。判据要能独立成立。
    const unsigned wantChrg = (wantPos == -1) ? 0u : 1u;
    if (r.hasChrg != wantChrg) {
        ++g_fail;
        std::printf("FAIL %s：hasChrg=%u（期望 %u；命中位置 %ld）\n",
                    name, r.hasChrg, wantChrg, static_cast<long>(wantPos));
    }
    // ★ 命中位置 0 是**最容易错的那一格**（canary 截断成 0 时与"没写"同形），
    //   单独钉住回写的内容：chrgText 必须正好是 (0, needleLen)。
    if (wantPos == 0) {
        const INT_PTR nl = static_cast<INT_PTR>(std::strlen(needle));
        if (r.chrgMin != 0 || r.chrgMax != nl) {
            ++g_fail;
            std::printf("FAIL %s：命中在 0，chrgText=(%ld,%ld)（期望 (0,%ld)）\n",
                        name, static_cast<long>(r.chrgMin), static_cast<long>(r.chrgMax),
                        static_cast<long>(nl));
        }
    }
    // 与进程内直调对撞：返回值 + "写没写 chrgText" + 写的内容
    LRESULT dres = 0;
    bool dWrote = false;
    INT_PTR dMin = 0, dMax = 0;
    DirectFind(msg, full, 0, static_cast<INT_PTR>(kDocLen), needle, &dres, &dWrote,
               &dMin, &dMax);
    if (r.result != dres) {
        ++g_fail;
        std::printf("FAIL %s：桥接返回值 %ld != 进程内直调 %ld\n",
                    name, static_cast<long>(r.result), static_cast<long>(dres));
    }
    if ((r.hasChrg != 0) != dWrote) {
        ++g_fail;
        std::printf("FAIL %s：hasChrg=%u 但进程内直调 %s chrgText\n",
                    name, r.hasChrg, dWrote ? "写了" : "没写");
    }
    if (dWrote && (r.chrgMin != dMin || r.chrgMax != dMax)) {
        ++g_fail;
        std::printf("FAIL %s：chrgText=(%ld,%ld)，进程内直调 (%ld,%ld)\n",
                    name, static_cast<long>(r.chrgMin), static_cast<long>(r.chrgMax),
                    static_cast<long>(dMin), static_cast<long>(dMax));
    }
}

static void TestRealScintilla() {
    if (!g_sci) return;

    // ---- kRangeOut ---------------------------------------------------------
    CrossCheckRange("GetTextRange(0,5)",       SCI_GETTEXTRANGE,     false, 0, 5);
    CrossCheckRange("GetTextRangeFull(0,11)",  SCI_GETTEXTRANGEFULL, true,  0, 11);
    CrossCheckRange("GetTextRange(3,11)",      SCI_GETTEXTRANGE,     false, 3, 11);
    CrossCheckRange("GetTextRange(5,5) 空范围", SCI_GETTEXTRANGE,     false, 5, 5);
    // ★ cpMax == -1 = "到文档末尾"：容量**算得出**（靠宿主侧 GETTEXTLENGTH）
    CrossCheckRange("GetTextRange(0,-1)",      SCI_GETTEXTRANGE,     false, 0, -1);
    CrossCheckRange("GetTextRangeFull(2,-1)",  SCI_GETTEXTRANGEFULL, true,  2, -1);

    // ---- kStyledOut --------------------------------------------------------
    CrossCheckStyled("GetStyledText(0,3)",     SCI_GETSTYLEDTEXT,     false, 0, 3);
    CrossCheckStyled("GetStyledTextFull(0,2)", SCI_GETSTYLEDTEXTFULL, true,  0, 2);
    CrossCheckStyled("GetStyledText(2,5)",     SCI_GETSTYLEDTEXT,     false, 2, 5);
    CrossCheckStyled("GetStyledText(4,4) 空",  SCI_GETSTYLEDTEXT,     false, 4, 4);

    // ---- kFindInOut --------------------------------------------------------
    // ★ "找到" ⇒ chrgText 被写；"没找到" ⇒ 一个字节都不写（hasChrg=0）
    CrossCheckFind("FindText(world)",     SCI_FINDTEXT,     false, "world", 6);
    CrossCheckFind("FindText(hello)",     SCI_FINDTEXT,     false, "hello", 0);
    CrossCheckFind("FindText(zzz) 未命中", SCI_FINDTEXT,     false, "zzz", -1);
    CrossCheckFind("FindTextFull(hello)", SCI_FINDTEXTFULL, true,  "hello", 0);
    CrossCheckFind("FindTextFull(world)", SCI_FINDTEXTFULL, true,  "world", 6);
    CrossCheckFind("FindTextFull(zzz) 未命中", SCI_FINDTEXTFULL, true, "zzz", -1);

    // 桥接侧的 hasChrg 语义再单独钉一次（这是本族与前三族最大的差别）
    {
        std::vector<unsigned char> hit = MakeStructWire(SCI_FINDTEXT, 0,
                                                        static_cast<INT_PTR>(kDocLen),
                                                        "world", 0);
        const WireRun a = CallWire(hit, static_cast<DWORD>(hit.size()));
        CHECK(a.ok && a.hasChrg == 1 && a.chrgMin == 6 && a.chrgMax == 11);
        std::vector<unsigned char> miss = MakeStructWire(SCI_FINDTEXT, 0,
                                                         static_cast<INT_PTR>(kDocLen),
                                                         "zzz", 0);
        const WireRun b = CallWire(miss, static_cast<DWORD>(miss.size()));
        // ★ 未命中：hasChrg **必须**是 0（照写就会把插件的原 chrgText 覆盖成 (0,0)，
        //   "没找到"与"找到了空匹配"就分不开了）。
        CHECK(b.ok && b.hasChrg == 0 && b.result == -1);
    }
    // ★★ 负控③ 的**回归**：命中位置 0 必须报 hasChrg=1 + chrgText=(0,len)。
    //   这是"canary 被截断成 0"那个真缺陷的正面钉子 —— 非 Full 变体尤其危险
    //   （它的 chrgText 字段是 long，最容易被 INT_PTR 的 canary 截断）。两个变体
    //   都写：它们走的是**两条独立**的 canary 分支，只测一条会漏掉另一条。
    {
        struct AtZero { unsigned msg; bool full; const char* name; };
        const AtZero kAtZero[] = {
            { SCI_FINDTEXT,     false, "FindText(hello)@0" },
            { SCI_FINDTEXTFULL, true,  "FindTextFull(hello)@0" },
        };
        for (const AtZero& c : kAtZero) {
            std::vector<unsigned char> w = MakeStructWire(
                c.msg, 0, static_cast<INT_PTR>(kDocLen), "hello", 0);
            const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
            if (!(r.ok && r.result == 0 && r.hasChrg == 1 &&
                  r.chrgMin == 0 && r.chrgMax == 5)) {
                ++g_fail;
                std::printf("FAIL %s：ok=%d result=%ld hasChrg=%u chrg=(%ld,%ld)"
                            "（期望 ok=1 result=0 hasChrg=1 chrg=(0,5)）\n",
                            c.name, r.ok ? 1 : 0, static_cast<long>(r.result), r.hasChrg,
                            static_cast<long>(r.chrgMin), static_cast<long>(r.chrgMax));
            }
        }
    }
    std::printf("  [9] 真 Scintilla：6 条可桥接 × 多组边界，逐条与进程内直调对撞"
                "（含命中位置 0 的 hasChrg 回归）\n");
}

// [10] ★★ 拒答桶：桥接必拒答 + 当场演示"为什么不可桥接"
static void TestRefuseHandleProof() {
    if (!g_sci) return;

    // (a) 消息级：lParam == 0 直接返回 0（Editor.cxx:1969）
    CHECK(::SendMessageW(g_sci, SCI_FORMATRANGE, 0, 0) == 0);

    // (b) 桥接侧：**没有布局** ⇒ 永远拒答（SciStructSize 对 kRefuseHandle 返回 0）
    CHECK(oop::SciStructSize(oop::SciStructKind::kRefuseHandle, false) == 0);
    CHECK(oop::SciStructSize(oop::SciStructKind::kRefuseHandle, true) == 0);
    CHECK(!oop::SciStructRelayable(SCI_FORMATRANGE));
    CHECK(!oop::SciStructRelayable(SCI_FORMATRANGEFULL));
    CHECK(oop::SciStructKindOf(SCI_FORMATRANGE) == oop::SciStructKind::kRefuseHandle);
    CHECK(oop::SciStructKindOf(SCI_FORMATRANGEFULL) == oop::SciStructKind::kRefuseHandle);

    // (c) ★ 拒答理由**当场演出来**（不写在注释里）：
    //     这条消息的效果**完全取决于**结构体里的两个 GDI 句柄，而"过桥"这套
    //     机制的全部依据是**能不能在接收进程里判定外来值属于谁** —— 内存指针
    //     可以（VirtualQuery + 我们自己的记账），句柄不行。
    //     下面三行把这件事分档：句柄**不是地址**（唯一的探针对它失效），而
    //     同一个探针对真地址**是有效的**（所以第一条不是"探针坏了"）；
    //     同时它**又确实是一个本进程合法的 DC 句柄**（"看起来合法"≠"可判定"）。
    //     推论：一个在插件进程里有效的 HDC，在编辑器进程里可能**恰好**是另一个
    //     DC 的句柄 ⇒ 会静默画到别处，且**没有任何错误信号**。
    {
        HDC mine = ::GetDC(nullptr);
        CHECK(mine != nullptr && ::GetObjectType(mine) == OBJ_DC);   // 本进程的合法 DC

        // 正对照：真地址**可以**被内存探针判定 —— 这是批次 110 指针判据成立的前提。
        int local = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T vqLocal = ::VirtualQuery(&local, &mbi, sizeof(mbi));
        CHECK(vqLocal != 0);
        const char* base = static_cast<const char*>(mbi.BaseAddress);
        const char* here = reinterpret_cast<const char*>(&local);
        CHECK(base <= here && here < base + mbi.RegionSize);   // 探针给出"含此地址的区域"

        // ★★ 反面**不能**这样断言：`::VirtualQuery(mine, ...) == 0`（"句柄不是地址"）。
        //    批次 116 原版就是这一条，它在 2026-09-24 的 Release 全量构建上变红，
        //    且与批次 117 的任何改动无关（本条是**既有**缺陷，被 117a 的负控顺带照出）。
        //    原因：GDI 句柄值是**句柄表索引**，它落在哪一段地址**由实现决定**，
        //    没有任何契约保证"一定不在已映射区间"。实测该值在某些进程布局下
        //    恰好命中已映射区间 ⇒ VirtualQuery 成功 ⇒ 断言红。
        //    更深一层：这条断言**即使为真也没有信息量** —— 它证明的只是"这一次的
        //    数值恰好没被映射"，与"句柄不能作为归属判据"无关 ⇒ 它量错了对象
        //    （LESSONS：探针必须能证明它在量对的东西）。
        //    有契约依据、且机制真正需要的是另外两条事实：
        //      · GDI 认得这个句柄（GetObjectType == OBJ_DC）
        //      · 内存探针对**真地址**能给出含此地址的区域（上面两条 CHECK）
        //    而"这个句柄属于谁"**没有任何 API 能回答** ⇒ 桥接侧无从判定，
        //    所以本段的判据落在 (b) 的 SciStructRelayable() == false 上。
        //    下面只把"句柄值是否命中已映射区间"当**观测**打印，不当判据用。
        MEMORY_BASIC_INFORMATION mbiH{};
        const SIZE_T vqHandle = ::VirtualQuery(mine, &mbiH, sizeof(mbiH));
        std::printf("  [10] 观测（非判据）：HDC=%p  VirtualQuery(HDC)=%llu  "
                    "VirtualQuery(&local)=%llu\n",
                    reinterpret_cast<void*>(mine),
                    static_cast<unsigned long long>(vqHandle),
                    static_cast<unsigned long long>(vqLocal));
        ::ReleaseDC(nullptr, mine);
    }
    // (d) 结构体里确实带着这两个句柄字段（用厂商头核）
    static_assert(std::is_same<decltype(Sci_RangeToFormat::hdc), Sci_SurfaceID>::value,
                  "FormatRange 的第一个字段是 SurfaceID（GDI HDC）");
    static_assert(std::is_same<decltype(Sci_RangeToFormat::hdcTarget), Sci_SurfaceID>::value,
                  "FormatRange 的第二个字段是 SurfaceID（GDI HDC）");
    // 形状：HDC/HDC/2×RECT/CHARRANGE（**无** HWND —— 与 Win32 的 FORMATRANGE 不同）
    CHECK(sizeof(Sci_Rectangle) == 16);                    // 4 × int（Scintilla.h:1390）
    CHECK(sizeof(Sci_RangeToFormat) == 56);                // 8+8+16+16+ 2×long
    CHECK(sizeof(Sci_RangeToFormatFull) == 64);            // 8+8+16+16+ 2×intptr_t ★ 不是 56
    CHECK(sizeof(Sci_CharacterRangeFull) - sizeof(Sci_CharacterRange) == 8);
    std::printf("  [10] 拒答桶 2 条：桥接必拒答；理由是 GDI 句柄进程私有且无可依赖判据\n");
}

int main() {
    std::printf("== test_sci_struct (batch 116) ==\n");
    TestScale();
    TestOrder();
    TestLookupAgreement();
    TestDerivedPredicates();
    TestShapeAgreement();
    TestRelayable();
    TestBytesToWrite();
    TestSizeFn();
    if (MakeScintilla()) {
        TestWireContract();
        TestRealScintilla();
        TestRefuseHandleProof();
    }
    std::printf("== %s (%d failure%s) ==\n", g_fail ? "FAILED" : "ALL PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    std::printf("NOTE: 本守卫证明表内部自洽 + 与形状表一致 + abi 镜像与厂商头一致 + "
                "对**真 Scintilla** 成立；\n"
                "      表 ↔ Scintilla.iface 的名字映射由 scripts/gen-sci-marshal.py "
                "--check 负责。两条都必须跑。\n");
    if (g_sci) ::DestroyWindow(g_sci);
    return g_fail ? 1 : 0;
}
