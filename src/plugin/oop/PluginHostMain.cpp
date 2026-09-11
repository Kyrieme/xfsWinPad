// PluginHostMain.cpp — xfsWinPadPluginHost.exe 进程外插件代理（v1）。
//
// 职责：加载一个 NPP 兼容插件 DLL，完成 setInfo/getFuncsArray 握手，把命令表
// 经 WM_COPYDATA 交给编辑器进程；此后在消息泵里替插件执行命令回调与
// beNotified（全程 SEH 包裹，捕获后返回 FALSE 继续服务）。插件的 exit()/
// 访问违例只死本进程——编辑器通过进程句柄观察到退出码并记录。
//
// 命令行：
//   --parent <hwnd>   OopHost 接收窗（OOPM_HANDSHAKE/EXEC/NOTIFY 通道落地端）
//   --npp <hwnd>      编辑器主窗口（插件 setInfo 的 nppHandle，NPPM_* 跨进程直达）
//   --plugin <path>   插件 DLL 绝对路径
//   --sci <hwnd>      活动文档 Scintilla 句柄（setInfo 的 scintillaMain；可空 0）
//   --deadline <ms>   启动看门狗时限（默认 15000）
//
// FuncItem 布局契约（现代 SDK，步长 152）：
//   [0..127]  itemName  wchar_t[64]（内联，偏移 0 即文本）
//   [128..135] func      void(*)()
//   [136..139] cmdID     int（宿主回填）
//   [140]      initCheck bool（+3 填充）
//   [144..151] shortcut  ShortcutKey*（{bool ctrl,alt,shift,uchar key}）
//
// 法律边界：布局 = 公开契约（docs/plugin-system.md §5.1），实现为 xfsWinPad
// 原创，不含 NPP 源码/头文件。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "OopProtocol.h"

// NppData 布局契约镜像（三个连续指针尺寸成员）
struct NppDataWire {
    HWND npp;
    HWND scintillaMain;
    HWND scintillaSecond;
};

typedef void (*VoidFn)(void);
static const size_t kStride = 152;   // FuncItem 步长（公开契约）

// ---- 插件导出与状态 --------------------------------------------------------
static HMODULE g_plugin = nullptr;
static void  (*g_setInfo)(void*) = nullptr;
static const wchar_t* (*g_getName)(void) = nullptr;
static void* (*g_getFuncsArray)(int*) = nullptr;   // 实为 FuncItem* (*)(int*)
static void  (*g_beNotified)(void*) = nullptr;
static BOOL  (*g_isUnicode)(void) = nullptr;

static void* g_items = nullptr;      // FuncItem 数组（按 152B 步长寻址）
static int   g_itemCount = 0;
static HWND  g_parent = nullptr;     // OopHost 接收窗（OOP 协议通道）
static HWND  g_npp = nullptr;        // 编辑器主窗口（NppData.npp / 通知 hwndFrom）
static DWORD g_deadlineMs = 15000;

