// OopHost.cpp — 进程外插件桥（编辑器侧编排器），v1 实现。
//
// 启动（Launch，UI 线程同步）：CreateProcess(代理) → 循环
// { MsgWaitForMultipleObjects(进程句柄, QS_SENDMESSAGE) + PeekMessage 泵 }。
// 代理的 SendMessage(HANDSHAKE) 只在本线程泵消息时投递；Handle() 收到后
// 注册命令并经 OOPM_CMDIDS 同步回填，代理收到即完成启动。
// 运行期：命令 → OOPM_EXEC（ctx 携带 index）；通知 → OOPM_NOTIFY。
// 代理死亡：看门狗线程等进程句柄 → PostMessage(WM_APP+x) → UI 线程记账。
#define WIN32_LEAN_AND_MEAN
#include "OopHost.h"
#include "../PluginManager.h"
#include "../../core/Log.h"
#include "../../core/Util.h"

namespace xfs {

namespace {
constexpr wchar_t kHostExe[] = L"xfsWinPadPluginHost.exe";
constexpr wchar_t kRecvClass[] = L"xfsWinPadOopHostWnd";
constexpr UINT kDeadMsg = WM_APP + 0x0F01;   // wp=index into proxies_, lp=exit code

void AddFailure(std::vector<PluginLoadFailure>& out, const std::wstring& path,
                const wchar_t* reason) {
    PluginLoadFailure f;
    f.path = path;
    f.reason = reason;
    out.push_back(std::move(f));
}
} // namespace

// ---- 生命周期 ---------------------------------------------------------------

OopHost::~OopHost() { ShutdownAll(); }

bool OopHost::Launch(PluginManager& mgr, const std::wstring& dllPath,
                     HWND editorWnd, HWND sciMain, unsigned timeoutMs) {
    mgr_ = &mgr;
    editorWnd_ = editorWnd;

    // message-only 接收窗（首次 Launch 创建）
    if (!selfWnd_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProcThunk;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kRecvClass;
        ::RegisterClassExW(&wc);
        selfWnd_ = ::CreateWindowExW(0, kRecvClass, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!selfWnd_) {
            Logger::Error("OopHost: receiver window creation failed");
            AddFailure(failures_, dllPath, L"oop-host");
            return false;
        }
    }

    // 代理 exe 与编辑器同目录
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir.resize(slash + 1);
    std::wstring hostExe = exeDir + kHostExe;

    wchar_t parentHex[24] = {}, nppHex[24] = {}, sciHex[24] = {};
    // parent = 本类接收窗（OOPM_HANDSHAKE/CMDIDS/EXEC 通道落地端）；
    // npp    = 编辑器主窗口（插件 setInfo 的 nppHandle，NPPM_* 跨进程直达）。
    // 两者分离：消息窗口不是合法的 NPPM_* 目标，主窗口不处理 OOP 魔数。
    swprintf_s(parentHex, L"%llx",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(selfWnd_)));
    swprintf_s(nppHex, L"%llx",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(editorWnd)));
    swprintf_s(sciHex, L"%llx",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(sciMain)));

    std::wstring args = L"\"" + hostExe + L"\" --parent " + parentHex +
                        L" --plugin \"" + dllPath + L"\" --npp " + nppHex +
                        L" --sci " + sciHex;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(hostExe.c_str(), args.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, exeDir.c_str(), &si, &pi)) {
        Logger::Error("OopHost: CreateProcess(" + WideToUtf8(kHostExe) +
                      ") failed gle=" + std::to_string(::GetLastError()));
        AddFailure(failures_, dllPath, L"oop-spawn");
        return false;
    }
    ::CloseHandle(pi.hThread);

    // ---- 握手等待：泵消息直到 HANDSHAKE 处理完成（pendingProxy_ 就绪）----
    auto proxy = std::make_unique<Proxy>();
    const DWORD start = ::GetTickCount();
    bool ok = false;
    while (::GetTickCount() - start < timeoutMs) {
        DWORD w = ::MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 100,
                                              QS_SENDMESSAGE);
        if (w == WAIT_OBJECT_0) {              // 代理在握手期死亡
            DWORD code = 0;
            ::GetExitCodeProcess(pi.hProcess, &code);
            Logger::Error("OopHost: proxy died during handshake exit=" +
                          std::to_string(code) + " plugin=" + WideToUtf8(dllPath));
            AddFailure(failures_, dllPath, L"oop-died");
            ::CloseHandle(pi.hProcess);
            return false;
        }
        PumpOnce(pi.hProcess, 60);
        if (pendingProxy_) { ok = true; break; }
    }
    if (!ok) {                                  // 超时：杀代理（看门狗多半已自尽）
        Logger::Error("OopHost: handshake timeout " + std::to_string(timeoutMs) +
                      "ms plugin=" + WideToUtf8(dllPath));
        ::TerminateProcess(pi.hProcess, xfs::oop::kExitWatchdog);
        ::CloseHandle(pi.hProcess);
        AddFailure(failures_, dllPath, L"oop-timeout");
        return false;
    }

    *proxy = std::move(*pendingProxy_);
    pendingProxy_.reset();
    proxy->proc = pi.hProcess;
    StartWatchdog(*proxy);
    proxies_.push_back(std::move(proxy));

    Logger::Info("OopHost: '" + WideToUtf8(proxies_.back()->info.name) +
                 "' out-of-process, " +
                 std::to_string(proxies_.back()->info.itemCount) + " command(s)");
    return true;
}

