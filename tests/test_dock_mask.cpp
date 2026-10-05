// test_dock_mask.cpp — 批次 119：DockedWidgetData.uMask 的解码判据。
//
// 【为什么值得单独一个目标】
//   本项目的 dock 体系**只实现底部容器**，其余请求一律降级。批次 119 之前这件事
//   只写在注释里，而且注释与实现**不一致**（注释说"浮动/左/右/上一律按底部处理并
//   记日志"，实现里只有浮动打了日志）。把解码抽成纯函数（npp/NppDockMask.h）之后，
//   位编码里的两个坑才谈得上"钉住"：
//     1. `kDwsDfContLeft == 0` 与"完全没指定"在编码上重合 ⇒ 显式请求左容器不可表达；
//     2. `kDwsDfFloating` 与容器位是两个不重叠字段，可同时置 ⇒ 矛盾请求要定优先级。
//   这两条都不是"实现取舍"，而是上游位编码的固有性质 —— 所以判据要写清**我们的口径**，
//   而不是假装能做到。
//
// 【判据的两半】（本项目纪律：放行/降级类判据必须含"名单外的活样本"）
//   · 会降级的：浮动（唯一）⇒ IsDockRequestCoerced 必须为真；
//   · **不会**降级的：未指定 / 显式底部（含编码上重合的左）/ 右 / 上（批次 122
//     起为真实容器）⇒ 必须为假。
//   只测前一半 = 只测"名字对得上"。

#include <cstdio>
#include <string>

#include "../src/plugin/npp/NppDockMask.h"

using namespace xfs::npp;

// ---- 诚实标签骨架（照抄既有守卫，别手打标签）-----------------------------------
// 裸 printf("[n] ...: ok\n") 会在段落已经失败时照样打印 ok，把红说成绿。
static int g_fail = 0;
static int g_sinceLabel = 0;
#define FAIL() do { ++g_fail; ++g_sinceLabel; } while (0)
#define CHECK(cond) do { if (!(cond)) { FAIL(); \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void SectionEnd(const char* label) {
    if (g_sinceLabel == 0) {
        std::printf("%s: ok\n", label);
    } else {
        std::printf("%s: FAILED (%d new failure(s))\n", label, g_sinceLabel);
    }
    g_sinceLabel = 0;
}

// 把枚举打成可读名字（失败时能直接看出是哪一档）。
static const char* Name(DockRequest r) { return DockRequestName(r); }

static void CheckReq(UINT mask, DockRequest want, const char* what) {
    const DockRequest got = RequestedDock(mask);
    if (got == want) return;
    FAIL();
    std::printf("  %s: mask=0x%08X want=%s got=%s\n", what,
                (unsigned)mask, Name(want), Name(got));
}

// =============================================================================

static void TestContainerEncoding() {
    // 未指定 ⇒ 底部（与既有夹具 uMask = 0 的口径一致）。
    CheckReq(0u, DockRequest::Bottom, "[1] uMask=0 ⇒ bottom");
    // 容器位在 bits28-29；kContLeft/Right/Top/Bottom = 0/1/2/3。
    CheckReq(kDwsDfContBottom, DockRequest::Bottom, "[1] 显式 bottom");
    CheckReq(kDwsDfContRight, DockRequest::Right, "[1] 显式 right");
    CheckReq(kDwsDfContTop, DockRequest::Top, "[1] 显式 top");
    CheckReq(kDwsDfFloating, DockRequest::Floating, "[1] 显式 floating");
    // 位位置本身也要钉住：容器编号必须落在 bits28-29。
    CHECK((kDwsDfContRight >> 28) == (UINT)kContRight);
    CHECK((kDwsDfContTop >> 28) == (UINT)kContTop);
    CHECK((kDwsDfContBottom >> 28) == (UINT)kContBottom);
    SectionEnd("[1] 五种请求的位编码");
}

static void TestLeftIsIndistinguishable() {
    // ★ 坑 1：kContLeft == 0 ⇒ kDwsDfContLeft == 0，与"未指定"完全重合。
    //   所以"显式请求左容器"在编码上**不可表达**，只能按"未指定 = 底部"处理。
    CHECK(kDwsDfContLeft == 0u);
    CheckReq(kDwsDfContLeft, DockRequest::Bottom, "[2] 显式 left（== 0）只能按未指定处理");
    // 反证：把它当成"左"会与 uMask=0 的默认底部冲突，而夹具就是按 uMask=0 = 底部写的。
    CHECK(RequestedDock(kDwsDfContLeft) == RequestedDock(0u));
    SectionEnd("[2] ★左容器与「未指定」在编码上重合");
}

