// oop_good.cpp — 进程外桥 e2e 用「行为良好」NPP 形态插件。
// 与 test_npp_core.cpp 同规则：不包含宿主头文件，手写公开契约布局。
// 可观察出口：命令执行/beNotified 收到后经 SendMessage(WM_COPYDATA) 回传
// 标记（magic='GOOD'），测试进程据此断言跨进程执行链路。
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
struct NotifyHdr { HWND hwndFrom; UINT_PTR idFrom; unsigned int code; };

static NPData g_data{};
static FI items[2];
static SK skA = { true, true, false, 'K' };   // Ctrl+Alt+K

// ---- 观察标记：{tag, value} 回传给测试窗口 ----------------------------------
struct Marker { UINT_PTR tag; INT_PTR value; };

static void SendMarker(UINT_PTR tag, INT_PTR value) {
    if (!g_data.npp) return;
    Marker m{ tag, value };
    COPYDATASTRUCT cds{};
    cds.dwData = 0x474F4F44;   // 'GOOD'
    cds.cbData = sizeof(m);
    cds.lpData = &m;
    ::SendMessageW(g_data.npp, WM_COPYDATA, 0, (LPARAM)&cds);
}

extern "C" __declspec(dllexport) int oop_good_cmdId0(void) { return items[0].cmdID; }

static void fa(void) { SendMarker(1, items[0].cmdID); }   // tag1：cmdID 回填证明
static void fb(void) { SendMarker(1, items[1].cmdID); }

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-good"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 2;
    wcscpy_s(items[0].itemName, L"OOP Ping");
    items[0].func = fa;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    wcscpy_s(items[1].itemName, L"OOP Pong");
    items[1].func = fb;
    items[1].cmdID = 0;
    items[1].initCheck = false;
    items[1].shortcut = nullptr;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void* scn) {
    if (!scn) return;
    const NotifyHdr* h = (const NotifyHdr*)scn;
    SendMarker(2, (INT_PTR)h->code);   // tag2：通知桥证明
}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
