// oop_hang.cpp — 装载即卡死夹具：setInfo 永不返回，代理装载看门狗
// （ExitProcess kExitWatchdog）是唯一出路；测试编辑器侧死亡归因+幸存者重加。
#include <windows.h>

struct NPData { HWND npp; HWND sciMain; HWND sciSecond; };
struct SK { bool ctrl, alt, shift; unsigned char key; };
struct FI {
    wchar_t itemName[64];
    void (*func)(void);
    int cmdID;
    bool initCheck;
    SK* shortcut;
};

static FI items[1];

static void fa(void) {}

extern "C" __declspec(dllexport) void setInfo(NPData*) {
    for (;;) ::Sleep(1000);   // 卡死在装载里
}
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-hang"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"Hang Never");
    items[0].func = fa;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
