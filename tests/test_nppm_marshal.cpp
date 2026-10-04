// test_nppm_marshal.cpp — NPPM_* 过桥分类表的守卫（批次 111）。
//
// 为什么单开一个测试目标：这是**两侧唯一事实源**（代理侧中转窗 + 宿主侧
// HandleNppmCall 都读它）。折进别的目标里，一旦"忘了注册进 CMakeLists"，
// 缺测会伪装成 100% 通过 —— 而这张表恰恰是"漏一条 = 某条 NPPM_* 静默返回
// 0"的那类缺陷的唯一防线。
//
// 覆盖：
//   [1] 完备性：kNppKnownMsgs ↔ kNppmTable 双向对账（34 条）
//   [2] 唯一性：表内 msg 无重复
//   [3] 分类：各形状的条数 + 代表项的精确形状（把"决策"钉住，不是描述现象）
//   [4] 不变式：why 非空 ⟺ kUnsupported；受支持项 why 必须为空
//   [5] 区间：表内每条都落在宿主拦截的两个 NPPM 区间内
//   [6] 出参容量：NppmOutCapBytes 的钳制与边界
//   [7] wire 布局：调用/回包的内存往返（偏移算术错了就是这里红）
//   [8] 值槽契约：kValue 的 lp 是值、其余形状的 lp 是指针槽（批次 117）
//   [9] 指针槽契约：wp 默认是值、只有 kDmmTwoStr 是指针槽（批次 117b）
//       —— 两个槽的**默认方向相反**，写反任一条都是静默错
//
// ★ 本守卫的边界（写进输出，避免下一个读它的人高估它）：
//   它证明「kNppKnownMsgs ↔ kNppmTable」一致，**不**证明「kNppKnownMsgs ↔
//   NppMsg 枚举」一致 —— 枚举不可遍历，后者靠 NppMessages.h 里紧邻枚举的
//   那条纪律（加枚举值必须同步加清单）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/plugin/oop/NppmMarshal.h"

namespace oop = xfs::oop;
namespace npp = xfs::npp;

static int g_fail = 0;
// 段落内新增的失败数：段落标签靠它保持**诚实**（见 SectionEnd）。
static int g_sinceLabel = 0;
// 段落内的失败统一走 FAIL()：CHECK 与手写分支都计数，标签才不会说谎。
#define FAIL() do { ++g_fail; ++g_sinceLabel; } while (0)
#define CHECK(cond) do { if (!(cond)) { FAIL(); \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ★ 段落标签必须诚实：无条件打印 "ok" 会把红说成绿 —— 这个缺陷是批次 117a 的
//   负控照出来的（把 NppmLpIsValue 改成恒 false 后，[8] 明明有 10 条 FAIL，
//   末行却照样打印 "[8] value slots: ok"）。标签说谎比没有标签更危险，因为
//   "grep ok" 正是核对守卫的常规手段。
static void SectionEnd(const char* label) {
    if (g_sinceLabel == 0) {
        std::printf("%s: ok\n", label);
    } else {
        std::printf("%s: FAILED (%d new failure(s))\n", label, g_sinceLabel);
    }
    g_sinceLabel = 0;
}

static int CountShape(oop::NppmShape sh) {
    int n = 0;
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i)
        if (oop::kNppmTable[i].shape == sh) ++n;
    return n;
}

static bool InKnownList(unsigned msg) {
    for (std::size_t i = 0; i < oop::kNppmKnownCount; ++i)
        if (oop::kNppmKnownMsgs[i] == msg) return true;
    return false;
}

static bool InTable(unsigned msg) {
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i)
        if (oop::kNppmTable[i].msg == msg) return true;
    return false;
}

