// test_npp_core.cpp — 最小合规的 Notepad++ 形态插件（六导出），用于回归
// 兼容装载链路。注意：本文件【故意】不包含宿主的任何头文件——它自己手写
// 与公开契约一致的结构布局，正如真实第三方插件的作者那样。布局是否匹配
// 由测试通过行为断言（命令可达、回调可触发）来验证。
//
// 布局契约来源：插件系统设计笔记 §5.1（原创声明，成员顺序=二进制事实）。
#include <windows.h>

// ---- 与宿主镜像的契约结构（顺序即契约，勿改） ------------------------------
// 布局 = 现代 NPP 插件 SDK（PluginInterface.h）：内联名 buffer + func +
// cmdID + initCheck + ShortcutKey*。ShortcutKey 为 {ctrl,alt,shift,key}。
struct NPData { HWND npp; HWND sciMain; HWND sciSecond; };
struct SK { bool ctrl, alt, shift; unsigned char key; };
struct FI {
    wchar_t itemName[64];
    void (*func)(void);
    int cmdID;                 // 宿主回填
    bool initCheck;
    SK* shortcut;
};

// beNotified 收到的 SCNotification 以 nmhdr 兼容头打头；宿主只广播 NPPN_* 通知
// （不涉及 Scintilla 编辑事件），镜像头三成员即可验证布局契约。
struct NotifyHdr { HWND hwndFrom; UINT_PTR idFrom; unsigned int code; };

// ---- 插件状态与可观察出口 --------------------------------------------------
static NPData g_data{};
static int g_setInfoSeen = 0;
static int g_fire = 0;
static int g_funcsArraySeen = 0;

static NotifyHdr g_notify[64];
static int g_notifyCount = 0;

extern "C" __declspec(dllexport) int    test_npp_fires(void) { return g_fire; }
extern "C" __declspec(dllexport) int    test_npp_setInfoSeen(void) { return g_setInfoSeen; }
extern "C" __declspec(dllexport) HWND   test_npp_nppHandle(void) { return g_data.npp; }
extern "C" __declspec(dllexport) HWND   test_npp_mainSci(void) { return g_data.sciMain; }
// beNotified 观察口（4c）：收到的通知条数、逐条 code/idFrom/hwndFrom
extern "C" __declspec(dllexport) int        test_npp_notifyCount(void) { return g_notifyCount; }
extern "C" __declspec(dllexport) unsigned   test_npp_notifyCode(int i) {
    return (i >= 0 && i < g_notifyCount) ? g_notify[i].code : 0;
}
extern "C" __declspec(dllexport) UINT_PTR   test_npp_notifyId(int i) {
    return (i >= 0 && i < g_notifyCount) ? g_notify[i].idFrom : 0;
}
extern "C" __declspec(dllexport) HWND       test_npp_notifyHwnd(int i) {
    return (i >= 0 && i < g_notifyCount) ? g_notify[i].hwndFrom : nullptr;
}

static wchar_t wName[] = L"npp-test";
static wchar_t wA[] = L"NPP Action One";
static wchar_t wB[] = L"NPP Action Two";
static SK skA = { true, false, true, '7' };   // Ctrl+Shift+7（现代 SDK 顺序）
static SK skB = { true, false, true, '8' };   // Ctrl+Shift+8
static FI items[2];

// 宿主回填后的 cmdID 观察口（放在 items 定义之后）
extern "C" __declspec(dllexport) int    test_npp_cmdId0(void) { return items[0].cmdID; }
extern "C" __declspec(dllexport) int    test_npp_cmdId1(void) { return items[1].cmdID; }

static void fa(void) { ++g_fire; }
static void fb(void) { g_fire += 10; }

extern "C" __declspec(dllexport) void setInfo(NPData* d) {
    if (d) { g_data = *d; ++g_setInfoSeen; }
}
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return wName; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }

extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    ++g_funcsArraySeen;
    *nbF = 2;
    wcscpy_s(items[0].itemName, wA);          // 内联名 buffer（现代 SDK）
    items[0].func = fa;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    wcscpy_s(items[1].itemName, wB);
    items[1].func = fb;
    items[1].cmdID = 0;
    items[1].initCheck = false;
    items[1].shortcut = &skB;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void* scn) {
    if (!scn) return;
    if (g_notifyCount < 64)
        g_notify[(size_t)g_notifyCount++] = *(const NotifyHdr*)scn;
}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT m, WPARAM wp, LPARAM lp) {
    (void)m; (void)wp; (void)lp; return 0;
}
