#pragma once
// xfsWinPad - Folder Workspace: dockable left panel with directory TreeView.
// Double-click opens files; context menu for common operations.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <string>
#include <filesystem>

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

private:
    friend LRESULT CALLBACK FeWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK FeTreeProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    void PopulateTree(HTREEITEM parent, const std::filesystem::path& dir);
    void OpenSelection();
    void ShowContextMenu(POINT screenPt);
    std::wstring GetItemFullPath(HTREEITEM hti) const;

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
};

} // namespace xfs
