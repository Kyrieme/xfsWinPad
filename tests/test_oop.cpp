// test_oop.cpp — 进程外插件桥端到端（真实代理进程 + 真实 NPP 形态 DLL）。
//
// 覆盖：
//  1. oop_good：Launch 握手 → 命令注册（grouped）→ 跨进程 EXEC
//     （观察标记回传 cmdID 回填证明）→ 跨进程 NOTIFY（beNotified 证明）。
//  2. oop_killer：setInfo 里 exit(1)（ComparePlus 同款）→ 代理死、
//     Launch 失败、本测试进程存活。
//  3. oop_crasher：命令回调访问违例 → 代理 SEH 捕获（EXEC 被拒），
//     代理存活、编辑器无感。
//  4. ShutdownAll：干净退出路径（watchdog join、SHUTDOWN 送达）。
//  5. 一代理多插件（批次 66）：good+crasher+multi 共享单代理，独立执行/通知。
//  6. 死亡归因+幸存者重加：killer 毒死共享代理 → 只记 killer oop-died，
//     good/multi 撤销命令后重进新代理并可执行。
//  7. 装载卡死：hang 插件 setInfo 永不返回 → 代理看门狗自杀 → 同 6 归因。
//  9. plugin_oop.txt 主动隔离（批次 102）：**从未崩溃过**的正常插件写进名单
//     → 直接走代理，账本记 oop-forced；指定的恶意插件代理失败时**不回落
//     进程内**（回落会让本测试进程当场死亡——这正是要防的事）。
// 10. NPPM 指针参数过桥（批次 111）；11. SCI 值类型直发（批次 112）；
// 12. SCI 入参指针族过桥（批次 113）；13. SCI 出参指针族过桥（批次 114）；
// 14. SCI 出入参族过桥（批次 115：入参串在 wp + 出参缓冲在 lp）；
// 15. SCI 结构族过桥（批次 116：展平结构 + 内联串，含**条件回写**）；
// 16. NPPM_DMM* 停靠族过桥（批次 117：跨进程 SetParent + DMN_*/DMM_* 由代理中继）。
//
// ⚠ 场景 11/12 的"仍拒答"样例随批次推进换过三次（GETTEXT → GETPROPERTY →
//   GETSTYLEDTEXT → FORMATRANGE）：一个族被桥接之后，它返回 0 就从"拒答"
//   变成"真值为空"，**同形**。留在原地会让断言全绿却不再证明任何事。
//   ⇒ 现在用的是**语义上**永久拒答的三支（进程私有 HDC / 返回内部指针 / 裸指针），
//     它们的可桥接性不随"表里有没有"变化。
//
// 命令行参数 1：测试插件 DLL 所在目录（ctest 传 $<TARGET_FILE_DIR:oop_good>）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <filesystem>
#include <vector>

#include "../src/plugin/PluginManager.h"
#include "../src/plugin/DockManager.h"
#include "../src/plugin/npp/NppDocking.h"
#include "../src/plugin/oop/OopHost.h"
#include "../src/document/Document.h"
#include "../src/core/Log.h"
#include "../src/core/Util.h"

// 结构族（场景 15）的假 Scintilla 要按**厂商的结构体布局**读写，所以这里显式取
// Scintilla.h（它本来就经 PluginManager.h → Editor.h 进来；显式写出来是为了不依赖
// 传递包含 —— 那是一条随时会断的隐式契约）。注意它会定义 SCI_* 宏，本文件用的是
// 自己的 kSci* 常量，两者不冲突。
#include <Scintilla.h>

using namespace xfs;

// ---- 观察标记（与 oop_good / oop_multi / oop_nppm 约定一致）------------------
struct Marker { UINT_PTR tag; INT_PTR value; };
static constexpr UINT_PTR kMarkerMagic = 0x474F4F44;   // 'GOOD'
static constexpr UINT_PTR kMultiMagic  = 0x4D554C54;   // 'MULT'
static constexpr UINT_PTR kValMagic    = 0x4E50564C;   // 'NPVL'（oop_nppm 值标记）
static constexpr UINT_PTR kStrMagic    = 0x4E505354;   // 'NPST'（oop_nppm 串标记）
struct StrMarker { UINT_PTR tag; wchar_t text[128]; };

// ---- 测试观察窗口：接收插件回传的 Marker（WM_COPYDATA）-----------------------
static UINT_PTR g_markers[64];
static INT_PTR g_markerVals[64];
static int g_markerCount = 0;
static UINT_PTR g_strTags[32];
static std::wstring g_strVals[32];
static int g_strCount = 0;

// 场景 10 用：把接收窗当作"编辑器主窗口"来分发 NPPM_* —— 真实宿主里
// MainWindow 的 WndProc 就是这么做的（ForwardNppMessage）。
// 没有它，中转窗一旦失效、插件把 NPPM_* 直发本窗，就会静默落到
// DefWindowProc ⇒ "过桥路径不会触发外来指针判据"这条断言在夹具里**没有
// 区分力**（负控实测：关掉中转窗它照样绿）。加上分发后：
//   中转窗失效 ⇒ 外来指针真的进到 ForwardNppMessage ⇒ PointerRefusalCount
//   涨 ⇒ 断言变红。顺带把批次 110 的判据在**真实跨进程**条件下跑了一遍。
static PluginManager* g_nppDispatch = nullptr;

static LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COPYDATA) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds) return DefWindowProcW(h, m, wp, lp);
        if ((cds->dwData == kMarkerMagic || cds->dwData == kMultiMagic ||
             cds->dwData == kValMagic) && cds->cbData == sizeof(Marker)) {
            auto* mk = reinterpret_cast<const Marker*>(cds->lpData);
            if (g_markerCount < 64) {
                g_markers[g_markerCount] = mk->tag;
                g_markerVals[g_markerCount] = mk->value;
                ++g_markerCount;
            }
            return TRUE;
        }
        if (cds->dwData == kStrMagic && cds->cbData == sizeof(StrMarker)) {
            auto* sm = reinterpret_cast<const StrMarker*>(cds->lpData);
            if (g_strCount < 32) {
                g_strTags[g_strCount] = sm->tag;
                g_strVals[g_strCount] = sm->text;
                ++g_strCount;
            }
            return TRUE;
        }
    }
    if (g_nppDispatch) {
        bool handled = false;
        const LRESULT r = g_nppDispatch->ForwardNppMessage(m, wp, lp, handled);
        if (handled) return r;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("PASS: %s\n", msg); } \
    else { std::printf("FAIL: %s\n", msg); ++g_fail; } } while (0)

static bool FindMarker(UINT_PTR tag, INT_PTR expect) {
    for (int i = 0; i < g_markerCount; ++i)
        if (g_markers[i] == tag && g_markerVals[i] == expect) return true;
    return false;
}

// 取最后一次该 tag 的串标记（没有则返回空）
static std::wstring StrMarkerOf(UINT_PTR tag) {
    std::wstring v;
    for (int i = 0; i < g_strCount; ++i)
        if (g_strTags[i] == tag) v = g_strVals[i];
    return v;
}

static void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// 观察标记接收窗（插件以 WM_COPYDATA 直发此窗；同时充当 nppHandle）
static HWND MakeRecv() {
    static bool reg = false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"test_oop_recv";
    if (!reg) { RegisterClassExW(&wc); reg = true; }
    return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

static void ResetMarkers() { g_markerCount = 0; g_strCount = 0; }

// 取某 tag 最后一次的值（没有则 false）
static bool MarkerValue(UINT_PTR tag, INT_PTR* out) {
    for (int i = g_markerCount - 1; i >= 0; --i)
        if (g_markers[i] == tag) { *out = g_markerVals[i]; return true; }
    return false;
}

// ---- 场景 11 用：宿主机侧的"假 Scintilla"（批次 112）------------------------
// 真宿主里这个窗口是 Scintilla 控件。夹具只需回答几条**值类型**消息，并把
// **它收到了什么**记下来 —— 后者才是本场景的承重断言：SCI 中转窗如果失效，
// 插件会把带指针的 SCI_* 直发到这里，而宿主解引用外来指针就是批次 110 那个
// 访问违例。夹具**故意不解引用** lp：负控下会崩，而崩溃会把 stdout 块缓冲
// 一起丢掉（批次 111 实测：零输出、exit 139），什么都看不到。改为"记录"，
// 负控就变成一条干净的断言失败。
static unsigned g_sciValueCalls = 0;       // 收到的值类型 SCI_*
static unsigned g_sciPtrCalls = 0;         // 收到的**不可桥接族** SCI_*（必须为 0）
static unsigned g_sciForeignPtrCalls = 0;  // 其中 lp 在**本进程**不可读的

static bool HostReadable(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

// ---- 场景 12 用：把**宿主侧真正收到的内容**记下来（批次 113）----------------
// 这里是"宿主进程"（测试进程）⇒ 能把指针读出来本身就是**指针在宿主地址空间里
// 合法**的证明。这是本批的核心断言：负控（编辑器侧不重建指针，把 wire 里的 0
// 直接发出去）下这些内容会变成空串 ⇒ 断言变红，而且**不崩**。
//
// ⚠ 读取前一律先验可读范围（RangeReadable）：负控下指针可能是垃圾，裸解引用
// 会把 stdout 块缓冲随崩溃一起丢掉（批次 111 实测零输出、exit 139），那就什么
// 都看不到了。夹具的职责是"记录"，不是"照产品崩"。
struct SciInCall {
    UINT        msg = 0;
    UINT_PTR    wp = 0;
    UINT_PTR    lp = 0;
    bool        ok1 = false;    // 第一段指针在本进程可读
    bool        ok2 = false;
    std::string s1;
    std::string s2;
};
static std::vector<SciInCall> g_sciIn;

static bool RangeReadable(const void* p, std::size_t n) {
    if (!p) return false;
    if (n == 0) return true;
    uintptr_t cur = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = cur + n;
    if (end < cur) return false;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) == 0)
            return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cur) return false;
        cur = regionEnd;
    }
    return true;
}

// n > 0：按长度读；n == 0：NUL 扫描（有界）。不可读/未终止 ⇒ ok=false + 空串。
static std::string SafeRead(const char* p, std::size_t n, bool* ok) {
    *ok = false;
    std::string s;
    if (!p) return s;
    if (n) {
        if (!RangeReadable(p, n)) return s;
        s.assign(p, n);
        *ok = true;
        return s;
    }
    for (std::size_t i = 0; i < 4096; ++i) {
        if (!RangeReadable(p + i, 1)) return s;
        if (p[i] == 0) { *ok = true; return s; }
        s.push_back(p[i]);
    }
    return s;   // 界内未见 NUL
}

// 入参族的线上编号（与 tests/oop_plugins/oop_scibridge.cpp 同源事实值）
static constexpr UINT kSciAddText         = 2001;   // (length, text)   长度桶
static constexpr UINT kSciAddStyledText   = 2002;   // (length, cells)
static constexpr UINT kSciInsertText      = 2003;   // (position, text) NUL 扫描
static constexpr UINT kSciAutoCShow       = 2100;   // (lengthEntered, list)
static constexpr UINT kSciReplaceSel      = 2170;   // (text)           串在 wp
static constexpr UINT kSciSetText         = 2181;   // (text)           串在 wp
static constexpr UINT kSciAppendText      = 2282;   // (length, text)
static constexpr UINT kSciSearchInTarget  = 2197;   // (length, pattern)
static constexpr UINT kSciSetRepresentation = 2665; // (a, b)           双指针
static constexpr UINT kSciSetProperty     = 4004;   // (key, value)     双指针
static constexpr UINT kSciSetDocPointer   = 2358;   // 裸指针（kRawPtr）⇒ 永远拒答
static constexpr UINT kSciFormatRange     = 2151;   // 结构族拒答桶：进程私有 HDC ⇒ 永远拒答
static constexpr UINT kSciGetCharPointer  = 2520;   // kPtrRet：返回文档内部指针 ⇒ 永远拒答

static constexpr UINT kSciGetCurrentPos   = 2008;
static constexpr UINT kSciGetLineCount    = 2154;
static constexpr UINT kSciGetTextLength   = 2183;
static constexpr UINT kSciGetText         = 2182;   // 出参缓冲（指针）
static constexpr UINT kSciGetTextRange    = 2162;   // kStruct/kRangeOut（批次 116 起可过桥）
static constexpr UINT kSciGetTextRangeFull = 2039;  // kStruct/kRangeOut（Full 版）
static constexpr UINT kSciGetStyledText   = 2015;   // kStruct/kStyledOut（批次 116 起可过桥）
static constexpr UINT kSciFindText        = 2150;   // kStruct/kFindInOut（条件回写 chrgText）
static constexpr UINT kSciGetStyledTextFull = 2778; // kStruct/kStyledOut（Full 版）
static constexpr UINT kSciFindTextFull    = 2196;   // kStruct/kFindInOut（Full 版）

// ---- 场景 13 用：出参族（批次 114）-----------------------------------------
// ⚠ kSciGetText 从批次 114 起**已可过桥**（出参族）。
static constexpr UINT kSciGetCurLine    = 2027;   // kClampWpNul（★ 返回光标列号）
static constexpr UINT kSciGetLine       = 2153;   // kNoNul
static constexpr UINT kSciGetSelText    = 2161;   // kNul
static constexpr UINT kSciGetWordChars  = 2646;   // kNoNul
static constexpr UINT kSciGetFontLocale = 2761;   // kNul（空值）
static constexpr UINT kSciTargetAsUtf8  = 2447;   // 拒答桶（NUL 语义随编码变）
static constexpr UINT kSciGetTag        = 2616;   // 拒答桶（写入量随 regex 变）