static void TestFloatingWinsOverContainer() {
    // ★ 坑 2：bit31（floating）与 bits28-29（容器）是两个不重叠的字段，可以同时置。
    //   同时置 = 矛盾请求 ⇒ 取**更强**的"浮动"。
    CheckReq(kDwsDfFloating | kDwsDfContRight, DockRequest::Floating,
             "[3] floating|right ⇒ floating");
    CheckReq(kDwsDfFloating | kDwsDfContTop, DockRequest::Floating,
             "[3] floating|top ⇒ floating");
    CheckReq(kDwsDfFloating | kDwsDfContBottom, DockRequest::Floating,
             "[3] floating|bottom ⇒ floating");
    CheckReq(kDwsDfFloating | kDwsDfContLeft, DockRequest::Floating,
             "[3] floating|left(==0) ⇒ floating");
    SectionEnd("[3] ★矛盾请求：floating 优先于容器位");
}

static void TestFeatureBitsDoNotInterfere() {
    // 特性位（IconTab / IconBar / AddInfo）与容器解码无关：8 种组合 × 5 种请求
    // 必须逐一得到同一个答案。穷举而不是抽样 —— 这类"位域串味"的缺陷正是抽样漏掉的。
    const UINT features[8] = {
        0u,
        kDwsIconTab,
        kDwsIconBar,
        kDwsIconTab | kDwsIconBar,
        kDwsAddInfo,
        kDwsAddInfo | kDwsIconTab,
        kDwsAddInfo | kDwsIconBar,
        kDwsAddInfo | kDwsIconTab | kDwsIconBar,
    };
    const UINT requests[5] = { 0u, kDwsDfContRight, kDwsDfContTop, kDwsDfContBottom,
                               kDwsDfFloating };
    int cases = 0;
    for (UINT f : features) {
        for (UINT r : requests) {
            const UINT mask = f | r;
            if (RequestedDock(mask) != RequestedDock(r)) {
                FAIL();
                std::printf("  [4] 特性位串味: mask=0x%08X ⇒ %s（裸请求是 %s）\n",
                            (unsigned)mask, Name(RequestedDock(mask)),
                            Name(RequestedDock(r)));
            }
            ++cases;
        }
    }
    // "项数"本身就是断言（8 × 5 = 40）：循环没跑够也说明穷举没做完。
    CHECK(cases == 40);
    SectionEnd("[4] 特性位不干扰容器解码（8×5 穷举）");
}

static void TestCoercionPredicateBothHalves() {
    // 批次 123：五种请求（底/左/右/上/浮动）全部有真实容器 ⇒ 不再有任何降级。
    // 谓词保留恒假（兼容报告口径），本节验证它对**全类型**都为假（不挑档位）。
    CHECK(!IsDockRequestCoerced(0u));
    CHECK(!IsDockRequestCoerced(kDwsDfContBottom));
    CHECK(!IsDockRequestCoerced(kDwsDfContLeft));
    CHECK(!IsDockRequestCoerced(kDwsDfContRight));
    CHECK(!IsDockRequestCoerced(kDwsDfContTop));
    CHECK(!IsDockRequestCoerced(kDwsDfFloating));
    CHECK(!IsDockRequestCoerced(kDwsAddInfo | kDwsDfFloating));   // 特性位不影响
    SectionEnd("[5] 批次 123：五容器齐备，无降级（谓词恒假）");
}

static void TestPanelTitle() {
    const UINT bottomInfo = kDwsDfContBottom | kDwsAddInfo;
    const UINT bottomOnly = kDwsDfContBottom;
    // 正常：置了 kDwsAddInfo 且附加信息非空 ⇒ 接在后面
    CHECK(DockPanelTitle(L"Panel", L"extra text", bottomInfo) == L"Panel — extra text");
    // 没置 kDwsAddInfo ⇒ 只有名字（附加信息被忽略）
    CHECK(DockPanelTitle(L"Panel", L"extra text", bottomOnly) == L"Panel");
    // 置了 kDwsAddInfo 但附加信息是空串 / NULL ⇒ 只有名字（不留下孤立的分隔符）
    CHECK(DockPanelTitle(L"Panel", L"", bottomInfo) == L"Panel");
    CHECK(DockPanelTitle(L"Panel", nullptr, bottomInfo) == L"Panel");
    // 名字为 NULL ⇒ 空标题（不崩）
    CHECK(DockPanelTitle(nullptr, L"extra", bottomInfo) == L" — extra");
    SectionEnd("[6] 标题口径（kDwsAddInfo 才拼接）");
}

