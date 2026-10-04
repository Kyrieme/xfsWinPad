// test_dpi.cpp — 批次 138a：per-monitor DPI 的换算与建议矩形钳制。
//
// 【为什么值得单独一个目标】
//   本程序自第一天起就在 app.manifest 里声明了 PerMonitorV2，但全仓没有一处处理
//   WM_DPICHANGED。补这条路径时有两个判据藏在窗口系统后面、平时跑不到：
//     1. **建议矩形不能照单全收**。系统给的是它算的，不是我们能接受的——它可能
//        小于我们的最小尺寸，也可能被推到工作区外（尤其是**负坐标**的左边那块
//        显示器）。这类缺陷只有在那种硬件布局上才现形，没有单测就等于没验证。
//     2. **舍入口径必须与既有的 MulDiv(v, dpi, 96) 一致**。差 1px 不会报错，
//        只会让新旧两条路径的界面慢慢对不齐。
//   两条都与窗口系统无关，所以抽成纯函数（src/core/Dpi.h）钉在这里。
//
// 【判据的两半】（本项目纪律：钳制类判据既要测"该改的改了"，也要测"不该改的没动"）
//   · 该改的：小于最小尺寸要抬、伸出工作区要拉回、离谱输入要落回工作区；
//   · **不该改的**：本来就合规的矩形必须**逐字段原样返回**——只测前一半，
//     等于只测"函数被调用过"。

#include <cstdio>

#include "../src/core/Dpi.h"

using namespace xfs;

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

// 逐字段比较（RECT 没有 operator==）。
static bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

static void DumpRect(const char* what, const RECT& r) {
    std::printf("  %s: L=%d T=%d R=%d B=%d\n", what, (int)r.left, (int)r.top,
                (int)r.right, (int)r.bottom);
}

static RECT Mk(int l, int t, int r, int b) {
    RECT rc{}; rc.left = l; rc.top = t; rc.right = r; rc.bottom = b; return rc;
}

// =============================================================================

static void TestScalePx() {
    // 基准 dpi：恒等（这条挡住"顺手给所有值都乘一遍"的错误）。
    CHECK(ScalePx(0, 96) == 0);
    CHECK(ScalePx(1, 96) == 1);
    CHECK(ScalePx(16, 96) == 16);
    CHECK(ScalePx(220, 96) == 220);

    // 常见档位：全是整除，任何时候都不该有偏差。
    CHECK(ScalePx(16, 120) == 20);
    CHECK(ScalePx(16, 144) == 24);
    CHECK(ScalePx(16, 192) == 32);
    CHECK(ScalePx(16, 240) == 40);

    // ★ 与 MulDiv 的舍入口径必须一致（四舍五入、半数远离零）。
    //   15*144/96 = 22.5 ⇒ 23，不是 22。
    CHECK(ScalePx(15, 144) == 23);
    //   -9*144/96 = -13.5 ⇒ -14（远离零，不是 -13）。
    CHECK(ScalePx(-9, 144) == -14);
    //   非整数缩放：15*120/96 = 18.75 ⇒ 19。
    CHECK(ScalePx(15, 120) == 19);

    // dpi 未知 / 非法：按基准处理，绝不除零、不产出 0 尺寸。
    CHECK(ScalePx(24, 0) == 24);
    CHECK(ScalePx(24, -96) == 24);
    SectionEnd("[1] ScalePx 换算（基准 / 档位 / 舍入 / 非法 dpi）");
}

static void TestWParamDecoding() {
    // WM_DPICHANGED 的 wParam：LOWORD = dpiX，HIWORD = dpiY。
    const WPARAM wp = static_cast<WPARAM>(MAKELONG(144, 192));
    CHECK(DpiXFromWParam(wp) == 144);
    CHECK(DpiYFromWParam(wp) == 192);
    // ★ 钉住经典写错法：把 HIWORD 当 dpi。非等比缩放时两者不等，这条会红。
    CHECK(DpiXFromWParam(wp) != DpiYFromWParam(wp));

    // 等比缩放时两者相等（不构成"取错了也能过"的借口，上面那条才是判据）。
    const WPARAM eq = static_cast<WPARAM>(MAKELONG(144, 144));
    CHECK(DpiXFromWParam(eq) == 144);
    CHECK(DpiYFromWParam(eq) == 144);

    // 边界：wParam=0（窗口尚未建立时的可疑取值）不得崩，且两个分量都是 0。
    CHECK(DpiXFromWParam(0) == 0);
    CHECK(DpiYFromWParam(0) == 0);
    SectionEnd("[2] ★wParam 解码（LOWORD 是 dpiX，不是 HIWORD）");
}

