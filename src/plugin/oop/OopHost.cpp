// OopHost.cpp — 进程外插件桥（编辑器侧编排器），v2 多槽位共享代理实现。
//
// 代理池：PickProcess 找有空位的活代理，无则 SpawnProcess 新建并泵到
// OOPM_READY。装载经 AddOne：对 slot0 发 OOPM_ADD（同步语义——阻塞期间
// HANDSHAKE/REJECT 以 incoming sent message 送达本线程，CMDIDS 再同步回
// 槽窗，链条与 v1 启动序列同构，无死锁）。死亡归因：Launch 期间代理死亡
// 记在正被装载的插件头上（v1 killer 语义），同车幸存者撤销命令后连同
// 未试者重进新代理；每轮死亡净淘汰一个嫌疑插件，必然收敛（最坏退化为
// 一插件一代理）。运行期死亡（无装载在途）：同车全部记账 oop-died。
#define WIN32_LEAN_AND_MEAN
#include "OopHost.h"
#include "../PluginManager.h"
#include "../../core/Log.h"
#include "../../core/Util.h"

namespace xfs {

namespace {
constexpr wchar_t kHostExe[] = L"xfsWinPadPluginHost.exe";
constexpr wchar_t kRecvClass[] = L"xfsWinPadOopHostWnd";
constexpr UINT kDeadMsg = WM_APP + 0x0F01;   // wp=procs_ 下标，lp=退出码

void AddFailure(std::vector<PluginLoadFailure>& out, const std::wstring& path,
                const wchar_t* reason) {
    PluginLoadFailure f;
    f.path = path;
    f.reason = reason;
    out.push_back(std::move(f));
}

const wchar_t* MapReject(UINT_PTR reason) {
    switch (reason) {
        case xfs::oop::kExitLoadFail: return L"oop-load";
        case xfs::oop::kExitAnsi:     return L"oop-ansi";
        case xfs::oop::kExitExport:   return L"oop-export";
        case xfs::oop::kExitFault:    return L"oop-fault";
        default:                      return L"oop-reject";
    }
}
} // namespace

// ---- 生命周期 ---------------------------------------------------------------

OopHost::~OopHost() { ShutdownAll(); }

bool OopHost::EnsureSelfWnd() {
    if (selfWnd_) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kRecvClass;
    ::RegisterClassExW(&wc);
    selfWnd_ = ::CreateWindowExW(0, kRecvClass, L"", 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (!selfWnd_)
        Logger::Error("OopHost: receiver window creation failed");
    return selfWnd_ != nullptr;
}

bool OopHost::Launch(PluginManager& mgr, const std::wstring& dllPath,
                     HWND editorWnd, HWND sciMain, unsigned timeoutMs) {
    mgr_ = &mgr;
    editorWnd_ = editorWnd;
    sciMain_ = sciMain;
    if (!EnsureSelfWnd()) {
        AddFailure(failures_, dllPath, L"oop-host");
        return false;
    }

    std::vector<std::wstring> todo(1, dllPath);
    int guard = 0;
    while (!todo.empty() && guard++ < kPluginsPerProxy * 8) {
        size_t pi = PickProcess();
        if (pi == static_cast<size_t>(-1)) {
            std::wstring err;
            if (!SpawnProcess(timeoutMs, err)) {
                Logger::Error("OopHost: proxy spawn failed (" + WideToUtf8(err) + ")");
                for (auto& d : todo) AddFailure(failures_, d, L"oop-spawn");
                todo.clear();
                break;
            }
            pi = procs_.size() - 1;
        }

        size_t i = 0;
        bool died = false;
        for (; i < todo.size(); ++i) {
            UINT_PTR reason = 0;
            AddResult r = AddOne(pi, todo[i], timeoutMs, reason);
            if (r == AddResult::kAdded) continue;
            if (r == AddResult::kRejected) {
                AddFailure(failures_, todo[i], MapReject(reason));
                continue;
            }
            died = true;   // kDied
            break;
        }
        if (!died) { todo.clear(); break; }

        // 死亡归因：元凶 = 正被装载的 todo[i]；同车幸存者先重加（大概率
        // 无辜，尽快恢复服务），未试者随后，元凶出局记 oop-died。
        std::vector<std::wstring> requeue;
        CollectSurvivors(pi, requeue);
        AddFailure(failures_, todo[i], L"oop-died");
        Logger::Info("OopHost: attribution: '" + WideToUtf8(todo[i]) +
                     "' killed proxy #" + std::to_string(pi) +
                     "; re-adding " + std::to_string(requeue.size()) +
                     " survivor(s), " +
                     std::to_string(todo.size() - i - 1) + " untried");
        std::vector<std::wstring> next;
        for (auto& s : requeue) next.push_back(s);
        for (size_t j = i + 1; j < todo.size(); ++j) next.push_back(todo[j]);
        todo.swap(next);
    }

    for (auto& p : plugins_)
        if (!p->dead && p->info.dllPath == dllPath) return true;
    return false;
}

size_t OopHost::PickProcess() {
    for (size_t i = 0; i < procs_.size(); ++i) {
        if (procs_[i]->dead || !procs_[i]->ready) continue;
        int live = 0;
        for (auto& p : plugins_)
            if (p->procIdx == static_cast<int>(i) && !p->dead) ++live;
        if (live < kPluginsPerProxy) return i;
    }
    return static_cast<size_t>(-1);
}

bool OopHost::SpawnProcess(unsigned deadline, std::wstring& err) {
    wchar_t exePath[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir.resize(slash + 1);
    std::wstring hostExe = exeDir + kHostExe;

    wchar_t parentHex[24] = {}, nppHex[24] = {}, sciHex[24] = {};
    // parent = 本类接收窗（READY/HANDSHAKE/REJECT/CMDIDS 通道落地端）；
    // npp    = 编辑器主窗口（插件 setInfo 的 nppHandle，NPPM_* 跨进程直达）。
    swprintf_s(parentHex, L"%llx",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(selfWnd_)));
    swprintf_s(nppHex, L"%llx",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(editorWnd_)));
    swprintf_s(sciHex, L"%llx",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(sciMain_)));