bool OopHost::PumpOnce(HANDLE, unsigned sliceMs) {
    DWORD end = ::GetTickCount() + sliceMs;
    MSG m;
    while (::GetTickCount() < end) {
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
        ::Sleep(10);
    }
    return true;
}

// ---- WM_COPYDATA 落地端（HANDSHAKE）-----------------------------------------

LRESULT CALLBACK OopHost::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    if (msg == kDeadMsg) {   // 看门狗线程 → UI 线程：代理死亡记账
        auto* self = reinterpret_cast<OopHost*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) self->Handle(static_cast<size_t>(wp), static_cast<DWORD>(lp));
        return 0;
    }
    auto* self = reinterpret_cast<OopHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

void OopHost::Handle(size_t index, DWORD exitCode) {
    if (index >= proxies_.size()) return;
    Proxy& p = *proxies_[index];
    if (p.dead) return;
    p.dead = true;
    Logger::Error("OopHost: plugin '" + WideToUtf8(p.info.name) +
                  "' proxy exited code=" + std::to_string(exitCode) +
                  " (isolated; editor unaffected)");
    AddFailure(failures_, p.info.dllPath, L"oop-died");
    p.hostWnd = nullptr;
}

LRESULT OopHost::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_COPYDATA)
        return ::DefWindowProcW(selfWnd_, msg, wp, lp);

    auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
    if (!cds || cds->dwData != xfs::oop::kMagic || !cds->lpData ||
        cds->cbData < sizeof(UINT_PTR) * 2)
        return FALSE;
    auto* wire = reinterpret_cast<const UINT_PTR*>(cds->lpData);
    if (wire[0] != xfs::oop::kMagic) return FALSE;
    if (wire[1] != xfs::oop::OOPM_HANDSHAKE ||
        cds->cbData < sizeof(xfs::oop::HandshakeWire))
        return FALSE;
    auto* hs = reinterpret_cast<const xfs::oop::HandshakeWire*>(cds->lpData);
    const int n = hs->itemCount;
    if (n < 0 || n > xfs::oop::kHandshakeItemsMax) return FALSE;

    auto proxy = std::make_unique<Proxy>();
    proxy->hostWnd = reinterpret_cast<HWND>(wp);
    proxy->info.dllPath = pendingDllPath_;
    proxy->info.name = hs->pluginName;
    proxy->info.itemCount = n;

    std::vector<int> ids(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        const auto& it = hs->items[i];
        std::wstring label = it.name[0] ? it.name : L"(unnamed)";
        auto ctx = std::make_unique<OopExecCtx>();
        ctx->host = this;
        ctx->hostWnd = proxy->hostWnd;
        ctx->index = i;

        xfs_plugin_command* h =
            mgr_->HostAddCommand(WideToUtf8(label).c_str(),
                                 WideToUtf8(proxy->info.name).c_str(),
                                 &OopHost::ExecTrampoline, ctx.get());
        if (!h) continue;
        ids[(size_t)i] = static_cast<int>(mgr_->CommandIdOfHandle(h));
        proxy->cmdIds.push_back(ids[(size_t)i]);
        proxy->cmdHandles.push_back(h);
        proxy->ctxs.push_back(std::move(ctx));

        if (PluginCommand* pc = mgr_->CommandPtrAt(h)) {
            pc->grouped = true;
            if (it.key) {
                pc->hasKey = true;
                pc->vk = it.key;
                BYTE f = FVIRTKEY;
                if (it.ctrl)  f |= FCONTROL;
                if (it.alt)   f |= FALT;
                if (it.shift) f |= FSHIFT;
                pc->fVirt = f;
            }
        }
    }

    // 回填 cmdID（同步：SendMessage 返回即已送达代理）
    std::vector<unsigned char> buf(sizeof(xfs::oop::CmdIdsWire) +
                                   sizeof(int) * static_cast<size_t>(n));
    auto* out = reinterpret_cast<xfs::oop::CmdIdsWire*>(buf.data());
    out->magic = xfs::oop::kMagic;
    out->msg = xfs::oop::OOPM_CMDIDS;
    out->count = n;
    for (int i = 0; i < n; ++i)
        reinterpret_cast<int*>(out + 1)[i] = ids[(size_t)i];
    COPYDATASTRUCT r{};
    r.dwData = xfs::oop::kMagic;
    r.cbData = static_cast<DWORD>(buf.size());
    r.lpData = buf.data();
    ::SendMessageW(proxy->hostWnd, WM_COPYDATA,
                   reinterpret_cast<WPARAM>(selfWnd_),
                   reinterpret_cast<LPARAM>(&r));

    pendingProxy_ = std::move(proxy);
    return TRUE;
}

