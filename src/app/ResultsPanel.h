#pragma once
// xfsWinPad - bottom-docked search results panel (file / line / content list,
// double-click activates the match in its editor).

#include "../search/SearchAll.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <vector>

namespace xfs {

// Automation/testing hook: activate results row (wp = row index).
constexpr UINT WM_APP_GOTOHIT = WM_APP + 11;

class ResultsPanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ != nullptr && ::IsWindowVisible(hwnd_); }

    void SetResults(std::vector<SearchHit> hits, const std::wstring& summary);
    void Clear();

    // row -> hit for activation; index validated by caller via HitCount()
    int HitCount() const { return (int)hits_.size(); }
    const SearchHit* HitAt(int i) const;

    // invoked with the row index on double-click / Enter
    std::function<void(int)> onActivateRow;
    // invoked when the user presses the panel close (�? button
    std::function<void()> onClose;
    // invoked while the user drags the top splitter; arg = desired height (px)
    std::function<void(int)> onHeightChange;

    // re-apply localized texts after a language switch
    void Retranslate();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<SearchHit> hits_;

    // top-edge splitter drag state (screen coordinates: the panel moves
    // under the cursor while resizing, so client-relative deltas cancel out)
    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;
};

} // namespace xfs