static void TestNeedsResourceRebuild() {
    // 变了 ⇒ 要重建。
    CHECK(NeedsResourceRebuild(96, 144));
    CHECK(NeedsResourceRebuild(144, 96));
    CHECK(NeedsResourceRebuild(96, 120));
    // ★ 没变 ⇒ **不**重建（否则每次 WM_DPICHANGED 都会白白重建字体与图标位图）。
    CHECK(!NeedsResourceRebuild(96, 96));
    CHECK(!NeedsResourceRebuild(192, 192));
    // 任一为 0（未知，窗口还没建好）⇒ 不重建：挡住启动路径上的误触发。
    CHECK(!NeedsResourceRebuild(0, 144));
    CHECK(!NeedsResourceRebuild(144, 0));
    CHECK(!NeedsResourceRebuild(0, 0));
    SectionEnd("[3] NeedsResourceRebuild（没变 / 未知一律不重建）");
}

static void TestClampKeepsCompliantRect() {
    // ★ 判据的另一半：本来就合规的矩形必须**逐字段原样**返回。
    //   主屏工作区 0,0,1920,1040（减去任务栏），建议矩形 100,100,900,700。
    const RECT work = Mk(0, 0, 1920, 1040);
    const RECT good = Mk(100, 100, 900, 700);
    const RECT got = ClampSuggestedRect(good, 640, 400, work);
    CHECK(SameRect(got, good));
    if (!SameRect(got, good)) { DumpRect("want", good); DumpRect("got", got); }

    // 恰好等于最小尺寸、恰好贴住工作区边界 ⇒ 也不该动。
    const RECT exact = Mk(0, 0, 640, 400);
    CHECK(SameRect(ClampSuggestedRect(exact, 640, 400, work), exact));
    // 右下角贴边（right/bottom 正好等于工作区右下）⇒ 合法。
    const RECT atEdge = Mk(1280, 640, 1920, 1040);
    CHECK(SameRect(ClampSuggestedRect(atEdge, 640, 400, work), atEdge));
    SectionEnd("[4] ★合规矩形原样返回（钳制不该多动一分）");
}

static void TestClampEnforcesMinSize() {
    const RECT work = Mk(0, 0, 1920, 1040);
    // 小于最小尺寸 ⇒ 抬到最小，位置保持（右下仍放得下）。
    const RECT small = Mk(50, 60, 350, 260);     // 300×200
    const RECT got = ClampSuggestedRect(small, 640, 400, work);
    CHECK(got.left == 50 && got.top == 60);
    CHECK(got.right - got.left == 640);
    CHECK(got.bottom - got.top == 400);
    if (!(got.left == 50 && got.top == 60)) DumpRect("got", got);

    // 抬到最小之后右下角会伸出工作区 ⇒ 位置同时被拉回。
    const RECT nearEdge = Mk(1800, 1000, 1900, 1030);
    const RECT g2 = ClampSuggestedRect(nearEdge, 640, 400, work);
    CHECK(g2.right <= work.right && g2.bottom <= work.bottom);
    CHECK(g2.right - g2.left == 640 && g2.bottom - g2.top == 400);

    // 退化输入：right < left（负尺寸）也要被抬到最小尺寸。
    const RECT inverted = Mk(500, 500, 400, 400);
    const RECT g3 = ClampSuggestedRect(inverted, 640, 400, work);
    CHECK(g3.right - g3.left == 640);
    CHECK(g3.bottom - g3.top == 400);
    SectionEnd("[5] 最小尺寸钳制（含负尺寸的退化输入）");
}