static void TestRequestNames() {
    // 日志与守卫输出靠这张名字表；缺一档就会打 "?"，那种日志等于没打。
    const DockRequest all[5] = { DockRequest::Bottom, DockRequest::Left,
                                 DockRequest::Right, DockRequest::Top,
                                 DockRequest::Floating };
    for (DockRequest r : all) {
        const char* n = DockRequestName(r);
        CHECK(n && n[0] && n[0] != '?');
    }
    CHECK(std::string(DockRequestName(DockRequest::Bottom)) == "bottom");
    CHECK(std::string(DockRequestName(DockRequest::Right)) == "right");
    CHECK(std::string(DockRequestName(DockRequest::Top)) == "top");
    CHECK(std::string(DockRequestName(DockRequest::Floating)) == "floating");
    // 互不相同（复制粘贴写错档位时，这条会红）
    CHECK(std::string(DockRequestName(DockRequest::Bottom)) !=
          std::string(DockRequestName(DockRequest::Right)));
    SectionEnd("[7] 请求名字表完整且互不相同");
}

// 批次 127 补测：批次 126 把吸附判据抽成纯函数时只写了"可单测"，
// **没有真的建这条断言** —— 未实测的覆盖不算覆盖，本节把它补上。
static void TestSnapEdgeTo() {
    // 宿主框架 {0,0,1000,800}（屏幕坐标），吸附带 48。
    const RECT frame{ 0, 0, 1000, 800 };
    const int zone = 48;
    DockRequest out = DockRequest::Floating;

    // ---- 不该吸附的那一半：离四边都远 / 带外 ----------------------------
    out = DockRequest::Floating;
    CHECK(!SnapEdgeTo(frame, POINT{ 500, 400 }, zone, &out));   // 正中
    CHECK(!SnapEdgeTo(frame, POINT{ 500, 848 + 1 }, zone, &out));  // 下带外 1px
    CHECK(!SnapEdgeTo(frame, POINT{ 500, -48 - 1 }, zone, &out));  // 上带外 1px
    CHECK(!SnapEdgeTo(frame, POINT{ -48 - 1, 400 }, zone, &out));  // 左带外 1px
    CHECK(!SnapEdgeTo(frame, POINT{ 1000 + 48 + 1, 400 }, zone, &out)); // 右带外
    // 纵向带外时，横向再贴边也不算（inX/inY 必须同时成立）。
    // 注意 y 要真出带：带内上界是 frame.bottom + zone = 848，449 仍在带内。
    CHECK(!SnapEdgeTo(frame, POINT{ 2, 848 + 1 }, zone, &out));

    // ---- 该吸附的那一半：四条边各取一点 --------------------------------
    const POINT mid[4] = { { 2, 400 }, { 998, 400 }, { 500, 2 }, { 500, 798 } };
    const DockRequest want[4] = { DockRequest::Bottom,   // ★左边缘 ⇒ 改判 Bottom
                                  DockRequest::Right,
                                  DockRequest::Top,
                                  DockRequest::Bottom };
    int snapped = 0;
    for (int i = 0; i < 4; ++i) {
        out = DockRequest::Floating;
        const bool got = SnapEdgeTo(frame, mid[i], zone, &out);
        CHECK(got);
        CHECK(out == want[i]);
        if (got) ++snapped;
    }
    CHECK(snapped == 4);   // "项数"本身就是断言：循环没跑完也算失败

    // ★ 编码重合：Left 与"未指定"同值 ⇒ 左边缘**永远不返回 Left**。
    //   只断言"返回 Bottom"不够 —— 还要断言它没退化成 Floating（没吸附）。
    out = DockRequest::Floating;
    CHECK(SnapEdgeTo(frame, POINT{ 1, 400 }, zone, &out));
    CHECK(out != DockRequest::Left);
    CHECK(out != DockRequest::Floating);

    // ---- 角点：两轴同时近边时取**先判**的那条（Left 优先被吸附，再改判 Bottom）
    out = DockRequest::Floating;
    CHECK(SnapEdgeTo(frame, POINT{ 2, 2 }, zone, &out));
    CHECK(out == DockRequest::Bottom);          // 左上角 ⇒ Left ⇒ 改判 Bottom
    out = DockRequest::Floating;
    CHECK(SnapEdgeTo(frame, POINT{ 998, 2 }, zone, &out));
    CHECK(out == DockRequest::Right);           // 右上角：dR 严格小于 dT 才轮到它

    // ---- 非法入参：zone <= 0 / out == nullptr ⇒ 恒 false（不写 out）------
    CHECK(!SnapEdgeTo(frame, POINT{ 2, 400 }, 0, &out));
    CHECK(!SnapEdgeTo(frame, POINT{ 2, 400 }, -1, &out));
    CHECK(!SnapEdgeTo(frame, POINT{ 2, 400 }, zone, nullptr));
    // 边界恰好 = zone：dL == zone 时 bestD == zone，仍算带内（> zone 才 false）
    out = DockRequest::Floating;
    CHECK(SnapEdgeTo(frame, POINT{ 48, 400 }, zone, &out));
    CHECK(out == DockRequest::Bottom);
    SectionEnd("[8] 拖回吸附判据 SnapEdgeTo（含左边缘改判 Bottom）");
}