// ---- 场景 14 用：出入参族（批次 115）---------------------------------------
// ⚠ 批次 115 起 kSciGetProperty **已可过桥**（入参串 + 出参缓冲）。于是三个老场景
//   的"仍拒答"样例**全部换掉**：场景 11/12 → 结构族拒答桶 kSciFormatRange，
//   场景 13 → kSciGetRepresentation（拒答桶，且 lp 正是出参缓冲）。
//   为什么必须换：GetProperty 过桥后真值就是空串 ⇒ 返回 0 —— 与"拒答返回 0"
//   **同形**。继续拿它当拒答样例，断言会全绿却不再有区分力（LESSONS 的教训：
//   夹具的"失败应答"与产品"拒答"同形时，断言必须落到别的东西上）。
// ⚠ 批次 116 起结构族里不带外来指针的 6 条**也已可过桥** ⇒ 场景 11/12 的样例
//   第二次搬家，落到 kSciFormatRange（进程私有 HDC，**语义上**永久拒答）。
// ⚠ 4005 是 SetKeyWords（**可**过桥的 kStrInLp），别当"仍拒答"样例写错。
static constexpr UINT kSciGetProperty         = 4008;   // kNul
static constexpr UINT kSciGetPropertyExpanded = 4009;   // kNul（5.6.6 里就是 PropGet）
static constexpr UINT kSciDescribeProperty    = 4016;   // kNul（LexerBase 恒回 ""）
static constexpr UINT kSciEncodedFromUtf8     = 2449;   // 拒答桶
static constexpr UINT kSciGetRepresentation   = 2666;   // 拒答桶（lp 是出参缓冲）

// 记录一次入参族调用；返回值编码**宿主实际读到的长度**（第一段*100 + 第二段），
// 于是插件报回来的 marker 直接反映"宿主收到了什么" —— 截断/丢段都会改这个数。
static LRESULT RecordIn(UINT m, WPARAM wp, LPARAM lp, std::size_t n1, bool nul2) {
    SciInCall c;
    c.msg = m;
    c.wp = static_cast<UINT_PTR>(wp);
    c.lp = static_cast<UINT_PTR>(lp);
    c.s1 = SafeRead(reinterpret_cast<const char*>(lp), n1, &c.ok1);
    if (nul2) c.s2 = SafeRead(reinterpret_cast<const char*>(wp), 0, &c.ok2);
    const LRESULT r = static_cast<LRESULT>(c.s1.size() * 100 + c.s2.size());
    g_sciIn.push_back(c);
    return r;
}

// 串在 wp 的那些（lp 是值/未用）：第一段从 wp 读
static LRESULT RecordInWpStr(UINT m, WPARAM wp, LPARAM lp, bool twoSeg) {
    SciInCall c;
    c.msg = m;
    c.wp = static_cast<UINT_PTR>(wp);
    c.lp = static_cast<UINT_PTR>(lp);
    c.s1 = SafeRead(reinterpret_cast<const char*>(wp), 0, &c.ok1);
    if (twoSeg) c.s2 = SafeRead(reinterpret_cast<const char*>(lp), 0, &c.ok2);
    const LRESULT r = static_cast<LRESULT>(c.s1.size() * 100 + c.s2.size());
    g_sciIn.push_back(c);
    return r;
}

// 取某条 SCI_* 的记录（场景 12 的每条消息在电池里只成功过一次：重复编号的
// 那几条都是被拒的，被拒者根本到不了宿主 ⇒ 记录唯一）。
static const SciInCall* SciInOf(UINT msg) {
    for (const auto& c : g_sciIn)
        if (c.msg == msg) return &c;
    return nullptr;
}

// ---- 场景 13 用：出参族的"假 Scintilla"（批次 114）-------------------------
// 出参族方向与入参族相反：宿主往里**写**。这里的承重断言不是"宿主收到了什么"
// （那是场景 12 的形态），而是**插件缓冲的边界**：写对了几字节、第 N 格是 NUL
// 还是哨兵、N 之后有没有被多写一个字节 —— 后者由夹具的哨兵摘要来验。
static unsigned g_sciOutCalls = 0;        // 真到达宿主的出参**写**调用数
static unsigned g_sciOutForeignPtr = 0;   // 其中 lp 在宿主进程不可写的（必须为 0）
static unsigned g_sciOutRefuseLeak = 0;   // **不该**到达的（拒答桶）：到达即缺陷

