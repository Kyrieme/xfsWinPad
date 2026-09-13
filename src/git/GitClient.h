#pragma once
// xfsWinPad - Git integration v1: async git status client + blob fetcher.
// Runs git.exe on a worker thread, posts results to the main window.

#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "git/GitStatus.h"

namespace xfs {

constexpr UINT WM_APP_GIT_DONE = WM_APP + 79;  // lParam = GitSnapshot* (receiver deletes)
constexpr UINT WM_APP_GIT_BLOB = WM_APP + 80;  // lParam = GitBlobResult* (receiver deletes)

struct GitSnapshot {
    bool ok = false;
    bool spawnFailed = false;   // git.exe could not be launched at all
    std::wstring root;
    std::wstring branch;
    std::shared_ptr<git::StateMap> states;
};

struct GitBlobResult {
    bool ok = false;
    std::string data;
    std::wstring absPath;    // working-tree file the blob was fetched for
    std::wstring tempPath;   // where the caller wants the blob written
};

class GitClient {
public:
    void SetMainWnd(HWND h) { main_ = h; }

    // Call from the main thread with any file/dir path; resolves the repo
    // root, then refreshes branch+status asynchronously (coalesced).
    void RequestForPath(const std::wstring& path);

    // Main-thread handler for WM_APP_GIT_DONE; takes ownership of snap.
    // Returns true when root/branch/states changed (UI should sync).
    bool OnDone(GitSnapshot* snap);

    // One-shot async `git show HEAD:<rel>` -> posted WM_APP_GIT_BLOB.
    // Returns false when preconditions fail (no repo / another fetch busy).
    bool FetchHeadBlob(const std::wstring& absPath, const std::wstring& tempPath);

    void ClearNow();  // folder closed / non-repo: drop everything synchronously

    bool Available() const { return !disabled_; }
    bool HasRoot() const { return !root_.empty(); }
    const std::wstring& Root() const { return root_; }
    const std::wstring& Branch() const { return branch_; }
    std::shared_ptr<const git::StateMap> States() const { return states_; }
    // repo-relative slash path, or empty when absPath is outside the repo
    std::wstring RelOf(const std::wstring& absPath) const;

    // Blocking capture (worker threads only). spawnFailed reports that
    // git.exe itself could not be launched.
    static bool Run(const std::wstring& cwd, const std::wstring& args,
                    std::string& out, bool* spawnFailed = nullptr);

private:
    void StartThread(const std::wstring& root);

    HWND main_ = nullptr;
    bool disabled_ = false;
    std::atomic<bool> inflight_{false};
    std::wstring pending_;        // main-thread-only coalesce slot
    std::wstring root_, branch_;
    std::shared_ptr<git::StateMap> states_;
    std::atomic<bool> blobBusy_{false};
};

} // namespace xfs