// ---- SEH 包裹（无 C++ 对象，规避 MSVC C2712）--------------------------------
static int CallSetInfoSeh(void* data) {
    __try {
        g_setInfo(data);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static void* CallGetFuncsArraySeh(int* count) {
    __try {
        return g_getFuncsArray(count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool CallFuncSeh(int index) {
    void* fn = *reinterpret_cast<void**>(reinterpret_cast<char*>(g_items) +
                                         (size_t)index * kStride + 128);
    __try {
        reinterpret_cast<VoidFn>(fn)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallNotifySeh(void* scn) {
    __try {
        g_beNotified(scn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- 启动看门狗：setInfo/getFuncsArray 卡死则代理自杀 -----------------------
static HANDLE g_watchdogStop = nullptr;

static DWORD WINAPI WatchdogThread(LPVOID) {
    if (::WaitForSingleObject(g_watchdogStop, g_deadlineMs) == WAIT_OBJECT_0)
        return 0;                                  // 握手完成，正常撤销
    ::ExitProcess(xfs::oop::kExitWatchdog);
}

// ---- 消息窗口 ---------------------------------------------------------------
static std::vector<unsigned char> g_cmdIds;   // CMDIDS 载荷副本（握手期间回填）
static bool g_cmdIdsDone = false;

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds || cds->dwData != xfs::oop::kMagic || !cds->lpData) return FALSE;
        if (cds->cbData < sizeof(UINT_PTR) * 2) return FALSE;
        auto* wire = reinterpret_cast<const UINT_PTR*>(cds->lpData);
        if (wire[0] != xfs::oop::kMagic) return FALSE;
        switch (wire[1]) {
            case xfs::oop::OOPM_CMDIDS: {
                if (cds->cbData < sizeof(xfs::oop::CmdIdsWire)) return FALSE;
                g_cmdIds.assign(reinterpret_cast<const unsigned char*>(cds->lpData),
                                reinterpret_cast<const unsigned char*>(cds->lpData) +
                                    cds->cbData);
                g_cmdIdsDone = true;
                return TRUE;
            }
            case xfs::oop::OOPM_EXEC: {
                if (cds->cbData < sizeof(xfs::oop::ExecWire)) return FALSE;
                auto* ex = reinterpret_cast<const xfs::oop::ExecWire*>(cds->lpData);
                if (ex->index < 0 || ex->index >= g_itemCount) return FALSE;
                return CallFuncSeh(ex->index) ? TRUE : FALSE;
            }
            case xfs::oop::OOPM_NOTIFY: {
                if (!g_beNotified) return TRUE;
                if (cds->cbData < sizeof(xfs::oop::ExecWire)) return FALSE;
                auto* nt = reinterpret_cast<const xfs::oop::ExecWire*>(cds->lpData);
                // SCNotification 布局：nmhdr 三件套打头，尾部整体置零——插件
                // 读到的额外字段全部为 0（与宿主进程内合成路径一致）。
                // 尾部 160B ≥ 真实 SCNotification 全长。
                static const size_t kScnSize = 24 + 160;
                unsigned char scn[kScnSize] = {};
                void** h = reinterpret_cast<void**>(scn);
                h[0] = reinterpret_cast<void*>(g_npp ? g_npp : g_parent); // hwndFrom
                h[1] = reinterpret_cast<void*>(nt->idFrom);        // idFrom
                *reinterpret_cast<unsigned int*>(scn + 16) = nt->code;
                return CallNotifySeh(scn) ? TRUE : FALSE;
            }
            case xfs::oop::OOPM_SHUTDOWN:
                // 注意：SHUTDOWN 通知已随 Notify 广播送达插件，这里只退出，
                // 不二次回调 beNotified。
                ::PostQuitMessage(0);
                return TRUE;
            default:
                return FALSE;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 参数解析 ---------------------------------------------------------------
static bool ArgValue(const wchar_t* const* argv, int argc, int& i,
                     const wchar_t* name, std::wstring& out) {
    if (wcscmp(argv[i], name) != 0) return false;
    if (i + 1 >= argc) ::ExitProcess(xfs::oop::kExitExport);
    out = argv[++i];
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    const wchar_t* const* argv =
        reinterpret_cast<const wchar_t* const*>(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (!argv) return xfs::oop::kExitExport;

    std::wstring pluginPath, parentStr, nppStr, sciStr, deadlineStr;
    for (int i = 1; i < argc; ++i) {
        if (ArgValue(argv, argc, i, L"--parent", parentStr)) continue;
        if (ArgValue(argv, argc, i, L"--npp", nppStr)) continue;
        if (ArgValue(argv, argc, i, L"--plugin", pluginPath)) continue;
        if (ArgValue(argv, argc, i, L"--sci", sciStr)) continue;
        if (ArgValue(argv, argc, i, L"--deadline", deadlineStr)) continue;
    }
    if (pluginPath.empty() || parentStr.empty())
        return xfs::oop::kExitExport;
    g_parent = reinterpret_cast<HWND>(wcstoull(parentStr.c_str(), nullptr, 16));
    g_npp = reinterpret_cast<HWND>(wcstoull(nppStr.c_str(), nullptr, 16));
    if (!deadlineStr.empty())
        g_deadlineMs = static_cast<DWORD>(wcstoul(deadlineStr.c_str(), nullptr, 10));

    // 坏 DLL 不得弹「损坏的映像」硬错误框拖住后台
    ::SetErrorMode(SEM_FAILCRITICALERRORS);

    // message-only 窗口：编辑器 WM_COPYDATA 的落地端
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"xfsWinPadPluginHostWnd";
    ::RegisterClassExW(&wc);
    HWND self = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!self) return xfs::oop::kExitLoadFail;

    g_plugin = ::LoadLibraryW(pluginPath.c_str());
    if (!g_plugin) return xfs::oop::kExitLoadFail;

    g_setInfo       = reinterpret_cast<void(*)(void*)>(::GetProcAddress(g_plugin, "setInfo"));
    g_getName       = reinterpret_cast<const wchar_t* (*)()>(::GetProcAddress(g_plugin, "getName"));
    g_getFuncsArray = reinterpret_cast<void* (*)(int*)>(::GetProcAddress(g_plugin, "getFuncsArray"));
    g_beNotified    = reinterpret_cast<void(*)(void*)>(::GetProcAddress(g_plugin, "beNotified"));
    g_isUnicode     = reinterpret_cast<BOOL(*)()>(::GetProcAddress(g_plugin, "isUnicode"));
    // messageProc 在代理进程内无桥接价值（v1：不转发窗口消息），不要求导出。
    if (!g_setInfo || !g_getName || !g_getFuncsArray || !g_isUnicode)
        return xfs::oop::kExitExport;
    if (g_isUnicode() != TRUE)
        return xfs::oop::kExitAnsi;

    // 启动看门狗：覆盖 setInfo + getFuncsArray + 握手全程
    g_watchdogStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE watchdog = nullptr;
    if (g_watchdogStop)
        watchdog = ::CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);

    // setInfo：nppHandle = 编辑器主窗口（NPPM_* 跨进程直达消息垫片）。
    // 无 --npp 时退化为 parent（测试场景：插件只用 npp 收发自有 WM_COPYDATA）。
    NppDataWire data{ g_npp ? g_npp : g_parent,
                      reinterpret_cast<HWND>(wcstoull(sciStr.c_str(), nullptr, 16)),
                      nullptr };
    if (CallSetInfoSeh(&data) != 0)
        return xfs::oop::kExitFault;

    int n = 0;
    void* items = CallGetFuncsArraySeh(&n);
    if (!items || n < 0 || n > xfs::oop::kHandshakeItemsMax)
        return xfs::oop::kExitExport;
    g_items = items;
    g_itemCount = n;

    // ---- 握手：FuncItem 表转录 → 编辑器；同步回等 OOPM_CMDIDS ---------------
    xfs::oop::HandshakeWire hs{};
    hs.magic = xfs::oop::kMagic;
    hs.msg = xfs::oop::OOPM_HANDSHAKE;
    hs.itemCount = n;
    const wchar_t* pname = g_getName();
    wcsncpy_s(hs.pluginName, pname && pname[0] ? pname : L"(unnamed)", _TRUNCATE);
    for (int i = 0; i < n; ++i) {
        const unsigned char* base =
            reinterpret_cast<const unsigned char*>(items) + (size_t)i * kStride;
        wcsncpy_s(hs.items[i].name, reinterpret_cast<const wchar_t*>(base), 63);
        const void* skPtr =
            *reinterpret_cast<void* const*>(base + 144);   // shortcut 指针
        unsigned char sk[4] = {};
        if (skPtr) memcpy(sk, skPtr, 4);                   // {ctrl, alt, shift, key}
        hs.items[i].ctrl = sk[0];
        hs.items[i].alt = sk[1];
        hs.items[i].shift = sk[2];
        hs.items[i].key = sk[3];
    }
    COPYDATASTRUCT cds{};
    cds.dwData = xfs::oop::kMagic;
    cds.cbData = sizeof(hs);
    cds.lpData = &hs;
    if (!::SendMessageW(g_parent, WM_COPYDATA, reinterpret_cast<WPARAM>(self),
                        reinterpret_cast<LPARAM>(&cds))) {
        return xfs::oop::kExitExport;   // 编辑器拒绝：代理没有存在意义
    }
    // 回填 cmdID（编辑器在处理握手时同步回发了 CMDIDS）
    if (g_cmdIdsDone &&
        g_cmdIds.size() >= sizeof(xfs::oop::CmdIdsWire)) {
        auto* ids = reinterpret_cast<const xfs::oop::CmdIdsWire*>(g_cmdIds.data());
        const int* arr = reinterpret_cast<const int*>(ids + 1);
        int cnt = ids->count < g_itemCount ? ids->count : g_itemCount;
        for (int i = 0; i < cnt; ++i)
            *reinterpret_cast<int*>(reinterpret_cast<char*>(g_items) +
                                    (size_t)i * kStride + 136) = arr[i];
    }

    // 看门狗退场：启动阶段结束，进入消息泵
    if (g_watchdogStop) ::SetEvent(g_watchdogStop);
    if (watchdog) {
        ::WaitForSingleObject(watchdog, 2000);
        ::CloseHandle(watchdog);
        ::CloseHandle(g_watchdogStop);
        g_watchdogStop = nullptr;
    }

    // ---- 消息泵：EXEC / NOTIFY / SHUTDOWN 由 HostWndProc 处理 ---------------
    MSG m;
    while (::GetMessageW(&m, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
    return xfs::oop::kExitClean;
}