// ---- 命令执行 / 通知 ---------------------------------------------------------

void OopHost::ExecTrampoline(void* user) {
    auto* ctx = static_cast<OopExecCtx*>(user);
    ctx->host->SendExec(ctx->hostWnd, ctx->index);
}

void OopHost::SendExec(HWND hostWnd, int index) {
    if (!hostWnd) return;
    xfs::oop::ExecWire w{};
    w.magic = xfs::oop::kMagic;
    w.msg = xfs::oop::OOPM_EXEC;
    w.index = index;
    COPYDATASTRUCT cds{};
    cds.dwData = xfs::oop::kMagic;
    cds.cbData = sizeof(w);
    cds.lpData = &w;
    // 1s 限期：代理卡死时编辑器不陪葬（SEH 崩溃路径返回 FALSE 同样不挂）
    ::SendMessageTimeoutW(hostWnd, WM_COPYDATA,
                          reinterpret_cast<WPARAM>(selfWnd_),
                          reinterpret_cast<LPARAM>(&cds),
                          SMTO_ABORTIFHUNG, 1000, nullptr);
}

void OopHost::BroadcastNotify(int code, UINT_PTR idFrom) {
    for (auto& p : proxies_) {
        if (p->dead || !p->hostWnd) continue;
        xfs::oop::ExecWire w{};
        w.magic = xfs::oop::kMagic;
        w.msg = xfs::oop::OOPM_NOTIFY;
        w.code = static_cast<unsigned int>(code);
        w.idFrom = idFrom;
        COPYDATASTRUCT cds{};
        cds.dwData = xfs::oop::kMagic;
        cds.cbData = sizeof(w);
        cds.lpData = &w;
        ::SendMessageTimeoutW(p->hostWnd, WM_COPYDATA,
                              reinterpret_cast<WPARAM>(selfWnd_),
                              reinterpret_cast<LPARAM>(&cds),
                              SMTO_ABORTIFHUNG, 1000, nullptr);
    }
}

// ---- 代理生命周期观察 ---------------------------------------------------------

void OopHost::StartWatchdog(Proxy& p) {
    HANDLE proc = p.proc;
    HWND self = selfWnd_;
    // 代理在 proxies_ 的下标在 push_back 前已知
    size_t index = proxies_.size();
    p.watchdog = std::thread([proc, self, index]() {
        ::WaitForSingleObject(proc, INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(proc, &code);
        ::PostMessageW(self, kDeadMsg, static_cast<WPARAM>(index),
                       static_cast<LPARAM>(code));
    });
}

void OopHost::ShutdownAll() {
    for (auto& p : proxies_) {
        if (p->dead) continue;
        // 先置 dead：看门狗随后的死亡通知被 Handle() 的 dead 短路忽略，
        // 干净关停绝不记 oop-died 失败。
        p->dead = true;
        if (p->hostWnd) {
            UINT_PTR m[2] = { xfs::oop::kMagic, xfs::oop::OOPM_SHUTDOWN };
            COPYDATASTRUCT cds{};
            cds.dwData = xfs::oop::kMagic;
            cds.cbData = sizeof(m);
            cds.lpData = m;
            ::SendMessageTimeoutW(p->hostWnd, WM_COPYDATA,
                                  reinterpret_cast<WPARAM>(selfWnd_),
                                  reinterpret_cast<LPARAM>(&cds),
                                  SMTO_ABORTIFHUNG, 1000, nullptr);
        }
        if (p->proc &&
            ::WaitForSingleObject(p->proc, 2000) != WAIT_OBJECT_0) {
            ::TerminateProcess(p->proc, xfs::oop::kExitClean);
        }
    }
    for (auto& p : proxies_)
        if (p->watchdog.joinable()) p->watchdog.join();
    // 先撤销命令再销毁 ctx（命令 impl 引用 ctx，悬空即 UAF 隐患）
    if (mgr_) {
        for (auto& p : proxies_)
            for (auto* h : p->cmdHandles) mgr_->HostRemoveCommand(h);
    }
    proxies_.clear();
    if (selfWnd_) { ::DestroyWindow(selfWnd_); selfWnd_ = nullptr; }
}

std::vector<OopPluginInfo> OopHost::Alive() const {
    std::vector<OopPluginInfo> out;
    for (auto& p : proxies_)
        if (!p->dead) out.push_back(p->info);
    return out;
}

std::vector<PluginLoadFailure> OopHost::TakeFailures() {
    return std::move(failures_);
}

} // namespace xfs
