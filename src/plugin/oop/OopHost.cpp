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
#include "NppmMarshal.h"
#include "SciBridge.h"
#include "DmmMarshal.h"
#include "../npp/NppDocking.h"
#include "../PluginManager.h"
#include "../../core/Log.h"
#include "../../core/Util.h"
#include <cstring>

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
    // 死掉的代理所承载的插件对话框记录一并作废（v2.8）：HWND 会被系统复用，
    // 留一条死记录等于给"将来复用同一 HWND 值的陌生窗口"发通行证。
    // 判据用记录里的**代理中转窗**：代理进程一退，它的窗口全部销毁。
    // 这里不看 recordFailures —— 无论走哪条归因路径，记录都该作废。
    for (auto it = dmmClients_.begin(); it != dmmClients_.end();) {
        if (!it->second || !::IsWindow(it->second)) it = dmmClients_.erase(it);
        else ++it;
    }
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
    if (wire[1] == xfs::oop::OOPM_NPPMCALL) {
        // NPPM_* 过桥请求（v2.2）：中转窗在代理进程里接住插件的调用，
        // 把编号/值参数/入参串封送过来。这里在**宿主本地**重建所有指针。
        if (cds->cbData < sizeof(xfs::oop::NppmCallWire)) return FALSE;
        auto* cw = reinterpret_cast<const xfs::oop::NppmCallWire*>(cds->lpData);
        if (cw->magic != xfs::oop::kMagic) return FALSE;
        HandleNppmCall(*cw, cds->cbData);
        return TRUE;
    }
    if (wire[1] == xfs::oop::OOPM_SCICALL) {
        // SCI_* **入参**过桥请求（v2.4）：同上，只是参数形态更多（串可能在
        // wp、可能两段、长度可能由 wp 给出）—— 布局表是两侧共用的生成物。
        if (cds->cbData < sizeof(xfs::oop::SciCallWire)) return FALSE;
        auto* sw = reinterpret_cast<const xfs::oop::SciCallWire*>(cds->lpData);
        if (sw->magic != xfs::oop::kMagic) return FALSE;
        HandleSciCall(*sw, cds->cbData);
        return TRUE;
    }
    if (wire[1] == xfs::oop::OOPM_SCIOUTCALL) {
        // SCI_* **出参**过桥请求（v2.5，批次 114）+ 出入参族（批次 115）：
        // wire 里的字节是**入参串**（kInOutStr 族才有），"插件想要多少"由
        // writeBack 表达。容量规则（NUL 语义差一个字节）来自两侧共用的
        // SciOutTable.inc / SciInOutTable.inc。
        //
        // ⚠ cbData 与 inBytes 的一致性**不在这里重验**：入参族同一位置也没验
        //   （见下面的 OOPM_SCICALL 分支），权威判定统一在自由函数
        //   oop::SciBridgeOutCall 里 —— e2e 夹具直接调它，两边共用一份逻辑就
        //   不会出现"代理以为带了串、宿主没看见"的漂移。这里只保证"读 inBytes
        //   这个字段本身是安全的"（cbData 至少装得下结构体）。
        if (cds->cbData < sizeof(xfs::oop::SciOutCallWire)) return FALSE;
        auto* ow = reinterpret_cast<const xfs::oop::SciOutCallWire*>(cds->lpData);
        if (ow->magic != xfs::oop::kMagic) return FALSE;
        HandleSciOutCall(*ow, cds->cbData);
        return TRUE;
    }
    if (wire[1] == xfs::oop::OOPM_SCISTRUCTCALL) {
        // SCI_* **结构体指针**过桥请求（v2.7，批次 116）：代理侧已经把插件那个
        // 结构体**展平**成 cpMin/cpMax（+ FindText 的 needle）—— wire 里**没有**
        // 外来指针。full 标志取自两侧共用的 SciStructTable.inc。
        //
        // ⚠ cbData 与 inBytes 的一致性**不在这里重验**（与入参/出参族同一位置
        //   同款）：权威判定统一在自由函数 oop::SciBridgeStructCall 里 —— e2e
        //   夹具直接调它，两边共用一份逻辑就不会出现"代理以为带了 needle、宿主
        //   没看见"的漂移。这里只保证"读这些字段本身是安全的"。
        if (cds->cbData < sizeof(xfs::oop::SciStructCallWire)) return FALSE;
        auto* sw = reinterpret_cast<const xfs::oop::SciStructCallWire*>(cds->lpData);
        if (sw->magic != xfs::oop::kMagic) return FALSE;
        HandleSciStructCall(*sw, cds->cbData);
        return TRUE;
    }
    if (wire[1] == xfs::oop::OOPM_DMMRELAYREPLY) {
        // 停靠中继的应答（v2.8，批次 117）：与 MSGREPLY 同构 —— 编辑器 UI 线程
        // 此刻正阻塞在自己的 SendMessageTimeout 里，代理的回包被投递到本窗，
        // 这里只把结果放进等待槽，由 RelayDmm* 返回后自取。
        if (cds->cbData < sizeof(xfs::oop::DmmRelayReplyWire)) return FALSE;
        auto* dr = reinterpret_cast<const xfs::oop::DmmRelayReplyWire*>(cds->lpData);
        if (dr->magic != xfs::oop::kMagic) return FALSE;
        for (auto& w : dmmWaits_) {
            if (w.reqId == dr->reqId && !w.got) {
                w.got = true;
                w.delivered = (dr->delivered != 0);
                w.result = dr->result;
                break;
            }
        }
        return TRUE;   // 过期 / 重复 reqId：静默丢弃
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

// ---- NPPM_* 过桥（v2.2，批次 111）------------------------------------------
// 中转窗把插件的 NPPM_* 调用封送过来：入参串内联在 wire 尾部，出参容量由
// 中转窗按 NppmMarshal.h 算出。**本函数在宿主进程里重建所有指针** —— 因此
// PluginManager 那条"外来指针判据"在这条路径上永远不会触发。这正是"过桥"
// 与批次 110 的"拒答"的区别，也是本批测试的核心断言（PointerRefusalCount
// 在过桥路径上必须不涨）。
//
// wire 是外来数据，两条长度都必须钳：
//   · inBytes ≤ cbData - sizeof(wire)，且 ≤ kNppmPayloadMax；
//   · outCap  ≤ kNppmPayloadMax；kOutWideBuf 的 hostWp 再钳到
//     kNppmOutWCharsMax ⇒ 宿主侧 WideStrTwoCall 最多写 hostWp 个 wchar_t
//     ≤ outCap/2，绝不会越界写我们这块本地缓冲。
void OopHost::HandleNppmCall(const oop::NppmCallWire& cw, DWORD cbData) {
    ++nppmRelayed_;

    const unsigned bodyMax =
        static_cast<unsigned>(cbData - sizeof(oop::NppmCallWire));
    unsigned inBytes = cw.inBytes < bodyMax ? cw.inBytes : bodyMax;
    if (inBytes > oop::kNppmPayloadMax) inBytes = oop::kNppmPayloadMax;
    unsigned outCap = cw.outCap;
    if (outCap > oop::kNppmPayloadMax) outCap = oop::kNppmPayloadMax;

    std::vector<unsigned char> in(inBytes);
    if (inBytes)
        memcpy(in.data(),
               reinterpret_cast<const unsigned char*>(&cw) + sizeof(cw), inBytes);
    std::vector<unsigned char> out(outCap);

    const oop::NppmShape shape = oop::NppmShapeOf(cw.nppm);
    WPARAM hostWp = static_cast<WPARAM>(cw.wParam);
    LPARAM hostLp = 0;
    // 停靠族要在**本函数的栈上**重建参数（一个结构体 / 两个串），生命周期必须
    // 覆盖下面的 ForwardNppMessage ⇒ 在这里声明、在 switch 里填。
    npp::DockedWidgetData reg{};      // kDmmReg 的重建目标
    bool dmmOk = true;                // false = 载荷布局不过 ⇒ 本侧拒答
    HICON regIcon = nullptr;          // kDmmReg 重建的图标；用完必须 DestroyIcon
    switch (shape) {
    case oop::NppmShape::kValue:
        // ★ 批次 117：kValue 的 lp 是**值**（勾选位 / 视图类型 / 强制位），
        //   原来这里落到 default ⇒ hostLp 恒 0 ⇒ 宿主永远看不到 lp。
        //   症状是静默错（不是崩），所以只有"答案取决于 lp"的断言能抓住它
        //   （见 tests/oop_plugins/oop_nppm.cpp 的 GETNBOPENFILES 那两条）。
        //   批次 117b 起停靠族的 DMMSHOW/DMMHIDE/DMMUPDATEDISPINFO 也走这里
        //   （lp 是 HWND 值）。
        hostLp = static_cast<LPARAM>(cw.lParam);
        break;
    case oop::NppmShape::kInWideStr:
        hostLp = in.empty() ? 0 : reinterpret_cast<LPARAM>(in.data());
        break;
    case oop::NppmShape::kOutWideBuf:
        if (hostWp > oop::kNppmOutWCharsMax) hostWp = oop::kNppmOutWCharsMax;
        hostLp = out.empty() ? 0 : reinterpret_cast<LPARAM>(out.data());
        break;
    case oop::NppmShape::kOutInt:
    case oop::NppmShape::kOutStruct4:
        hostLp = out.empty() ? 0 : reinterpret_cast<LPARAM>(out.data());
        break;
    case oop::NppmShape::kDmmReg: {
        // 停靠注册：wire 里**没有**外来地址 —— 代理侧已把 DockedWidgetData
        // **展平**成 值字段 + 3 个内联宽串（布局见 DmmMarshal.h）。这里在自己
        // 的栈上重建一个，再走与进程内插件**完全相同**的路径
        // （ForwardNppMessage → DockManager::DockWidget）。
        // ★ 重建出来的三个串指向 `in` 内部；DockWidget 会**深拷贝**它们
        //   （见 DockManager.cpp），所以本函数返回后失效是安全的。
        // ★ rcFloat / iPrevCont 不带 —— 是判断不是遗漏，理由写在 DmmMarshal.h
        //   的 DmmRegHead 注释里。
        const oop::DmmRegView v =
            oop::DmmParseReg(in.empty() ? nullptr : in.data(),
                             static_cast<unsigned>(in.size()));
        if (!v.ok) {
            dmmOk = false;
            break;
        }
        reg.hClient = reinterpret_cast<HWND>(v.hClient);
        reg.pszName = v.name;
        reg.dlgID = v.dlgID;
        reg.uMask = static_cast<UINT>(v.uMask);
        reg.pszAddInfo = v.addInfo;
        reg.pszModuleName = v.module;
        // 标签图标（批次 137）：hIconTab 是**用户对象句柄**，跨进程传值无意义 ⇒
        // 代理把它转码成图像块过桥，这里用 CreateIconIndirect **重建**一个本进程
        // 的 HICON。无图标 / GDI 失败 ⇒ nullptr ⇒ 面板照常注册、只是没图标。
        // ★ 归属表态：regIcon 归**本函数**所有，必须在 ForwardNppMessage 返回后
        //   DestroyIcon。安全性来自 DockManager::DockWidget 会先 CopyIcon 收编
        //   自己的副本（见 DockManager.cpp 的 data.hIconTab 分支）；它只在
        //   CopyIcon 失败（仅 OOM）时才直用原件 —— 那时本函数照旧销毁，代价是
        //   该面板的图标控件持有失效句柄（不崩，仅不显示）。换成"不销毁"则是
        //   每次注册漏一个图标，两种取舍里这里选前者。
        regIcon = oop::DmmIconFromView(v);
        reg.hIconTab = regIcon;
        hostLp = reinterpret_cast<LPARAM>(&reg);
        break;
    }
    case oop::NppmShape::kDmmTwoStr: {
        // 按名查句柄：两个串分别在 wp 与 lp（唯一把指针放 wp 的形状）⇒
        // 代理侧把两个串都内联了，这里换成**宿主本地**缓冲的地址。
        const oop::DmmTwoStrView v =
            oop::DmmParseTwoStr(in.empty() ? nullptr : in.data(),
                                static_cast<unsigned>(in.size()));
        if (!v.ok) {
            dmmOk = false;
            break;
        }
        hostWp = reinterpret_cast<WPARAM>(const_cast<wchar_t*>(v.wp));
        hostLp = reinterpret_cast<LPARAM>(v.lp);
        break;
    }
    default:
        break;                                   // kUnsupported：主机侧拒答
    }

    // ★ 注册登记必须在 ForwardNppMessage **之前**：DockManager::DockWidget 的
    //   跨进程守卫会问 IsTrustedDmmClient(hClient)，而它查的就是这张表 ——
    //   登记晚了守卫必然先拒（死循环式的"永远 dock 不上"）。
    //   记录的是**承载它的代理中转窗**（cw.replyTo = 代理的 slot0 窗），
    //   也就是"通知该发给谁"。表条目只在注册请求里写入，而注册请求只能来自
    //   本编辑器自己启动的代理进程（代理侧还验过 hClient 是它自己的窗口）
    //   ⇒ 表里的窗口一定是"我们的代理进程里的窗口"。
    if (dmmOk && shape == oop::NppmShape::kDmmReg && cw.replyTo)
        dmmClients_[reinterpret_cast<HWND>(reg.hClient)] =
            reinterpret_cast<HWND>(cw.replyTo);

    LRESULT res = 0;
    if (!dmmOk) {
        ++nppmRefused_;      // 载荷布局不过：明确拒答（与"宿主真的答 0"分开记账）
    } else if (mgr_) {
        bool handled = false;
        res = mgr_->ForwardNppMessage(static_cast<UINT>(cw.nppm), hostWp, hostLp,
                                      handled);
    }
    // 图标（批次 137）：DockWidget 已通过 CopyIcon 收编自己的副本 ⇒ 这里释放
    // 重建件。放在 ForwardNppMessage **之后**是硬要求 —— reg.hIconTab 必须活到
    // 那一步（归属取舍得失见上面 kDmmReg 分支的注释）。
    if (regIcon) {
        ::DestroyIcon(regIcon);
        regIcon = nullptr;
    }

    // 出参字节数：定长形状给满（4）；变长形状按实际写入的 NUL 结尾串长。
    // 宿主未写时缓冲全零 ⇒ 回填一个 NUL，等价"空串"，与本地调用语义一致。
    unsigned outBytes = 0;
    if (!out.empty()) {
        if (shape == oop::NppmShape::kOutWideBuf) {
            const std::size_t capW = out.size() / sizeof(wchar_t);
            const std::size_t n =
                wcsnlen_s(reinterpret_cast<const wchar_t*>(out.data()), capW);
            outBytes = static_cast<unsigned>((n + 1) * sizeof(wchar_t));
        } else {
            outBytes = static_cast<unsigned>(out.size());
        }
    }

    std::vector<unsigned char> rbuf(sizeof(oop::NppmReplyWire) + outBytes);
    auto* rw = reinterpret_cast<oop::NppmReplyWire*>(rbuf.data());
    rw->magic = xfs::oop::kMagic;
    rw->msg = xfs::oop::OOPM_NPPMREPLY;
    rw->nppm = cw.nppm;
    rw->reqId = cw.reqId;
    rw->result = static_cast<LONG_PTR>(res);
    rw->outBytes = outBytes;
    if (outBytes) memcpy(rbuf.data() + sizeof(*rw), out.data(), outBytes);

    HWND to = reinterpret_cast<HWND>(cw.replyTo);
    if (!to) return;
    COPYDATASTRUCT r{};
    r.dwData = xfs::oop::kMagic;
    r.cbData = static_cast<DWORD>(rbuf.size());
    r.lpData = rbuf.data();
    // SMTO 带超时：中转窗若已死/卡死，本线程（编辑器 UI 线程）不能被拖住。
    // 不加 SMTO_ABORTIFHUNG —— 中转窗此刻正常地"阻塞在 SendMessage 里"，
    // 那不算 hung，加了反而可能被误判成超时（回包丢了 = 插件侧拿到拒答）。
    ::SendMessageTimeoutW(to, WM_COPYDATA, reinterpret_cast<WPARAM>(selfWnd_),
                          reinterpret_cast<LPARAM>(&r),
                          SMTO_NORMAL, 2000, nullptr);
}

// ---- 停靠族：反向中继（v2.8，批次 117）-------------------------------------
// 编辑器 → 代理。为什么必须由代理发，见 OopProtocol.h 的 OOPM_DMMRELAY 注释。
//
// 三个入口都跑在**编辑器 UI 线程**（DockManager 的全部入口都在 UI 线程），
// 所以一律用 `SendMessageTimeout + SMTO_ABORTIFHUNG`：代理卡死时编辑器不陪葬
// （与 SendExec 同款）。
// ⚠ **不加 SMTO_BLOCK**：加了会让本线程不再处理 incoming sent message，而代理
//   的回包正是以 WM_COPYDATA 投递到本窗的（回包丢了 = DMN_CLOSE 的 veto 读不到，
//   而"读不到"与"插件没 veto"同形 ⇒ 会静默关掉一个插件明确要求保留的面板）。
bool OopHost::IsTrustedDmmClient(HWND hClient) const {
    if (!hClient) return false;
    const auto it = dmmClients_.find(hClient);
    if (it == dmmClients_.end()) return false;
    // 承载它的代理窗必须还活着：代理死了这条记录就作废。留着一条死记录等于给
    // "将来复用同一 HWND 值的陌生窗口"发通行证（HWND 会被系统复用）。
    return it->second && ::IsWindow(it->second) && ::IsWindow(hClient);
}

bool OopHost::RelayDmm(UINT kind, HWND hClient, UINT_PTR idFrom, int code,
                       unsigned action, LONG_PTR& outResult, unsigned timeoutMs) {
    outResult = 0;
    if (!hClient) return false;
    const auto it = dmmClients_.find(hClient);
    if (it == dmmClients_.end() || !it->second || !::IsWindow(it->second))
        return false;                       // 未登记 / 代理已死：本地就能判定

    xfs::oop::DmmRelayWire rw{};
    rw.magic = xfs::oop::kMagic;
    rw.msg = xfs::oop::OOPM_DMMRELAY;
    rw.hClient = reinterpret_cast<UINT_PTR>(hClient);
    rw.kind = kind;
    rw.idFrom = idFrom;
    rw.code = code;
    rw.action = action;
    rw.reqId = ++dmmNotifySeq_;

    dmmWaits_.push_back(DmmWait{rw.reqId, false, false, 0});

    COPYDATASTRUCT cds{};
    cds.dwData = xfs::oop::kMagic;
    cds.cbData = sizeof(rw);
    cds.lpData = &rw;
    ::SendMessageTimeoutW(it->second, WM_COPYDATA,
                          reinterpret_cast<WPARAM>(selfWnd_),
                          reinterpret_cast<LPARAM>(&cds),
                          SMTO_ABORTIFHUNG, timeoutMs, nullptr);

    // 取回等待槽（按 reqId 重新查找：等待期间可能发生嵌套调用，迭代器不可持有）。
    bool delivered = false;
    for (auto w = dmmWaits_.begin(); w != dmmWaits_.end(); ++w) {
        if (w->reqId == rw.reqId) {
            if (w->got) {
                delivered = w->delivered;
                outResult = w->result;
            }
            dmmWaits_.erase(w);
            break;
        }
    }
    return delivered;
}

LRESULT OopHost::RelayDmmNotify(HWND hClient, UINT_PTR idFrom, int code,
                                unsigned timeoutMs) {
    LONG_PTR res = 0;
    const bool delivered = RelayDmm(xfs::oop::kDmmRelayNotify, hClient, idFrom,
                                    code, 0, res, timeoutMs);
    if (delivered) ++dmmNotifies_; else ++dmmNotifyRefused_;
    return static_cast<LRESULT>(res);
}

void OopHost::RelayDmmAction(HWND hClient, UINT action, unsigned timeoutMs) {
    LONG_PTR res = 0;
    const bool delivered = RelayDmm(xfs::oop::kDmmRelayAction, hClient, 0, 0,
                                    action, res, timeoutMs);
    if (delivered) ++dmmActions_; else ++dmmActionRefused_;
}

// ---- SCI_* 入参指针过桥（v2.4，批次 113）-----------------------------------
// 中转窗在**代理进程**里把插件给的串读出来内联进 wire；这里把它们还原成
// **宿主本地**指针，再发给宿主真 Scintilla。与 HandleNppmCall 同构，区别是
// 参数形态多（指针可能在 wp、可能两段、长度可能由 wp 给出）—— 那些形态全部
// 来自两侧共用的 SciBridgeTable.inc，本函数不自己判形态。
//
// handled 位是必需的：宿主拒答与"Scintilla 真返回 0"在 result 上同形，而
// 代理侧要分开记账（批次 112 的教训：判据落在"对方是否收到"上）。
void OopHost::HandleSciCall(const oop::SciCallWire& cw, DWORD cbData) {
    ++sciRelayed_;

    LRESULT res = 0;
    const bool ok = oop::SciBridgeCall(sciMain_, cw, cbData, res);
    if (!ok) ++sciRefused_;

    std::vector<unsigned char> rbuf(sizeof(oop::SciReplyWire));
    auto* rw = reinterpret_cast<oop::SciReplyWire*>(rbuf.data());
    rw->magic = xfs::oop::kMagic;
    rw->msg = xfs::oop::OOPM_SCIREPLY;
    rw->sciMsg = cw.sciMsg;
    rw->reqId = cw.reqId;
    rw->result = static_cast<LONG_PTR>(res);
    rw->handled = ok ? 1u : 0u;

    HWND to = reinterpret_cast<HWND>(cw.replyTo);
    if (!to) return;
    COPYDATASTRUCT r{};
    r.dwData = xfs::oop::kMagic;
    r.cbData = static_cast<DWORD>(rbuf.size());
    r.lpData = rbuf.data();
    // 与 NPPM 回包同一条超时策略（见 HandleNppmCall 的注释）。
    ::SendMessageTimeoutW(to, WM_COPYDATA, reinterpret_cast<WPARAM>(selfWnd_),
                          reinterpret_cast<LPARAM>(&r),
                          SMTO_NORMAL, 2000, nullptr);
}

// ---- SCI_* 出参指针过桥（v2.5，批次 114）-----------------------------------
// 与入参族方向相反：插件给的是**接收缓冲**，Scintilla 往里写。跨进程的困难
// 不在"搬内容"而在"搬多少"—— 容量**不在消息里**，只能靠 Scintilla 出参协议
// 自带的 lParam==0 调用探出来（详见 SciBridge.h）。
//
// 本函数只做编排：真正的三步（探长度 → 真调用 → 哨兵校验）全在
// oop::SciBridgeOutCall 里，e2e 夹具直接调那个自由函数 ⇒ 断言证的是**产品
// 代码**，不是夹具自己重写一遍的编辑器侧。
//
// wire 是外来数据：`copied` 回带前按实际载荷再钳一次（本侧已钳过，但回包
// 是唯一把字节送回代理的出口，多钳一道的代价是零）。
void OopHost::HandleSciOutCall(const oop::SciOutCallWire& cw, DWORD cbData) {
    ++sciOutRelayed_;

    std::vector<unsigned char> bytes;
    unsigned long need = 0;
    unsigned long copied = 0;
    LRESULT res = 0;
    const bool ok =
        oop::SciBridgeOutCall(sciMain_, cw, cbData, bytes, need, copied, res);
    if (!ok) {
        ++sciOutRefused_;
        need = 0;
        copied = 0;
        bytes.clear();
    } else {
        sciOutBytes_ += copied;
    }

    unsigned n = static_cast<unsigned>(bytes.size());
    if (n > oop::kSciPayloadMax + 1) n = oop::kSciPayloadMax + 1;
    if (n > copied) n = static_cast<unsigned>(copied);

    std::vector<unsigned char> rbuf(sizeof(oop::SciOutReplyWire) + n);
    auto* rw = reinterpret_cast<oop::SciOutReplyWire*>(rbuf.data());
    rw->magic = xfs::oop::kMagic;
    rw->msg = xfs::oop::OOPM_SCIOUTREPLY;
    rw->sciMsg = cw.sciMsg;
    rw->reqId = cw.reqId;
    rw->result = static_cast<LONG_PTR>(res);
    rw->handled = ok ? 1u : 0u;
    rw->need = static_cast<unsigned>(need);
    rw->copied = n;
    if (n) memcpy(rbuf.data() + sizeof(*rw), bytes.data(), n);

    HWND to = reinterpret_cast<HWND>(cw.replyTo);
    if (!to) return;
    COPYDATASTRUCT r{};
    r.dwData = xfs::oop::kMagic;
    r.cbData = static_cast<DWORD>(rbuf.size());
    r.lpData = rbuf.data();
    // 与 NPPM/SCI 入参回包同一条超时策略（见 HandleNppmCall 的注释）。
    ::SendMessageTimeoutW(to, WM_COPYDATA, reinterpret_cast<WPARAM>(selfWnd_),
                          reinterpret_cast<LPARAM>(&r),
                          SMTO_NORMAL, 2000, nullptr);
}

// ---- SCI_* 结构体指针过桥（v2.7，批次 116）---------------------------------
// 插件那个结构体已被代理侧**展平**成 cpMin/cpMax（+ FindText 的 needle）；这里按
// full 标志重建宿主本地结构体、真调用、写后白名单校验，再把**两路可选**结果回带：
//   * copied 字节 payload        —— kRangeOut / kStyledOut 的**出缓冲内容**；
//   * hasChrg + chrgMin/chrgMax  —— kFindInOut 的 **chrgText**（**只在被写时**）。
//
// 编排全在 oop::SciBridgeStructCall 里，e2e 夹具直接调那个自由函数 ⇒ 断言证的
// 是**产品代码**，不是夹具自己重写一遍的编辑器侧。
//
// wire 是外来数据：`copied` 回带前按实际载荷再钳一次（本侧已钳过，但回包是唯一
// 把字节送回代理的出口，多钳一道的代价是零）。
void OopHost::HandleSciStructCall(const oop::SciStructCallWire& cw, DWORD cbData) {
    ++sciStructRelayed_;

    std::vector<unsigned char> bytes;
    unsigned long copied = 0;
    unsigned hasChrg = 0;
    INT_PTR chrgMin = 0;
    INT_PTR chrgMax = 0;
    LRESULT res = 0;
    const bool ok = oop::SciBridgeStructCall(sciMain_, cw, cbData, bytes, copied,
                                             hasChrg, chrgMin, chrgMax, res);
    if (!ok) {
        ++sciStructRefused_;
        copied = 0;
        hasChrg = 0;
        chrgMin = 0;
        chrgMax = 0;
        bytes.clear();
    } else {
        sciStructBytes_ += copied;
        if (hasChrg) ++sciStructChrgWrites_;
    }

    unsigned n = static_cast<unsigned>(bytes.size());
    if (n > oop::kSciPayloadMax) n = oop::kSciPayloadMax;
    if (n > copied) n = static_cast<unsigned>(copied);

    std::vector<unsigned char> rbuf(sizeof(oop::SciStructReplyWire) + n);
    auto* rw = reinterpret_cast<oop::SciStructReplyWire*>(rbuf.data());
    rw->magic = xfs::oop::kMagic;
    rw->msg = xfs::oop::OOPM_SCISTRUCTREPLY;
    rw->sciMsg = cw.sciMsg;
    rw->reqId = cw.reqId;
    rw->result = static_cast<LONG_PTR>(res);
    rw->handled = ok ? 1u : 0u;
    rw->copied = n;
    rw->hasChrg = hasChrg;
    rw->chrgMin = chrgMin;
    rw->chrgMax = chrgMax;
    if (n) memcpy(rbuf.data() + sizeof(*rw), bytes.data(), n);

    HWND to = reinterpret_cast<HWND>(cw.replyTo);
    if (!to) return;
    COPYDATASTRUCT r{};
    r.dwData = xfs::oop::kMagic;
    r.cbData = static_cast<DWORD>(rbuf.size());
    r.lpData = rbuf.data();
    // 与 NPPM/SCI 各回包同一条超时策略（见 HandleNppmCall 的注释）。
    ::SendMessageTimeoutW(to, WM_COPYDATA, reinterpret_cast<WPARAM>(selfWnd_),
                          reinterpret_cast<LPARAM>(&r),
                          SMTO_NORMAL, 2000, nullptr);
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
