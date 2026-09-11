#pragma once
// xfsWinPad - WindowsListDialog: N++ WindowsDlg-style open-document switcher.
// Modal dialog: filter box + multi-select ListView (name / path / view / size)
// with Activate / Save / Close buttons. Entries map to Workspace documents by
// (view, index) pairs captured at Refill time.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace xfs {

class MainWindow;

class WindowsListDialog {
public:
    struct Entry {
        std::wstring name;     // display name (tab title incl. dirty marker)
        std::wstring path;     // full path or "" for untitled
        int view = 0;          // split view that owns the document
        int index = -1;        // index inside that view
        bool active = false;   // active in its view
        bool dirty = false;
        unsigned long long size = 0;   // file size in bytes (0 = untitled)
    };

    // Runs the modal loop. Returns true if the dialog mutated documents
    // (closes/saves), so the host can refresh menus/status afterwards.
    static bool Run(HWND parent, HINSTANCE inst, MainWindow& host);

private:
    WindowsListDialog() = default;
};

} // namespace xfs
