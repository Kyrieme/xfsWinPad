// PluginHostMain.cpp — xfsWinPadPluginHost.exe 进程外插件代理（v2 多槽位）。
//
// 职责：一个代理进程承载多个 NPP 兼容插件 DLL。启动后仅建 slot0 控制窗并
// 发 OOPM_READY 报到；编辑器对 slot0 发 OOPM_ADD（DLL 路径 + cookie），
// 代理在 ADD 处理里同步完成 LoadLibrary → setInfo/getFuncsArray → 每插件
// 一个消息窗 → HANDSHAKE 交命令表 → 等编辑器回填 CMDIDS。此后 EXEC/NOTIFY
// 按各自插件窗寻址（路由与 v1 单插件形态完全一致）。
// 插件 setInfo 里 exit()/卡死（看门狗自杀）带走整个代理——编辑器把死亡
// 归因于正在装载的那个，幸存者重进新代理。运行期某插件把代理搞崩则同车
// 连坐（内存换隔离粒度的既定取舍）。
//
// 命令行（v2 不再有 --plugin）：
//   --parent <hwnd>   OopHost 接收窗（READY/HANDSHAKE/REJECT 通道落地端）
//   --npp <hwnd>      编辑器主窗口（插件 setInfo 的 nppHandle，NPPM_* 跨进程直达）
//   --sci <hwnd>      活动文档 Scintilla 句柄（setInfo 的 scintillaMain；可空 0）
//   --deadline <ms>   单次装载看门狗时限（默认 15000）
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

// ---- 每插件槽位状态 ----------------------------------------------------------
struct Slot {
    HMODULE mod = nullptr;
    void (*setInfo)(void*) = nullptr;
    const wchar_t* (*getName)(void) = nullptr;
    void* (*getFuncsArray)(int*) = nullptr;   // 实为 FuncItem* (*)(int*)
    void (*beNotified)(void*) = nullptr;
    LRESULT (*messageProc)(UINT, WPARAM, LPARAM) = nullptr;  // 可选（v2.1 OOPM_MSG 桥）
    void* items = nullptr;                    // FuncItem 数组（152B 步长寻址）
    int itemCount = 0;
    HWND wnd = nullptr;
    std::vector<unsigned char> cmdIds;        // CMDIDS 载荷副本（同步回填）
    bool cmdIdsDone = false;
};

static std::vector<Slot*> g_slots;     // [0]=控制槽（无插件）；vector 存指针保地址稳定
static HWND  g_parent = nullptr;       // OopHost 接收窗（OOP 协议通道）
static HWND  g_npp = nullptr;          // 编辑器主窗口（NppData.npp / 通知 hwndFrom）
static HWND  g_sci = nullptr;          // NppData.scintillaMain
static DWORD g_deadlineMs = 15000;