    std::wstring args = L"\"" + hostExe + L"\" --parent " + parentHex +
                        L" --npp " + nppHex + L" --sci " + sciHex +
                        L" --deadline " + std::to_wstring(deadline);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(hostExe.c_str(), args.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, exeDir.c_str(), &si, &pi)) {
        err = L"createprocess-gle" + std::to_wstring(::GetLastError());
        Logger::Error("OopHost: CreateProcess(" + WideToUtf8(kHostExe) +
                      ") failed gle=" + std::to_string(::GetLastError()));
        return false;
    }
    ::CloseHandle(pi.hThread);

    auto pr = std::make_unique<Process>();
    pr->proc = pi.hProcess;
    procs_.push_back(std::move(pr));
    const size_t idx = procs_.size() - 1;
    StartWatchdog(idx);

    pendingReady_ = false;
    pendingReadyWnd_ = nullptr;
    const DWORD start = ::GetTickCount();
    while (::GetTickCount() - start < deadline) {
        HANDLE h = procs_[idx]->proc;
        DWORD w = ::MsgWaitForMultipleObjects(1, &h, FALSE, 100, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0 || procs_[idx]->dead) {
            err = L"died-before-ready";
            DWORD code = 0;
            ::GetExitCodeProcess(h, &code);
            HandleProcessDead(idx, code, /*recordFailures=*/false);
            return false;
        }
        PumpOnce(40);
        if (pendingReady_) {
            procs_[idx]->addWnd = pendingReadyWnd_;
            procs_[idx]->ready = true;
            Logger::Info("OopHost: proxy #" + std::to_string(idx) + " ready");
            return true;
        }
    }
    err = L"ready-timeout";
    ::TerminateProcess(procs_[idx]->proc, xfs::oop::kExitWatchdog);
    return false;
}