// ---- [1] 完备性：双向对账 ----------------------------------------------------
static void TestCompleteness() {
    std::printf("[1] completeness: %zu known, %zu classified\n",
                oop::kNppmKnownCount, oop::kNppmTableCount);
    CHECK(oop::kNppmTableCount == oop::kNppmKnownCount);

    // 清单 → 表：声明了却"没分类"= 静默返回 0 的源头
    for (std::size_t i = 0; i < oop::kNppmKnownCount; ++i)
        if (!InTable(oop::kNppmKnownMsgs[i])) {
            FAIL();
            std::printf("FAIL declared but unclassified: 0x%X\n",
                        oop::kNppmKnownMsgs[i]);
        }
    // 表 → 清单：表里有、清单没有 = 清单漏加（或表写错编号）
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i)
        if (!InKnownList(oop::kNppmTable[i].msg)) {
            FAIL();
            std::printf("FAIL table entry not in kNppKnownMsgs: 0x%X\n",
                        oop::kNppmTable[i].msg);
        }
    // 清单自身无重复（重复会让上面对账的条数对不上却看不出来）
    for (std::size_t i = 0; i < oop::kNppmKnownCount; ++i)
        for (std::size_t j = i + 1; j < oop::kNppmKnownCount; ++j)
            if (oop::kNppmKnownMsgs[i] == oop::kNppmKnownMsgs[j]) {
                FAIL();
                std::printf("FAIL duplicate in kNppKnownMsgs: 0x%X\n",
                            oop::kNppmKnownMsgs[i]);
            }
    SectionEnd("[1] completeness");
}

// ---- [2] 唯一性 -------------------------------------------------------------
static void TestUniqueness() {
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i)
        for (std::size_t j = i + 1; j < oop::kNppmTableCount; ++j)
            if (oop::kNppmTable[i].msg == oop::kNppmTable[j].msg) {
                FAIL();
                std::printf("FAIL duplicate in kNppmTable: 0x%X\n",
                            oop::kNppmTable[i].msg);
            }
    SectionEnd("[2] uniqueness");
}

// ---- [3] 分类：条数 + 代表项 -------------------------------------------------
// 条数被钉住的意义：新增一条消息时若顺手给了 kUnsupported（"先不支持"），
// kUnsupported 的条数就会变 ⇒ 这里红 ⇒ 逼着人显式做决定而不是默默降级。
static void TestClassification() {
    // ★ 批次 117：6 条 NPPM_DMM* 从 kUnsupported 升级 ⇒ 三个数字同时变
    //   （kUnsupported 11→5、kValue 9→12、kInWideStr 3→4），并多出两个新形状。
    // ★ 批次 120：`NPPM_ISTABBARHIDDEN`（原名误作上游不存在的 `NPPM_GETMENUBAR`）
    //   从 kUnsupported 改为 kValue（它返回 BOOL，没有指针）⇒ kValue 12→13、
    //   kUnsupported 5→4。**升级/改类都是显式表态**，数字变了就得在这里表态。
    struct { oop::NppmShape sh; int expect; } want[] = {
        { oop::NppmShape::kValue,       13 },
        { oop::NppmShape::kInWideStr,    4 },
        { oop::NppmShape::kOutInt,       2 },
        { oop::NppmShape::kOutStruct4,   1 },
        { oop::NppmShape::kOutWideBuf,   8 },
        { oop::NppmShape::kDmmReg,       1 },
        { oop::NppmShape::kDmmTwoStr,    1 },
        { oop::NppmShape::kUnsupported,  4 },
    };
    int sum = 0;
    for (const auto& w : want) {
        const int got = CountShape(w.sh);
        sum += got;
        if (got != w.expect) {
            FAIL();
            std::printf("FAIL shape %s: got %d, want %d\n",
                        oop::NppmShapeName(w.sh), got, w.expect);
        }
    }
    CHECK(sum == (int)oop::kNppmTableCount);

    // 代表项：把"这一条到底怎么过桥"钉死
    struct { unsigned msg; oop::NppmShape sh; } reps[] = {
        { npp::NPPM_GETNPPVERSION,      oop::NppmShape::kValue },
        { npp::NPPM_GETCURRENTBUFFERID, oop::NppmShape::kValue },
        { npp::NPPM_SWITCHTOFILE,       oop::NppmShape::kInWideStr },
        { npp::NPPM_DOOPEN,             oop::NppmShape::kInWideStr },
        { npp::NPPM_GETCURRENTSCINTILLA, oop::NppmShape::kOutInt },
        { npp::NPPM_ALLOCATECMDID,      oop::NppmShape::kOutInt },
        { npp::NPPM_GETSHORTCUTBYCMDID, oop::NppmShape::kOutStruct4 },
        { npp::NPPM_GETFULLCURRENTPATH, oop::NppmShape::kOutWideBuf },
        { npp::NPPM_GETFULLPATHFROMBUFFERID, oop::NppmShape::kOutWideBuf },
        { npp::NPPM_GETMENUHANDLE,      oop::NppmShape::kUnsupported },
        { npp::NPPM_ISTABBARHIDDEN,     oop::NppmShape::kValue },
        { npp::NPPM_GETOPENFILENAMES_DEPRECATED, oop::NppmShape::kUnsupported },
        // ★ 停靠族 6 条：批次 117 从"拒答"升级为"过桥"，逐条钉住（只留一条
        //   代表会让另外 5 条可以悄悄退回 kUnsupported 而不被守卫发现）。
        { npp::NPPM_DMMSHOW,           oop::NppmShape::kValue },
        { npp::NPPM_DMMHIDE,           oop::NppmShape::kValue },
        { npp::NPPM_DMMUPDATEDISPINFO, oop::NppmShape::kValue },
        { npp::NPPM_DMMVIEWOTHERTAB,   oop::NppmShape::kInWideStr },
        { npp::NPPM_DMMREGASDCKDLG,        oop::NppmShape::kDmmReg },
        { npp::NPPM_DMMGETPLUGINHWNDBYNAME, oop::NppmShape::kDmmTwoStr },
    };
    for (const auto& r : reps)
        if (oop::NppmShapeOf(r.msg) != r.sh) {
            FAIL();
            std::printf("FAIL rep 0x%X: got %s, want %s\n", r.msg,
                        oop::NppmShapeName(oop::NppmShapeOf(r.msg)),
                        oop::NppmShapeName(r.sh));
        }
    SectionEnd("[3] classification");
}

