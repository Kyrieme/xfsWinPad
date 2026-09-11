#pragma once
// xfsWinPad - Notepad++-style search dialog with tabbed pages:
//   0 查找 | 1 替换 | 2 文件中查找 | 3 项目中查找 | 4 标记
// Built from explicit CreateWindowExW calls (no dialog templates).

#include "../search/SearchService.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {

// WM_APP message: the dialog requests a search action from the main window.
// wParam = 0, lParam = SearchAction value.
constexpr UINT WM_APP_SEARCHACT = WM_APP + 9;
// Automation/testing hook: switch page (wParam = 0-based page index).
constexpr UINT WM_APP_SETPAGE   = WM_APP + 12;

enum class SearchAction : int {
    FindNext = 1,
    FindPrev,
    Replace,
    ReplaceAll,        // current document
    CountCurrent,      // occurrences in current document
    FindAllCurrent,    // list matches of current doc in results panel
    FindAllOpen,       // list matches across all open documents
    ReplaceAllOpen,    // replace across all open documents
    FindInFiles,       // disk scope (文件中查找)
    ReplaceInFiles,    // disk scope replace (文件中替换)
    FindInProjects,    // folders of open documents (项目中查找)
    MarkAll,           // 标记页: mark matching lines in scope
    MarkClear,         // 标记页: clear marks in scope
};

struct FindInFilesUi {         // collected from the 文件中查找 page
    std::wstring directory;
    std::wstring filters;
    std::wstring replaceWith;
    bool recursive = true;
    bool doReplace = false;
};

struct FindInProjectsUi {      // collected from the 项目中查找 page
    std::wstring filters;
};

class FindDialog {
public:
    void Show(HWND parent, HINSTANCE hInst, int pageIndex);
    void Close();
    void Retranslate();   // refresh all control texts after a language switch

    bool IsVisible() const { return hwnd_ != nullptr; }

    // Single source of truth: the dialog reads/writes the caller-owned state
    // so accelerators (F3) share exactly what the dialog shows.
    void BindState(FindState* external) { state_ = external; }
    void SetSearchText(const std::wstring& t);
    const FindInFilesUi& LastFif() const { return fif_; }
    const FindInProjectsUi& LastProj() const { return proj_; }
    HWND Hwnd() const { return hwnd_; }
    bool MarkAllOpenScope() const;    // 标记页 checkbox state
    bool MarkBookmarkLines() const;

private:
    friend LRESULT CALLBACK FindDlgProc(HWND, UINT, WPARAM, LPARAM);

    void SwitchPage(int pageIndex);
    void ApplyFromControls();
    void Request(SearchAction act);
    void AddHistory(HWND combo, const std::wstring& t);
    void BrowseFolder();

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HINSTANCE inst_ = nullptr;
    FindState* state_ = nullptr;
    int page_ = 0;
    FindInFilesUi fif_;
    FindInProjectsUi proj_;
};

class GotoDialog {
public:
    // returns 1-based line or -1 when cancelled; runs a local modal loop
    static int Run(HWND parent, HINSTANCE hInst, int maxLine);
};

} // namespace xfs
