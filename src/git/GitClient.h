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
constexpr UINT WM_APP_GIT_OP   = WM_APP + 81;  // lParam = GitOpResult* (receiver deletes)

struct GitSnapshot {
    bool ok = false;
    bool spawnFailed = false;   // git.exe could not be launched at all
    std::wstring root;
    std::wstring branch;
    git::BranchTracking track;
    std::shared_ptr<git::StateMap> states;
};

struct GitBlobResult {
    bool ok = false;
    std::string data;
    std::wstring absPath;    // working-tree file the blob was fetched for
    std::wstring tempPath;   // where the caller wants the blob written
};

enum class GitOpKind : int {
    Stage = 0, Unstage = 1, Commit = 2, ListBranches = 3, Checkout = 4,
    CreateBranch = 5, Push = 6, Fetch = 7, Pull = 8, Revert = 9, Merge = 10,
    Stash = 11, Unstash = 12, DeleteBranch = 13, RenameBranch = 14,
    CheckoutTrack = 15,
};

struct GitOpResult {
    GitOpKind kind;
    bool ok = false;
    std::wstring arg;     // abs path (stage/unstage) or message (commit)
    std::string output;   // captured stdout+stderr for diagnostics
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

    // Async file/folder ops on the working tree (path must be inside repo).
    // `git add`/`git restore --staged` take a repo-relative path; commit takes
    // a message. Returns false when preconditions fail (no repo / op busy).
    bool Stage(const std::wstring& absPath);
    bool Unstage(const std::wstring& absPath);
    bool Commit(const std::wstring& message);
    bool ListBranches();
    bool Checkout(const std::wstring& branch);
    bool CheckoutTrack(const std::wstring& remoteBranch);
    bool CreateBranch(const std::wstring& branch);
    bool Merge(const std::wstring& branch);
    // Safe delete of a local branch: `git branch -d <name>`. Git itself
    // refuses unmerged branches, so no extra confirmation is needed.
    bool DeleteBranch(const std::wstring& branch);
    bool RenameBranch(const std::wstring& newName);
    bool Push();
    bool Fetch();
    bool Pull();
    // Discard uncommitted changes in one file: `git checkout -- <rel>`.
    // Refuses for untracked paths (nothing to revert from HEAD).
    bool RevertFile(const std::wstring& absPath);
    // Stash tracked local changes under an auto-timestamped message, and
    // pop the most recent stash back (both whole-repo ops).
    bool Stash();
    bool Unstash();

    void ClearNow();  // folder closed / non-repo: drop everything synchronously

    bool Available() const { return !disabled_; }
    bool HasRoot() const { return !root_.empty(); }
    const std::wstring& Root() const { return root_; }
    const std::wstring& Branch() const { return branch_; }
    int Ahead() const { return track_.ahead; }
    int Behind() const { return track_.behind; }
    bool UpstreamGone() const { return track_.gone; }
    std::shared_ptr<const git::StateMap> States() const { return states_; }
    // repo-relative slash path, or empty when absPath is outside the repo
    std::wstring RelOf(const std::wstring& absPath) const;

    // Blocking capture (worker threads only). spawnFailed reports that
    // git.exe itself could not be launched. Network-ish ops may pass a longer
    // timeout; blockPrompts also disables interactive credential prompts.
    static bool Run(const std::wstring& cwd, const std::wstring& args,
                    std::string& out, bool* spawnFailed = nullptr,
                    DWORD timeoutMs = 15000, bool blockPrompts = false);

private:
    void StartThread(const std::wstring& root);
    void StartOp(GitOpKind kind, const std::wstring& args, const std::wstring& arg,
                 DWORD timeoutMs = 15000, bool blockPrompts = false);

    HWND main_ = nullptr;
    bool disabled_ = false;
    std::atomic<bool> inflight_{false};
    std::wstring pending_;        // main-thread-only coalesce slot
    std::wstring root_, branch_;
    git::BranchTracking track_;
    std::shared_ptr<git::StateMap> states_;
    std::atomic<bool> blobBusy_{false};
    std::shared_ptr<std::atomic<bool>> cmdBusy_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace xfs