// ---- [4] 不变式：why 只属于 kUnsupported -------------------------------------
static void TestWhyInvariant() {
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i) {
        const auto& e = oop::kNppmTable[i];
        if (e.shape == oop::NppmShape::kUnsupported) {
            if (!e.why || !e.why[0]) {
                FAIL();
                std::printf("FAIL unsupported without reason: 0x%X\n", e.msg);
            }
        } else if (e.why) {
            FAIL();
            std::printf("FAIL supported entry carries a reason: 0x%X\n", e.msg);
        }
    }
    // 表外编号：不支持、且没有"原因"（原因只写给显式决定过的那些）
    CHECK(oop::NppmShapeOf(0xDEADBEEF) == oop::NppmShape::kUnsupported);
    CHECK(oop::NppmUnsupportedWhy(0xDEADBEEF) == nullptr);
    CHECK(oop::NppmUnsupportedWhy(npp::NPPM_GETFULLCURRENTPATH) == nullptr);
    CHECK(oop::NppmUnsupportedWhy(npp::NPPM_GETMENUHANDLE) != nullptr);
    CHECK(!oop::NppmSupported(npp::NPPM_GETMENUHANDLE));
    CHECK(oop::NppmSupported(npp::NPPM_GETFULLCURRENTPATH));

    // ★ 批次 117：停靠族 6 条**必须**是受支持且**不带原因**。原来的理由是
    //   "needs relay + DockManager whitelist"，两进程实测推翻了它 ⇒ 若有人
    //   把某条退回 kUnsupported，这里红（同时 [3] 的条数也会红）。
    const unsigned dmmFamily[] = {
        npp::NPPM_DMMSHOW, npp::NPPM_DMMHIDE, npp::NPPM_DMMUPDATEDISPINFO,
        npp::NPPM_DMMVIEWOTHERTAB, npp::NPPM_DMMREGASDCKDLG,
        npp::NPPM_DMMGETPLUGINHWNDBYNAME,
    };
    for (unsigned m : dmmFamily) {
        if (!oop::NppmSupported(m)) {
            FAIL();
            std::printf("FAIL DMM family regressed to unsupported: 0x%X\n", m);
        }
        if (oop::NppmUnsupportedWhy(m) != nullptr) {
            FAIL();
            std::printf("FAIL DMM family carries a stale reason: 0x%X\n", m);
        }
    }
    SectionEnd("[4] why-invariant");
}

