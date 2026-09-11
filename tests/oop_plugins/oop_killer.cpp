// oop_killer.cpp — 进程外桥 e2e：setInfo 里直接 exit(1)（复刻 ComparePlus 行为）。
// 进程内加载 = 宿主死亡；进程外加载 = 代理 exit(1)、编辑器无感。
#include <windows.h>
#include <stdlib.h>

struct NPData { HWND npp; HWND sciMain; HWND sciSecond; };

extern "C" __declspec(dllexport) void setInfo(NPData*) { exit(1); }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-killer"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) void* getFuncsArray(int* nbF) {
    if (nbF) *nbF = 0;
    return nullptr;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
