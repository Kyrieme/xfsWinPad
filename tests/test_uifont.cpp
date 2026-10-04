// test_uifont.cpp — 批次 145：UI 字体自适应（src/core/UiFont.*）。
//
// 【为什么值得单独一个目标】
//   用户报「标签页 / 查找弹窗 / 查找结果窗口在 1080p 下发虚」。根因不是 DPI 感知
//   （manifest 是 PerMonitorV2、全仓零 awareness 覆盖），而是字体写法：全仓 UI
//   一律 CreateFontW(-MulDiv(9, dpi, 96), ..., L"Segoe UI")，把 9 当成「96 dpi 下的
//   9 **像素**」——100% 缩放时字高只有 9px，远低于系统 UI 基准（9pt≈12px），所以又小
//   又糊。修法是改从系统取「消息字体」（零写死），于是必须有三条测试钉住：
//     1. **别退回 9px**（这正是 bug 本身）；
//     2. **高度确实随 dpi 成比例**（换屏 / 改缩放要自动跟上，用户明确要的"自适应"）；
//     3. **确实等于系统消息字体**（不是又换了个写死的字号）。
//   三条都只在真实 user32/gdi32 上成立，所以走真实调用、GetObjectW 读回 LOGFONT。
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <windows.h>

#include "../src/core/UiFont.h"

using namespace xfs;

static int g_fail = 0;
static int g_sinceLabel = 0;
#define FAIL() do { ++g_fail; ++g_sinceLabel; } while (0)
#define CHECK(cond) do { if (!(cond)) { FAIL(); \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void SectionEnd(const char* label) {
    if (g_sinceLabel == 0) std::printf("%s: ok\n", label);
    else std::printf("%s: FAILED (%d new failure(s))\n", label, g_sinceLabel);
    g_sinceLabel = 0;
}

// 读回 HFONT 的 LOGFONTW。失败返回 false（不识别的句柄）。
static bool ReadLogFont(HFONT f, LOGFONTW& out) {
    out = LOGFONTW{};
    if (!f) return false;
    return ::GetObjectW(f, sizeof(out), &out) == sizeof(out);
}

// 系统在 dpi 下的消息字体高度（与 helper 同源，用作"确实来自系统"的对照）。
static bool SystemMessageFont(int dpi, int& outH, wchar_t* face, int faceCap) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                      static_cast<UINT>(dpi)))
        return false;
    outH = ncm.lfMessageFont.lfHeight;
    if (face && faceCap > 0)
        ::wcsncpy_s(face, static_cast<size_t>(faceCap),
                    ncm.lfMessageFont.lfFaceName, _TRUNCATE);
    return true;
}

static void TestValidity() {
    const int dpis[] = {96, 120, 144, 192};
    int n = 0;
    for (int d : dpis) {
        HFONT f = CreateUiFont(d);
        CHECK(f != nullptr);
        LOGFONTW lf{};
        CHECK(ReadLogFont(f, lf));
        CHECK(lf.lfHeight != 0);           // 有实际字高
        CHECK(lf.lfFaceName[0] != L'\0');  // 有面名
        CHECK(lf.lfHeight < 0);            // 字符高（负），不是格子高
        ++n;
        if (f) ::DeleteObject(f);
    }
    CHECK(n == 4);   // 项数本身即断言：循环没跑够也说明没测全
    SectionEnd("[1] 有效性（非空 / 可读回 / 有面名 / 高度为负）");
}

static void TestNotTheOldNinePixelBug() {
    // ★ 这条就是 bug 本身：旧写法在 100% 下产出 9px。修完必须**大于**它。
    HFONT f = CreateUiFont(96);
    LOGFONTW lf{};
    CHECK(ReadLogFont(f, lf));
    CHECK(-lf.lfHeight > 9);
    if (!(-lf.lfHeight > 9))
        std::printf("  h96=%d（<=9 说明又退回旧字号的像素口径了）\n", (int)lf.lfHeight);
    if (f) ::DeleteObject(f);
    SectionEnd("[2] ★不得退回旧 9px 口径");
}

static void TestScalesWithDpi() {
    // ★ 自适应：高度随 dpi 成比例。容差吸收各档取整。
    HFONT f96 = CreateUiFont(96);
    HFONT f144 = CreateUiFont(144);
    HFONT f192 = CreateUiFont(192);
    LOGFONTW a{}, b{}, c{};
    CHECK(ReadLogFont(f96, a));
    CHECK(ReadLogFont(f144, b));
    CHECK(ReadLogFont(f192, c));

    const int h96 = -a.lfHeight, h144 = -b.lfHeight, h192 = -c.lfHeight;
    CHECK(h144 > h96);                            // 严格变大
    CHECK(h192 > h144);
    CHECK(std::abs(h144 * 2 - h96 * 3) <= 2);     // ≈1.5×
    CHECK(std::abs(h192 - h96 * 2) <= 1);         // ≈2.0×

    // dpi<=0（未就绪窗口）按 96 处理 ⇒ 与 h96 相同。
    HFONT f0 = CreateUiFont(0);
    LOGFONTW z{};
    CHECK(ReadLogFont(f0, z));
    CHECK(z.lfHeight == a.lfHeight);

    if (f0) ::DeleteObject(f0);
    if (f192) ::DeleteObject(f192);
    if (f144) ::DeleteObject(f144);
    if (f96) ::DeleteObject(f96);
    SectionEnd("[3] ★随 dpi 成比例（含 dpi<=0 回退 96）");
}

static void TestComesFromSystem() {
    // ★ 确实等于系统消息字体：不是又换了个写死字号。
    HFONT f = CreateUiFont(96);
    LOGFONTW lf{};
    CHECK(ReadLogFont(f, lf));

    int sysH = 0;
    wchar_t sysFace[LF_FACESIZE]{};
    if (SystemMessageFont(96, sysH, sysFace, LF_FACESIZE)) {
        CHECK(lf.lfHeight == sysH);
        CHECK(std::wcscmp(lf.lfFaceName, sysFace) == 0);
    } else {
        std::printf("  (SPI_GETNONCLIENTMETRICS 不可用，跳过与系统对照)\n");
    }
    if (f) ::DeleteObject(f);
    SectionEnd("[4] ★与系统消息字体同源（高度 + 面名）");
}

int main() {
    TestValidity();
    TestNotTheOldNinePixelBug();
    TestScalesWithDpi();
    TestComesFromSystem();

    if (g_fail == 0) { std::printf("uifont: ALL PASS\n"); return 0; }
    std::printf("uifont: %d FAILURE(S)\n", g_fail);
    return 1;
}