// ---- [5] 区间 ---------------------------------------------------------------
static void TestRanges() {
    // 表内每条都必须落在宿主拦截区间里，否则"中转窗接管了但宿主不认"
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i)
        if (!oop::NppmInRanges(oop::kNppmTable[i].msg)) {
            FAIL();
            std::printf("FAIL table entry outside NPPM ranges: 0x%X\n",
                        oop::kNppmTable[i].msg);
        }
    CHECK(oop::NppmInRanges(npp::kNppMsgBase));
    CHECK(oop::NppmInRanges(npp::kNppMsgBase + 128));
    CHECK(!oop::NppmInRanges(npp::kNppMsgBase + 129));
    CHECK(!oop::NppmInRanges(npp::kNppMsgBase - 1));
    CHECK(oop::NppmInRanges(npp::kRunCmdBase));
    CHECK(oop::NppmInRanges(npp::kRunCmdBase + 64));
    CHECK(!oop::NppmInRanges(npp::kRunCmdBase + 65));
    CHECK(!oop::NppmInRanges(WM_USER));
    CHECK(!oop::NppmInRanges(WM_COPYDATA));
    SectionEnd("[5] ranges");
}

// ---- [6] 出参容量钳制 --------------------------------------------------------
static void TestOutCap() {
    // kValue / kUnsupported：一律不要出参
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kValue, 260, 0x1000) == 0);
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kUnsupported, 260, 0x1000) == 0);
    // 定长：lp==0 时只问返回值
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kOutInt, 0, 0) == 0);
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kOutInt, 0, 0x1000) == 4);
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kOutStruct4, 0, 0x1000) == 4);
    // 变长：wp 是调用方声明的容量（wchar_t 计）
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kOutWideBuf, 260, 0) == 0);
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kOutWideBuf, 260, 0x1000) == 520);
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kOutWideBuf, 0, 0x1000) == 0);
    // 上限钳制：不管调用方要多少，都不能越过载荷上限
    CHECK(oop::NppmOutCapBytes(oop::NppmShape::kOutWideBuf, 0xFFFFFFFFu, 0x1000)
          == oop::kNppmOutWCharsMax * sizeof(wchar_t));
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i) {
        const unsigned cap = oop::NppmOutCapBytes(
            oop::kNppmTable[i].shape, (WPARAM)0xFFFFFFFFFFFFFFFFull, (LPARAM)0x1000);
        if (cap > oop::kNppmPayloadMax) {
            FAIL();
            std::printf("FAIL outCap exceeds payload max: 0x%X -> %u\n",
                        oop::kNppmTable[i].msg, cap);
        }
    }
    // 入参串上限也必须在载荷上限之内（否则中转窗读出来的串放不进载荷）
    CHECK(oop::kNppmInCharsMax * sizeof(wchar_t) <= oop::kNppmPayloadMax);
    CHECK(oop::kNppmOutWCharsMax * sizeof(wchar_t) <= oop::kNppmPayloadMax);
    SectionEnd("[6] out-cap");
}