// 批次 127 新增：拖出判据 DragOutOfSlot（SnapEdgeTo 的反方向）。
static void TestDragOutOfSlot() {
    // 槽位 = {100,100,300,260}（200 × 160），容差 4。
    const RECT slot{ 100, 100, 300, 260 };
    const int slack = 4;

    // ---- 不该拖出的那一半：槽位内 / 贴边 / 恰好压在容差边界 ---------------
    CHECK(!DragOutOfSlot(slot, POINT{ 200, 180 }, slack));   // 正中
    CHECK(!DragOutOfSlot(slot, POINT{ 100, 100 }, slack));   // 左上角（含）
    CHECK(!DragOutOfSlot(slot, POINT{ 300, 260 }, slack));   // 右下角（含）
    CHECK(!DragOutOfSlot(slot, POINT{ 96, 180 }, slack));    // 恰好左边界
    CHECK(!DragOutOfSlot(slot, POINT{ 304, 180 }, slack));   // 恰好右边界
    CHECK(!DragOutOfSlot(slot, POINT{ 200, 96 }, slack));    // 恰好上边界
    CHECK(!DragOutOfSlot(slot, POINT{ 200, 264 }, slack));   // 恰好下边界

    // ---- 该拖出的那一半：越过容差的四个方向 -------------------------------
    CHECK(DragOutOfSlot(slot, POINT{ 95, 180 }, slack));     // 左：100-4=96，95<96
    CHECK(DragOutOfSlot(slot, POINT{ 305, 180 }, slack));    // 右：300+4=304
    CHECK(DragOutOfSlot(slot, POINT{ 200, 95 }, slack));     // 上：100-4=96
    CHECK(DragOutOfSlot(slot, POINT{ 200, 265 }, slack));    // 下：260+4=264

    // ---- slack = 0：判定退化成"离开矩形"（仍不含边界）--------------------
    CHECK(!DragOutOfSlot(slot, POINT{ 100, 180 }, 0));
    CHECK(!DragOutOfSlot(slot, POINT{ 300, 180 }, 0));
    CHECK(DragOutOfSlot(slot, POINT{ 99, 180 }, 0));
    CHECK(DragOutOfSlot(slot, POINT{ 301, 180 }, 0));

    // ---- 非法入参：负 slack ⇒ 恒 false（宁可不转浮动，也不误判）------------
    CHECK(!DragOutOfSlot(slot, POINT{ 0, 0 }, -1));
    CHECK(!DragOutOfSlot(slot, POINT{ 200, 180 }, -1));      // 连"在内"也须 false

    // ---- 单调性：slack 越大越**难**拖出 ---------------------------------
    // 符号写反（用 <= / >= 或把 slack 减到坐标上）时这条会红。
    for (int s = 0; s < 12; ++s) {
        const bool a = DragOutOfSlot(slot, POINT{ 95, 180 }, s);
        const bool b = DragOutOfSlot(slot, POINT{ 95, 180 }, s + 1);
        CHECK(!(b && !a));   // 不许出现"容差变大反而更容易判拖出"
    }
    // 起点：s=0 越界为真；随 s 增大到 5 时 95 >= 100-5=95 ⇒ 转为不在外
    CHECK(DragOutOfSlot(slot, POINT{ 95, 180 }, 0));
    CHECK(!DragOutOfSlot(slot, POINT{ 95, 180 }, 5));
    SectionEnd("[9] 拖出判据 DragOutOfSlot（四方向 + 边界 + 单调性）");
}

