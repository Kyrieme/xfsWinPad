#include "git/GitClient.h"

#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Util.h"

namespace xfs {

bool GitClient::Run(const std::wstring& cwd, const std::wstring& args,
                    std::string& out, bool* spawnFailed,
                    DWORD timeoutMs, bool blockPrompts) {
    if (spawnFailed) *spawnFailed = false;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return false;
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;   // merged: only consumed when the exit code is 0
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmdline = L"git " + args;
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    // Run serializes git commands (cmdBusy_), so mutating our own process env
    // around the spawn is safe and inherits like any other variable. Passing a
    // custom lpEnvironment block proved unreliable across environments.
    if (blockPrompts)
        ::SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");

    PROCESS_INFORMATION pi{};
    BOOL started = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW, nullptr,
                                    cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (blockPrompts)
        ::SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", nullptr);
    ::CloseHandle(wr);
    if (!started) {
        ::CloseHandle(rd);
        DWORD e = ::GetLastError();
        if (spawnFailed)
            *spawnFailed = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ||
                            e == ERROR_BAD_EXE_FORMAT);
        return false;
    }

    // Reader thread keeps draining the pipe so the process cannot block on a
    // full buffer while we enforce the deadline on the process handle.
    std::thread reader([&rd, &out]() {
        char chunk[4096];
        DWORD got = 0;
        for (;;) {
            if (!::ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) || got == 0)
                break;
            out.append(chunk, got);
        }
    });
    DWORD wait = ::WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) ::GetExitCodeProcess(pi.hProcess, &code);
    else ::TerminateProcess(pi.hProcess, 1);   // unblocks the reader (pipe EOF)
    reader.join();
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    return wait == WAIT_OBJECT_0 && code == 0;
}

static void PostSnapshot(HWND main, GitSnapshot* snap) {
    if (main) ::PostMessageW(main, WM_APP_GIT_DONE, 0, (LPARAM)snap);
    else delete snap;
}

void GitClient::StartThread(const std::wstring& root) {
    inflight_ = true;
    HWND main = main_;
    std::thread([main, root]() {
        auto* snap = new GitSnapshot;
        snap->root = root;
        std::string branchOut, statusOut;
        bool spawn = false;
        bool b = Run(root, L"rev-parse --abbrev-ref HEAD", branchOut, &spawn);
        snap->spawnFailed = spawn;
        bool s = b && Run(root, L"status --porcelain=v1 -b -z", statusOut);
        snap->ok = b && s;
        if (snap->ok) {
            snap->branch = git::ParseBranch(branchOut);
            snap->track = git::ParseTracking(statusOut);
            snap->states = std::make_shared<git::StateMap>(
                git::ParseStatusPorcelainZ(statusOut, root));
            git::AggregateDirs(*snap->states, root);
        } else {
            snap->states = std::make_shared<git::StateMap>();
        }
        PostSnapshot(main, snap);
    }).detach();
}

void GitClient::RequestForPath(const std::wstring& path) {
    if (disabled_ || !main_ || path.empty()) return;
    std::wstring root = git::FindRepoRoot(path);
    if (root.empty()) {
        if (!root_.empty() || !branch_.empty()) ClearNow();
        return;
    }
    if (inflight_) { pending_ = path; return; }
    StartThread(root);
}

bool GitClient::OnDone(GitSnapshot* snap) {
    if (!snap) return false;
    bool changed = false;
    if (snap->spawnFailed) {
        disabled_ = true;
        Logger::Warn("git: git.exe not available, integration disabled");
        if (!root_.empty()) { changed = true; ClearNow(); }
    } else if (!snap->ok) {
        // repo vanished or command failed transiently: drop stale data
        changed = !root_.empty() || !branch_.empty();
        root_.clear(); branch_.clear(); track_ = {}; states_.reset();
    } else {
        changed = root_ != snap->root || branch_ != snap->branch ||
                  states_ != snap->states ||
                  track_.ahead != snap->track.ahead ||
                  track_.behind != snap->track.behind ||
                  track_.gone != snap->track.gone;
        if (snap->branch != branch_)
            Logger::Info("git: branch " + WideToUtf8(snap->branch));
        if (changed && snap->track.hasUpstream &&
            (snap->track.ahead != track_.ahead || snap->track.behind != track_.behind ||
             snap->track.gone != track_.gone))
            Logger::Info("git: tracking ahead=" + std::to_string(snap->track.ahead) +
                         " behind=" + std::to_string(snap->track.behind) +
                         (snap->track.gone ? " gone" : ""));
        root_ = snap->root;
        branch_ = snap->branch;
        track_ = snap->track;
        states_ = snap->states;
    }
    delete snap;
    inflight_ = false;
    if (!pending_.empty()) {
        std::wstring p;
        p.swap(pending_);
        RequestForPath(p);
    }
    return changed;
}