// ---- [7] wire 布局往返 -------------------------------------------------------
// 照 PluginHostMain.cpp 的写法构造、照 OopHost.cpp 的读法解析。偏移算术
// （结构体尾部接变长载荷）是那种"编译过、跑起来读到隔壁字节"的缺陷。
// ★ 首两字段的取值是**协议契约**：两侧 WndProc 都先校验 wire[0]==kMagic、
//   再按 wire[1] 分派。这里显式钉住它 —— 本批第一版把 NPPM_* 编号写在 msg
//   上，症状是"所有 NPPM_* 静默返回 0、而 WM_COPYDATA 标记照常送达"。
static void TestWireLayout() {
    const wchar_t* arg = L"D:\\proj\\oop\\relay.txt";
    const std::size_t argBytes = (wcslen(arg) + 1) * sizeof(wchar_t);
    std::vector<unsigned char> buf(sizeof(oop::NppmCallWire) + argBytes);
    auto* cw = reinterpret_cast<oop::NppmCallWire*>(buf.data());
    cw->magic = xfs::oop::kMagic;
    cw->msg = xfs::oop::OOPM_NPPMCALL;
    cw->nppm = npp::NPPM_SWITCHTOFILE;
    cw->wParam = 0;
    cw->reqId = 42;
    cw->replyTo = 0x1234;
    cw->inBytes = (unsigned)argBytes;
    cw->outCap = 0;
    cw->lParam = 0xABCD;                 // 值槽往返（批次 117 新增的那一格）
    std::memcpy(buf.data() + sizeof(*cw), arg, argBytes);

    auto* rd = reinterpret_cast<const oop::NppmCallWire*>(buf.data());
    // wire[0]/wire[1] 分派契约（WndProc 在分发前就查这两个）
    const UINT_PTR* wire = reinterpret_cast<const UINT_PTR*>(buf.data());
    CHECK(wire[0] == xfs::oop::kMagic);
    CHECK(wire[1] == xfs::oop::OOPM_NPPMCALL);
    CHECK(rd->nppm == (unsigned)npp::NPPM_SWITCHTOFILE);
    CHECK(rd->reqId == 42 && rd->replyTo == 0x1234);
    CHECK(rd->inBytes <= (unsigned)(buf.size() - sizeof(*rd)));
    CHECK(rd->lParam == 0xABCD);         // ★ 值槽必须真的在 wire 里（批次 117）
    // 载荷起点必须在结构体**之后** —— 尾部追加一格最容易把载荷顶歪
    CHECK(sizeof(oop::NppmCallWire) % sizeof(UINT_PTR) == 0);
    CHECK(offsetof(oop::NppmCallWire, lParam) > offsetof(oop::NppmCallWire, outCap));
    const wchar_t* back = reinterpret_cast<const wchar_t*>(
        reinterpret_cast<const unsigned char*>(rd) + sizeof(*rd));
    CHECK(std::wcscmp(back, arg) == 0);

    // 回包方向
    const wchar_t* txt = L"relay.txt";
    const std::size_t txtBytes = (wcslen(txt) + 1) * sizeof(wchar_t);
    std::vector<unsigned char> rbuf(sizeof(oop::NppmReplyWire) + txtBytes);
    auto* rw = reinterpret_cast<oop::NppmReplyWire*>(rbuf.data());
    rw->magic = xfs::oop::kMagic;
    rw->msg = xfs::oop::OOPM_NPPMREPLY;
    rw->nppm = npp::NPPM_GETFILENAME;
    rw->reqId = 42;
    rw->result = (LONG_PTR)TRUE;
    rw->outBytes = (unsigned)txtBytes;
    std::memcpy(rbuf.data() + sizeof(*rw), txt, txtBytes);

    auto* rr = reinterpret_cast<const oop::NppmReplyWire*>(rbuf.data());
    const UINT_PTR* rwire = reinterpret_cast<const UINT_PTR*>(rbuf.data());
    CHECK(rwire[0] == xfs::oop::kMagic);
    CHECK(rwire[1] == xfs::oop::OOPM_NPPMREPLY);
    CHECK(rr->nppm == (unsigned)npp::NPPM_GETFILENAME);
    CHECK(rr->reqId == 42);
    CHECK(rr->result == (LONG_PTR)TRUE);
    CHECK(rr->outBytes <= (unsigned)(rbuf.size() - sizeof(*rr)));
    const wchar_t* rback = reinterpret_cast<const wchar_t*>(
        reinterpret_cast<const unsigned char*>(rr) + sizeof(*rr));
    CHECK(std::wcscmp(rback, txt) == 0);

    // 两个消息 id 必须互不相同、且与既有 id 不撞（同一条 switch 上分派）
    CHECK(xfs::oop::OOPM_NPPMCALL != xfs::oop::OOPM_NPPMREPLY);
    for (UINT_PTR other = xfs::oop::OOPM_HANDSHAKE;
         other <= xfs::oop::OOPM_MSGREPLY; ++other) {
        if (other == xfs::oop::OOPM_NPPMCALL || other == xfs::oop::OOPM_NPPMREPLY) {
            FAIL();
            std::printf("FAIL new msg id collides with an existing one: %llu\n",
                        (unsigned long long)other);
        }
    }

    // 载荷上限与"请求的出参字节数"必须能被同一个 unsigned 表达
    CHECK(oop::kNppmPayloadMax <= 0xFFFFFFFFu);
    SectionEnd("[7] wire layout");
}

