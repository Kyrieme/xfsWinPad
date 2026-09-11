#pragma once
// xfsWinPad - TabBar: owner-drawn tab strip with close buttons,
// modified markers, hover states, middle-click close and drag reordering.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <string>
#include <vector>

namespace xfs {

class TabBar {
public:
    struct Callbacks {
        std::function<void(int index)> onSelect;
        std::function<void(int index)> onCloseRequest;
        std::function<void()> onNewRequest;
        std::function<void(int from, int to)> onReorder;
        std::function<std::wstring(int index)> onTooltipText;
        std::function<void(int index, int screenX, int screenY)> onContextMenu;
        // drag a tab out of the strip (detached-window request, N++ style)
        std::function<void(int index)> onDetachRequest;
    };

    bool Create(HWND parent, HINSTANCE hInst, int id);
    void SetCallbacks(Callbacks cb) { cb_ = std::move(cb); }
    void Destroy();

    HWND Hwnd() const { return hwnd_; }

    void Insert(int index, const std::wstring& title);
    void Remove(int index);
    void Rename(int index, const std::wstring& title);
    void SetCurrent(int index);
    int  Current() const;
    int  Count() const;

    // Painting is fully custom (see WM_PAINT below). The main window supplies
    // the strip background colour and per-tab drawing, which need theme +
    // workspace data. Set right after the tab controls are created.
    void SetDrawHandler(
        std::function<COLORREF()> stripBg,
        std::function<void(HDC hdc, int index, const RECT& rc, bool selected)> draw) {
        stripBg_ = std::move(stripBg);
        onDraw_ = std::move(draw);
    }

    // Close-button geometry shared by hit-testing and painting (DPI-scaled).
    RECT CloseRect(const RECT& itemRect) const;
    bool IsCloseHovered(int index) const {
        return index >= 0 && index == hoverIndex_ && hoverInClose_;
    }

    // Preferred control height for the current DPI
    int HeightForDpi(int dpi) const;

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    int HitTest(POINT pt, bool* inClose) const;
    void RedrawItem(int index);
    void UpdateHover(POINT pt);

    HWND hwnd_ = nullptr;
    WNDPROC origProc_ = nullptr;
    Callbacks cb_;
    std::function<COLORREF()> stripBg_;
    std::function<void(HDC, int, const RECT&, bool)> onDraw_;

    HFONT font_ = nullptr;
    HWND tooltip_ = nullptr;
    std::wstring tipText_;
    int hoverIndex_ = -1;
    bool hoverInClose_ = false;
    bool dragging_ = false;
    int dragIndex_ = -1;
    POINT dragStart_{};
    bool detachPending_ = false;   // drag left the strip area -> detach on release

    // The stock tab control (no TCS_OWNERDRAWFIXED) auto-sizes each tab from
    // its item text only, ignoring the close-button area we draw, so long
    // titles get ellipsized a few px short. Pad the stored text with trailing
    // spaces so every tab reserves text + close button + margins.
    std::wstring PaddedTitle(const std::wstring& title) const;
};

} // namespace xfs