// ---- SEH 包裹（无 C++ 对象，规避 MSVC C2712）--------------------------------
static int CallSetInfoSeh(Slot* s, void* data) {
    __try {
        s->setInfo(data);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static void* CallGetFuncsArraySeh(Slot* s, int* count) {
    __try {
        return s->getFuncsArray(count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool CallFuncSeh(Slot* s, int index) {
    void* fn = *reinterpret_cast<void**>(reinterpret_cast<char*>(s->items) +
                                         (size_t)index * kStride + 128);
    __try {
        reinterpret_cast<VoidFn>(fn)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallNotifySeh(Slot* s, void* scn) {
    __try {
        s->beNotified(scn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// messageProc（v2.1 桥）：崩溃按 NPP「未处理」语义回 0，代理不自杀。
static LRESULT CallMessageProcSeh(Slot* s, UINT m, WPARAM w, LPARAM l) {
    __try {
        return s->messageProc(m, w, l);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- 装载看门狗：单次 ADD 的 setInfo/getFuncsArray/握手卡死则代理自杀 -------
// ADD 全程串行（代理主线程同步处理），单事件即可；时限由 AddWire 逐次携带。
static HANDLE g_addStopEvent = nullptr;

static DWORD WINAPI WatchdogThread(LPVOID param) {
    DWORD ms = static_cast<DWORD>(reinterpret_cast<uintptr_t>(param));
    if (::WaitForSingleObject(g_addStopEvent, ms) == WAIT_OBJECT_0)
        return 0;                                  // 装载完成，正常撤销
    ::ExitProcess(xfs::oop::kExitWatchdog);
}

// ---- 协议回带小工具 -----------------------------------------------------------
static void SendToParent(UINT_PTR msgId, const void* data, DWORD size) {
    COPYDATASTRUCT cds{};
    cds.dwData = xfs::oop::kMagic;
    cds.cbData = size;
    cds.lpData = const_cast<void*>(data);
    ::SendMessageW(g_parent, WM_COPYDATA, reinterpret_cast<WPARAM>(g_slots[0]->wnd),
                   reinterpret_cast<LPARAM>(&cds));
}

static void SendReject(UINT_PTR cookie, UINT_PTR reason) {
    xfs::oop::RejectWire rj{};
    rj.magic = xfs::oop::kMagic;
    rj.msg = xfs::oop::OOPM_REJECT;
    rj.cookie = cookie;
    rj.reason = reason;
    SendToParent(xfs::oop::OOPM_REJECT, &rj, sizeof(rj));
}

// ---- 槽位消息窗（slot0 与插件槽共用同一个 WndProc，按 GWLP_USERDATA 分流）----
static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// ADD 处理：在 slot0 的 wndproc 内同步完成一个插件的装载+握手。
// 期间 HANDSHAKE 的 SendMessage 阻塞等待编辑器处理，而编辑器会在同一窗口期
// 对插件槽窗回发 CMDIDS——阻塞方照常被投递 incoming sent messages（v1 启动
// 序列即依赖此语义），故无死锁。
static BOOL HandleAdd(xfs::oop::AddWire* aw) {
    UINT_PTR cookie = aw->cookie;
    auto* s = new Slot();

    s->mod = ::LoadLibraryW(aw->path);
    if (!s->mod) { SendReject(cookie, xfs::oop::kExitLoadFail); delete s; return TRUE; }

    s->setInfo       = reinterpret_cast<void(*)(void*)>(::GetProcAddress(s->mod, "setInfo"));
    s->getName       = reinterpret_cast<const wchar_t* (*)()>(::GetProcAddress(s->mod, "getName"));
    s->getFuncsArray = reinterpret_cast<void* (*)(int*)>(::GetProcAddress(s->mod, "getFuncsArray"));
    s->beNotified    = reinterpret_cast<void(*)(void*)>(::GetProcAddress(s->mod, "beNotified"));
    auto isUnicode   = reinterpret_cast<BOOL(*)()>(::GetProcAddress(s->mod, "isUnicode"));
    s->messageProc   = reinterpret_cast<LRESULT(*)(UINT, WPARAM, LPARAM)>(
                           ::GetProcAddress(s->mod, "messageProc"));
    // messageProc 可选（v2.1 起经 OOPM_MSG 桥接；缺省按未处理回 0），不要求导出。
    if (!s->setInfo || !s->getName || !s->getFuncsArray || !isUnicode) {
        ::FreeLibrary(s->mod);
        SendReject(cookie, xfs::oop::kExitExport);
        delete s;
        return TRUE;
    }
    if (isUnicode() != TRUE) {
        ::FreeLibrary(s->mod);
        SendReject(cookie, xfs::oop::kExitAnsi);
        delete s;
        return TRUE;
    }

    // 槽窗先建：HANDSHAKE 阻塞期间编辑器要按此 HWND 回发 CMDIDS
    s->wnd = ::CreateWindowExW(0, L"xfsWinPadPluginHostWnd", L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr,
                               ::GetModuleHandleW(nullptr), s);
    if (!s->wnd) {
        ::FreeLibrary(s->mod);
        SendReject(cookie, xfs::oop::kExitLoadFail);
        delete s;
        return TRUE;
    }
    g_slots.push_back(s);

    // 看门狗覆盖 setInfo + getFuncsArray + 握手全程（卡死 = 自杀，
    // 编辑器死亡归因会把本插件隔离、幸存者重进新代理）
    const DWORD addMs =
        aw->deadlineMs ? static_cast<DWORD>(aw->deadlineMs) : g_deadlineMs;
    HANDLE watchdog = nullptr;
    g_addStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_addStopEvent)
        watchdog = ::CreateThread(
            nullptr, 0, WatchdogThread,
            reinterpret_cast<LPVOID>(static_cast<uintptr_t>(addMs)), 0, nullptr);

    NppDataWire data{ g_npp ? g_npp : g_parent, g_sci, nullptr };
    if (CallSetInfoSeh(s, &data) != 0) {
        // setInfo 抛异常：进程状态存疑，整进程陪葬（编辑器归因装载者）
        if (watchdog) ::WaitForSingleObject(watchdog, 500);
        ::ExitProcess(xfs::oop::kExitFault);
    }
    int n = 0;
    void* items = CallGetFuncsArraySeh(s, &n);
    if (!items || n < 0 || n > xfs::oop::kHandshakeItemsMax) {
        if (g_addStopEvent) ::SetEvent(g_addStopEvent);
        if (watchdog) { ::WaitForSingleObject(watchdog, 500); ::CloseHandle(watchdog); }
        if (g_addStopEvent) { ::CloseHandle(g_addStopEvent); g_addStopEvent = nullptr; }
        // 表无效但进程未坏：撤槽（不 FreeLibrary——DllMain 可能已生线程）
        ::DestroyWindow(s->wnd);
        s->wnd = nullptr;
        std::vector<Slot*> kept;
        for (auto* p : g_slots) if (p != s) kept.push_back(p);
        g_slots.swap(kept);
        delete s;
        SendReject(cookie, xfs::oop::kExitExport);
        return TRUE;
    }
    s->items = items;
    s->itemCount = n;

    xfs::oop::HandshakeWire hs{};
    hs.magic = xfs::oop::kMagic;
    hs.msg = xfs::oop::OOPM_HANDSHAKE;
    hs.itemCount = n;
    hs.cookie = cookie;
    hs.slotWnd = reinterpret_cast<UINT_PTR>(s->wnd);
    const wchar_t* pname = s->getName();
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
    SendToParent(xfs::oop::OOPM_HANDSHAKE, &hs, sizeof(hs));

    // 回填 cmdID（编辑器处理握手时同步回发 CMDIDS，已投递到本槽窗）
    if (s->cmdIdsDone && s->cmdIds.size() >= sizeof(xfs::oop::CmdIdsWire)) {
        auto* ids = reinterpret_cast<const xfs::oop::CmdIdsWire*>(s->cmdIds.data());
        const int* arr = reinterpret_cast<const int*>(ids + 1);
        int cnt = ids->count < s->itemCount ? ids->count : s->itemCount;
        for (int i = 0; i < cnt; ++i)
            *reinterpret_cast<int*>(reinterpret_cast<char*>(s->items) +
                                    (size_t)i * kStride + 136) = arr[i];
    }

    if (g_addStopEvent) ::SetEvent(g_addStopEvent);
    if (watchdog) { ::WaitForSingleObject(watchdog, 2000); ::CloseHandle(watchdog); }
    if (g_addStopEvent) { ::CloseHandle(g_addStopEvent); g_addStopEvent = nullptr; }
    return TRUE;
}

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* slot = reinterpret_cast<Slot*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_COPYDATA) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!cds || cds->dwData != xfs::oop::kMagic || !cds->lpData) return FALSE;
        if (cds->cbData < sizeof(UINT_PTR) * 2) return FALSE;
        auto* wire = reinterpret_cast<const UINT_PTR*>(cds->lpData);
        if (wire[0] != xfs::oop::kMagic) return FALSE;
        switch (wire[1]) {
            case xfs::oop::OOPM_ADD: {
                if (cds->cbData < sizeof(xfs::oop::AddWire) || !slot ||
                    slot->itemCount != 0 || slot->mod)   // 仅 slot0 受理
                    return FALSE;
                auto* aw = reinterpret_cast<xfs::oop::AddWire*>(cds->lpData);
                aw->path[xfs::oop::kAddPathMax - 1] = L'\0';   // 信任边界钳制
                return HandleAdd(aw);
            }
            case xfs::oop::OOPM_CMDIDS: {
                if (!slot || cds->cbData < sizeof(xfs::oop::CmdIdsWire)) return FALSE;
                slot->cmdIds.assign(reinterpret_cast<const unsigned char*>(cds->lpData),
                                    reinterpret_cast<const unsigned char*>(cds->lpData) +
                                        cds->cbData);
                slot->cmdIdsDone = true;
                return TRUE;
            }
            case xfs::oop::OOPM_EXEC: {
                if (!slot || cds->cbData < sizeof(xfs::oop::ExecWire)) return FALSE;
                auto* ex = reinterpret_cast<const xfs::oop::ExecWire*>(cds->lpData);
                if (ex->index < 0 || ex->index >= slot->itemCount) return FALSE;
                return CallFuncSeh(slot, ex->index) ? TRUE : FALSE;
            }
            case xfs::oop::OOPM_NOTIFY: {
                if (!slot || !slot->beNotified) return TRUE;
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
                return CallNotifySeh(slot, scn) ? TRUE : FALSE;
            }
            case xfs::oop::OOPM_SHUTDOWN:
                // 注意：SHUTDOWN 通知已随 Notify 广播送达插件，这里只退出，
                // 不二次回调 beNotified。
                ::PostQuitMessage(0);
                return TRUE;
            case xfs::oop::OOPM_MSG: {
                // messageProc 桥（v2.1）：仅编辑器白名单（值类型参数）可达此处。
                // 插件卡死 → 本消息处理不返回 → 编辑器侧 SMTO 超时自行放弃。
                if (!slot || !slot->mod ||
                    cds->cbData < sizeof(xfs::oop::MsgWire)) return FALSE;
                auto* mw = reinterpret_cast<const xfs::oop::MsgWire*>(cds->lpData);
                LRESULT res = 0;
                if (slot->messageProc)
                    res = CallMessageProcSeh(slot, static_cast<UINT>(mw->wndMsg),
                                             static_cast<WPARAM>(mw->wParam),
                                             static_cast<LPARAM>(mw->lParam));
                xfs::oop::MsgReplyWire rp{};
                rp.magic = xfs::oop::kMagic;
                rp.msg = xfs::oop::OOPM_MSGREPLY;
                rp.reqId = mw->reqId;
                rp.wndMsg = mw->wndMsg;
                rp.result = res;
                rp.slotWnd = reinterpret_cast<UINT_PTR>(slot->wnd);
                SendToParent(xfs::oop::OOPM_MSGREPLY, &rp, sizeof(rp));
                return TRUE;
            }
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

    std::wstring parentStr, nppStr, sciStr, deadlineStr;
    for (int i = 1; i < argc; ++i) {
        if (ArgValue(argv, argc, i, L"--parent", parentStr)) continue;
        if (ArgValue(argv, argc, i, L"--npp", nppStr)) continue;
        if (ArgValue(argv, argc, i, L"--sci", sciStr)) continue;
        if (ArgValue(argv, argc, i, L"--deadline", deadlineStr)) continue;
    }
    if (parentStr.empty())
        return xfs::oop::kExitExport;
    g_parent = reinterpret_cast<HWND>(wcstoull(parentStr.c_str(), nullptr, 16));
    g_npp = reinterpret_cast<HWND>(wcstoull(nppStr.c_str(), nullptr, 16));
    g_sci = reinterpret_cast<HWND>(wcstoull(sciStr.c_str(), nullptr, 16));
    if (!deadlineStr.empty())
        g_deadlineMs = static_cast<DWORD>(wcstoul(deadlineStr.c_str(), nullptr, 10));

    // 坏 DLL 不得弹「损坏的映像」硬错误框拖住后台
    ::SetErrorMode(SEM_FAILCRITICALERRORS);

    // slot0 控制窗：编辑器 ADD/SHUTDOWN 的落地端；HWND 经 OOPM_READY 报到
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"xfsWinPadPluginHostWnd";
    ::RegisterClassExW(&wc);
    auto* ctl = new Slot();               // slot0 无插件：仅占位承载 GWLP_USERDATA
    HWND self = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, ctl);
    if (!self) return xfs::oop::kExitLoadFail;
    ctl->wnd = self;
    g_slots.push_back(ctl);

    UINT_PTR ready[2] = { xfs::oop::kMagic, xfs::oop::OOPM_READY };
    SendToParent(xfs::oop::OOPM_READY, ready, sizeof(ready));

    // ---- 消息泵：ADD/EXEC/NOTIFY/SHUTDOWN 由 HostWndProc 按槽位处理 ----------
    MSG m;
    while (::GetMessageW(&m, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
    return xfs::oop::kExitClean;
}