// ---- [8] 值槽契约（批次 117）-------------------------------------------------
// kValue 的 lp 是**值**，必须真的过桥。这一格原来不存在（wire 里只有 wParam），
// 于是 kValue 消息的 lp 被静默换成 0 —— 而表里真有 4 条 kValue 读 lp。
// 本段把"哪个槽是值"钉成契约：新增形状漏表态 ⇒ 这里红，而不是等运行时静默错。
static void TestValueSlots() {
    using oop::NppmShape;
    // kValue：两个槽都是值 ⇒ lp 必须原样透传
    CHECK(oop::NppmLpIsValue(NppmShape::kValue));
    // 其余形状的 lp 都是指针/缓冲 ⇒ 一律填 0，绝不把外来地址放上 wire
    CHECK(!oop::NppmLpIsValue(NppmShape::kInWideStr));
    CHECK(!oop::NppmLpIsValue(NppmShape::kOutInt));
    CHECK(!oop::NppmLpIsValue(NppmShape::kOutStruct4));
    CHECK(!oop::NppmLpIsValue(NppmShape::kOutWideBuf));
    CHECK(!oop::NppmLpIsValue(NppmShape::kDmmReg));      // lp = DockedWidgetData*
    CHECK(!oop::NppmLpIsValue(NppmShape::kDmmTwoStr));   // lp = 模块名串
    CHECK(!oop::NppmLpIsValue(NppmShape::kUnsupported));

    // ★ 分类与判据必须**双向**一致（只有 kValue 的 lp 是值）。
    //   单向检查会漏掉"某形状被判据当成值、分类却不是"的那种漂移。
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i) {
        const auto sh = oop::kNppmTable[i].shape;
        const bool isValue = oop::NppmLpIsValue(sh);
        if (isValue != (sh == NppmShape::kValue)) {
            FAIL();
            std::printf("FAIL value-slot drift: msg=0x%X shape=%s\n",
                        oop::kNppmTable[i].msg, oop::NppmShapeName(sh));
        }
    }

    // ★ 4 条"读 lp 的 kValue"必须留在 kValue 里：把它们挪去别的形状会让 lp
    //   变成指针槽（填 0）⇒ 本批修的缺陷原地复活。判据来自宿主侧实现
    //   （PluginManager::ForwardNppMessage 里这 4 条真的读了 lp）。
    const unsigned lpReaders[] = {
        npp::NPPM_SETMENUITEMCHECK, npp::NPPM_GETNBOPENFILES,
        npp::NPPM_RELOADBUFFERID,   npp::NPPM_GETBUFFERIDFROMPOS,
    };
    for (unsigned m : lpReaders)
        CHECK(oop::NppmShapeOf(m) == NppmShape::kValue);
    SectionEnd("[8] value slots");
}

