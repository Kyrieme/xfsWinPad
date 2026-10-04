// test_sci_inout.cpp — SCI_* **出入参**族（kInOutStr，5 条）守卫 + 真 Scintilla
// 实证（批次 115）。
//
// 为什么单开一个测试目标：本族与批次 113/114 的两张表**都**有关系 —— 形状是
// (string, stringresult)：**入参串在 wParam、出参缓冲在 lParam**。于是它 =
// 入参族的一半 + 出参族的一半：
//   * 出参侧的容量规则与批次 114 的 kOutStr **完全一样** ⇒ 两表共用同一个
//     SciOutKind 桶空间（由 test_sci_out.cpp 钉桶语义，本文件钉"哪 5 条"）；
//   * 多出来的唯一一件事是"wParam 是一个必须搬过来的串"（SciOutNeedsInStr）。
// 折进别的目标里，"忘了注册进 CMakeLists"会让这 5 条**静默失去覆盖**，而它们
// 恰恰是"入参串没搬对 ⇒ 宿主按 C 串读一个 0 指针"这类缺陷的唯一防线。
//
// ★ 判据 B（真 Scintilla）在本文件有两条独立用法，缺一不可：
//   ① **对撞**：每条 kNul 消息真过桥一次，再对**同一个真 Scintilla** 进程内
//      直调一次，两份字节必须逐字节相等 + 返回值相等。只断言"过桥成功"是不够的
//      —— 搬错了内容也照样成功。
//   ② **反证拒答**：两条 kRefuseRuntime 的拒答理由**不写在注释里、当场演出来**
//      （GetRepresentation 的两种 0；EncodedFromUTF8 的写入量随编码模式变）。
//      "为什么拒答"如果只存在于注释，下一个人只会看到"这两条被拒了"。
//
// 覆盖：
//   [1] 规模：逐条重数与 .inc 自报计数对账（5 = kNul 3 + kRefuseRuntime 2）
//   [2] 有序性：msg 严格升序（SciOutKindOf 二分查找的前提）
//   [3] 二分 ↔ 线性对撞（**两张表的并集**）
//   [4] ★ SciOutNeedsInStr ⟺ 在 kSciInOutTable 里（两个方向都查）
//   [5] 与形状表一致：kInOutStr 5 条 ⟺ 本表 5 条
//   [6] ★ wire 契约：inBytes 的五道校验（缺串 / 多串 / 末字节非 NUL / cbData 不足
//       / 带串时 argWp 非 0）
//   [7] ★★ 真 Scintilla：3 条 kNul 真过桥 + 与进程内直调**逐字节对撞**
//   [8] ★★ 拒答桶 2 条：桥接必拒答 + **当场演示**拒答理由
//   [9] ★ 查长度路径（writeBack=0）也要求带串（探长度同样需要那个属性名）
//
// ★ 本守卫的边界（写进输出，避免下一个读它的人高估它）：
//   它证明「表内部自洽 + 与形状表一致 + wire 契约完备 + **对真 Scintilla 成立**」，
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

static int CountInOutKind(oop::SciOutKind k) {
    int n = 0;
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i)
        if (oop::kSciInOutTable[i].kind == k) ++n;
    return n;
}

// 线性查找（与表里的二分实现**独立**）。必须扫**两张**表 —— 只扫一张会让
// "二分 ↔ 线性对撞"退化成假绿。
static oop::SciOutKind LinearLookup(unsigned msg) {
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i)
        if (oop::kSciInOutTable[i].msg == msg) return oop::kSciInOutTable[i].kind;
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        if (oop::kSciOutTable[i].msg == msg) return oop::kSciOutTable[i].kind;
    return oop::SciOutKind::kNone;
}

// [1] 规模 + 自报计数
static void TestScale() {
    CHECK(oop::kSciInOutCount == 5);
    CHECK(oop::kSciInOutCount == oop::kSciCountInOutStr);   // 与形状表自洽
    CHECK(CountInOutKind(oop::SciOutKind::kNul) ==
          static_cast<int>(oop::kSciInOutCountNul));
    CHECK(CountInOutKind(oop::SciOutKind::kRefuseRuntime) ==
          static_cast<int>(oop::kSciInOutCountRefuseRuntime));
    // 桶和 == 表长：多一个漏归类的条目就会在这里露出来
    CHECK(static_cast<unsigned>(CountInOutKind(oop::SciOutKind::kNul) +
                                CountInOutKind(oop::SciOutKind::kRefuseRuntime)) ==
          oop::kSciInOutCount);
    // 本族只有这两个桶：别的桶出现在这里说明分类器变了，必须显式复核
    CHECK(CountInOutKind(oop::SciOutKind::kNone) == 0);
    CHECK(CountInOutKind(oop::SciOutKind::kNoNul) == 0);
    CHECK(CountInOutKind(oop::SciOutKind::kClampWpNul) == 0);
    std::printf("  [1] 5 条：kNul 3 / kRefuseRuntime 2\n");
}