static void TestClampKeepsInsideWorkArea() {
    // 工作区在**左边的负坐标显示器**上：这是真实布局，也是最容易写错的一段。
    const RECT leftMon = Mk(-1920, 0, 0, 1040);
    // 系统把窗口推到了左显示器之外（x 远小于 left）⇒ 拉回工作区左上。
    const RECT outside = Mk(-3000, -200, -2200, 200);
    const RECT g = ClampSuggestedRect(outside, 640, 400, leftMon);
    CHECK(g.left >= leftMon.left && g.top >= leftMon.top);
    CHECK(g.right <= leftMon.right && g.bottom <= leftMon.bottom);
    if (!(g.left >= leftMon.left && g.right <= leftMon.right)) DumpRect("got", g);

    // 主屏工作区右下越界 ⇒ 拉回。
    const RECT work = Mk(0, 0, 1920, 1040);
    const RECT over = Mk(1900, 1030, 2600, 1500);
    const RECT g2 = ClampSuggestedRect(over, 640, 400, work);
    CHECK(g2.right <= work.right && g2.bottom <= work.bottom);
    CHECK(g2.left >= work.left && g2.top >= work.top);

    // 工作区比最小尺寸还小（例如 500×320 的小屏减去任务栏）：以工作区为准，
    // 否则窗口会被撑出屏幕——那比"比最小尺寸还小"更糟。
    const RECT tiny = Mk(0, 0, 500, 320);
    const RECT g3 = ClampSuggestedRect(Mk(-50, -50, 100, 100), 640, 400, tiny);
    CHECK(g3.right - g3.left == 500);
    CHECK(g3.bottom - g3.top == 320);
    CHECK(g3.left == tiny.left && g3.top == tiny.top);
    if (!(g3.right - g3.left == 500)) DumpRect("got", g3);
    SectionEnd("[6] ★守在工作区内（含负坐标左屏 / 工作区小于最小尺寸）");
}

static void TestClampTotality() {
    // 不论输入多离谱，结果必须完全落在工作区内。穷举几种极端组合而不是抽样——
    // 这类"越界"缺陷正是抽样漏掉的。
    const RECT areas[4] = {
        Mk(0, 0, 1920, 1040),
        Mk(-1920, 0, 0, 1040),        // 左屏（负坐标）
        Mk(1920, -200, 3840, 900),    // 右上屏（负 top，非零原点）
        Mk(0, 0, 800, 560),           // 小屏
    };
    const RECT cands[6] = {
        Mk(100, 100, 900, 700),       // 合规
        Mk(-9999, -9999, -9000, -9000), // 远在左上之外
        Mk(9999, 9999, 10999, 10999),   // 远在右下之外
        Mk(0, 0, 0, 0),                 // 零尺寸
        Mk(500, 500, 400, 400),         // 负尺寸
        Mk(-5, -5, 1920 + 5, 1040 + 5), // 比工作区还大一点
    };
    int cases = 0;
    for (const RECT& a : areas) {
        for (const RECT& c : cands) {
            const RECT r = ClampSuggestedRect(c, 640, 400, a);
            // ① 完全落在工作区内
            const bool inside = r.left >= a.left && r.top >= a.top &&
                                r.right <= a.right && r.bottom <= a.bottom;
            // ② 尺寸非负
            const bool sized = r.right - r.left >= 0 && r.bottom - r.top >= 0;
            if (!inside || !sized) {
                FAIL();
                std::printf("  [7] 越界: area L=%d T=%d R=%d B=%d\n",
                            (int)a.left, (int)a.top, (int)a.right, (int)a.bottom);
                DumpRect("  in ", c);
                DumpRect("  out", r);
            }
            ++cases;
        }
    }
    // "项数"本身就是断言（4 × 6 = 24）：循环没跑够也说明穷举没做完。
    CHECK(cases == 24);
    SectionEnd("[7] 全域性质：结果恒在工作区内（4×6 穷举）");
}

int main() {
    TestScalePx();
    TestWParamDecoding();
    TestNeedsResourceRebuild();
    TestClampKeepsCompliantRect();
    TestClampEnforcesMinSize();
    TestClampKeepsInsideWorkArea();
    TestClampTotality();

    if (g_fail == 0) {
        std::printf("dpi: ALL PASS\n");
        return 0;
    }
    std::printf("dpi: %d FAILURE(S)\n", g_fail);
    return 1;
}
