// oop_crasher.cpp — 进程外桥 e2e：命令回调里访问违例。
// 进程内 = 宿主崩溃（或靠 NppCompat SEH 勉强续命）；进程外 = 代理退出
// （退出码任意），编辑器无感且命令表里该插件命令自动失效。
#include <windows.h>

struct NPData { HWND npp; HWND sciMain; HWND sciSecond; };
struct FI {
    wchar_t itemName[64];
    void (*func)(void);
    int cmdID;
    bool initCheck;
    void* shortcut;
};

static NPData g_data{};
static FI items[1];

static void crash(void) {
    volatile int* p = reinterpret_cast<volatile int*>(0xDEADBEEF);
    *p = 1;   // 0xC0000005
}

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-crasher"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"OOP Crash");
    items[0].func = crash;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = nullptr;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