// ---- [9] 指针槽契约：wp 的默认方向与 lp **相反**（批次 117b）-------------------
// 本段存在的理由：lp 默认是**指针**（只有 kValue 是值），wp 默认是**值**
// （只有 kDmmTwoStr 是指针）。两个默认相反 ⇒ 很容易被"对称化"成同一个判据，
// 而 117a 的负控已经证明过：把 wp 的判据写成 lp 的镜像（只认 kValue）会把
// kOutWideBuf 的**容量**填 0 —— 插件拿不到数据，且**不崩**。
// 所以这里同时钉两件事：① 只有 kDmmTwoStr 的 wp 是指针；② 判据本身与
// "wp 会被消费"的形状集合一致（kValue / kOutWideBuf / kOutInt / kOutStruct4
// 都不是指针，即它们的 wp 必须原样透传）。
static void TestWpSlots() {
    using oop::NppmShape;
    // 唯一把指针放在 wp 的形状
    CHECK(oop::NppmWpIsPointer(NppmShape::kDmmTwoStr));
    // 其余形状的 wp 都是值（或 0），必须原样透传
    CHECK(!oop::NppmWpIsPointer(NppmShape::kValue));
    CHECK(!oop::NppmWpIsPointer(NppmShape::kInWideStr));
    CHECK(!oop::NppmWpIsPointer(NppmShape::kOutInt));
    CHECK(!oop::NppmWpIsPointer(NppmShape::kOutStruct4));
    CHECK(!oop::NppmWpIsPointer(NppmShape::kOutWideBuf));   // wp=容量，会被用！
    CHECK(!oop::NppmWpIsPointer(NppmShape::kDmmReg));
    CHECK(!oop::NppmWpIsPointer(NppmShape::kUnsupported));

    // ★ 判据与表**双向**一致（单向会漏掉"判据认了但表里没有"的漂移）
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i) {
        const auto sh = oop::kNppmTable[i].shape;
        if (oop::NppmWpIsPointer(sh) != (sh == NppmShape::kDmmTwoStr)) {
            FAIL();
            std::printf("FAIL wp-pointer drift: msg=0x%X shape=%s\n",
                        oop::kNppmTable[i].msg, oop::NppmShapeName(sh));
        }
    }

    // ★ 与 lp 判据**必须不是同一个集合**：若有人把两者对称化，这条会红。
    //   （lp 的值槽是 kValue；wp 的指针槽是 kDmmTwoStr ⇒ 两集合不相交。）
    for (std::size_t i = 0; i < oop::kNppmTableCount; ++i) {
        const auto sh = oop::kNppmTable[i].shape;
        if (oop::NppmLpIsValue(sh) && oop::NppmWpIsPointer(sh)) {
            FAIL();
            std::printf("FAIL lp/wp contracts collapsed onto one shape: 0x%X\n",
                        oop::kNppmTable[i].msg);
        }
    }

    // ★ 三条"读 lp 的 kValue"里 wp 也不能被误当指针（它们是值形状 ⇒ wp 透传）
    CHECK(!oop::NppmWpIsPointer(oop::NppmShapeOf(npp::NPPM_GETNBOPENFILES)));
    CHECK(!oop::NppmWpIsPointer(oop::NppmShapeOf(npp::NPPM_SETMENUITEMCHECK)));
    SectionEnd("[9] wp slots");
}

int main() {
    std::printf("== test_nppm_marshal: NPPM_* relay classification guard ==\n");
    TestCompleteness();
    TestUniqueness();
    TestClassification();
    TestWhyInvariant();
    TestRanges();
    TestOutCap();
    TestValueSlots();
    TestWpSlots();
    TestWireLayout();
    std::printf("NOTE: this guard proves kNppKnownMsgs <-> kNppmTable.\n");
    std::printf("      The other half - kNppKnownMsgs <-> NppMsg enum - is NOT proved\n");
    std::printf("      here (enums are not iterable from C++), but batch 120 closed it\n");
    std::printf("      in scripts/check-nppm-contract.py rule R4, which parses the enum\n");
    std::printf("      BODY as text and reconciles it with the list in both directions.\n");
    std::printf("      That script also checks every name/value against upstream.\n");
    std::printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