bool GitClient::FetchHeadBlob(const std::wstring& absPath,
                              const std::wstring& tempPath) {
    if (!HasRoot() || blobBusy_.exchange(true)) return false;
    std::wstring rel = RelOf(absPath);
    if (rel.empty()) { blobBusy_ = false; return false; }
    std::wstring root = root_;
    std::wstring args = L"show --textconv \"HEAD:" + rel + L"\"";
    HWND main = main_;
    std::thread([main, root, args, absPath, tempPath]() {
        auto* res = new GitBlobResult;
        res->absPath = absPath;
        res->tempPath = tempPath;
        res->ok = Run(root, args, res->data);
        if (res->ok) {
            HANDLE f = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD done = 0;
                if (!res->data.empty())
                    ::WriteFile(f, res->data.data(), (DWORD)res->data.size(), &done, nullptr);
                ::CloseHandle(f);
            } else {
                res->ok = false;
            }
        }
        if (main) ::PostMessageW(main, WM_APP_GIT_BLOB, 0, (LPARAM)res);
        else delete res;
    }).detach();
    return true;
}

void GitClient::StartOp(GitOpKind kind, const std::wstring& args,
                        const std::wstring& arg, DWORD timeoutMs,
                        bool blockPrompts) {
    HWND main = main_;
    std::wstring root = root_;
    auto busy = cmdBusy_;
    std::thread([main, root, kind, args, arg, busy, timeoutMs, blockPrompts]() {
        auto* res = new GitOpResult;
        res->kind = kind;
        res->arg = arg;
        res->ok = GitClient::Run(root, args, res->output, nullptr,
                                 timeoutMs, blockPrompts);
        *busy = false;
        if (main) ::PostMessageW(main, WM_APP_GIT_OP, 0, (LPARAM)res);
        else delete res;
    }).detach();
}

bool GitClient::Stage(const std::wstring& absPath) {
    if (!HasRoot() || disabled_ || cmdBusy_->exchange(true)) return false;
    std::wstring rel = RelOf(absPath);
    if (rel.empty()) { *cmdBusy_ = false; return false; }
    StartOp(GitOpKind::Stage, L"add -- " + git::QuoteArg(rel), absPath);
    Logger::Info("git: stage " + WideToUtf8(rel));
    return true;
}

bool GitClient::Unstage(const std::wstring& absPath) {
    if (!HasRoot() || disabled_ || cmdBusy_->exchange(true)) return false;
    std::wstring rel = RelOf(absPath);
    if (rel.empty()) { *cmdBusy_ = false; return false; }
    StartOp(GitOpKind::Unstage, L"restore --staged -- " + git::QuoteArg(rel), absPath);
    Logger::Info("git: unstage " + WideToUtf8(rel));
    return true;
}

bool GitClient::Commit(const std::wstring& message) {
    if (!HasRoot() || disabled_ || message.empty() || cmdBusy_->exchange(true))
        return false;
    StartOp(GitOpKind::Commit, L"commit -m " + git::QuoteArg(message), message);
    Logger::Info("git: commit started");
    return true;
}

bool GitClient::ListBranches() {
    if (!HasRoot() || disabled_ || cmdBusy_->exchange(true)) return false;
    StartOp(GitOpKind::ListBranches,
            L"for-each-ref --format=" +
                git::QuoteArg(L"%(HEAD)%(refname)") +
                L" refs/heads refs/remotes",
            L"");
    Logger::Info("git: branch list started");
    return true;
}

bool GitClient::Checkout(const std::wstring& branch) {
    if (!HasRoot() || disabled_ || branch.empty() || cmdBusy_->exchange(true))
        return false;
    StartOp(GitOpKind::Checkout, L"checkout " + git::QuoteArg(branch), branch);
    Logger::Info("git: checkout started " + WideToUtf8(branch));
    return true;
}

bool GitClient::CreateBranch(const std::wstring& branch) {
    if (!HasRoot() || disabled_ || branch.empty() || cmdBusy_->exchange(true))
        return false;
    StartOp(GitOpKind::CreateBranch,
            L"checkout -b " + git::QuoteArg(branch), branch);
    Logger::Info("git: branch create started " + WideToUtf8(branch));
    return true;
}

bool GitClient::Push() {
    if (!HasRoot() || disabled_ || cmdBusy_->exchange(true)) return false;
    StartOp(GitOpKind::Push, L"push", L"", 120000, true);
    Logger::Info("git: push started");
    return true;
}

bool GitClient::Fetch() {
    if (!HasRoot() || disabled_ || cmdBusy_->exchange(true)) return false;
    StartOp(GitOpKind::Fetch, L"fetch --prune", L"", 120000, true);
    Logger::Info("git: fetch started");
    return true;
}

std::wstring GitClient::RelOf(const std::wstring& absPath) const {
    if (root_.empty()) return std::wstring();
    std::wstring r = root_;
    for (auto& ch : r) ch = towlower(ch);
    std::wstring a = absPath;
    for (auto& ch : a) ch = towlower(ch);
    if (a.size() <= r.size() || a.compare(0, r.size(), r) != 0 ||
        (r.size() > 3 && r[r.size() - 1] != L'\\' && a[r.size()] != L'\\'))
        return std::wstring();
    std::wstring rel = absPath.substr(r.size());
    while (!rel.empty() && rel.front() == L'\\') rel.erase(rel.begin());
    for (auto& ch : rel) if (ch == L'\\') ch = L'/';
    return rel;
}

void GitClient::ClearNow() {
    root_.clear();
    branch_.clear();
    track_ = {};
    states_.reset();
}

} // namespace xfs
