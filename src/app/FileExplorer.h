#pragma once
// xfsWinPad - Folder Workspace: dockable left panel with directory TreeView.
// Double-click opens files; context menu for common operations.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <memory>
#include <string>
#include <filesystem>

#include "../git/GitStatus.h"

namespace xfs {

class FileExplorer {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    void Show() { if (hwnd_) ::ShowWindow(hwnd_, SW_SHOW); }
    void Hide() { if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE); }

    void SetRoot(const std::wstring& dir);
    void Refresh();                 // re-scan current root
    void CloseFolder();             // clear root + empty the tree
    std::wstring Root() const { return rootDir_; }

    // double-click on a file �?open request
    std::function<void(const std::wstring& path)> onOpenFile;
    // "compare with HEAD" request (git repo files only; unset hides the item)
    std::function<void(const std::wstring& path)> onGitCompare;
    // git stage/unstage (item must carry a git state); commit opens dialog
    std::function<void(const std::wstring& path)> onGitStage;
    std::function<void(const std::wstring& path)> onGitUnstage;
    std::function<void()> onGitCommit;
    std::function<void()> onGitBranch;
    std::function<void()> onGitBranchNew;
    std::function<void()> onGitPush;
    std::function<void()> onGitFetch;
    std::function<void()> onGitPull;
    std::function<void()> onGitMerge;
    // discard uncommitted changes (tracked files only; unset hides the item)
    std::function<void(const std::wstring& path)> onGitRevert;

    // git working-set coloring: lowercase abs path -> state (shared snapshot)
    void SetGitStates(std::shared_ptr<const git::StateMap> states) {
        gitStates_ = std::move(states);
        if (tree_) ::InvalidateRect(tree_, nullptr, FALSE);
    }

private:
    friend LRESULT CALLBACK FeWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK FeTreeProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    void PopulateTree(HTREEITEM parent, const std::filesystem::path& dir);
    void OpenSelection();
    void ShowContextMenu(POINT screenPt);
    std::wstring GetItemFullPath(HTREEITEM hti) const;
    COLORREF GitTextColorOf(HTREEITEM hti) const;   // RGB or CLR_NONE

    // file operations (context menu)
    std::wstring TargetDir(HTREEITEM sel) const;   // dir to create new items in
    void NewFile(HTREEITEM sel);
    void NewFolder(HTREEITEM sel);
    void RenameItem(HTREEITEM sel);
    void DeleteItem(HTREEITEM sel);

    HWND hwnd_ = nullptr;
    HWND tree_ = nullptr;
    HFONT font_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    std::wstring rootDir_;
    unsigned treeItemIdCounter_ = 0;
    std::shared_ptr<const git::StateMap> gitStates_;
};

} // namespace xfs