OopHost::AddResult OopHost::AddOne(size_t pi, const std::wstring& dllPath,
                                   unsigned deadline, UINT_PTR& rejectReason) {
    Process* P = procs_[pi].get();
    pendingCookie_ = ++cookieSeq_;
    pendingPath_ = dllPath;
    pendingPlugin_.reset();
    pendingReject_ = false;
    pendingReason_ = 0;

    if (dllPath.size() >= xfs::oop::kAddPathMax) {
        rejectReason = xfs::oop::kExitLoadFail;
        pendingCookie_ = 0;
        return AddResult::kRejected;
    }
    xfs::oop::AddWire w{};
    w.magic = xfs::oop::kMagic;
    w.msg = xfs::oop::OOPM_ADD;
    w.cookie = pendingCookie_;
    w.deadlineMs = deadline;
    wcsncpy_s(w.path, dllPath.c_str(), _TRUNCATE);
    COPYDATASTRUCT cds{};
    cds.dwData = xfs::oop::kMagic;
    cds.cbData = sizeof(w);
    cds.lpData = &w;

    // ADD 在代理侧同步完成装载+握手（含插件 setInfo）；SMTO_ABORTIFHUNG
    // 只挡真挂死，忙（插件在回调里跑长任务）不误杀。上限：装载看门狗时限
    // + 富余——代理看门狗必先到场，超时兜底是异常中的异常。
    ::SendMessageTimeoutW(P->addWnd, WM_COPYDATA,
                          reinterpret_cast<WPARAM>(selfWnd_),
                          reinterpret_cast<LPARAM>(&cds),
                          SMTO_ABORTIFHUNG, deadline + 5000, nullptr);

    const DWORD start = ::GetTickCount();
    const DWORD cap = deadline + 5000;
    while (true) {
        if (pendingPlugin_) {
            pendingCookie_ = 0;
            CommitPending(pi);
            return AddResult::kAdded;
        }
        if (pendingReject_) {
            pendingReject_ = false;
            rejectReason = pendingReason_;
            pendingCookie_ = 0;
            return AddResult::kRejected;
        }
        if (P->dead || ::WaitForSingleObject(P->proc, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            ::GetExitCodeProcess(P->proc, &code);
            // 归因在途：记账与幸存者回收由 Launch 统一处理，此处不记失败
            HandleProcessDead(pi, code, /*recordFailures=*/false);
            pendingCookie_ = 0;
            return AddResult::kDied;
        }
        if (::GetTickCount() - start > cap) {
            // 代理存活却不应答：罕见病态（前插件回调长阻塞）。兜底整程
            // 陪葬——幸存者走同一条重加路径，语义不变。
            Logger::Error("OopHost: add timed out alive; recycling proxy");
            ::TerminateProcess(P->proc, xfs::oop::kExitWatchdog);
            ::WaitForSingleObject(P->proc, 2000);
            DWORD code = 0;
            ::GetExitCodeProcess(P->proc, &code);
            HandleProcessDead(pi, code, /*recordFailures=*/false);
            pendingCookie_ = 0;
            return AddResult::kDied;
        }
        HANDLE h = P->proc;
        ::MsgWaitForMultipleObjects(1, &h, FALSE, 100, QS_ALLINPUT);
        PumpOnce(20);
    }
}

void OopHost::CommitPending(size_t pi) {
    pendingPlugin_->procIdx = static_cast<int>(pi);
    Logger::Info("OopHost: '" + WideToUtf8(pendingPlugin_->info.name) +
                 "' on proxy #" + std::to_string(pi) + ", " +
                 std::to_string(pendingPlugin_->info.itemCount) + " command(s)");
    plugins_.push_back(std::move(pendingPlugin_));
    pendingPlugin_.reset();
}

void OopHost::CollectSurvivors(size_t pi, std::vector<std::wstring>& out) {
    for (auto& p : plugins_) {
        if (p->procIdx != static_cast<int>(pi) || p->dead) continue;
        p->dead = true;
        p->hostWnd = nullptr;
        // 撤销旧命令：重加会重新注册；不撤销则统一命令表出现重复项，
        // 且旧 ctx 指向已死槽窗。
        if (mgr_)
            for (auto* h : p->cmdHandles) mgr_->HostRemoveCommand(h);
        p->cmdHandles.clear();
        p->cmdIds.clear();
        out.push_back(p->info.dllPath);
    }
}

// ---- WM_COPYDATA 落地端（READY / HANDSHAKE / REJECT）-------------------------

LRESULT CALLBACK OopHost::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == kDeadMsg) {   // 看门狗线程 → UI 线程：代理死亡记账
        auto* self = reinterpret_cast<OopHost*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self)
            self->HandleProcessDead(static_cast<size_t>(wp),
                                    static_cast<DWORD>(lp),
                                    /*recordFailures=*/self->pendingCookie_ == 0);
        return 0;
    }
    auto* self = reinterpret_cast<OopHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

