// oop_multi.cpp — 批次 66「一代理多插件」e2e 夹具。与 oop_good 同构，
// 仅名字/命令/标记魔数不同（'MULT'），用于断言同代理内两个插件各自
// 独立执行与通知。
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

struct Marker { UINT_PTR tag; INT_PTR value; };

static void SendMarker(UINT_PTR tag, INT_PTR value) {
    if (!g_data.npp) return;
    Marker m{ tag, value };
    COPYDATASTRUCT cds{};
    cds.dwData = 0x4D554C54;   // 'MULT'
    cds.cbData = sizeof(m);
    cds.lpData = &m;
    ::SendMessageW(g_data.npp, WM_COPYDATA, 0, (LPARAM)&cds);
}

static void ma(void) { SendMarker(5, items[0].cmdID); }
static void mb(void) { SendMarker(5, items[1].cmdID); }

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-multi"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 2;
    wcscpy_s(items[0].itemName, L"Multi Alpha");
    items[0].func = ma;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = nullptr;
    wcscpy_s(items[1].itemName, L"Multi Beta");
    items[1].func = mb;
    items[1].cmdID = 0;
    items[1].initCheck = false;
    items[1].shortcut = nullptr;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void* scn) {
    if (!scn) return;
    const NotifyHdr* h = (const NotifyHdr*)scn;
    SendMarker(6, (INT_PTR)h->code);   // tag6：multi 的通知桥证明
}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
