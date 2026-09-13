#include "git/GitClient.h"

#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Util.h"

namespace xfs {

bool GitClient::Run(const std::wstring& cwd, const std::wstring& args,
                    std::string& out, bool* spawnFailed) {
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

    PROCESS_INFORMATION pi{};
    BOOL started = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW, nullptr,
                                    cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    ::CloseHandle(wr);
    if (!started) {
        ::CloseHandle(rd);
        DWORD e = ::GetLastError();
        if (spawnFailed)
            *spawnFailed = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ||
                            e == ERROR_BAD_EXE_FORMAT);
        return false;
    }

    char chunk[4096];
    DWORD got = 0;
    for (;;) {
        if (!::ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
        out.append(chunk, got);
    }
    DWORD wait = ::WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) ::GetExitCodeProcess(pi.hProcess, &code);
    else ::TerminateProcess(pi.hProcess, 1);
    ::CloseHandle(pi.hThread);
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
        bool s = b && Run(root, L"status --porcelain=v1 -z", statusOut);
        snap->ok = b && s;
        if (snap->ok) {
            snap->branch = git::ParseBranch(branchOut);
            snap->states = std::make_shared<git::StateMap>(
                git::ParseStatusPorcelainZ(statusOut, root));
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
        root_.clear(); branch_.clear(); states_.reset();
    } else {
        changed = root_ != snap->root || branch_ != snap->branch ||
                  states_ != snap->states;
        if (snap->branch != branch_)
            Logger::Info("git: branch " + WideToUtf8(snap->branch));
        root_ = snap->root;
        branch_ = snap->branch;
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
    states_.reset();
}

} // namespace xfs