void OopHost::HandleProcessDead(size_t pi, DWORD exitCode, bool recordFailures) {
    if (pi >= procs_.size()) return;
    Process& P = *procs_[pi];
    if (P.dead) return;
    P.dead = true;
    Logger::Error("OopHost: proxy #" + std::to_string(pi) +
                  " exited code=" + std::to_string(exitCode) +
                  " (isolated; editor unaffected)");
    if (!recordFailures) return;   // 装载在途：Launch 归因路径接管
    for (auto& p : plugins_) {
        if (p->procIdx != static_cast<int>(pi) || p->dead) continue;
        p->dead = true;
        p->hostWnd = nullptr;
        AddFailure(failures_, p->info.dllPath, L"oop-died");
    }
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

    if (wire[1] == xfs::oop::OOPM_READY) {
        pendingReady_ = true;
        pendingReadyWnd_ = reinterpret_cast<HWND>(wp);
        return TRUE;
    }
    if (wire[1] == xfs::oop::OOPM_REJECT) {
        if (cds->cbData < sizeof(xfs::oop::RejectWire)) return FALSE;
        auto* rj = reinterpret_cast<const xfs::oop::RejectWire*>(cds->lpData);
        if (pendingCookie_ != 0 && rj->cookie == pendingCookie_) {
            pendingReject_ = true;
            pendingReason_ = rj->reason;
        }
        return TRUE;
    }
    if (wire[1] == xfs::oop::OOPM_MSGREPLY) {
        if (cds->cbData < sizeof(xfs::oop::MsgReplyWire)) return FALSE;
        auto* rp = reinterpret_cast<const xfs::oop::MsgReplyWire*>(cds->lpData);
        for (auto& w : msgWaits_) {
            if (w.reqId == rp->reqId) { w.results.push_back(rp->result); break; }
        }
        return TRUE;   // 过期 reqId 静默丢弃
    }
    if (wire[1] != xfs::oop::OOPM_HANDSHAKE ||
        cds->cbData < sizeof(xfs::oop::HandshakeWire))
        return FALSE;
    auto* hs = reinterpret_cast<const xfs::oop::HandshakeWire*>(cds->lpData);
    if (pendingCookie_ == 0 || hs->cookie != pendingCookie_) {
        Logger::Error("OopHost: unexpected handshake cookie");
        return FALSE;
    }
    const int n = hs->itemCount;
    if (n < 0 || n > xfs::oop::kHandshakeItemsMax) return FALSE;

    auto plugin = std::make_unique<Plugin>();
    plugin->hostWnd = reinterpret_cast<HWND>(hs->slotWnd);
    plugin->info.dllPath = pendingPath_;
    plugin->info.name = hs->pluginName;
    plugin->info.itemCount = n;

    std::vector<int> ids(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        const auto& it = hs->items[i];
        std::wstring label = it.name[0] ? it.name : L"(unnamed)";
        auto ctx = std::make_unique<OopExecCtx>();
        ctx->host = this;
        ctx->hostWnd = plugin->hostWnd;
        ctx->index = i;

        xfs_plugin_command* h =
            mgr_->HostAddCommand(WideToUtf8(label).c_str(),
                                 WideToUtf8(plugin->info.name).c_str(),
                                 &OopHost::ExecTrampoline, ctx.get());
        if (!h) continue;
        ids[(size_t)i] = static_cast<int>(mgr_->CommandIdOfHandle(h));
        plugin->cmdIds.push_back(ids[(size_t)i]);
        plugin->cmdHandles.push_back(h);
        plugin->ctxs.push_back(std::move(ctx));

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

    // 回填 cmdID（同步：SendMessage 返回即已送达槽窗）
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
    ::SendMessageW(plugin->hostWnd, WM_COPYDATA,
                   reinterpret_cast<WPARAM>(selfWnd_),
                   reinterpret_cast<LPARAM>(&r));

    pendingPlugin_ = std::move(plugin);
    return TRUE;
}

bool OopHost::PumpOnce(unsigned sliceMs) {
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
    for (auto& p : plugins_) {
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

LRESULT OopHost::BroadcastMessage(UINT msg, WPARAM wp, LPARAM lp, bool* handled,
                                  unsigned timeoutMs) {
    if (handled) *handled = false;
    std::vector<HWND> slots;
    for (auto& p : plugins_)
        if (!p->dead && p->hostWnd) slots.push_back(p->hostWnd);
    if (slots.empty()) return 0;

    msgWaits_.push_back(MsgWait{ ++msgReqSeq_, {} });
    const UINT_PTR id = msgWaits_.back().reqId;
    auto collected = [id, this]() {
        for (auto& w : msgWaits_)
            if (w.reqId == id) return static_cast<int>(w.results.size());
        return 0;
    };

    int expected = 0;
    for (HWND h : slots) {
        xfs::oop::MsgWire w{};
        w.magic = xfs::oop::kMagic;
        w.msg = xfs::oop::OOPM_MSG;
        w.reqId = id;
        w.wndMsg = msg;
        w.wParam = static_cast<UINT_PTR>(wp);
        w.lParam = static_cast<LONG_PTR>(lp);
        COPYDATASTRUCT cds{};
        cds.dwData = xfs::oop::kMagic;
        cds.cbData = sizeof(w);
        cds.lpData = &w;
        // WM_COPYDATA 同步语义：SMTO 正常返回时该槽回包已记入 Handle()。
        // 被 ABORT 的慢槽可能在恢复后迟到回包，由下方短暂泵等收。
        if (::SendMessageTimeoutW(h, WM_COPYDATA, reinterpret_cast<WPARAM>(selfWnd_),
                                  reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG,
                                  timeoutMs, nullptr))
            ++expected;
    }

    DWORD end = ::GetTickCount() + 500;
    while (collected() < expected && ::GetTickCount() < end) PumpOnce(20);

    LRESULT result = 0;
    for (auto it = msgWaits_.begin(); it != msgWaits_.end(); ++it) {
        if (it->reqId != id) continue;
        for (LONG_PTR r : it->results) {
            if (r == 0) continue;
            result = r;                       // 首个非零 = NPP 广播消费语义
            if (handled) *handled = true;
            break;
        }
        msgWaits_.erase(it);
        break;
    }
    return result;
}

// ---- 代理生命周期观察 ---------------------------------------------------------

void OopHost::StartWatchdog(size_t procIdx) {    HANDLE proc = procs_[procIdx]->proc;
    HWND self = selfWnd_;
    procs_[procIdx]->watchdog = std::thread([this, proc, self, procIdx]() {
        ::WaitForSingleObject(proc, INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(proc, &code);
        ::PostMessageW(self, kDeadMsg, static_cast<WPARAM>(procIdx),
                       static_cast<LPARAM>(code));
    });
}

void OopHost::ShutdownAll() {
    // 先置 dead：看门狗随后的死亡通知被 HandleProcessDead 的 dead 短路忽略，
    // 干净关停绝不记 oop-died 失败。
    for (auto& p : plugins_) {
        p->dead = true;
        p->hostWnd = nullptr;
    }
    for (auto& pr : procs_) {
        if (pr->dead) continue;
        pr->dead = true;
        if (pr->ready && pr->addWnd) {
            UINT_PTR m[2] = { xfs::oop::kMagic, xfs::oop::OOPM_SHUTDOWN };
            COPYDATASTRUCT cds{};
            cds.dwData = xfs::oop::kMagic;
            cds.cbData = sizeof(m);
            cds.lpData = m;
            ::SendMessageTimeoutW(pr->addWnd, WM_COPYDATA,
                                  reinterpret_cast<WPARAM>(selfWnd_),
                                  reinterpret_cast<LPARAM>(&cds),
                                  SMTO_ABORTIFHUNG, 1000, nullptr);
        }
        if (pr->proc &&
            ::WaitForSingleObject(pr->proc, 2000) != WAIT_OBJECT_0) {
            ::TerminateProcess(pr->proc, xfs::oop::kExitClean);
        }
    }
    for (auto& pr : procs_)
        if (pr->watchdog.joinable()) pr->watchdog.join();
    for (auto& pr : procs_)
        if (pr->proc) { ::CloseHandle(pr->proc); pr->proc = nullptr; }
    // 先撤销命令再销毁 ctx（命令 impl 引用 ctx，悬空即 UAF 隐患）
    if (mgr_) {
        for (auto& p : plugins_)
            for (auto* h : p->cmdHandles) mgr_->HostRemoveCommand(h);
    }
    plugins_.clear();
    procs_.clear();
    pendingPlugin_.reset();
    pendingCookie_ = 0;
    if (selfWnd_) { ::DestroyWindow(selfWnd_); selfWnd_ = nullptr; }
}

std::vector<OopPluginInfo> OopHost::Alive() const {
    std::vector<OopPluginInfo> out;
    for (auto& p : plugins_)
        if (!p->dead) out.push_back(p->info);
    return out;
}

int OopHost::ProcessCount() const {
    int n = 0;
    for (auto& pr : procs_)
        if (!pr->dead) ++n;
    return n;
}

std::vector<PluginLoadFailure> OopHost::TakeFailures() {
    return std::move(failures_);
}

} // namespace xfs
