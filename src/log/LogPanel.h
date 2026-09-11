#pragma once
// xfsWinPad - LogPanel: bottom-docked multi-tab log analyzer (Phase 18 v3).
//
// - Container with a top tab strip: each tab is a LogSession owning its own
//   read-only Scintilla, so several logs can be followed/filtered at once.
// - LogSession: tail-follow (500ms poll) with multibyte boundary hold-back,
//   rotation detection (volume serial + file index), substring/regex
//   include/exclude filtering (SCI_HIDELINES + background highlight marker),
//   and timestamp jump.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Scintilla.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xfs {

struct ThemeDef;

// --- per-tab log view ---------------------------------------------------------
class LogSession {
public:
    static constexpr int MARK_LOG_HL = 1;   // background marker for matches

    bool Create(HWND parent, HINSTANCE hInst, HFONT font);
    void Destroy();
    HWND Sci() const { return sci_; }
    bool HasFocus() const { return sci_ && ::GetFocus() == sci_; }

    bool LoadFile(const std::wstring& path, bool preserveState);
    void PollFile();
    void ApplyFilter();
    void ClearFilter();
    void GoToTimestamp(const std::wstring& target);
    void SetTheme(COLORREF bg, COLORREF fg, COLORREF hl);
    void UpdateLineNumberMargin();

    // shared UI state (container reads/writes for toolbar sync)
    std::wstring path_, filter_;
    bool follow_ = true, highlight_ = true, filterOn_ = false, regexOn_ = false;
    bool exclude_ = false;
    unsigned long long lastSize_ = 0, fileSize_ = 0;
    bool truncated_ = false, utf8Mode_ = false;
    std::string pending_;
    int totalLines_ = 0;

private:
    static LRESULT CALLBACK SciProcThunk(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT SciProc(HWND, UINT, WPARAM, LPARAM);
    std::string DecodeChunk(const std::string& raw) const;
    void AppendText(const std::string& utf8);
    sptr_t Send(HWND h, UINT m, uptr_t wp = 0, LPARAM lp = 0) {
        return ::SendMessageW(h, m, (WPARAM)wp, lp);
    }

    HWND parent_ = nullptr;
    HWND sci_ = nullptr;
    HINSTANCE inst_ = nullptr;
    DWORD fileSerial_ = 0, fileIndexHi_ = 0, fileIndexLo_ = 0;
};

// --- container ----------------------------------------------------------------
class LogPanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    bool HasFocus() const;
    void Hide();

    // opens the log file (or focuses an already-open tab) and shows the panel
    bool LoadFile(const std::wstring& path, bool preserveState = false);

    std::function<void()> onClose;                 // close button
    std::function<void(int)> onHeightChange;       // splitter drag (px)

    void ApplyTheme(const ThemeDef& t);
    void Layout(int w, int h);

    // re-apply localized texts after a language switch
    void Retranslate();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    static LRESULT CALLBACK TabProcThunk(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT TabProc(HWND, UINT, WPARAM, LPARAM);
    void DrawTabItem(const DRAWITEMSTRUCT* dis);
    RECT TabCloseRect(const RECT& item) const;
    int TabHitTest(POINT pt, bool* inClose);
    void RedrawTab(int index);

    void OpenFileDlg();
    void PollTick();
    LogSession* Active() const;
    int AddTab(const std::wstring& title, bool select);   // create session + entry
    void CloseTab(int index);
    void ActivateTab(int index);
    void SyncToolbar();
    void UpdateLabel();

    void LoadPresets();
    void ApplyPreset(int index);
    void SavePreset();
    void DeletePreset();

    HWND hwnd_ = nullptr;
    HWND tabs_ = nullptr;                  // SysTabControl
    HWND addBtn_ = nullptr;
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HWND openBtn_ = nullptr;
    HWND followChk_ = nullptr;
    HWND hlChk_ = nullptr;
    HWND filterEdit_ = nullptr;
    HWND excludeChk_ = nullptr;
    HWND regexChk_ = nullptr;
    HWND applyBtn_ = nullptr;
    HWND clearBtn_ = nullptr;
    HWND gotoEdit_ = nullptr;
    HWND gotoBtn_ = nullptr;
    HWND presetLbl_ = nullptr;
    HWND presetCombo_ = nullptr;
    HWND presetSave_ = nullptr;
    HWND presetDel_ = nullptr;
    HFONT font_ = nullptr;
    HINSTANCE inst_ = nullptr;

    // top-edge splitter drag state (screen coords; see HexPanel)
    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;

    std::vector<std::unique_ptr<LogSession>> sessions_;
    int active_ = -1;
    int hoverCloseIndex_ = -1;             // tab strip: hovered close button

    enum { kTimerId = 1 };
    struct {
        COLORREF bg = RGB(0xFF, 0xFF, 0xFF), fg = RGB(0x20, 0x20, 0x20);
        COLORREF hl = RGB(0xFF, 0xF1, 0x9C);
    } colors_;
};

} // namespace xfs