// 批次 139：OOP 停靠面板键盘焦点桥判据。判据有"名单外的活样本"两半 ——
// 只测"跨进程鼠标按下 ⇒ 桥"等于只测名字对得上；真正的回归风险在**另一半**
// （进程内面板被误桥 ⇒ 夺走插件控件焦点），故那半必须逐一钉住。
static void TestShouldBridgeKeyboardFocus() {
    // ---- 该桥的那一半：跨进程 + 三种鼠标按下 -----------------------------
    CHECK(ShouldBridgeKeyboardFocus(WM_LBUTTONDOWN, true, true));
    CHECK(ShouldBridgeKeyboardFocus(WM_MBUTTONDOWN, true, true));
    CHECK(ShouldBridgeKeyboardFocus(WM_RBUTTONDOWN, true, true));

    // ---- 不该桥的那一半（a）：进程内面板 ⇒ 一律不桥（回归红线）------------
    CHECK(!ShouldBridgeKeyboardFocus(WM_LBUTTONDOWN, true, false));
    CHECK(!ShouldBridgeKeyboardFocus(WM_MBUTTONDOWN, true, false));
    CHECK(!ShouldBridgeKeyboardFocus(WM_RBUTTONDOWN, true, false));

    // ---- 不该桥的那一半（b）：非鼠标按下事件 --------------------------------
    // WM_PARENTNOTIFY 也会在子窗口创建/销毁时到达（WM_CREATE/WM_DESTROY），
    // 它们是本判据最容易被"只判消息类型"写漏的活样本。
    CHECK(!ShouldBridgeKeyboardFocus(WM_CREATE, true, true));
    CHECK(!ShouldBridgeKeyboardFocus(WM_DESTROY, true, true));
    CHECK(!ShouldBridgeKeyboardFocus(WM_MOUSEMOVE, true, true));   // 不是 PARENTNOTIFY 事件
    CHECK(!ShouldBridgeKeyboardFocus(WM_LBUTTONUP, true, true));   // 抬起也不算

    // ---- 不该桥的那一半（c）：hClient 不是有效窗口 -------------------------
    CHECK(!ShouldBridgeKeyboardFocus(WM_LBUTTONDOWN, false, true));
    CHECK(!ShouldBridgeKeyboardFocus(WM_LBUTTONDOWN, false, false));

    // ---- 穷举：3 事件 × 2(是否窗口) × 2(是否跨进程) = 12，与真值逐项比 ----
    const UINT events[3] = { WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN };
    int cases = 0;
    for (UINT ev : events) {
        for (int isWin = 0; isWin < 2; ++isWin) {
            for (int isForeign = 0; isForeign < 2; ++isForeign) {
                const bool want = (isWin != 0) && (isForeign != 0);
                if (ShouldBridgeKeyboardFocus(ev, isWin != 0, isForeign != 0) != want) {
                    FAIL();
                    std::printf("  [10] 穷举失配: ev=0x%X win=%d foreign=%d\n",
                                (unsigned)ev, isWin, isForeign);
                }
                ++cases;
            }
        }
    }
    CHECK(cases == 12);   // "项数"本身就是断言
    SectionEnd("[10] 批次 139：OOP 键盘焦点桥判据（含进程内不桥的回归红线）");
}

int main() {
    std::printf("== test_dock_mask ==\n");
    TestContainerEncoding();
    TestLeftIsIndistinguishable();
    TestFloatingWinsOverContainer();
    TestFeatureBitsDoNotInterfere();
    TestCoercionPredicateBothHalves();
    TestPanelTitle();
    TestRequestNames();
    TestSnapEdgeTo();
    TestDragOutOfSlot();
    TestShouldBridgeKeyboardFocus();

    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
