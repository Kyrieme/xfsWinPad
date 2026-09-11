// dump_nppexec.cpp - 临时诊断 v3：setInfo 前后对比 getFuncsArray 返回。
#include <windows.h>
#include <cstdio>

typedef void (*SetInfoFn)(void*);
typedef const wchar_t* (*GetNameFn)();
typedef void* (*GetFuncsArrayFn)(int*);
typedef BOOL (*IsUnicodeFn)();

struct NppData {
    HWND npp;
    HWND scintillaMain;
    HWND scintillaSecond;
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m >= 0x0800 && m < 0x0900) {
        if (m == 0x081A) return (LRESULT)((8 << 16) | 6);  // NPPM_GETNPPVERSION
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void DumpHex(const char* tag, void* items, int n, int bytes) {
    printf("%s items=%p n=%d\n", tag, items, n);
    fflush(stdout);
    if (!items) return;
    unsigned char* p = (unsigned char*)items;
    for (int i = 0; i < bytes; i += 16) {
        printf("  %04X: ", i);
        for (int k = 0; k < 16; k++) printf("%02X ", p[i + k]);
        printf(" | ");
        for (int k = 0; k < 16; k++) {
            unsigned char c = p[i + k];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("\n");
    }
    fflush(stdout);
}

static void Pump(int ms) {
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

int main() {
    const wchar_t* path = L"C:\\Users\\Kyrie\\AppData\\Roaming\\xfsWinPad\\plugins\\NppExec.dll";
    HMODULE h = LoadLibraryW(path);
    if (!h) { printf("load fail err=%u\n", GetLastError()); return 1; }

    auto setInfo = (SetInfoFn)GetProcAddress(h, "setInfo");
    auto getName = (GetNameFn)GetProcAddress(h, "getName");
    auto getFuncsArray = (GetFuncsArrayFn)GetProcAddress(h, "getFuncsArray");
    auto isUnicode = (IsUnicodeFn)GetProcAddress(h, "isUnicode");

    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)setInfo, &self);
    printf("dll base=%p setInfo=%p name=%ls unicode=%d\n",
           self, setInfo, getName ? getName() : L"?", isUnicode ? isUnicode() : -1);
    fflush(stdout);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DumpHostWnd";
    RegisterClassW(&wc);
    HWND host = CreateWindowExW(0, L"DumpHostWnd", L"host", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND sci = CreateWindowExW(0, L"DumpHostWnd", L"sci", WS_OVERLAPPEDWINDOW,
                               0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    // 1) setInfo 之前
    int n0 = 0;
    void* items0 = getFuncsArray ? getFuncsArray(&n0) : nullptr;
    DumpHex("BEFORE setInfo", items0, n0, 64);

    // 2) setInfo
    if (setInfo) {
        NppData d;
        d.npp = host;
        d.scintillaMain = sci;
        d.scintillaSecond = nullptr;
        printf("calling setInfo...\n");
        fflush(stdout);
        setInfo(&d);
        printf("setInfo done\n");
        fflush(stdout);
        Pump(500);
    }

    // 3) setInfo 之后
    int n1 = 0;
    void* items1 = getFuncsArray ? getFuncsArray(&n1) : nullptr;
    DumpHex("AFTER setInfo", items1, n1, 96);
    return 0;
}