// [2] 有序性
static void TestOrder() {
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i) {
        CHECK(oop::kSciInOutTable[i].kind != oop::SciOutKind::kNone);
        if (i) CHECK(oop::kSciInOutTable[i - 1].msg < oop::kSciInOutTable[i].msg);
        CHECK(oop::kSciInOutTable[i].msg >= oop::kSciMsgMin);
        CHECK(oop::kSciInOutTable[i].msg <= oop::kSciMsgMax);
        CHECK(oop::kSciInOutTable[i].msg > WM_USER);
    }
}

// [3] 二分 ↔ 线性对撞
static void TestLookupAgreement() {
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i)
        CHECK(oop::SciOutKindOf(oop::kSciInOutTable[i].msg) ==
              LinearLookup(oop::kSciInOutTable[i].msg));
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        CHECK(oop::SciOutKindOf(oop::kSciOutTable[i].msg) ==
              LinearLookup(oop::kSciOutTable[i].msg));
    const unsigned probes[] = { 0, 1, WM_USER, 1999, 2000, 2448, 2449, 2666, 2667,
                                oop::kSciMsgMin - 1, oop::kSciMsgMin,
                                oop::kSciMsgMax, oop::kSciMsgMax + 1, 65535 };
    for (unsigned p : probes)
        CHECK(oop::SciOutKindOf(p) == LinearLookup(p));
}

// [4] ★ SciOutNeedsInStr 与 kSciInOutTable 必须**同一个来源**
static void TestNeedsInStr() {
    // 方向一：表里每条都要带串
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i)
        CHECK(oop::SciOutNeedsInStr(oop::kSciInOutTable[i].msg));
    // 方向二：出参族那 30 条一条都不带串（否则 inBytes 会成为一个旁路契约）
    for (unsigned i = 0; i < oop::kSciOutCount; ++i)
        CHECK(!oop::SciOutNeedsInStr(oop::kSciOutTable[i].msg));
    // 方向三：表外一律不带串
    for (unsigned m : { 0u, 1u, (unsigned)WM_USER, 2000u, 3001u, 5000u, 65535u })
        CHECK(!oop::SciOutNeedsInStr(m));
    // 与 SciOutKindOf 的口径一致：needsInStr ⇒ 有桶
    for (unsigned m = oop::kSciMsgMin; m <= oop::kSciMsgMax; ++m)
        if (oop::SciOutNeedsInStr(m))
            CHECK(oop::SciOutKindOf(m) != oop::SciOutKind::kNone);
    std::printf("  [4] needsInStr ⟺ 在 kSciInOutTable 里（三方向都查）\n");
}

// [5] 与形状表一致（两个方向）
static void TestShapeAgreement() {
    for (unsigned i = 0; i < oop::kSciInOutCount; ++i)
        CHECK(oop::SciShapeOf(oop::kSciInOutTable[i].msg) == oop::SciShape::kInOutStr);
    int shaped = 0, inTable = 0;
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        if (oop::kSciTable[i].shape != oop::SciShape::kInOutStr) continue;
        ++shaped;
        if (oop::SciOutKindOf(oop::kSciTable[i].msg) != oop::SciOutKind::kNone)
            ++inTable;
    }
    CHECK(shaped == 5);
    CHECK(inTable == shaped);
    // 形状表里带指针的其余四族**一条都不许**出现在出参分类里
    for (unsigned i = 0; i < oop::kSciCount; ++i) {
        const oop::SciShape sh = oop::kSciTable[i].shape;
        if (sh != oop::SciShape::kStruct && sh != oop::SciShape::kRawPtr &&
            sh != oop::SciShape::kPtrRet) continue;
        CHECK(oop::SciOutKindOf(oop::kSciTable[i].msg) == oop::SciOutKind::kNone);
    }
    std::printf("  [5] 形状表 kInOutStr 5 条 ⟺ 本表 5 条；kStruct/kRawPtr/kPtrRet 全不在\n");
}

