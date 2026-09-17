// oop_msg.cpp — 进程外桥批次 71（messageProc 双向同步）测试插件。
// 与 oop_good 同规则：不包含宿主头文件，手写公开契约布局。
// 可观察出口：messageProc 收到过桥消息后把 (msg,wParam,lParam) 以三条
// Marker 回传测试窗（tag 10/11/12），并返回 (wParam+1) 作为非零 LRESULT
// ——测试据此断言参数逐位还原与返回值跨进程回带。
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

static NPData g_data{};
static FI items[1];

struct Marker { UINT_PTR tag; INT_PTR value; };

static void SendMarker(UINT_PTR tag, INT_PTR value) {
    if (!g_data.npp) return;
    Marker m{ tag, value };
    COPYDATASTRUCT cds{};
    cds.dwData = 0x474F4F44;   // 'GOOD'（与 oop_good 同一测试窗约定）
    cds.cbData = sizeof(m);
    cds.lpData = &m;
    ::SendMessageW(g_data.npp, WM_COPYDATA, 0, (LPARAM)&cds);
}

static void noop(void) { }

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-msg"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"OOP Msg Noop");
    items[0].func = noop;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = nullptr;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) { }

// 批次 71 主角：参数回显 + 非零返回值（wParam+1 供测试精确断言）。
extern "C" __declspec(dllexport) LRESULT messageProc(UINT m, WPARAM w, LPARAM l) {
    SendMarker(10, (INT_PTR)m);
    SendMarker(11, (INT_PTR)w);
    SendMarker(12, (INT_PTR)l);
    return (LRESULT)((INT_PTR)w + 1);
}