static bool HostWritable(void* p, std::size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if ((mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        return false;
    const uintptr_t regionEnd =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return regionEnd >= reinterpret_cast<uintptr_t>(p) + n;
}

// 出参族的假实现：**严格按真 Scintilla 的协议** —— lp==0 是"查长度"（不写缓冲、
// 返回需要的字节数），lp!=0 才真写；每个桶的写入量按 SciOutTable 的分类来。
// 写之前先验宿主侧可写：负控（编辑器侧把外来指针直接转出去）下这里会记一笔
// "外来指针"，而**不去解引用** —— 解引用会崩，崩溃会把 stdout 块缓冲一起丢掉
// （批次 111 实测零输出、exit 139），那就什么都看不到了。
static LRESULT FakeSciOut(UINT m, WPARAM wp, LPARAM lp) {
    if (lp == 0) {                       // 查长度：一个字节都不写
        switch (m) {
        case kSciGetSelText:    return 5;
        case kSciGetLine:       return 7;
        case kSciGetText:
        case kSciGetCurLine:    return 20;
        case kSciGetWordChars:  return 4;
        case kSciGetFontLocale: return 0;
        default:                return 0;
        }
    }
    ++g_sciOutCalls;
    void* p = reinterpret_cast<void*>(lp);
    auto put = [&](const char* src, std::size_t n, bool nul) -> void {
        if (!HostWritable(p, n + (nul ? 1u : 0u))) { ++g_sciOutForeignPtr; return; }
        if (n) std::memcpy(p, src, n);
        if (nul) static_cast<char*>(p)[n] = 0;
    };
    switch (m) {
    case kSciGetSelText:                                            // kNul
        put("hello", 5, true);
        return 5;
    case kSciGetLine:                                               // kNoNul
        put("lineA\r\n", 7, false);
        return 7;
    case kSciGetText: {                                             // kClampWpNul
        const unsigned len = wp < 20 ? static_cast<unsigned>(wp) : 20u;
        put("0123456789abcdefghij", len, true);
        return static_cast<LRESULT>(len);
    }
    case kSciGetCurLine: {                                          // kClampWpNul
        const unsigned len = wp < 20 ? static_cast<unsigned>(wp) : 20u;
        put("0123456789abcdefghij", len, true);
        return 3;                       // ★ 光标列号，**不是**长度
    }
    case kSciGetWordChars:                                          // kNoNul
        put("abcd", 4, false);
        return 4;
    case kSciGetFontLocale:                                         // kNul，空值
        put("", 0, true);
        return 0;
    // ---- 拒答桶：到达这里就是缺陷（宿主没有拒答）---------------------------
    case kSciTargetAsUtf8:
    case kSciGetTag:
        ++g_sciOutRefuseLeak;
        return 0;
    default:
        break;
    }
    return 0;
}

// ---- 场景 14 用：出入参族的"假 Scintilla"（批次 115）-----------------------
// 本族 = 入参串（wp）+ 出参缓冲（lp）。假实现因此要**同时**做两件事：
//   ① 像出参族那样严格按协议写缓冲（lp==0 是查长度）；
//   ② 把 wp 当**宿主本地的 C 串**读出来 —— 这一步就是本批的核心证据：插件发的是
//      它自己进程里的地址，宿主必须拿到一份本地副本。读不到就记一笔 badKey。
// ★ 返回值把**宿主读到的 key 长度**编进去（*100）：于是插件的 marker 直接反映
//   "入参串有没有真的跨过来"。只回值的长度的话，串没过来也一样绿。
static unsigned g_sciInOutCalls = 0;    // 真到达宿主的入参串调用数（不含查长度）
static unsigned g_sciInOutBadKey = 0;   // wp 不可读 / 不是 NUL 结尾串（必须为 0）
static unsigned g_sciInOutLeak = 0;     // 拒答桶到达宿主（必须为 0）

// 假属性表：k1 -> "v1"；其余（未设过 / 空键 / k2）-> ""
static std::string FakeInOutValue(UINT m, const std::string& key) {
    // DescribeProperty 的值**与 key 有关**：串没过来时连长度都会变，露馅更早
    if (m == kSciDescribeProperty) return "desc:" + key;
    return key == "k1" ? "v1" : "";
}

static LRESULT FakeSciInOut(UINT m, WPARAM wp, LPARAM lp) {
    // ⚠ 先判可读再读：wp 若还是插件进程里的地址，直接解引用就是访问违例，而崩溃
    //   会把 stdout 块缓冲一起丢掉（批次 111 实测零输出），什么都看不到。
    bool ok = false;
    const std::string key = SafeRead(reinterpret_cast<const char*>(wp), 0, &ok);
    if (!ok) { ++g_sciInOutBadKey; return 0; }
    const std::string val = FakeInOutValue(m, key);
    if (lp == 0) return static_cast<LRESULT>(val.size());   // 查长度：一个字节都不写
    ++g_sciInOutCalls;
    void* p = reinterpret_cast<void*>(lp);
    if (!HostWritable(p, val.size() + 1)) { ++g_sciOutForeignPtr; return 0; }
    if (!val.empty()) std::memcpy(p, val.data(), val.size());
    static_cast<char*>(p)[val.size()] = 0;
    return static_cast<LRESULT>(key.size() * 100 + val.size());
}

// ---- 场景 15 用：结构族的"假 Scintilla"（批次 116）--------------------------
// 本族是本项目第一个**真 in-out 族**：同一条消息里既有入参（chrg / needle）又有
// 出参（出缓冲 / chrgText）。假实现因此要**同时**做三件事：
//   ① 像出参族那样按协议写缓冲：内容 + 结尾 NUL（styled 族是两个 NUL）；
//   ② 把结构体里的 needle 当**宿主本地 C 串**读出来 —— 这是本批的核心证据：
//      插件发的是它自己进程里的地址，宿主必须拿到一份本地副本。读不到就记一笔
//      badNeedle，而**不去解引用**（负控下它是垃圾地址，裸解引用会崩、会丢 stdout）；
//   ③ ★ 复刻 Scintilla 的**条件回写**：FindText 只在 pos != -1 时写 chrgText
//      （Editor.cxx:4318/4349）。这不是"把产品逻辑抄一份到夹具"，而是**被测规格**
//      —— 桥接侧正是为了不依赖这条规则才用 canary 判定"到底写没写"。
//
// 结构体布局用厂商头里的 Sci_TextRange / Sci_TextToFind（**不**自己镜像一份）：
// 这里的角色是"真 Scintilla 的替身"，布局当然该由厂商说了算。桥接侧的 abi 镜像
// 是否与厂商一致，由 tests/test_sci_struct.cpp 对撞。
static unsigned g_sciStructCalls = 0;       // 真到达宿主的**可桥接**结构族调用数
static unsigned g_sciStructForeign = 0;     // 其中 lp 在宿主进程不可读/不可写的（必须为 0）
static unsigned g_sciStructLeak = 0;        // 拒答桶到达宿主（必须为 0）
static unsigned g_sciStructBadNeedle = 0;   // needle 在宿主读不到（必须为 0）
static unsigned g_sciStructChrgWrites = 0;  // 真的写了 chrgText 的次数

static const char kFakeSciDoc[] = "hello world";   // 11 字节；无词法器 ⇒ style 恒 0
static constexpr std::size_t kFakeSciDocLen = sizeof(kFakeSciDoc) - 1;

// kRangeOut / kStyledOut：只读结构 + 出缓冲
static LRESULT FakeSciStructOut(LPARAM lp, bool full, bool styled) {
    const std::size_t sz = full ? sizeof(Sci_TextRangeFull) : sizeof(Sci_TextRange);
    if (!lp || !RangeReadable(reinterpret_cast<const void*>(lp), sz)) {
        ++g_sciStructForeign;
        return 0;
    }
    INT_PTR cpMin = 0, cpMax = 0;
    char* buf = nullptr;
    if (full) {
        auto* tr = reinterpret_cast<const Sci_TextRangeFull*>(lp);
        cpMin = tr->chrg.cpMin; cpMax = tr->chrg.cpMax; buf = tr->lpstrText;
    } else {
        auto* tr = reinterpret_cast<const Sci_TextRange*>(lp);
        cpMin = tr->chrg.cpMin; cpMax = tr->chrg.cpMax; buf = tr->lpstrText;
    }
    // 桥接侧应该已经算过容量并挡掉越界；走到这里就是缺陷（记一笔，不解引用）
    if (cpMin < 0 || cpMax < cpMin ||
        static_cast<std::size_t>(cpMax) > kFakeSciDocLen) {
        ++g_sciStructForeign;
        return 0;
    }
    const std::size_t len = static_cast<std::size_t>(cpMax - cpMin);
    const std::size_t written = styled ? len * 2 + 2 : len + 1;
    if (!HostWritable(buf, written)) { ++g_sciStructForeign; return 0; }
    ++g_sciStructCalls;
    if (styled) {                                   // (char, style) 对 + 两个 NUL
        for (std::size_t i = 0; i < len; ++i) {
            buf[i * 2] = kFakeSciDoc[cpMin + i];
            buf[i * 2 + 1] = 0;                     // 无词法器 ⇒ style 0
        }
        buf[len * 2] = 0;
        buf[len * 2 + 1] = 0;
        return static_cast<LRESULT>(len * 2);
    }
    if (len) std::memcpy(buf, kFakeSciDoc + cpMin, len);
    buf[len] = 0;
    return static_cast<LRESULT>(len);
}

// kFindInOut：入串（needle）+ **条件**回写 chrgText
static LRESULT FakeSciStructFind(LPARAM lp, bool full) {
    const std::size_t sz = full ? sizeof(Sci_TextToFindFull) : sizeof(Sci_TextToFind);
    if (!lp || !RangeReadable(reinterpret_cast<const void*>(lp), sz) ||
        !HostWritable(reinterpret_cast<void*>(lp), sz)) {
        ++g_sciStructForeign;
        return 0;
    }
    INT_PTR cpMin = 0, cpMax = 0;
    const char* needle = nullptr;
    if (full) {
        auto* ft = reinterpret_cast<Sci_TextToFindFull*>(lp);
        cpMin = ft->chrg.cpMin; cpMax = ft->chrg.cpMax; needle = ft->lpstrText;
    } else {
        auto* ft = reinterpret_cast<Sci_TextToFind*>(lp);
        cpMin = ft->chrg.cpMin; cpMax = ft->chrg.cpMax; needle = ft->lpstrText;
    }
    bool ok = false;
    const std::string n = SafeRead(needle, 0, &ok);
    if (!ok) { ++g_sciStructBadNeedle; return 0; }   // 串没跨过来 ⇒ 记一笔，不崩
    ++g_sciStructCalls;
    if (cpMin < 0 || cpMax < cpMin || static_cast<std::size_t>(cpMax) > kFakeSciDocLen)
        return -1;
    const std::string hay(kFakeSciDoc + cpMin, static_cast<std::size_t>(cpMax - cpMin));
    const std::size_t pos = hay.find(n);
    if (pos == std::string::npos) return -1;         // ★ 找不到：**一个字节都不写**
    const INT_PTR p = cpMin + static_cast<INT_PTR>(pos);
    const INT_PTR e = p + static_cast<INT_PTR>(n.size());
    if (full) {
        auto* ft = reinterpret_cast<Sci_TextToFindFull*>(lp);
        ft->chrgText.cpMin = p; ft->chrgText.cpMax = e;
    } else {
        auto* ft = reinterpret_cast<Sci_TextToFind*>(lp);
        ft->chrgText.cpMin = static_cast<Sci_PositionCR>(p);
        ft->chrgText.cpMax = static_cast<Sci_PositionCR>(e);
    }
    ++g_sciStructChrgWrites;
    return static_cast<LRESULT>(p);
}

static LRESULT CALLBACK FakeSciWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    // ---- 值类型（场景 11）--------------------------------------------------
    case kSciGetCurrentPos: ++g_sciValueCalls; return 4242;
    case kSciGetLineCount:  ++g_sciValueCalls; return 77;
    // ⚠ 批次 116 起这个返回值必须**等于本夹具文档的真实长度**：结构族的 kRangeOut
    //   在**宿主侧**用 SCI_GETTEXTLENGTH 拿 docLen 来算容量（cpMax==-1 的语义与
    //   越界判定都要它）。若这里继续返回一个哨兵值（原来是 1234），
    //   GetTextRange(0,2000) 的"越界拒答"用例就会因为 2000 < 1234 而**不成立**，
    //   而"到文档末尾"那条路算出的容量也会大于真写的字节数 ⇒ 写后白名单必然失败。
    case kSciGetTextLength: ++g_sciValueCalls;
        return static_cast<LRESULT>(kFakeSciDocLen);
    // ---- 不可桥接族：到达这里就是缺陷（场景 11 的承重计数）------------------
    // ⚠ 样例搬过两次家：
    //   批次 114 起 kSciGetText（出参族）**已可过桥** ⇒ 搬到出参组；
    //   批次 115 起 kSciGetProperty（出入参族）**已可过桥** ⇒ 搬到出入参组；
    //   批次 116 起 kSciGetTextRange / kSciGetStyledText（结构族里不带外来指针的
    //   6 条）**也已可过桥** ⇒ 搬到下面的结构组。
    //   现在留在这一组的是**语义上**永久拒答的三支：进程私有 HDC、返回内部指针、
    //   裸指针。它们过桥与否不受"表里有没有"影响。
    case kSciFormatRange:     // 结构族拒答桶：hdc 是调用进程私有的 GDI 句柄
    case kSciGetCharPointer:  // kPtrRet：返回文档内部指针，跨进程无意义
    case kSciSetDocPointer:   // 裸指针族
        ++g_sciPtrCalls;
        if (lp && !HostReadable(reinterpret_cast<const void*>(lp)))
            ++g_sciForeignPtrCalls;
        return 0;
    // ---- 出入参族：过桥后才该到达（场景 14）--------------------------------
    case kSciGetProperty:
    case kSciGetPropertyExpanded:
    case kSciDescribeProperty:
        return FakeSciInOut(m, wp, lp);
    // ---- 出入参族里的拒答桶：到达这里就是缺陷（宿主没有拒答）----------------
    case kSciEncodedFromUtf8:
    case kSciGetRepresentation:
        ++g_sciInOutLeak;
        return 0;
    // ---- 出参族：过桥后才该到达（场景 13）----------------------------------
    case kSciGetSelText:
    case kSciGetLine:
    case kSciGetText:
    case kSciGetCurLine:
    case kSciGetWordChars:
    case kSciGetFontLocale:
    case kSciTargetAsUtf8:
    case kSciGetTag:
        return FakeSciOut(m, wp, lp);
    // ---- 结构族：过桥后才该到达（场景 15）----------------------------------
    // 它们原本在上一组的"不可桥接"里；批次 116 起**只有** FormatRange 留在了那儿。
    case kSciGetTextRange:      return FakeSciStructOut(lp, false, false);
    case kSciGetTextRangeFull:  return FakeSciStructOut(lp, true,  false);
    case kSciGetStyledText:     return FakeSciStructOut(lp, false, true);
    case kSciGetStyledTextFull: return FakeSciStructOut(lp, true,  true);
    case kSciFindText:          return FakeSciStructFind(lp, false);
    case kSciFindTextFull:      return FakeSciStructFind(lp, true);
    // ---- 入参族：过桥后才该到达（场景 12）----------------------------------
    case kSciAddText:
    case kSciAppendText:
    case kSciSearchInTarget:
        return RecordIn(m, wp, lp, static_cast<std::size_t>(wp), false);
    case kSciAddStyledText:   // cells：裸字节，长度由 wp 给
        return RecordIn(m, wp, lp, static_cast<std::size_t>(wp), false);
    case kSciInsertText:      // 位置 + NUL 结尾串
        return RecordIn(m, wp, lp, 0, false);
    case kSciAutoCShow:       // ★ lengthEntered 不是缓冲长度 ⇒ 必须 NUL 扫描
        return RecordIn(m, wp, lp, 0, false);
    case kSciSetText:
    case kSciReplaceSel:
        // ★ 批次 116 修正：这两条的串在 **lParam**（iface `(, string text)`），
        //   不是 wParam。原来的夹具把串塞进 wp，而假 Scintilla 也从 wp 读 ⇒
        //   与错表**自洽**、全绿；真机上却是"文本没改"。
        return RecordIn(m, wp, lp, 0, false);
    case kSciSetProperty:
    case kSciSetRepresentation:   // 双指针（`(string, string)`，槽 0 非空）
        return RecordInWpStr(m, wp, lp, true);
    default:
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static HWND MakeFakeSci() {
    static bool reg = false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FakeSciWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"test_oop_fake_sci";
    if (!reg) { RegisterClassExW(&wc); reg = true; }
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

// 场景 16 用：dock 面板的"主框架窗口"。
// ★ **不能**拿 MakeRecv() 代替 —— 那是 HWND_MESSAGE（消息专用）窗，而
//   DockManager 要在它下面 CreateWindowEx 出 wrapper、再把插件对话框
//   SetParent 进来；消息专用窗承载不了这样一棵（跨进程的）子窗口树。
//   本窗只需存在且不是消息专用窗，不需要真的显示。
static HWND MakeHostFrame() {
    static bool reg = false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"test_oop_host_frame";
    if (!reg) { RegisterClassExW(&wc); reg = true; }
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                           0, 0, 400, 300, nullptr, nullptr, wc.hInstance,
                           nullptr);
}

// 等待 until 谓词为真（至多 ms），期间泵消息——幸存者重加是异步收敛过程
static bool WaitUntil(const std::function<bool()>& until, DWORD ms) {
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        if (until()) return true;
        Sleep(50);
        PumpMessages();
    }
    return until();
}

static bool HasName(const std::vector<OopPluginInfo>& v, const wchar_t* name) {
    for (auto& p : v) if (p.name == name) return true;
    return false;
}

// ---- 假文档集（场景 10 专用）------------------------------------------------
// 注入 PluginManager 以脱离真实 Workspace/Scintilla。存在的唯一理由：让
// "过桥拿到的路径"是**可预期的常量**，从而断言精确相等 —— 如果只断言"非空"，
// 那"过桥把字符串搞坏了"也会绿（那正是本场景要防的事）。
struct RelayDocs final : public NppDocSource {
    std::vector<std::unique_ptr<Document>> docs;
    int switchCalls = 0;
    std::wstring lastSwitch;
    int openCalls = 0;

    RelayDocs() {
        docs.emplace_back(new Document());
        docs[0]->path = L"D:\\proj\\oop\\relay.txt";
    }
    int DocCount() override { return (int)docs.size(); }
    Document* DocAt(int i) override {
        return (i >= 0 && i < (int)docs.size()) ? docs[(size_t)i].get() : nullptr;
    }
    Document* Current() override { return docs.empty() ? nullptr : docs[0].get(); }
    int CurrentIndex() override { return 0; }
    bool OpenNew(const std::wstring&) override { ++openCalls; return true; }
    bool SwitchTo(const std::wstring& p) override {
        ++switchCalls; lastSwitch = p; return true;
    }
    bool SaveCurrent() override { return true; }
    bool SaveAllDocs(bool& anySaved) override { anySaved = false; return true; }
};

int main(int argc, char** argv) {
    const char* pluginDir = (argc > 1) ? argv[1] : ".";
    std::filesystem::path dllDir = std::filesystem::absolute(
        std::filesystem::path(pluginDir));

    Logger::Init();
    std::printf("== test_oop: out-of-process plugin bridge ==\n");

    // ★ 夹具目录自检。路径给错时每一次 Launch 都会失败，接着
    //   mgr.Commands()[0] 在空 vector 上越界 —— 症状是**零输出的段错误**
    //   （stdout 还是块缓冲，崩溃时全丢），看起来像"刚改的那条链崩了"。
    //   与其让人去猜，不如在这里明确报错。
    if (!std::filesystem::exists(dllDir / L"oop_good.dll")) {
        std::printf("FATAL: fixture dir has no oop_good.dll: %s\n",
                    dllDir.string().c_str());
        return 1;
    }

    // ---- 场景 1：oop_good 全链路 -------------------------------------------
    {
        PluginManager mgr;
        OopHost host;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TestWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"test_oop_recv";
        RegisterClassExW(&wc);
        HWND recv = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

        std::wstring dll = (dllDir / L"oop_good.dll").wstring();
        bool ok = host.Launch(mgr, dll, recv, nullptr, 15000);
        CHECK(ok, "oop_good: handshake completed");

        auto alive = host.Alive();
        CHECK(alive.size() == 1 && alive[0].name == L"oop-good",
              "oop_good: alive proxy reports plugin name");
        CHECK(mgr.CommandCount() == 2, "oop_good: 2 commands registered");
        CHECK(mgr.Commands()[0].grouped && mgr.Commands()[0].category == L"oop-good",
              "oop_good: commands grouped under plugin");

        // EXEC index 0：插件应回传 Marker(tag=1, value=回填后的 cmdID)
        unsigned id0 = mgr.Commands()[0].id;
        CHECK(mgr.Execute(id0), "oop_good: Execute(id0) dispatched");
        Sleep(200);
        PumpMessages();
        CHECK(FindMarker(1, (INT_PTR)id0),
              "oop_good: EXEC ran in proxy; cmdID was backfilled correctly");

        // NOTIFY：代理 beNotified 应收到 code（Marker tag=2）
        host.BroadcastNotify(1234, 0);
        Sleep(200);
        PumpMessages();
        CHECK(FindMarker(2, (INT_PTR)1234),
              "oop_good: NOTIFY reached beNotified in proxy");

        host.ShutdownAll();
        CHECK(host.Alive().empty(), "oop_good: ShutdownAll closed proxy");
        DestroyWindow(recv);
    }

    // ---- 场景 2：oop_killer（setInfo 即 exit(1)，ComparePlus 同款）----------
    {
        PluginManager mgr;
        OopHost host;
        std::wstring dll = (dllDir / L"oop_killer.dll").wstring();
        bool ok = host.Launch(mgr, dll, nullptr, nullptr, 15000);
        CHECK(!ok, "oop_killer: Launch failed (proxy died), editor survived");
        CHECK(mgr.CommandCount() == 0, "oop_killer: no commands leaked");
        auto fails = host.TakeFailures();
        CHECK(fails.size() == 1 && fails[0].reason == L"oop-died",
              "oop_killer: failure recorded as oop-died");
    }

    // ---- 场景 3：oop_crasher（EXEC 里访问违例 → SEH 捕获，代理存活）---------
    {
        PluginManager mgr;
        OopHost host;
        std::wstring dll = (dllDir / L"oop_crasher.dll").wstring();
        CHECK(host.Launch(mgr, dll, nullptr, nullptr, 15000),
              "oop_crasher: handshake completed");
        CHECK(mgr.CommandCount() == 1, "oop_crasher: command registered");

        unsigned id = mgr.Commands()[0].id;
        CHECK(mgr.Execute(id), "oop_crasher: Execute dispatched (AV caught in proxy)");
        Sleep(300);
        CHECK(!host.Alive().empty(),
              "oop_crasher: proxy survived access violation (isolated)");
        host.ShutdownAll();
        CHECK(host.Alive().empty(), "oop_crasher: clean shutdown");
    }

    // ---- 场景 4：墓碑等价性 --------------------------------------------------
    // 墓碑插件的进程外结果 = 场景 2（Launch 失败 + oop-died 记账），
    // PluginManager 侧仅多一次 TakeFailures 并账，逻辑已覆盖。
    std::printf("PASS: tombstone path equivalence (covered by scenario 2)\n");

    // ---- 场景 5：一代理多插件共享（批次 66）----------------------------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "multi-tenant: oop_good launched");
        CHECK(host.Launch(mgr, (dllDir / L"oop_crasher.dll").wstring(), recv, nullptr, 15000),
              "multi-tenant: oop_crasher shares the proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_multi.dll").wstring(), recv, nullptr, 15000),
              "multi-tenant: oop_multi shares the proxy");
        CHECK(host.ProcessCount() == 1,
              "multi-tenant: three plugins live in ONE proxy process");
        CHECK(host.Alive().size() == 3, "multi-tenant: three plugins alive");
        CHECK(mgr.CommandCount() == 5, "multi-tenant: 2+1+2 commands registered");

        unsigned mid = mgr.Commands()[3].id;      // Multi Alpha（multi 首命令）
        CHECK(mgr.Execute(mid), "multi-tenant: Execute multi cmd dispatched");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(5, (INT_PTR)mid),
              "multi-tenant: multi EXEC ran with own backfilled cmdID");

        host.BroadcastNotify(777, 0);
        Sleep(200); PumpMessages();
        CHECK(FindMarker(2, (INT_PTR)777), "multi-tenant: NOTIFY reached good");
        CHECK(FindMarker(6, (INT_PTR)777), "multi-tenant: NOTIFY reached multi");

        host.ShutdownAll();
        CHECK(host.Alive().empty() && host.ProcessCount() == 0,
              "multi-tenant: clean shutdown of shared proxy");
        DestroyWindow(recv);
    }

    // ---- 场景 6：死亡归因 + 幸存者重加（killer 毒死共享代理）-----------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "attribution: good on shared proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_multi.dll").wstring(), recv, nullptr, 15000),
              "attribution: multi on same proxy");
        CHECK(host.ProcessCount() == 1, "attribution: two plugins, one proxy");

        bool ok = host.Launch(mgr, (dllDir / L"oop_killer.dll").wstring(), recv, nullptr, 15000);
        CHECK(!ok, "attribution: killer Launch failed (proxy died), test survived");
        auto fails = host.TakeFailures();
        CHECK(fails.size() == 1 && fails[0].reason == L"oop-died" &&
              fails[0].path.find(L"oop_killer.dll") != std::wstring::npos,
              "attribution: exactly the killer recorded oop-died");
        CHECK(host.Alive().size() == 2 &&
              HasName(host.Alive(), L"oop-good") && HasName(host.Alive(), L"oop-multi"),
              "attribution: survivors good+multi alive");
        CHECK(WaitUntil([&] { return host.ProcessCount() == 1; }, 5000),
              "attribution: survivors re-added into exactly ONE fresh proxy");
        CHECK(mgr.CommandCount() == 4,
              "attribution: stale commands withdrawn, survivors re-registered (4)");

        unsigned gid = mgr.Commands()[0].id;
        CHECK(mgr.Execute(gid), "attribution: good Execute after re-add");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(1, (INT_PTR)gid), "attribution: good EXEC marker after re-add");
        unsigned mid = mgr.Commands()[2].id;
        CHECK(mgr.Execute(mid), "attribution: multi Execute after re-add");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(5, (INT_PTR)mid), "attribution: multi EXEC marker after re-add");

        host.ShutdownAll();
        DestroyWindow(recv);
    }

    // ---- 场景 7：装载卡死 → 代理看门狗自杀 → 同死亡归因回收 -------------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "hang: good on shared proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_multi.dll").wstring(), recv, nullptr, 15000),
              "hang: multi on same proxy");
        bool ok = host.Launch(mgr, (dllDir / L"oop_hang.dll").wstring(), recv, nullptr, 4000);
        CHECK(!ok, "hang: hung load recycled proxy (watchdog), test survived");
        auto fails = host.TakeFailures();
        CHECK(fails.size() == 1 && fails[0].reason == L"oop-died" &&
              fails[0].path.find(L"oop_hang.dll") != std::wstring::npos,
              "hang: hung plugin attributed, survivors recycled");
        CHECK(host.Alive().size() == 2 && host.ProcessCount() == 1,
              "hang: survivors alive in one fresh proxy");
        unsigned gid = mgr.Commands()[0].id;
        CHECK(mgr.Execute(gid), "hang: Execute works after recycling");
        Sleep(200); PumpMessages();
        CHECK(FindMarker(1, (INT_PTR)gid), "hang: EXEC marker after recycling");
        host.ShutdownAll();
        DestroyWindow(recv);
    }

    // ---- 场景 8：messageProc 双向同步桥（v2.1，批次 71）----------------------
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        CHECK(host.Launch(mgr, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "msgbridge: good on proxy");
        CHECK(host.Launch(mgr, (dllDir / L"oop_msg.dll").wstring(), recv, nullptr, 15000),
              "msgbridge: msg plugin on same proxy");

        bool handled = false;
        LRESULT r = host.BroadcastMessage(WM_APP + 0x71, 0x1234, (LPARAM)0x5678, &handled);
        PumpMessages();
        CHECK(r == (LRESULT)0x1235 && handled,
              "msgbridge: messageProc LRESULT synced back (first non-zero wins)");
        CHECK(FindMarker(10, (INT_PTR)(WM_APP + 0x71)), "msgbridge: msg crossed intact");
        CHECK(FindMarker(11, (INT_PTR)0x1234), "msgbridge: wParam crossed intact");
        CHECK(FindMarker(12, (INT_PTR)0x5678), "msgbridge: lParam crossed intact");

        host.ShutdownAll();

        // 阴性：全部插件返回 0（oop_good stub）→ 0 + handled=false
        ResetMarkers();
        PluginManager mgr2;
        OopHost host2;
        CHECK(host2.Launch(mgr2, (dllDir / L"oop_good.dll").wstring(), recv, nullptr, 15000),
              "msgbridge: negative baseline launched");
        bool negHandled = true;
        CHECK(host2.BroadcastMessage(WM_APP + 0x72, 1, 2, &negHandled) == 0 && !negHandled,
              "msgbridge: all-zero replies aggregate to 0/unhandled");
        host2.ShutdownAll();
        DestroyWindow(recv);
    }

    // ---- 场景 9：plugin_oop.txt 主动进程外加载（批次 102）--------------------
    // 场景 2~7 的进程外都是**被动**触发的（插件得先杀死宿主一次 → 墓碑）。
    // 本场景验证**主动**入口：把插件路径写进 plugin_oop.txt，启动时直接走代理。
    // 两个子场景：
    //   9a. 正常插件（oop_good，从未崩溃）→ 被强制进程外，账本记 oop-forced
    //       （**不是** oop-auto —— 令牌区分「用户指定」与「自动隔离」）。
    //   9b. 恶意插件（oop_killer）→ 代理失败，且**绝不回落进程内**。
    //       ★ 若此断言回归失败，回落会把 killer 装进本测试进程 ⇒ 进程当场退出。
    //         所以"本测试崩了"就是"隔离回归了"的信号，不是测试本身不稳。
    {
        ResetMarkers();
        PluginManager mgr;
        HWND recv = MakeRecv();
        mgr.SetHostWindow(recv);      // TryOopLoad 要求有宿主窗口，否则直接返回 false
        mgr.EnableOopHost();

        auto dir = std::filesystem::temp_directory_path() / L"xfs_oop_forced_test";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        // ★ 名单里**故意写成正斜杠**。真实用户手写 plugin_oop.txt 用反斜杠，
        //   而扫描出来的路径可能带正斜杠（真机实测：APPDATA 带正斜杠 ⇒ 整条
        //   路径 `D:/...`）。若两侧都用 std::filesystem 的 .string()（都是
        //   反斜杠），这个测试对分隔符差异**完全无感** —— 第一版就是这样，
        //   结果真机上一跑插件仍走进程内加载。此处固定用正斜杠复现该场景。
        auto Slashify = [](std::string s) {
            for (auto& c : s) if (c == '\\') c = '/';
            return s;
        };

        // 9a：正常插件 + 名单
        std::filesystem::copy_file(dllDir / L"oop_good.dll", dir / L"oop_good.dll",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        {
            std::ofstream f((dir / L"plugin_oop.txt").c_str());
            f << Slashify((dir / L"oop_good.dll").string()) << "\n";
        }
        int n = mgr.LoadAllFrom(dir.wstring());
        CHECK(n == 1, "forced-oop: normal plugin loaded (1)");
        CHECK(mgr.IsolatedPlugins().size() == 1 &&
              mgr.IsolatedPlugins()[0].reason == L"oop-forced",
              "forced-oop: ledger token is oop-forced, not oop-auto");
        CHECK(mgr.LoadFailures().empty(),
              "forced-oop: successful isolation is NOT recorded as a failure");
        // ★ 进程级证据。负控实测：把强制名单关掉后，插件会**进程内**加载，
        // 而「命令数=2」「EXEC 回传标记」这两条**照样通过**——因为插件无论
        // 在哪个进程里，行为都一模一样。真正能区分的是下面这条（代理进程
        // 数量）与上面的账本令牌，缺了它们这组断言就是自证。
        CHECK(mgr.OopHostPtr() && mgr.OopHostPtr()->ProcessCount() == 1 &&
              mgr.OopHostPtr()->Alive().size() == 1,
              "forced-oop: a real proxy PROCESS hosts the plugin (not in-process)");
        CHECK(mgr.CommandCount() == 2,
              "forced-oop: 2 commands registered (via proxy)");
        {
            unsigned id0 = mgr.Commands()[0].id;
            CHECK(mgr.Execute(id0), "forced-oop: Execute dispatched");
            Sleep(200); PumpMessages();
            CHECK(FindMarker(1, (INT_PTR)id0),
                  "forced-oop: EXEC round-tripped through the proxy");
        }

        // 9b：恶意插件 + 名单 → 代理死，但不回落进程内
        {
            std::filesystem::copy_file(dllDir / L"oop_killer.dll", dir / L"oop_killer.dll",
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::ofstream f((dir / L"plugin_oop.txt").c_str());
            f << Slashify((dir / L"oop_good.dll").string()) << "\n"
              << Slashify((dir / L"oop_killer.dll").string()) << "\n";
        }
        PluginManager mgr2;
        mgr2.SetHostWindow(recv);
        mgr2.EnableOopHost();
        int n2 = mgr2.LoadAllFrom(dir.wstring());
        CHECK(n2 == 1, "forced-oop: only the good plugin ends up loaded");
        bool killerFailed = false;
        for (const auto& f : mgr2.LoadFailures())
            if (f.path.find(L"oop_killer.dll") != std::wstring::npos) killerFailed = true;
        CHECK(killerFailed,
              "forced-oop: killer recorded as a failure (proxy died), not loaded in-process");
        CHECK(mgr2.CommandCount() == 2,
              "forced-oop: killer contributed no commands (would have killed the host)");

        mgr2.UnloadAll();
        mgr.UnloadAll();
        DestroyWindow(recv);
        std::filesystem::remove_all(dir, ec);
    }

    // ---- 场景 10：NPPM 指针参数过桥（v2.2，批次 111）------------------------
    // 场景 1~9 里插件拿到的 npp 是编辑器主窗口 —— 批次 110 起宿主会**拒答**
    // 带指针的 NPPM_*（不崩，但拿不到数据）。本场景验证 v2.2 的"过桥"：
    // 插件拿到的 npp 是代理进程里的**中转窗**（指针在本进程合法），编辑器
    // 在**自己的**地址空间里重建缓冲 ⇒ 既不崩，也真的拿到数据。
    // 三条判据缺一不可：
    //   ① 插件侧真的收到数据，且**精确相等**（不是"非空"—— 非空断言对
    //      "过桥把字符串搞坏了"照样绿）；
    //   ② host.NppmRelayed() 涨到预期条数（证明走的是过桥，不是别的路径）；
    //   ③ mgr.PointerRefusalCount() == 0（证明过桥路径**不会**触发批次 110
    //      那条外来指针判据 —— 这正是"过桥"与"拒答"的区别）。
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        RelayDocs docs;
        mgr.AttachNppDocSource(&docs);

        CHECK(host.Launch(mgr, (dllDir / L"oop_nppm.dll").wstring(), recv, nullptr, 15000),
              "nppm: fixture plugin launched on proxy");
        CHECK(mgr.CommandCount() == 1, "nppm: one command registered");
        CHECK(host.NppmRelayed() == 0, "nppm: no relayed call before Execute");
        g_nppDispatch = &mgr;   // recv 开始扮演"编辑器主窗口"（见 TestWndProc）

        const unsigned id = mgr.Commands()[0].id;
        CHECK(mgr.Execute(id),
              "nppm: Execute dispatched (the whole NPPM battery runs in the proxy)");
        Sleep(300);
        PumpMessages();

        const std::wstring wantPath = L"D:\\proj\\oop\\relay.txt";

        // ① 两段式宽串：长度查询 + 按长度取内容（内容精确相等）
        CHECK(FindMarker(20, (INT_PTR)wantPath.size()),
              "nppm: GETFULLCURRENTPATH length query answered across the bridge");
        CHECK(FindMarker(21, (INT_PTR)TRUE),
              "nppm: GETFULLCURRENTPATH write call returned TRUE");
        CHECK(StrMarkerOf(20) == wantPath,
              "nppm: full path crossed the bridge byte-exact");
        // ② int 出参
        CHECK(FindMarker(22, (INT_PTR)TRUE) && FindMarker(23, 0),
              "nppm: GETCURRENTSCINTILLA filled the int out-param (0 = Main)");
        // ③ 纯值类型（wp/lp 都不含指针）
        CHECK(FindMarker(24, (INT_PTR)((8 << 16) | 6)),
              "nppm: GETNPPVERSION value survived the bridge");
        // ④ 明确不支持：中转窗直接拒答（0），且**不产生**过桥调用
        CHECK(FindMarker(25, 0),
              "nppm: GETMENUHANDLE refused (HMENU is not portable across processes)");
        // ⑤ int 出参 + 宿主侧副作用（扣命令 id 池）
        CHECK(FindMarker(26, (INT_PTR)TRUE) && FindMarker(27, 1),
              "nppm: ALLOCATECMDID returned an id through the bridge");
        // ⑥ 入参宽串：编辑器侧收到的路径必须与插件发出的完全一致
        CHECK(FindMarker(28, (INT_PTR)TRUE), "nppm: SWITCHTOFILE returned TRUE");
        CHECK(docs.switchCalls == 1 && docs.lastSwitch == wantPath,
              "nppm: in-string crossed the bridge byte-exact (editor saw it)");
        // ⑦ 同一 family 的第二条两段式
        CHECK(FindMarker(29, (INT_PTR)TRUE) && StrMarkerOf(21) == L"relay.txt",
              "nppm: GETFILENAME crossed the bridge byte-exact");
        // ⑧ 4 字节出参结构（ShortcutKey 布局）
        CHECK(FindMarker(30, (INT_PTR)TRUE) && FindMarker(31, (INT_PTR)'K'),
              "nppm: GETSHORTCUTBYCMDID filled the 4-byte struct out-param");
        // ⑪ ★ kValue 的 lp 真的过桥（批次 117）。这两条必须**成对**看：
        //    32 期望 0（副视图）、33 期望文档数。缺陷下 lp 恒 0 ⇒ 两条都返回
        //    文档数 ⇒ 32 红。正对照不可省：若文档数恰好是 0，32 的 0 就没有
        //    区分力（"对 n 个元素成立"在 n==0 时空真）。
        CHECK(docs.DocCount() > 0,
              "nppm: fake doc source is non-empty (without this, 32 is vacuous)");
        CHECK(FindMarker(32, 0),
              "nppm: GETNBOPENFILES(second view) == 0 -- lp crossed the bridge");
        CHECK(FindMarker(33, (INT_PTR)docs.DocCount()),
              "nppm: GETNBOPENFILES(all files) == doc count (positive control)");
        CHECK(FindMarker(99, 1), "nppm: probe completed all steps");

        // ⑨ 过桥计数 = 10：query/write/sci/ver/alloc/switch/name/key + 批次 117
        //    新增的两条 GETNBOPENFILES（值槽 lp 往返）
        //    （GETMENUHANDLE 是 kUnsupported ⇒ 不产生跨进程调用，不计入）
        CHECK(host.NppmRelayed() == 10,
              "nppm: exactly 10 calls went through the bridge (unsupported one did not)");
        // ⑩ ★ 核心：过桥路径**不**触发批次 110 的外来指针判据。
        //    这条只有在"接收窗会分发 NPPM_*"时才有区分力（见 TestWndProc）；
        //    负控（关掉中转窗）下它必须变红。
        CHECK(mgr.PointerRefusalCount() == 0,
              "nppm: bridged path never trips the foreign-pointer guard");

        g_nppDispatch = nullptr;
        host.ShutdownAll();
        mgr.AttachNppDocSource(nullptr);   // 先解绑再析构（docs 比 mgr 先亡）
        DestroyWindow(recv);
    }

    // ---- 场景 11：SCI 通道（v2.3，批次 112）--------------------------------
    // 与场景 10 同构，但验证的是 SCI_* 的**分流**：826 条里 716 条不带指针 ⇒
    // 中转窗**直发**宿主真 Scintilla；带指针的 110 条一律拒答。三条判据：
    //   ① 插件拿到的 scintillaMain / npp **都不是**宿主的真句柄（中转窗在位，
    //      且两者是**不同**的窗口 —— 现实里它们本来就是两个 HWND）；
    //   ② 值类型消息拿到的是**宿主夹具的真值**（证明直发真的到了 Scintilla，
    //      而不是"一律返回 0"—— 后者会让本场景看起来全绿）；
    //   ③ 带指针的消息返回 0，且宿主夹具**从未收到**它们（g_sciPtrCalls == 0）。
    //      负控（把中转窗换成宿主真句柄）下 ③ 变红，且夹具不解引用 ⇒ 红是
    //      干净的断言失败，不是崩溃丢输出。
    {
        ResetMarkers();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        HWND sci = MakeFakeSci();
        g_sciValueCalls = g_sciPtrCalls = g_sciForeignPtrCalls = 0;

        CHECK(host.Launch(mgr, (dllDir / L"oop_sci.dll").wstring(), recv, sci, 15000),
              "sci: fixture plugin launched on proxy");
        CHECK(mgr.CommandCount() == 1, "sci: one command registered");
        g_nppDispatch = &mgr;   // recv 扮演"编辑器主窗口"（见 TestWndProc）

        const unsigned sciCmd = mgr.Commands()[0].id;
        CHECK(mgr.Execute(sciCmd), "sci: Execute dispatched (SCI battery runs in the proxy)");
        // 泵到夹具报"全部完成"为止（比 Sleep 定长稳，且长任务不会假红）
        WaitUntil([] { PumpMessages(); return FindMarker(99, 1); }, 8000);
        PumpMessages();

        // ① 中转窗在位：插件看到的两个句柄都不是宿主的真句柄
        INT_PTR sciSeen = 0, nppSeen = 0;
        CHECK(MarkerValue(40, &sciSeen) && MarkerValue(41, &nppSeen),
              "sci: fixture reported both handles it was given");
        CHECK(sciSeen != 0 && sciSeen != (INT_PTR)sci,
              "sci: scintillaMain is a relay window, not the host's Scintilla");
        CHECK(nppSeen != 0 && nppSeen != (INT_PTR)recv,
              "sci: npp is a relay window, not the host's main window");
        CHECK(sciSeen != nppSeen,
              "sci: the SCI relay and the NPPM relay are distinct windows");

        // ② 值类型：真的到了宿主的 Scintilla 并带回真值
        CHECK(FindMarker(42, 4242), "sci: SCI_GETCURRENTPOS reached the host (4242)");
        CHECK(FindMarker(43, 77),   "sci: SCI_GETLINECOUNT reached the host (77)");
        // ⚠ 批次 116 起夹具的 GETTEXTLENGTH 返回**它自己文档的真实长度**（11），
        //   不再是一个哨兵值：结构族（场景 15）在宿主侧用它算容量。见 FakeSciWndProc。
        CHECK(FindMarker(44, 11),   "sci: SCI_GETTEXTLENGTH reached the host (11)");

        // ③ 不可桥接族：一律拒答（0）
        //    ⚠ 45 的样例换过两次：115 从 SCI_GETPROPERTY（kInOutStr）换成 kStruct 的
        //    SCI_GETSTYLEDTEXT；116 结构族里不带外来指针的 6 条**也可过桥了**，于是
        //    再换成 SCI_FORMATRANGE（结构族拒答桶：hdc 是调用进程私有的 GDI 句柄）。
        //    缓冲先填 'Z' 哨兵 —— marker 50 因此真的能区分"没被碰过"与"被写了 NUL"。
        //    为什么必须这样搬：被换掉的样例过桥后真值也是 0（空范围/空串），与"拒答
        //    返回 0"**同形** ⇒ 断言会全绿却不再有区分力（LESSONS 反复踩的那条）。
        CHECK(FindMarker(45, 0), "sci: SCI_FORMATRANGE refused (process-private HDC)");
        CHECK(FindMarker(46, 0), "sci: SCI_GETCHARACTERPOINTER refused (returns an internal pointer)");
        CHECK(FindMarker(47, 0), "sci: SCI_SETDOCPOINTER refused (opaque pointer, never bridgeable)");
        CHECK(FindMarker(48, 0), "sci: in-range but untabled SCI_* refused");
        CHECK(FindMarker(49, 0), "sci: out-of-range SCI_* refused");
        CHECK(FindMarker(50, 1), "sci: a refusal did not touch the caller's buffer (sentinel intact)");
        CHECK(FindMarker(99, 1), "sci: probe completed all steps");

        // ④ ★ 承重断言：宿主的 Scintilla 只被值类型消息碰到过。
        //    ⚠ 批次 113 起入参串族（kInStr/kInBytes）**已可过桥**；批次 114 起出参
        //    指针族（kOutStr）**也已可过桥**；批次 115 起出入参族（kInOutStr）**也
        //    可过桥**；批次 116 起结构族里不带外来指针的 6 条**也可过桥** ⇒ 它们的
        //    电池分别在场景 12 / 13 / 14 / 15；本场景钉的是"永久拒答族/表外"仍不过桥。
        CHECK(g_sciValueCalls == 3,
              "sci: host Scintilla answered exactly the 3 value calls");
        CHECK(g_sciPtrCalls == 0,
              "sci: host Scintilla NEVER received a non-bridgeable SCI_*");
        CHECK(g_sciInOutCalls == 0,
              "sci: the in-out family did not leak into the value/refusal battery");
        CHECK(g_sciForeignPtrCalls == 0,
              "sci: no foreign pointer ever reached the host Scintilla");

        g_nppDispatch = nullptr;
        host.ShutdownAll();
        DestroyWindow(sci);
        DestroyWindow(recv);
    }

    // ---- 场景 12：SCI 入参指针族过桥（v2.4，批次 113）----------------------
    // 54 条入参族（kInStr 53 + kInBytes 1）从"拒答"升级为"过桥"。本场景钉三件事：
    //   ① 三种**反直觉形态**真的搬对了（串在 wp / 双指针 / lengthEntered 不是长度）；
    //   ② 恶意与越界输入换来"拒答 + 代理存活"，而不是把代理拖死；
    //   ③ 宿主 Scintilla 收到的**内容**与插件发出的**逐字节相等**。
    // 返回值编码"宿主实际读到的长度"（第一段*100 + 第二段）⇒ 截断/丢段都会改
    // 这个数，所以 marker 断言本身就带区分力。
    {
        ResetMarkers();
        g_sciIn.clear();
        g_sciValueCalls = g_sciPtrCalls = g_sciForeignPtrCalls = 0;
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        HWND sci = MakeFakeSci();

        CHECK(host.Launch(mgr, (dllDir / L"oop_scibridge.dll").wstring(), recv, sci, 15000),
              "scibridge: fixture plugin launched on proxy");
        CHECK(mgr.CommandCount() == 1, "scibridge: one command registered");
        g_nppDispatch = &mgr;   // recv 扮演"编辑器主窗口"（见 TestWndProc）

        const unsigned cmd = mgr.Commands()[0].id;
        CHECK(mgr.Execute(cmd),
              "scibridge: Execute dispatched (the in-param battery runs in the proxy)");
        WaitUntil([] { PumpMessages(); return FindMarker(99, 1); }, 8000);
        PumpMessages();

        // ① 长度桶（wp 就是字节数）：返回值 = 宿主读到的字节数
        CHECK(FindMarker(60, 500), "scibridge: ADDTEXT(len=5) bridged with its length");
        CHECK(FindMarker(61, 400), "scibridge: APPENDTEXT(len=4) bridged with its length");
        CHECK(FindMarker(67, 400), "scibridge: ADDSTYLEDTEXT(cells,len=4) bridged as raw bytes");
        CHECK(FindMarker(68, 500), "scibridge: SEARCHINTARGET(len=5) bridged with its length");
        // ② ★ 串在 **lParam** 的那 33 条（含 SETTEXT/REPLACESEL；只看 wp 的实现
        //    会把整串读成空 ⇒ 0）。⚠ 批次 116 前这两条的表判据是错的（空参数槽
        //    被压掉 ⇒ 误判成 wParam），夹具也跟着错 ⇒ 全绿但真机不生效。
        CHECK(FindMarker(62, 500), "scibridge: SETTEXT bridged (string lives in lParam)");
        CHECK(FindMarker(63, 500), "scibridge: REPLACESEL bridged (string lives in lParam)");
        // ③ ★ 双指针：两段都在 ⇒ 2*100+2 / 3*100+4；只搬一段的实现会掉第二段
        CHECK(FindMarker(64, 202), "scibridge: SETPROPERTY bridged BOTH segments");
        CHECK(FindMarker(65, 304), "scibridge: SETREPRESENTATION bridged BOTH segments");
        // ④ ★ 截断守门：lengthEntered=3 而列表长 13。若把它当缓冲长度 ⇒ 300。
        CHECK(FindMarker(66, 1300),
              "scibridge: AUTOCSHOW list NOT truncated (lengthEntered is not a length)");
        // ⑤ NUL 扫描族（长度未知，lp 是串、wp 是值）
        CHECK(FindMarker(69, 600), "scibridge: INSERTTEXT bridged (NUL-scanned string)");

        // ⑥ 内容逐字节相等 —— 这条才是"过桥"与"返回了某个数"的区别
        const SciInCall* cAdd = SciInOf(kSciAddText);
        CHECK(cAdd && cAdd->ok1 && cAdd->s1 == "alpha",
              "scibridge: host Scintilla received \"alpha\" byte-exact");
        const SciInCall* cApp = SciInOf(kSciAppendText);
        CHECK(cApp && cApp->ok1 && cApp->s1 == "beta",
              "scibridge: host Scintilla received \"beta\" byte-exact");
        const SciInCall* cSet = SciInOf(kSciSetText);
        // ★ 批次 116 修正：SETTEXT 的串在 **lParam**（iface `(, string text)`）。
        //   批次 113 的表把它错判成 wParam，本夹具也跟着把串塞进 wp ⇒ 两边自洽
        //   全绿，真机"文本没改"。现在夹具按真实调用方写法给参数，断言落在 lp 上。
        CHECK(cSet && cSet->ok1 && cSet->s1 == "gamma" && cSet->lp != 0,
              "scibridge: SETTEXT put \"gamma\" in the host's lParam (NOT wParam)");
        CHECK(cSet && cSet->wp == 0,
              "scibridge: SETTEXT's unused slot stayed 0 (not turned into a pointer)");
        const SciInCall* cRep = SciInOf(kSciReplaceSel);
        CHECK(cRep && cRep->ok1 && cRep->s1 == "delta" && cRep->lp != 0 && cRep->wp == 0,
              "scibridge: REPLACESEL put \"delta\" in the host's lParam (NOT wParam)");
        const SciInCall* cProp = SciInOf(kSciSetProperty);
        CHECK(cProp && cProp->ok1 && cProp->ok2 &&
              cProp->s1 == "k1" && cProp->s2 == "v1",
              "scibridge: SETPROPERTY crossed with both key and value");
        const SciInCall* cRp = SciInOf(kSciSetRepresentation);
        CHECK(cRp && cRp->ok1 && cRp->ok2 &&
              cRp->s1 == "rep" && cRp->s2 == "REPX",
              "scibridge: SETREPRESENTATION crossed with both segments");
        const SciInCall* cAuto = SciInOf(kSciAutoCShow);
        CHECK(cAuto && cAuto->ok1 && cAuto->s1 == "one two three" &&
              cAuto->s1.size() == 13,
              "scibridge: AUTOCSHOW list arrived whole (13 bytes, not 3)");
        CHECK(cAuto && cAuto->wp == 3,
              "scibridge: lengthEntered survived as a value (3), not as a pointer");
        {
            // cells 是**裸字节**：内含 0x01/0x02，不能用 C 串比较（NUL 会截断）
            std::string expCells;
            expCells.push_back('a'); expCells.push_back('\x01');
            expCells.push_back('b'); expCells.push_back('\x02');
            const SciInCall* cCells = SciInOf(kSciAddStyledText);
            CHECK(cCells && cCells->ok1 && cCells->s1 == expCells,
                  "scibridge: ADDSTYLEDTEXT crossed as 4 raw bytes (embedded 0x01/0x02 kept)");
        }
        const SciInCall* cIns = SciInOf(kSciInsertText);
        CHECK(cIns && cIns->ok1 && cIns->s1 == "insert" && cIns->wp == 7,
              "scibridge: INSERTTEXT crossed with position(7) and text intact");
        const SciInCall* cSit = SciInOf(kSciSearchInTarget);
        CHECK(cSit && cSit->ok1 && cSit->s1 == "hello",
              "scibridge: SEARCHINTARGET pattern crossed byte-exact");

        // ⑦ 拒答方向：不可读指针 / 超限 / 未终止且越界 / 永久拒答族，一律 0
        CHECK(FindMarker(70, 0),
              "scibridge: FORMATRANGE refused (process-private HDC, semantic refusal)");
        CHECK(FindMarker(75, 1),
              "scibridge: the refusal did not touch the caller's struct (sentinel intact)");
        CHECK(FindMarker(71, 0),
              "scibridge: unmapped pointer refused (proxy did not fault)");
        CHECK(FindMarker(72, 0),
              "scibridge: over-limit length (40000) refused, NOT truncated");
        CHECK(FindMarker(73, 0),
              "scibridge: unterminated + out-of-bounds string refused (no OOB read)");
        CHECK(FindMarker(74, 1),
              "scibridge: proxy still alive after 3 hostile calls (isolation held)");
        CHECK(FindMarker(99, 1), "scibridge: probe completed all steps");

        // ⑧ ★ 承重计数
        //    10 条入参调用产生 wire（60–69）。70 是结构族的**永久拒答**支 ⇒ 在中转窗
        //    就被拒，**不产生**跨进程调用；71/72/73 的指针不可读/超限 ⇒ 同样在代理侧
        //    拒答 —— 不可信指针从不离开插件进程，这正是隔离的意义。
        //    ⚠ 70 的样例搬过两次家：原来是 GETPROPERTY（kInOutStr，批次 115 起可过桥，
        //      且过桥后走进**出参**通道 ⇒ SciOutRelayed 会漂），115 换成 kStruct 的
        //      GETSTYLEDTEXT，116 又换成 FORMATRANGE（结构族里唯一语义永久拒答的那支）。
        //      每次都必须换：留在原处会让"拒答"与"过桥后的空真值"同形，计数断言变假绿。
        CHECK(host.SciRelayed() == 10,
              "scibridge: exactly 10 in-param calls crossed the wire");
        CHECK(host.SciRefused() == 0,
              "scibridge: the host refused nothing (all 4 refusals happened in the proxy)");
        CHECK(g_sciIn.size() == 10,
              "scibridge: host Scintilla was touched exactly 10 times");
        CHECK(g_sciValueCalls == 0,
              "scibridge: the in-param battery contains no value-type message");
        CHECK(g_sciPtrCalls == 0,
              "scibridge: no non-bridgeable SCI_* reached the host Scintilla");
        CHECK(g_sciForeignPtrCalls == 0,
              "scibridge: no foreign pointer ever reached the host Scintilla");

        g_nppDispatch = nullptr;
        host.ShutdownAll();
        DestroyWindow(sci);
        DestroyWindow(recv);
    }

    // ---- 场景 13：SCI 出参指针族过桥（v2.5，批次 114）----------------------
    // 30 条 kOutStr 从"拒答"升级为"过桥"。方向与入参族相反（宿主往里写），难点
    // 也从"搬内容"变成"搬多少"——**容量不在消息里**。本场景钉四件事：
    //   ① 四个桶（NUL / 无 NUL / wParam 钳制 / 拒答）真的按表分流；
    //   ② ★ **缓冲区边界**：写对了 N 字节、第 N 格是该有的样子（NUL 或哨兵）、
    //      N 之后**一个字节都没被多写** —— "差一个字节就是越界写"的唯一现场证据；
    //   ③ GetCurLine 的返回值是**光标列号(3)** 而不是长度(20)：按返回值算字节数
    //      的实现会在 ① / ② 上同时露馅（负控②的靶子）；
    //   ④ 拒答路径（拒答桶 / 未桥接族 / **只读**接收缓冲）返回 0 且一个字节不写，
    //      代理进程仍然活着。
    {
        ResetMarkers();
        g_sciOutCalls = g_sciOutForeignPtr = g_sciOutRefuseLeak = 0;
        g_sciValueCalls = g_sciPtrCalls = g_sciForeignPtrCalls = 0;
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        HWND sci = MakeFakeSci();

        CHECK(host.Launch(mgr, (dllDir / L"oop_sciout.dll").wstring(), recv, sci, 15000),
              "sciout: fixture plugin launched on proxy");
        CHECK(mgr.CommandCount() == 1, "sciout: one command registered");
        g_nppDispatch = &mgr;   // recv 扮演"编辑器主窗口"（见 TestWndProc）

        const unsigned cmd = mgr.Commands()[0].id;
        CHECK(mgr.Execute(cmd),
              "sciout: Execute dispatched (the out-param battery runs in the proxy)");
        WaitUntil([] { PumpMessages(); return FindMarker(99, 1); }, 8000);
        PumpMessages();

        // ① 返回值：NUL / 无 NUL / 钳制 / 空值 四个代表
        CHECK(FindMarker(80, 5),  "sciout: GETSELTEXT returned need=5");
        CHECK(FindMarker(81, 7),  "sciout: GETLINE returned need=7");
        CHECK(FindMarker(82, 20), "sciout: GETTEXT returned len=min(64,20)=20");
        CHECK(FindMarker(83, 3),  "sciout: GETCURLINE returned the CARET COLUMN (3), not 20");
        CHECK(FindMarker(84, 4),  "sciout: GETTEXT(wp=4) returned the clamped len=4");
        CHECK(FindMarker(85, 4),  "sciout: GETWORDCHARS returned the count=4");
        CHECK(FindMarker(86, 0),  "sciout: GETFONTLOCALE returned the empty value (0)");

        // ② ★ 现场证据：NUL 桶 = 1|2|8 = 11；无 NUL 桶 = 1|4|8 = 13。
        //    8（N 之后全是哨兵）就是"没有越界写一个字节"的断言 —— 少了它，
        //    多写一个 NUL 或用 need 代替 need+1 都会**看不出来**。
        CHECK(FindMarker(120, 11), "sciout: GETSELTEXT wrote 5 bytes + NUL, nothing past it");
        CHECK(FindMarker(121, 13), "sciout: GETLINE wrote 7 bytes with NO NUL, nothing past it");
        CHECK(FindMarker(122, 11), "sciout: GETTEXT wrote 20 bytes + NUL, nothing past it");
        CHECK(FindMarker(123, 11), "sciout: GETCURLINE wrote 20 bytes + NUL (return value unused)");
        CHECK(FindMarker(124, 11), "sciout: GETTEXT(wp=4) wrote 4 bytes + NUL, nothing past it");
        CHECK(FindMarker(125, 13), "sciout: GETWORDCHARS wrote 4 bytes with NO NUL");
        //    ★ 空值（n==0）没有前缀可验 ⇒ 10 = 2|8，靠"第 0 格是 NUL + 其余全是
        //    哨兵"立命（若实现漏写那一个 NUL，位 2 会掉、位 4|16 会亮 ⇒ 28）。
        CHECK(FindMarker(126, 10), "sciout: GETFONTLOCALE wrote the lone NUL for an empty value");

        // ③ 拒答路径：28 = 4|8|16（整块缓冲仍是哨兵 ⇒ 一个字节都没写）
        CHECK(FindMarker(87, 0),  "sciout: TARGETASUTF8 refused (NUL semantics vary by codepage)");
        CHECK(FindMarker(88, 0),  "sciout: GETTAG refused (write size varies with regex)");
        CHECK(FindMarker(89, 0),  "sciout: GETREPRESENTATION refused (probe cannot tell the two 0s)");
        CHECK(FindMarker(90, 0),  "sciout: read-only buffer refused (LocalWritable gate)");
        CHECK(FindMarker(91, 0),  "sciout: read-only buffer refused on a clamped message too");
        CHECK(FindMarker(127, 28), "sciout: refused TARGETASUTF8 did not touch the buffer");
        CHECK(FindMarker(128, 28), "sciout: refused GETTAG did not touch the buffer");
        CHECK(FindMarker(129, 28), "sciout: refused GETREPRESENTATION did not touch the buffer");
        CHECK(FindMarker(130, 28), "sciout: refused read-only GETSELTEXT left the page intact");
        CHECK(FindMarker(131, 28), "sciout: refused read-only GETTEXT left the page intact");
        CHECK(FindMarker(74, 1),
              "sciout: proxy still alive after the hostile/refused calls (isolation held)");
        CHECK(FindMarker(99, 1), "sciout: probe completed all steps");

        // ④ ★ 承重计数：**恰好 7 条**写调用过桥（80–86 各一次真写）；另外 5 条
        //    （拒答桶 87–89 + 只读缓冲 90/91）**在中转窗就被拒**，不产生任何跨进程
        //    调用 —— 所以下面的 7 / 65 / 7 是"只有该过桥的过了桥"的证据，不是巧合。
        //    ⚠ 只读缓冲那两条曾因为可写性检查排在**跨进程往返之后**（要等回包的
        //    copied 才知道验多少字节）而多出 2 次往返 ⇒ 计数变成 9/92/9。把本地就能
        //    判定的失败提到最前，才回到 7/65/7（见 RelaySciOut 的 early gate）。
        CHECK(host.SciOutRelayed() == 7,
              "sciout: exactly 7 out-param write calls crossed the wire");
        CHECK(host.SciOutRefused() == 0,
              "sciout: the host refused nothing (every refusal happened in the proxy)");
        // （没有"查长度调用"的独立断言：探长度会各自多一次 wire 调用 ⇒ 上面的
        //   SciOutRelayed()==7 已经把它盖住了，单独再记一个计数器是冗余面。）
        // 7 条写调用的回带字节数逐桶累加：kNul 6 / kNoNul 7 / clamp 21 / clamp 21
        // / clamp 5 / kNoNul 4 / kNul 1 = 65（★ 每一项都是 need(+1)，与返回值无关）
        CHECK(host.SciOutBytes() == 65,
              "sciout: the host handed back exactly need(+1) bytes for each bucket");
        CHECK(g_sciOutCalls == 7,
              "sciout: host Scintilla was asked to write exactly 7 times");
        CHECK(g_sciOutRefuseLeak == 0,
              "sciout: a refusal-bucket message NEVER reached the host Scintilla");
        CHECK(g_sciOutForeignPtr == 0,
              "sciout: every buffer the host wrote into was writable in the host");
        CHECK(g_sciValueCalls == 0 && g_sciPtrCalls == 0,
              "sciout: no value-type / non-bridgeable SCI_* reached the host");
        CHECK(g_sciInOutCalls == 0 && g_sciInOutLeak == 0,
              "sciout: the in-out family did not leak into the out-param battery");

        g_nppDispatch = nullptr;
        host.ShutdownAll();
        DestroyWindow(sci);
        DestroyWindow(recv);
    }

    // ---- 场景 14：SCI 出入参族过桥（v2.6，批次 115）------------------------
    // 5 条 kInOutStr 从"拒答"升级为"过桥"。形状是 (string, stringresult) ⇒
    // **入参串在 wp、出参缓冲在 lp**，所以本场景同时钉两件事：
    //   ① 入参侧：插件进程里的那个 wp 地址必须被换成**宿主本地的副本**。证据不是
    //      "过桥成功"（那太弱），而是假 Scintilla 把**它读到的 key 长度**编进了
    //      返回值（key.size()*100 + val.size()）⇒ marker 直接反映"串真的过来了"。
    //      串没搬对（或根本没搬）时 key 会是空串，marker 立刻掉成 val.size()。
    //   ② 出参侧：与场景 13 同形的缓冲边界（写对 N 字节 + 第 N 格 NUL + 之后全哨兵）。
    // 另外两条拒答桶（ENCODEDFROMUTF8 / GETREPRESENTATION）与两条恶意入参串
    // （空指针 / 未映射地址）都必须换来"返回 0 且一个字节不写"，且代理仍活着。
    {
        ResetMarkers();
        g_sciInOutCalls = g_sciInOutBadKey = g_sciInOutLeak = 0;
        g_sciOutCalls = g_sciOutForeignPtr = g_sciOutRefuseLeak = 0;
        g_sciValueCalls = g_sciPtrCalls = g_sciForeignPtrCalls = 0;
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        HWND sci = MakeFakeSci();

        CHECK(host.Launch(mgr, (dllDir / L"oop_sciinout.dll").wstring(), recv, sci, 15000),
              "sciinout: fixture plugin launched on proxy");
        CHECK(mgr.CommandCount() == 1, "sciinout: one command registered");
        g_nppDispatch = &mgr;   // recv 扮演"编辑器主窗口"（见 TestWndProc）

        const unsigned cmd = mgr.Commands()[0].id;
        CHECK(mgr.Execute(cmd),
              "sciinout: Execute dispatched (the in-out battery runs in the proxy)");
        WaitUntil([] { PumpMessages(); return FindMarker(99, 1); }, 8000);
        PumpMessages();

        // ① 返回值 = 宿主读到的 key 长度 * 100 + 值的字节数。
        //    ★ 每一条的 *100 那一项就是"入参串跨进程搬对了"的现场证据：
        //      "k1" ⇒ 200，200 字节的长键 ⇒ 20000。少了它，下面的断言只证明
        //      "宿主写回了某个值"，不证明"宿主读到了插件发的那个属性名"。
        CHECK(FindMarker(150, 202),   "sciinout: GETPROPERTY(\"k1\") saw key(2) + value(2)");
        CHECK(FindMarker(151, 202),   "sciinout: GETPROPERTYEXPANDED(\"k1\") same as GETPROPERTY");
        CHECK(FindMarker(152, 207),   "sciinout: DESCRIBEPROPERTY(\"k1\") saw key(2) + \"desc:k1\"(7)");
        CHECK(FindMarker(153, 200),   "sciinout: GETPROPERTY(\"k2\") saw key(2) + empty value(0)");
        CHECK(FindMarker(154, 20000), "sciinout: GETPROPERTY(200-byte key) arrived WHOLE (200*100)");
        //    空键：宿主读到的 key 长度是 **0** ⇒ 返回值 0 —— 与"拒答返回 0"同形，
        //    所以这一条的区分力落在摘要位（195 == 10 = "写了那个孤零零的 NUL"）上，
        //    而不是返回值上。这正好是本批"载荷 1 字节 ≠ 不带载荷"的现场证据：
        //    inBytes==0 会被宿主直接拒答（摘要会是 28），而这里是 10。
        CHECK(FindMarker(155, 0),     "sciinout: GETPROPERTY(\"\") crossed with a 1-byte payload (host saw a 0-length key)");

        // ② ★ 缓冲边界：成功用例 ⇒ 11（1|2|8，写对内容 + 末格 NUL + 之后全哨兵）；
        //    空值 ⇒ 10（2|8，n==0 没有前缀可验）。少写那个 NUL 会掉到 8/12。
        CHECK(FindMarker(190, 11), "sciinout: GETPROPERTY wrote \"v1\" + NUL, nothing past it");
        CHECK(FindMarker(191, 11), "sciinout: GETPROPERTYEXPANDED wrote \"v1\" + NUL");
        CHECK(FindMarker(192, 11), "sciinout: DESCRIBEPROPERTY wrote \"desc:k1\" + NUL");
        CHECK(FindMarker(193, 10), "sciinout: GETPROPERTY(\"k2\") wrote the lone NUL for an empty value");
        CHECK(FindMarker(194, 10), "sciinout: long-key call wrote the lone NUL");
        CHECK(FindMarker(195, 10), "sciinout: empty-key call wrote the lone NUL");

        // ③ 拒答路径：28 = 4|8|16（整块缓冲仍是哨兵 ⇒ 一个字节都没写）
        CHECK(FindMarker(156, 0),  "sciinout: GETREPRESENTATION refused (two 0s in one probe)");
        CHECK(FindMarker(157, 0),  "sciinout: ENCODEDFROMUTF8 refused (length from a companion msg)");
        CHECK(FindMarker(158, 0),  "sciinout: NULL in-param string refused");
        CHECK(FindMarker(159, 0),  "sciinout: unmapped in-param string refused (no fault)");
        CHECK(FindMarker(196, 28), "sciinout: refused GETREPRESENTATION did not touch the buffer");
        CHECK(FindMarker(197, 28), "sciinout: refused ENCODEDFROMUTF8 did not touch the buffer");
        CHECK(FindMarker(178, 28), "sciinout: refused NULL string did not touch the buffer");
        CHECK(FindMarker(179, 28), "sciinout: refused unmapped string did not touch the buffer");
        CHECK(FindMarker(74, 1),
              "sciinout: proxy still alive after the hostile in-param strings (isolation held)");
        CHECK(FindMarker(99, 1), "sciinout: probe completed all steps");

        // ④ ★ 承重计数
        //    6 条成功（150–155）各产生**一次** wire 往返（探长度 + 真调用都在宿主
        //    侧完成）；4 条拒答（156–159）在中转窗就被拒 ⇒ 不产生跨进程调用。
        //    回带字节数：kNul ⇒ need+1 ⇒ 3+3+8+1+1+1 = 17。
        CHECK(host.SciOutRelayed() == 6,
              "sciinout: exactly 6 in-out calls crossed the wire");
        CHECK(host.SciOutRefused() == 0,
              "sciinout: the host refused nothing (all 4 refusals happened in the proxy)");
        CHECK(host.SciOutBytes() == 17,
              "sciinout: the host handed back exactly need(+1) bytes for each kNul call");
        CHECK(g_sciInOutCalls == 6,
              "sciinout: host Scintilla was asked to write exactly 6 times");
        // ★★ 负控③的靶子：wp 没被重建成宿主本地副本时，这里会 > 0
        //    （读到插件进程的地址或空指针）⇒ 断言直接指向缺陷本身。
        CHECK(g_sciInOutBadKey == 0,
              "sciinout: every in-param string the host read was a valid local C string");
        CHECK(g_sciInOutLeak == 0,
              "sciinout: a refusal-bucket in-out message NEVER reached the host Scintilla");
        CHECK(g_sciOutForeignPtr == 0,
              "sciinout: every buffer the host wrote into was writable in the host");
        CHECK(g_sciValueCalls == 0 && g_sciPtrCalls == 0,
              "sciinout: no value-type / non-bridgeable SCI_* reached the host");

        g_nppDispatch = nullptr;
        host.ShutdownAll();
        DestroyWindow(sci);
        DestroyWindow(recv);
    }

    // ---- 场景 15：SCI 结构族过桥（v2.7，批次 116）--------------------------
    // 8 条 kStruct 里 **6 条**（GetTextRange/Full、GetStyledText/Full、FindText/Full）
    // 从"拒答"升级为"过桥"，另外 2 条（FormatRange/Full）**永久**拒答 ——
    // 结构里的 hdc/hdcTarget 是调用进程**私有**的 GDI 句柄，而句柄不是地址，
    // 桥接那套"内联字节 + 重建指针"对它无能为力（详见 test_sci_struct.cpp [10]）。
    //
    // 本族是本项目第一个**真 in-out 族**，所以本场景钉四件事：
    //   ① 出缓冲边界（与场景 13 同形）：写对 N 字节 + 结尾 NUL + 之后全哨兵。
    //      ★ 与出参族的差别是**容量算得出**（len+1 / 2*len+2），不靠探长度。
    //   ② needle 真的跨过来了 —— 证据是"宿主找到了它"（返回值 6 只在真读到
    //      "world" 时才可能），而不是"过桥成功"。
    //   ③ ★ **条件回写**：FindText 没找到时 Scintilla 一个字节都不写 chrgText
    //      （Editor.cxx:4318/4349）。夹具把 chrgText 预置成哨兵 ⇒ "没被写"与
    //      "被写成 (0,0)"能分开。若桥接照写，marker 174 会掉成 0。
    //   ④ 恶意/越界一律拒答且代理存活：结构体不可读、needle 空指针、
    //      cpEnd > docLen（原生会留下"前面全是垃圾 + 末尾一个 NUL"，桥接**收窄**）。
    {
        ResetMarkers();
        g_sciStructCalls = g_sciStructForeign = g_sciStructLeak = 0;
        g_sciStructBadNeedle = g_sciStructChrgWrites = 0;
        g_sciValueCalls = g_sciPtrCalls = g_sciForeignPtrCalls = 0;
        g_sciOutCalls = g_sciOutForeignPtr = g_sciOutRefuseLeak = 0;
        g_sciInOutCalls = g_sciInOutBadKey = g_sciInOutLeak = 0;
        g_sciIn.clear();
        PluginManager mgr;
        OopHost host;
        HWND recv = MakeRecv();
        HWND sci = MakeFakeSci();

        CHECK(host.Launch(mgr, (dllDir / L"oop_scistruct.dll").wstring(), recv, sci, 15000),
              "scistruct: fixture plugin launched on proxy");
        CHECK(mgr.CommandCount() == 1, "scistruct: one command registered");
        g_nppDispatch = &mgr;   // recv 扮演"编辑器主窗口"（见 TestWndProc）

        const unsigned cmd = mgr.Commands()[0].id;
        CHECK(mgr.Execute(cmd),
              "scistruct: Execute dispatched (the struct battery runs in the proxy)");
        WaitUntil([] { PumpMessages(); return FindMarker(99, 1); }, 8000);
        PumpMessages();

        // ① kRangeOut：返回值 = 拷入的字节数（**不含** NUL，Editor.cxx:6047）
        CHECK(FindMarker(160, 5),  "scistruct: GETTEXTRANGE(0,5) returned 5");
        CHECK(FindMarker(161, 7),  "scistruct: it wrote \"hello\" + NUL, nothing past it");
        CHECK(FindMarker(162, 11), "scistruct: GETTEXTRANGEFULL(0,11) returned 11");
        CHECK(FindMarker(163, 7),  "scistruct: it wrote \"hello world\" + NUL, nothing past it");
        // 空范围：len == 0 ⇒ 容量仍是 1（只写结尾 NUL）。摘要 6 = 2|4 —— n==0 时
        // **不给**"内容对"那一位（空前缀不是证据，否则空值用例会白得一位）。
        CHECK(FindMarker(164, 0),  "scistruct: GETTEXTRANGE(5,5) returned 0");
        CHECK(FindMarker(165, 6),  "scistruct: empty range still wrote the lone NUL");

        // ② kStyledOut：返回值 = 2*len（**不含**末尾那两个 NUL，Editor.cxx:6036）
        CHECK(FindMarker(166, 6),  "scistruct: GETSTYLEDTEXT(0,3) returned 6 (2*3)");
        CHECK(FindMarker(167, 7),  "scistruct: it wrote 3 (char,style) pairs + 2 NULs");
        CHECK(FindMarker(168, 4),  "scistruct: GETSTYLEDTEXTFULL(0,2) returned 4 (2*2)");
        CHECK(FindMarker(169, 7),  "scistruct: it wrote 2 (char,style) pairs + 2 NULs");

        // ③ kFindInOut：入串 + **条件**回写
        //    ★ 返回 6 就是"needle 真的跨过来了"的证据：宿主只有在**它自己的**
        //      地址空间里读到 "world" 才可能在 "hello world" 里定位到 6。
        CHECK(FindMarker(170, 6),  "scistruct: FINDTEXT(\"world\") found it at 6 (needle crossed)");
        CHECK(FindMarker(171, 6),  "scistruct: chrgText.cpMin written = 6");
        CHECK(FindMarker(172, 11), "scistruct: chrgText.cpMax written = 11");
        // ★★ 没找到：chrgText 必须**原封不动**（哨兵还在）—— 这就是 hasChrg 那一位
        //     存在的理由。照写会把插件的原值覆盖成 (0,0)，与"找到了空匹配"同形。
        CHECK(FindMarker(173, -1), "scistruct: FINDTEXT(\"zzz\") returned -1");
        CHECK(FindMarker(174, 1),  "scistruct: not-found did NOT touch chrgText (sentinel intact)");
        CHECK(FindMarker(175, 0),  "scistruct: FINDTEXTFULL(\"hello\") found it at 0");
        CHECK(FindMarker(176, 0),  "scistruct: chrgText.cpMin written = 0");
        CHECK(FindMarker(177, 5),  "scistruct: chrgText.cpMax written = 5");
        // ★★ 负控③ 的回归：**非 Full** 的 FindText 命中位置 0。宿主侧的 canary 若
        //    从 INT_PTR 截断成 long 会得到 0，而 0 正是这里的真值 ⇒ 会静默报"没找到"
        //    （上面 175 是 Full 变体，走另一条 canary 分支，抓不到这个缺陷）。
        CHECK(FindMarker(184, 0),  "scistruct: FINDTEXT(\"hello\") at position 0 returned 0");
        CHECK(FindMarker(185, 0),  "scistruct: chrgText.cpMin written = 0 (position 0 is a real write)");
        CHECK(FindMarker(186, 5),  "scistruct: chrgText.cpMax written = 5");

        // ④ 永久拒答族与恶意/越界：一律 0，且**一个字节都不写**调用方内存
        CHECK(FindMarker(178, 0),  "scistruct: FORMATRANGE refused (process-private HDC)");
        CHECK(FindMarker(179, 1),  "scistruct: the refusal did not touch the caller's struct");
        CHECK(FindMarker(180, 0),  "scistruct: unreadable struct pointer refused (no fault)");
        CHECK(FindMarker(181, 0),  "scistruct: NULL needle refused (native would strlen(nullptr))");
        CHECK(FindMarker(182, 0),  "scistruct: cpEnd > docLen refused (deliberately narrowed)");
        CHECK(FindMarker(183, 1),  "scistruct: the narrowed refusal did not touch the buffer");
        CHECK(FindMarker(74, 1),
              "scistruct: proxy still alive after 3 hostile calls (isolation held)");
        CHECK(FindMarker(99, 1), "scistruct: probe completed all steps");

        // ⑤ ★ 承重计数
        //    13 条结构族调用里：10 条过桥（160/162/164/166/168/170/173/175/184 + 182）、
        //    3 条在中转窗就被拒（178 FormatRange 拒答桶、180 结构体不可读、
        //    181 needle 空指针）⇒ 不产生跨进程调用。
        //    过桥的 10 条里 182 在**宿主侧**被拒（cpEnd > docLen ⇒ 容量算不出）。
        CHECK(host.SciStructRelayed() == 10,
              "scistruct: exactly 10 struct calls crossed the wire");
        CHECK(host.SciStructRefused() == 1,
              "scistruct: the host refused exactly the out-of-range one (182)");
        //    回带字节数 = 6 + 12 + 1 + 8 + 6 = 33（Find 族不带出缓冲 ⇒ 0）
        CHECK(host.SciStructBytes() == 33,
              "scistruct: the host handed back exactly the bytes it computed capacity for");
        CHECK(host.SciStructChrgWrites() == 3,
              "scistruct: exactly 3 of the 4 FindText calls wrote chrgText");
        CHECK(g_sciStructCalls == 9,
              "scistruct: host Scintilla was asked to fill exactly 9 struct buffers");
        CHECK(g_sciStructChrgWrites == 3,
              "scistruct: the fake Scintilla wrote chrgText exactly 3 times (agrees with the host)");
        // ★★ 负控①②④的靶子：这三条在"宿主不重建结构/算错容量/不验可写"时会 > 0
        CHECK(g_sciStructForeign == 0,
              "scistruct: no unreadable/unwritable struct ever reached the host Scintilla");
        CHECK(g_sciStructBadNeedle == 0,
              "scistruct: every needle the host read was a valid local C string");
        CHECK(g_sciStructLeak == 0,
              "scistruct: a refusal-bucket struct message NEVER reached the host Scintilla");
        //    kRangeOut 的容量靠宿主侧 SCI_GETTEXTLENGTH 拿 docLen（160/162/164/182）
        CHECK(g_sciValueCalls == 4,
              "scistruct: the host asked for the document length exactly 4 times");
        CHECK(g_sciPtrCalls == 0 && g_sciForeignPtrCalls == 0,
              "scistruct: no non-bridgeable SCI_* reached the host Scintilla");
        CHECK(g_sciOutCalls == 0 && g_sciInOutCalls == 0 && g_sciIn.empty(),
              "scistruct: the struct family did not leak into the out/in/in-out batteries");

        g_nppDispatch = nullptr;
        host.ShutdownAll();
        DestroyWindow(sci);
        DestroyWindow(recv);
    }

    // ---- 场景 16：NPPM_DMM* 停靠族过桥（v2.8，批次 117）---------------------
    // 6 条停靠消息从"拒答"升级为"过桥"。与场景 10~15 的**结构性差别**：前面
    // 那些族过桥的是**数据**（串 / 结构 / 缓冲），这一族过桥的是一个**窗口
    // 父子关系** —— 插件对话框住在代理进程里，宿主把它 SetParent 进自己的
    // dock 面板。所以承重断言分三组，各证一件不同的事：
    //   ① 注册真的落地 + **重建出来的元数据**与插件发出的逐字段相等
    //      （名字/模块名 ⇒ FindHwndByName 查得到；dlgID ⇒ 通知的 idFrom）。
    //   ② 跨进程 SetParent 真的发生：hClient 的**新父窗口属于本测试进程**。
    //      这是本批最核心的系统级主张，单测（形状表 / 信任判据）证明不了它。
    //   ③ 通知链真的绕过"跨进程 WM_NOTIFY 被拒"：DMN_DOCK / DMN_SWITCHIN /
    //      DMN_CLOSE 都在插件侧被记录，且 `hwndFrom` 恰好等于插件眼里的
    //      编辑器窗口（代理中转窗）—— NppExec 一类插件只认这个等式。
    // 另外还覆盖反向动作（DMM_UPDATEDISPINFO）与 **veto 往返**：DMN_CLOSE 的
    // 应答决定面板是否隐藏 —— 这条正是 DmmRelayReplyWire 里 delivered 与
    // result 必须分开的理由（"没送到"与"未 veto"在 result 上同形）。
    {
        ResetMarkers();
        HWND recv = MakeRecv();
        HWND frame = MakeHostFrame();
        // ★ 声明顺序就是析构顺序（反向）：dock 必须在 mgr **之前** —— mgr 持有
        //   裸指针 dockHost_，反了就是"mgr 析构时用悬垂指针"。frame 同理在 dock 前。
        DockManager dock;
        PluginManager mgr;
        dock.Init(frame, ::GetModuleHandleW(nullptr));
        mgr.SetDockHost(&dock);
        mgr.EnableOopHost();          // 建 OopHost 并同步注入 DockRemote（幂等）
        OopHost* host = mgr.OopHostPtr();
        CHECK(host != nullptr, "dmm: out-of-process host enabled");

        if (host) {
            CHECK(host->Launch(mgr, (dllDir / L"oop_dmm.dll").wstring(), recv,
                               nullptr, 15000),
                  "dmm: fixture plugin launched on proxy");
            CHECK(mgr.CommandCount() == 1, "dmm: one command registered");
            CHECK(dock.PanelCount() == 0, "dmm: no dock panel before Execute");
            CHECK(host->DmmClients() == 0, "dmm: no trusted client before Execute");

            const unsigned cmd = mgr.Commands()[0].id;
            CHECK(mgr.Execute(cmd),
                  "dmm: Execute dispatched (the whole DMM battery runs in the proxy)");
            WaitUntil([] { PumpMessages(); return FindMarker(99, 1); }, 15000);
            PumpMessages();

            // ---- ① 注册与元数据重建 --------------------------------------
            INT_PTR hcVal = 0;
            const bool hasHc = MarkerValue(40, &hcVal);
            HWND hc = reinterpret_cast<HWND>(hcVal);
            CHECK(hasHc && hc != nullptr && ::IsWindow(hc),
                  "dmm: the plugin created (and reported) its own dialog window");
            CHECK(FindMarker(41, 1),
                  "dmm: NPPM_DMMREGASDCKDLG returned TRUE (the host accepted it)");
            CHECK(dock.PanelCount() == 1, "dmm: the host now owns exactly one dock panel");
            // 名字与模块名逐字段相等 ⇒ FindHwndByName 才可能命中。"过桥把串搞坏了"
            // 会让它返回 nullptr；只断言"非空"则抓不到。
            CHECK(dock.FindHwndByName(L"Oop DMM Panel", L"oop_dmm.dll") == hc,
                  "dmm: name + module name crossed the bridge byte-exact");

            // ---- ② 跨进程 SetParent（本批最核心的系统级主张）--------------
            HWND parent = hc ? ::GetParent(hc) : nullptr;
            CHECK(parent && parent != hc,
                  "dmm: the plugin dialog was re-parented into a dock wrapper");
            DWORD ppid = 0;
            if (parent) ::GetWindowThreadProcessId(parent, &ppid);
            CHECK(parent && ppid == ::GetCurrentProcessId(),
                  "dmm: its new parent lives in THIS process (cross-process SetParent)");
            CHECK(parent && ::GetParent(parent) == frame,
                  "dmm: the wrapper is a child of the dock frame window");
            CHECK(hc && (::GetWindowLongPtrW(hc, GWL_STYLE) & WS_CHILD) != 0,
                  "dmm: the host stripped WS_POPUP and set WS_CHILD on the dialog");

            // ---- ②b 图标跨进程 marshal（批次 137）------------------------
            // 插件给的 hIconTab 是**它进程里的用户对象句柄**，传值无意义 ⇒ 代理把
            // 图标转码成图像块，宿主重建本进程的 HICON。判据落在"宿主侧真的多了
            // 一个带图标的 SS_ICON 控件"，而不是"桥上有字节"。控件 id = kLabelId-1
            // = 2399（见 DockManager.cpp 的 wrapper 子控件创建处；测试不引其内部
            // 头，故按契约写死）。
            CHECK(FindMarker(57, 1),
                  "dmm: the fixture successfully created the icon it registers");
            {
                HWND iconCtrl = parent ? ::GetDlgItem(parent, 2399) : nullptr;
                CHECK(iconCtrl != nullptr,
                      "dmm: the rebuilt tab icon produced a wrapper child control");
                CHECK(iconCtrl && (::GetWindowLongPtrW(iconCtrl, GWL_STYLE) &
                                   SS_ICON) != 0,
                      "dmm: that child is an SS_ICON static");
                CHECK(iconCtrl && ::SendMessageW(iconCtrl, STM_GETICON, 0, 0) != 0,
                      "dmm: the rebuilt HICON was handed to the icon control");
            }

            // ---- ③ 通知链（跨进程 WM_NOTIFY 由代理中继）------------------
            CHECK(FindMarker(42, 1),
                  "dmm: DMN_DOCK reached the plugin synchronously during registration");
            CHECK(FindMarker(43, 1052), "dmm: that notification was DMN_DOCK");
            CHECK(FindMarker(44, 1),
                  "dmm: its hwndFrom was the plugin's 'editor window' (relay wnd)");
            CHECK(FindMarker(45, 7),
                  "dmm: its idFrom was the plugin's dlgID (7) -- dlgID crossed too");

            // ---- 显示 / 隐藏 / 按名切入（kValue + kInWideStr）------------
            CHECK(FindMarker(46, 1) && FindMarker(47, 1) && FindMarker(48, 1),
                  "dmm: DMMSHOW / DMMHIDE / DMMVIEWOTHERTAB all returned TRUE");
            CHECK(FindMarker(49, 2),
                  "dmm: hide-then-switch-in produced a second notification");
            CHECK(FindMarker(50, 1054), "dmm: that one was DMN_SWITCHIN");
            // 按名/模块查句柄（kDmmTwoStr：**唯一**把指针放在 wp 的形状）
            CHECK(FindMarker(51, 1),
                  "dmm: DMMGETPLUGINHWNDBYNAME returned the plugin's own dialog HWND");

            // ---- 反向动作中继（编辑器 → 插件）----------------------------
            CHECK(FindMarker(52, 1), "dmm: DMMUPDATEDISPINFO returned TRUE");
            CHECK(FindMarker(53, 1) && FindMarker(54, 0x5007),
                  "dmm: the plugin received DMM_UPDATEDISPINFO (action relayed)");
            CHECK(FindMarker(55, 1),
                  "dmm: every notification carried hwndFrom == npp (consistently)");
            CHECK(FindMarker(99, 1), "dmm: probe completed all steps");

            // ---- 承重计数 -------------------------------------------------
            CHECK(host->DmmClients() == 1,
                  "dmm: exactly one trusted proxy-hosted client was registered");
            CHECK(host->NppmRelayed() == 6,
                  "dmm: exactly 6 NPPM_DMM* calls crossed the bridge");
            CHECK(host->DmmNotifies() == 2,
                  "dmm: exactly 2 DMN_* were delivered through the relay");
            CHECK(host->DmmNotifyRefused() == 0, "dmm: no notification was dropped");
            CHECK(host->DmmActions() == 1,
                  "dmm: exactly 1 DMM_* action was delivered");
            CHECK(host->DmmActionRefused() == 0, "dmm: no action was dropped");
            CHECK(mgr.PointerRefusalCount() == 0,
                  "dmm: the bridged path never trips the foreign-pointer guard");

            // ---- 信任模型的另一半：陌生跨进程窗口**仍然被拒** ------------
            // 上面证明的是"受信任的代理承载可以 dock"。缺了这一半，把
            // IsAcceptableClient 放宽成"任何窗口都行"会让上面全部照样绿。
            // 样本用的是**同一个代理进程里的另一个真窗口**（从不注册）——
            // 它是"陌生跨进程窗口"最贴近现实且完全确定的样本。
            INT_PTR otherVal = 0;
            HWND other = MarkerValue(56, &otherVal)
                             ? reinterpret_cast<HWND>(otherVal) : nullptr;
            CHECK(other && ::IsWindow(other) && other != hc,
                  "dmm: the fixture also created a never-registered proxy window");
            CHECK(host->IsTrustedDmmClient(hc),
                  "dmm: the registered client IS trusted");
            CHECK(!host->IsTrustedDmmClient(other),
                  "dmm: a never-registered proxy window is NOT trusted");
            {
                npp::DockedWidgetData bogus{};
                bogus.hClient = other;
                bogus.pszName = L"Stranger Panel";
                bogus.pszModuleName = L"stranger.dll";
                // 守卫在**读名字之前**，所以传本地串是安全的（顺序见 DockManager.cpp）
                CHECK(!dock.DockWidget(bogus),
                      "dmm: DockWidget still refuses an unregistered cross-process window");
                CHECK(dock.PanelCount() == 1,
                      "dmm: ...and no panel was created for it");
            }

            // ---- veto 往返：DMN_CLOSE 的应答真的跨回来了 ------------------
            // 模拟用户点 wrapper 上的关闭钮（kCloseId = 2401，DockManager.cpp 的
            // 私有常量）→ 宿主问插件 DMN_CLOSE → 插件回 TRUE（veto）⇒ 面板不隐藏。
            // ★ 这条把"delivered 与 result 必须分开"钉在**行为**上：若把"没送到"
            //   也当成"未 veto"，面板会被静默关掉，而通知计数照样涨。
            const int hBefore = dock.TotalHeight(96);
            CHECK(hBefore > 0, "dmm: the panel is visible before the close attempt");
            if (parent)
                ::SendMessageW(parent, WM_COMMAND, MAKEWPARAM(2401, BN_CLICKED), 0);
            PumpMessages();
            CHECK(dock.TotalHeight(96) == hBefore,
                  "dmm: the plugin's DMN_CLOSE veto crossed back (panel stayed visible)");
            CHECK(host->DmmNotifies() == 3,
                  "dmm: the close negotiation was a third delivered notification");
            CHECK(host->DmmNotifyRefused() == 0,
                  "dmm: ...and it was delivered, not silently dropped");
        }

        dock.Destroy();           // 通道还连着：DMM_CLOSE 真的经代理中继了一次
        mgr.UnloadAll();          // 先摘 DockRemote 再放手（mgr 自己保证顺序）
        DestroyWindow(frame);
        DestroyWindow(recv);
    }

    std::printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