// ============================================================================
// wire 组装 —— 直接喂给**产品代码**的自由函数（不是夹具自己重写一遍宿主侧）
// ============================================================================
// 真 Scintilla 窗口句柄。**必须先声明**：MakeWire/CallWire 都要用它，而它们的
// 定义在"判据 B"整段之前 —— 把句柄声明放到后面会让这一整段编不过。
static HWND g_sci = nullptr;
static HMODULE g_lexilla = nullptr;

// 载荷**含结尾 NUL**（与 SciCallWire 的"只装内容字节"相反，见 OopProtocol.h）。
// key == nullptr ⇒ 不带载荷（inBytes = 0）；key == "" ⇒ 载荷是 1 个 NUL。
static std::vector<unsigned char> MakeWire(unsigned sciMsg, const char* key,
                                           bool writeBack) {
    const std::size_t kb = key ? std::strlen(key) + 1 : 0;
    std::vector<unsigned char> w(sizeof(oop::SciOutCallWire) + kb, 0);
    auto* cw = reinterpret_cast<oop::SciOutCallWire*>(w.data());
    cw->magic = oop::kMagic;
    cw->msg = oop::OOPM_SCIOUTCALL;
    cw->sciMsg = sciMsg;
    cw->argWp = 0;          // 带串时 argWp 必须是 0（宿主侧硬校验）
    cw->reqId = 1;
    cw->replyTo = 0;
    cw->writeBack = writeBack ? 1u : 0u;
    cw->inBytes = static_cast<unsigned>(kb);
    if (kb) std::memcpy(w.data() + sizeof(oop::SciOutCallWire), key, kb);
    return w;
}

struct WireRun {
    bool ok = false;
    unsigned long need = 0;
    unsigned long copied = 0;
    LRESULT result = 0;
    std::vector<unsigned char> bytes;
};

static WireRun CallWire(const std::vector<unsigned char>& w, DWORD cbData) {
    WireRun r;
    const auto* cw = reinterpret_cast<const oop::SciOutCallWire*>(w.data());
    r.ok = oop::SciBridgeOutCall(g_sci, *cw, cbData, r.bytes, r.need, r.copied, r.result);
    return r;
}

// [6] ★ wire 契约的四道校验 —— 每一条都要能**单独**把请求挡下来
static void TestWireContract() {
    if (!g_sci) return;
    const std::size_t kHdr = sizeof(oop::SciOutCallWire);

    // 正例：带串的请求必须过桥（这是后面所有断言的前提）
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETPROPERTY, "k1", true);
        const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
        CHECK(r.ok);
        CHECK(w.size() == kHdr + 3);
    }
    // ① 缺串：表说要带，inBytes==0 ⇒ 拒答（绝不拿 argWp 的 0 当串指针）
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETPROPERTY, nullptr, true);
        const auto* cw = reinterpret_cast<const oop::SciOutCallWire*>(w.data());
        CHECK(cw->inBytes == 0);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ② 多串：**不该带串**的族带了串 ⇒ 拒答（否则 inBytes 就是绕过 argWp 的旁路）
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETTEXT, "k1", true);
        CHECK(!oop::SciOutNeedsInStr(SCI_GETTEXT));
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ③ 末字节不是 NUL：宿主会按 C 串读它 ⇒ 拒答（而不是读穿）
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETPROPERTY, "k1", true);
        w[kHdr + 2] = 'Z';                 // "k1\0" 的 NUL 被改成 'Z'
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ④ cbData 比 sizeof + inBytes 少 1 ⇒ 载荷没有真的到齐 ⇒ 拒答
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETPROPERTY, "k1", true);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size() - 1)).ok);
    }
    // ⑤ 空串 != 不带串：inBytes==1（只有 NUL）必须是**合法**请求
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETPROPERTY, "", true);
        CHECK(w.size() == kHdr + 1);
        const auto* cw = reinterpret_cast<const oop::SciOutCallWire*>(w.data());
        CHECK(cw->inBytes == 1);
        CHECK(CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    // ⑥ cbData 比结构体还小 ⇒ 连 inBytes 都不该读
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETPROPERTY, "k1", true);
        CHECK(!CallWire(w, static_cast<DWORD>(kHdr - 1)).ok);
    }
    // ⑦ 带串却把 argWp 也填了 ⇒ 拒答：否则同一个请求里会同时存在"值语义 wParam"
    //    与"串指针 wParam"两个答案，而 inBytes 就成了绕过 argWp 契约的旁路。
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETPROPERTY, "k1", true);
        reinterpret_cast<oop::SciOutCallWire*>(w.data())->argWp = 0x1234;
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    std::printf("  [6] wire 契约：缺串 / 多串 / 末字节非 NUL / cbData 不足 / argWp 非 0 "
                "各自单独拒答；空串合法\n");
}

