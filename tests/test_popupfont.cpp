// 批次 140：弹窗字体所有权 —— 命令面板 / 查找框。
//
// 背景：这两处的字体原先是**进程级单例**（首个调用者的 dpi 建一次，此后既不重建
// 也不释放）。而两个窗口都是「每次 Show 新建、Close 销毁」——于是单例的字号既
// 不会过期也不会更新：把主窗口换到另一块 dpi 的屏上再开一次，字号还是最初那次
// 建的。查找框那份还与跳转框**共用**同一个单例，于是"先开哪个就用哪个的 dpi"。
//
// 本测试把「字体不活得比它服务的窗口更久」这条规矩钉成判据。★ 两条在旧写法下必红：
// 旧写法把字体交给一个活到进程结束的全局，Close 之后句柄照样有效、GetObjectW
// 照样读得出字高。
//
// 诚实标注：**"换屏后重开即自愈"这一条无法在本测试里证明** —— 进程内改不了监视器
// dpi（GetDpiForWindow 只报真实环境）。本测试证明的是它的两个前提：
//   ①字号确实按**本窗口当前** dpi 折出来；②字体生命周期不越过窗口。
#include "../src/app/CommandPalette.h"
#include "../src/app/FindDialog.h"
#include "../src/search/SearchService.h"   // FindState（Show 会解引用它）

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// 字体实际建出来的字高；句柄已失效（不再是有效 GDI 对象）时返回 0。
// 字高为负是 CreateFontW 的约定（负 = 字符高度而非单元格高度），这里按原值比对。
static LONG FontHeight(HFONT f) {
    if (!f) return 0;
    LOGFONTW lf{};
    if (::GetObjectW(f, sizeof(lf), &lf) != sizeof(lf)) return 0;
    return lf.lfHeight;
}

// 两处的字体句柄都从**子控件**取：命令面板挂字的是 edit_ / list_ 两个子控件；
// 查找框虽然对 hwnd_ 本身也发了 WM_SETFONT，但那是**自定义窗口类**，DefWindowProc
// 不存字体、WM_GETFONT 恒回 0 —— 只有标准控件才回。Show 里所有控件挂的是同一个
// 字体句柄，所以取第一个挂上的即可。
static BOOL CALLBACK TakeFont(HWND c, LPARAM lp) {
    HFONT f = (HFONT)::SendMessageW(c, WM_GETFONT, 0, 0);
    if (f) { *reinterpret_cast<HFONT*>(lp) = f; return FALSE; }
    return TRUE;
}
static HFONT FirstChildFont(HWND parent) {
    HFONT f = nullptr;
    ::EnumChildWindows(parent, TakeFont, reinterpret_cast<LPARAM>(&f));
    return f;
}

static const wchar_t kHostClass[] = L"xfsPopupFontTestHost";
static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return ::DefWindowProcW(h, m, w, l);
}
static HWND MakeHost(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.lpszClassName = kHostClass;
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, kHostClass, L"popup-font-host", WS_OVERLAPPEDWINDOW,
                             0, 0, 800, 600, nullptr, nullptr, inst, nullptr);
}

int main() {
    ::SetProcessDPIAware();
    HINSTANCE inst = ::GetModuleHandleW(nullptr);
    HWND host = MakeHost(inst);
    if (!host) { printf("FAIL: cannot create host window\n"); return 2; }
    ::ShowWindow(host, SW_SHOW);
    printf("info: host dpi = %u\n", (unsigned)::GetDpiForWindow(host));

    // ---- 命令面板（10pt Segoe UI）------------------------------------------
    CommandPalette pal;
    pal.Show(host, inst);
    HWND pw = pal.Hwnd();
    CHECK(pw != nullptr);
    if (pw) {
        const int pdpi = (int)::GetDpiForWindow(pw);
        HFONT f1 = FirstChildFont(pw);
        CHECK(f1 != nullptr);
        // [1] 字号 = **本窗口** dpi 折出来的 10pt
        CHECK(FontHeight(f1) == -::MulDiv(10, pdpi, 96));
        pal.Close();
        CHECK(pal.Hwnd() == nullptr);
        // ★ [2] 窗口关了，它自己的字体也必须跟着没了
        //      （旧写法把字体挂在进程级单例上 ⇒ 这一条必红）
        CHECK(FontHeight(f1) == 0);

        // [3] 再开一次仍要拿到**有效**字体（证明释放没把重建路径弄坏）
        pal.Show(host, inst);
        HWND pw2 = pal.Hwnd();
        CHECK(pw2 != nullptr);
        if (pw2) {
            HFONT f2 = FirstChildFont(pw2);
            CHECK(f2 != nullptr);
            CHECK(FontHeight(f2) == -::MulDiv(10, (int)::GetDpiForWindow(pw2), 96));
            pal.Close();
            CHECK(FontHeight(f2) == 0);
        }
    } else {
        pal.Close();
    }

    // ---- 查找框（9pt Segoe UI）---------------------------------------------
    FindState st{};     // Show 内部会无保护地解引用 state_，必须先绑定
    FindDialog fd;
    fd.BindState(&st);
    fd.Show(host, inst, 0);
    HWND fw = fd.Hwnd();
    CHECK(fw != nullptr);
    if (fw) {
        const int fdpi = (int)::GetDpiForWindow(fw);
        HFONT g1 = FirstChildFont(fw);   // 自定义窗口类 WM_GETFONT 恒回 0，须问子控件
        CHECK(g1 != nullptr);
        // [4] 字号 = **本窗口** dpi 折出来的 9pt
        CHECK(FontHeight(g1) == -::MulDiv(9, fdpi, 96));
        fd.Close();
        // ★ [5] 同上：旧写法里它与跳转框共用一个进程级单例，必然存活
        CHECK(FontHeight(g1) == 0);
    } else {
        fd.Close();
    }

    ::DestroyWindow(host);

    if (g_fail == 0) printf("POPUPFONT TEST PASSED\n");
    else printf("POPUPFONT TEST FAILED: %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