// ============================================================================
// 判据 B：真 Scintilla
// ============================================================================
// Scintilla.dll 的 DllMain 在 DLL_PROCESS_ATTACH 里注册 "Scintilla" 窗口类
// （third_party/scintilla/win32/ScintillaDLL.cxx）⇒ LoadLibrary 之后就能直接
// CreateWindowEx。本 exe 与 Scintilla.dll / Lexilla.dll 同在 build/bin/Release/，
// 而 exe 所在目录是 DLL 搜索顺序的第一位 ⇒ 不需要额外的拷贝步骤。
//
// ⚠ 本族的三条 kNul 消息**需要装一个词法器**才有非空的属性值：Scintilla 5.x 里
//   GetProperty 走 DocumentLexState()->PropGet()，而 LexState::instance 只有在
//   SCI_SETILEXER 之后才非空（没装词法器时 PropGet 恒返回 nullptr）。
//   Scintilla 5.6.6 **没有** SCI_LOADLEXERLIBRARY / SCI_SETLEXERLANGUAGE 了
//   （5.x 起改由应用侧 CreateLexer + SCI_SETILEXER）⇒ 这里自己 LoadLibrary
//   Lexilla.dll 取 CreateLexer，用内置的 "null" 词法器。它是个真 LexerBase，
//   PropSet/PropGet 就是 PropSetSimple —— 足够让属性往返成立。
static bool MakeScintilla() {
    HMODULE mod = ::LoadLibraryW(L"Scintilla.dll");
    if (!mod) {
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
    return true;
}

// 装 "null" 词法器；返回是否**真的装上了**（用属性往返自证，不看返回值）
static bool AttachNullLexer() {
    typedef void* (__stdcall *CreateLexerFn)(const char*);
    g_lexilla = ::LoadLibraryW(L"Lexilla.dll");
    if (!g_lexilla) {
        ++g_fail;
        std::printf("FAIL 加载 Lexilla.dll 失败（err=%lu）\n",
                    static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    auto create = reinterpret_cast<CreateLexerFn>(::GetProcAddress(g_lexilla, "CreateLexer"));
    if (!create) {
        ++g_fail;
        std::printf("FAIL Lexilla.dll 没有导出 CreateLexer\n");
        return false;
    }
    void* lex = create("null");
    if (!lex) {
        ++g_fail;
        std::printf("FAIL CreateLexer(\"null\") 返回空\n");
        return false;
    }
    ::SendMessageW(g_sci, SCI_SETILEXER, 0, reinterpret_cast<LPARAM>(lex));

    // ★ 自证：装上了才有属性往返。只信返回值会得到"看起来装了、其实 PropGet 恒
    //   返回 nullptr"的假绿 —— 那样后面 3 条 kNul 全部退化成"need=0"，测试仍然
    //   全绿却什么都没验到。
    ::SendMessageW(g_sci, SCI_SETPROPERTY, reinterpret_cast<WPARAM>("selftest"),
                   reinterpret_cast<LPARAM>("1"));
    char b[8] = { 0 };
    const LRESULT n = ::SendMessageW(g_sci, SCI_GETPROPERTY,
                                     reinterpret_cast<WPARAM>("selftest"),
                                     reinterpret_cast<LPARAM>(b));
    if (n != 1 || b[0] != '1') {
        ++g_fail;
        std::printf("FAIL 词法器没装上：属性往返得到 n=%ld b=\"%s\"（应为 1 / \"1\"）。\n"
                    "  没有它，本文件的三条 kNul 用例全部退化成空值 ⇒ 判据 B 名存实亡。\n",
                    static_cast<long>(n), b);
        return false;
    }
    ::SendMessageW(g_sci, SCI_SETPROPERTY, reinterpret_cast<WPARAM>("selftest"),
                   reinterpret_cast<LPARAM>(""));
    return true;
}

// 进程内直调（**同一个**真 Scintilla）—— 对撞基准
static std::vector<unsigned char> DirectCall(unsigned msg, const char* key,
                                             std::size_t cap, LRESULT* res) {
    std::vector<unsigned char> b(cap + 8, 0xA5);
    *res = ::SendMessageW(g_sci, msg, reinterpret_cast<WPARAM>(key),
                          reinterpret_cast<LPARAM>(b.data()));
    return b;
}

// [7] ★★ 一条 kNul 消息的完整对撞
//   wantNeed  期望的 need（= probe 返回值 = 值的字节数）
//   wantValue 期望的值内容（wantNeed 个字节）
static void CrossCheck(const char* name, unsigned msg, const char* key,
                       unsigned long wantNeed, const char* wantValue) {
    // ① 查长度（桥接，writeBack=0）：带串也必须成立，且一个字节都不写
    {
        std::vector<unsigned char> w = MakeWire(msg, key, false);
        const WireRun p = CallWire(w, static_cast<DWORD>(w.size()));
        if (!p.ok) {
            ++g_fail;
            std::printf("FAIL %s：查长度被**拒答**（带串的探长度路径不通）\n", name);
            return;
        }
        if (p.need != wantNeed || p.copied != 0 || !p.bytes.empty()) {
            ++g_fail;
            std::printf("FAIL %s：查长度 need=%lu（期望 %lu）copied=%lu bytes=%zu\n",
                        name, p.need, wantNeed, p.copied, p.bytes.size());
            return;
        }
    }
    // ② 真写：kNul 桶 ⇒ copied = need + 1
    std::vector<unsigned char> w = MakeWire(msg, key, true);
    const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
    const unsigned long wantCopied = wantNeed + 1;
    if (!r.ok) {
        ++g_fail;
        std::printf("FAIL %s：真过桥被**拒答** —— 分类与真 Scintilla 的写入量不一致\n", name);
        return;
    }
    if (r.copied != wantCopied || r.bytes.size() != wantCopied) {
        ++g_fail;
        std::printf("FAIL %s：copied=%lu bytes=%zu（期望 %lu = need+1）\n",
                    name, r.copied, r.bytes.size(), wantCopied);
        return;
    }
    // ③ ★ 对撞：与进程内直调**逐字节相等** + 返回值相等
    LRESULT dres = 0;
    const std::vector<unsigned char> d =
        DirectCall(msg, key, static_cast<std::size_t>(wantCopied), &dres);
    if (r.result != dres) {
        ++g_fail;
        std::printf("FAIL %s：桥接返回值 %ld != 进程内直调 %ld\n",
                    name, static_cast<long>(r.result), static_cast<long>(dres));
    }
    if (std::memcmp(r.bytes.data(), d.data(), static_cast<std::size_t>(wantCopied)) != 0) {
        ++g_fail;
        std::printf("FAIL %s：桥接搬回的 %lu 字节与进程内直调**不一致**\n",
                    name, wantCopied);
    }
    // ④ 内容与 NUL 位置
    const std::size_t vlen = wantValue ? std::strlen(wantValue) : 0;
    if (vlen != wantNeed) {
        ++g_fail;
        std::printf("FAIL %s：用例自相矛盾（wantValue 长 %zu，wantNeed=%lu）\n",
                    name, vlen, wantNeed);
    }
    if (vlen && std::memcmp(r.bytes.data(), wantValue, vlen) != 0) {
        ++g_fail;
        std::printf("FAIL %s：内容不是 \"%s\"\n", name, wantValue);
    }
    if (r.bytes[wantNeed] != 0) {
        ++g_fail;
        std::printf("FAIL %s：第 need 格不是 NUL（kNul 桶的语义）\n", name);
    }
}

// 200 字节的长键：验载荷跨进程不截断（kSciPayloadMax = 32768）
static char g_longKey[201];

static void TestRealScintillaNul() {
    if (!g_sci) return;

    // 属性表：k1 = "v1"（其余键一律不存在 ⇒ PropSetSimple::Get 返回 ""）
    ::SendMessageW(g_sci, SCI_SETPROPERTY, reinterpret_cast<WPARAM>("k1"),
                   reinterpret_cast<LPARAM>("v1"));

    // ★ 全族的三个 kNul 消息都取到；GetProperty 另外取三种边界
    CrossCheck("GetProperty(k1)", SCI_GETPROPERTY, "k1", 2, "v1");
    // ★ 这条是"5.6.6 里 GetPropertyExpanded 就是 PropGet"的**真机证据**：
    //   它与 GetProperty 走同一个 PropGet（ScintillaBase.cxx:1079/1082），
    //   所以值必须一模一样。谁把 PropGetExpanded 那套老语义写进表里，这里会红。
    CrossCheck("GetPropertyExpanded(k1)", SCI_GETPROPERTYEXPANDED, "k1", 2, "v1");
    // LexerBase::DescribeProperty 恒返回 ""（lexlib/LexerBase.cxx:61）⇒ 空值
    CrossCheck("DescribeProperty(k1)", SCI_DESCRIBEPROPERTY, "k1", 0, "");
    // 没设过的键：PropSetSimple::Get 返回 ""（**不是** nullptr）⇒ 与空值同形
    CrossCheck("GetProperty(nope)", SCI_GETPROPERTY, "nope", 0, "");
    // 空键：载荷只有 1 个 NUL（inBytes==1）—— 与"不带串"是两回事，见 [6]⑤
    CrossCheck("GetProperty(\"\")", SCI_GETPROPERTY, "", 0, "");
    // 200 字节长键：载荷不截断
    CrossCheck("GetProperty(200B)", SCI_GETPROPERTY, g_longKey, 0, "");

    std::printf("  [7] 真 Scintilla：3 条 kNul 真过桥 + 与进程内直调逐字节对撞（6 个用例）\n");
}

// [8] ★★ 拒答桶：桥接必拒答，且**当场把拒答理由演出来**
static void TestRefuseRuntimeProof() {
    if (!g_sci) return;

    // ---- GetRepresentation(2666)：probe **分不了两种 0** ---------------------
    // (a) repr 不存在 ⇒ Editor.cxx:8624 直接 `return 0`，**一个字节都不写**
    ::SendMessageW(g_sci, SCI_CLEARREPRESENTATION, reinterpret_cast<WPARAM>("a"), 0);
    unsigned long needNoRepr = 0;
    {
        char buf[8];
        std::memset(buf, 0x5A, sizeof(buf));
        needNoRepr = static_cast<unsigned long>(
            ::SendMessageW(g_sci, SCI_GETREPRESENTATION,
                           reinterpret_cast<WPARAM>("a"), 0));
        ::SendMessageW(g_sci, SCI_GETREPRESENTATION, reinterpret_cast<WPARAM>("a"),
                       reinterpret_cast<LPARAM>(buf));
        CHECK(needNoRepr == 0);
        CHECK(static_cast<unsigned char>(buf[0]) == 0x5A);   // 写了 0 字节
    }
    // (b) repr 存在但内容是**空串** ⇒ 走 StringResult，写 1 个 NUL
    ::SendMessageW(g_sci, SCI_SETREPRESENTATION, reinterpret_cast<WPARAM>("a"),
                   reinterpret_cast<LPARAM>(""));
    unsigned long needEmptyRepr = 0;
    {
        char buf[8];
        std::memset(buf, 0x5A, sizeof(buf));
        needEmptyRepr = static_cast<unsigned long>(
            ::SendMessageW(g_sci, SCI_GETREPRESENTATION,
                           reinterpret_cast<WPARAM>("a"), 0));
        ::SendMessageW(g_sci, SCI_GETREPRESENTATION, reinterpret_cast<WPARAM>("a"),
                       reinterpret_cast<LPARAM>(buf));
        CHECK(needEmptyRepr == 0);
        CHECK(buf[0] == 0);                                  // 写了 1 字节 NUL
        CHECK(static_cast<unsigned char>(buf[1]) == 0x5A);
    }
    // ★★ 同一个 probe 值（0），两种不同的写入量（0 / 1）⇒ 容量不可知。
    //    "写 0 字节"那支若被当成 kNul（copied = need+1 = 1），哨兵校验会失败 ⇒
    //    同一个桶在两种运行期状态下结论相反 ⇒ 只能拒答。
    CHECK(needNoRepr == needEmptyRepr);
    CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kNul, 0, 0) == 1);
    ::SendMessageW(g_sci, SCI_CLEARREPRESENTATION, reinterpret_cast<WPARAM>("a"), 0);

    // 桥接侧：必须拒答 —— 而且**必须在两种运行期状态下都拒**。
    // ★ 这一步是有意设计的，否则断言没有区分力：只在"默认状态"下断言的话，把
    //   它错标成 kNul 也照样拒答（哨兵校验兜住了）⇒ 负控②会绿。两种状态分别是：
    //     · GetRepresentation：repr 存在但为空串时，kNul 的 copied = need+1 = 1
    //       恰好就是"写了 1 个 NUL"⇒ 会**成功**；
    //     · EncodedFromUTF8：非 Unicode 模式下写入量恰好是 need+1 ⇒ 会**成功**。
    ::SendMessageW(g_sci, SCI_SETREPRESENTATION, reinterpret_cast<WPARAM>("a"),
                   reinterpret_cast<LPARAM>(""));
    for (const char* key : { "a", "zz" }) {
        std::vector<unsigned char> w = MakeWire(SCI_GETREPRESENTATION, key, true);
        const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
        CHECK(!r.ok && r.copied == 0 && r.bytes.empty());
    }
    ::SendMessageW(g_sci, SCI_CLEARREPRESENTATION, reinterpret_cast<WPARAM>("a"), 0);
    ::SendMessageW(g_sci, SCI_SETCODEPAGE, 0, 0);      // 非 Unicode 模式
    {
        std::vector<unsigned char> w = MakeWire(SCI_ENCODEDFROMUTF8, "A", true);
        const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
        CHECK(!r.ok && r.copied == 0 && r.bytes.empty());
    }
    ::SendMessageW(g_sci, SCI_SETCODEPAGE, 65001, 0);
    {
        std::vector<unsigned char> w = MakeWire(SCI_ENCODEDFROMUTF8, "A", true);
        const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
        CHECK(!r.ok && r.copied == 0 && r.bytes.empty());
    }

    // ---- EncodedFromUTF8(2449)：容量来自**伴生消息**，且写入量随编码模式变 ----
    // (a) 入参长度不在消息里：同一个 wp，SetLengthForEncode 一变 need 就变
    ::SendMessageW(g_sci, SCI_SETLENGTHFORENCODE, 3, 0);
    CHECK(::SendMessageW(g_sci, SCI_ENCODEDFROMUTF8,
                         reinterpret_cast<WPARAM>("ABCD"), 0) == 3);
    ::SendMessageW(g_sci, SCI_SETLENGTHFORENCODE,
                   static_cast<WPARAM>(static_cast<INT_PTR>(-1)), 0);
    CHECK(::SendMessageW(g_sci, SCI_ENCODEDFROMUTF8,
                         reinterpret_cast<WPARAM>("ABCD"), 0) == 4);

    // (b) 写入量随 IsUnicodeMode() 变：同一 need，Unicode 写 need 字节（**无 NUL**），
    //     非 Unicode 写 need+1（含 NUL）—— 一个静态桶表达不了。
    //     ⚠ "非 Unicode" 只能用**合法**代码页：ScintillaWin::ValidCodePage 只接受
    //       0 / 65001 / DBCS（ScintillaWin.cxx:2512），拿 1252 去设会被**静默忽略**，
    //       于是两边都留在 UTF-8 模式、断言看不出模式差（本批实测踩过）。
    //       0 表示"按系统默认页"，一定能进非 Unicode 分支。
    auto measure = [](int codepage, unsigned long* needOut) -> std::size_t {
        ::SendMessageW(g_sci, SCI_SETCODEPAGE, static_cast<WPARAM>(codepage), 0);
        char buf[8];
        std::memset(buf, 0x5A, sizeof(buf));
        *needOut = static_cast<unsigned long>(
            ::SendMessageW(g_sci, SCI_ENCODEDFROMUTF8,
                           reinterpret_cast<WPARAM>("A"), 0));
        ::SendMessageW(g_sci, SCI_ENCODEDFROMUTF8, reinterpret_cast<WPARAM>("A"),
                       reinterpret_cast<LPARAM>(buf));
        std::size_t n = 0;
        while (n < sizeof(buf) && static_cast<unsigned char>(buf[n]) != 0x5A) ++n;
        return n;
    };
    unsigned long needUnicode = 0, needAnsi = 0;
    const std::size_t writtenUnicode = measure(65001, &needUnicode);   // UTF-8
    const std::size_t writtenAnsi = measure(0, &needAnsi);             // 默认页
    CHECK(needUnicode == 1 && needAnsi == 1);   // ★ probe 一样
    CHECK(writtenUnicode == needUnicode);       // Unicode：写 need 字节、**不写 NUL**
    CHECK(writtenAnsi == needAnsi + 1);         // 非 Unicode：写 need+1（含 NUL）
    CHECK(writtenUnicode != writtenAnsi);
    // 同一句"copied = need + 1"在两种模式下必有一错 ⇒ 静态桶不成立
    CHECK(oop::SciOutBytesToCopy(oop::SciOutKind::kNul, needUnicode, 0) !=
          static_cast<unsigned long>(writtenUnicode));
    ::SendMessageW(g_sci, SCI_SETCODEPAGE, 65001, 0);

    // 桥接侧：必须拒答
    {
        std::vector<unsigned char> w = MakeWire(SCI_ENCODEDFROMUTF8, "A", true);
        const WireRun r = CallWire(w, static_cast<DWORD>(w.size()));
        CHECK(!r.ok && r.copied == 0 && r.bytes.empty());
    }
    // 两条拒答桶都不可桥接（代理侧据此在**中转窗**就拒掉，不做跨进程往返）
    CHECK(!oop::SciOutRelayable(SCI_GETREPRESENTATION));
    CHECK(!oop::SciOutRelayable(SCI_ENCODEDFROMUTF8));
    std::printf("  [8] 拒答桶 2 条：GetRepresentation 两种 0 / EncodedFromUTF8 "
                "写入量随模式变 —— 当场演示\n");
}

// [9] ★ 查长度路径：**带串的探长度**也要通（插件拿不到容量的唯一出路）
// 少了这条，本族的"探长度"会被拒 ⇒ 插件永远只能盲猜缓冲大小。
static void TestProbePath() {
    if (!g_sci) return;
    static const unsigned kMsgs[] = { SCI_GETPROPERTY, SCI_GETPROPERTYEXPANDED,
                                      SCI_DESCRIBEPROPERTY };
    int n = 0;
    for (unsigned m : kMsgs) {
        std::vector<unsigned char> w = MakeWire(m, "k1", false);
        const WireRun p = CallWire(w, static_cast<DWORD>(w.size()));
        if (!p.ok || p.copied != 0 || !p.bytes.empty()) {
            ++g_fail;
            std::printf("FAIL %u 的带串查长度路径被拒 / 写了字节\n", m);
            continue;
        }
        // 与写模式对照：need 必须一致（同一份协议、同一个 wp）
        std::vector<unsigned char> w2 = MakeWire(m, "k1", true);
        const WireRun r = CallWire(w2, static_cast<DWORD>(w2.size()));
        if (!r.ok || r.need != p.need) {
            ++g_fail;
            std::printf("FAIL %u：探长度 need=%lu（ok=%d），写模式 need=%lu（ok=%d）\n",
                        m, p.need, p.ok ? 1 : 0, r.need, r.ok ? 1 : 0);
            continue;
        }
        ++n;
    }
    CHECK(n == static_cast<int>(sizeof(kMsgs) / sizeof(kMsgs[0])));
    // 拒答桶的探长度也必须拒（否则插件会拿到一个"看起来可用"的 need）
    {
        std::vector<unsigned char> w = MakeWire(SCI_GETREPRESENTATION, "a", false);
        CHECK(!CallWire(w, static_cast<DWORD>(w.size())).ok);
    }
    std::printf("  [9] 带串查长度路径 %d 条：只回 need、不写任何字节\n", n);
}

int main() {
    std::printf("== test_sci_inout (batch 115) ==\n");
    for (int i = 0; i < 200; ++i) g_longKey[i] = 'L';
    g_longKey[200] = '\0';

    TestScale();
    TestOrder();
    TestLookupAgreement();
    TestNeedsInStr();
    TestShapeAgreement();
    if (MakeScintilla() && AttachNullLexer()) {
        TestWireContract();
        TestRealScintillaNul();
        TestRefuseRuntimeProof();
        TestProbePath();
    }
    std::printf("== %s (%d failure%s) ==\n", g_fail ? "FAILED" : "ALL PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    std::printf("NOTE: 本守卫证明表内部自洽 + 与形状表一致 + wire 契约完备 + "
                "对**真 Scintilla** 成立；\n"
                "      表 ↔ Scintilla.iface 的名字映射由 scripts/gen-sci-marshal.py "
                "--check 负责。两条都必须跑。\n");
    if (g_sci) ::DestroyWindow(g_sci);
    return g_fail ? 1 : 0;
}
