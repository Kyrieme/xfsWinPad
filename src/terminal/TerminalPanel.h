#pragma once
// xfsWinPad - TerminalPanel: bottom-docked pseudo-terminal (ConPTY).
//
// A ConPTY host runs cmd.exe/PowerShell. Its output stream is fed through a VT
// parser into a read-only Scintilla buffer where SGR colours are mapped to a
// small set of terminal styles. Command input is a single-line EDIT at the
// bottom: pressing Enter sends the whole line to the child, and the child's
// conhost echoes it back into the output stream (so the visible transcript
// stays authoritative). This keeps the editor free of a full terminal-emulation
// layer while still running real interactive shells.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Scintilla.h>
#include <functional>
#include <string>
#include <vector>
#include "ConPTY.h"
#include "VtParser.h"

namespace xfs {

struct ThemeDef;

// style indexes allocated in the Sci style array for terminal colours
enum TermStyle {
    TS_DEFAULT = 0,
    TS_BOLD   ,
    TS_BLACK, TS_RED, TS_GREEN, TS_YELLOW, TS_BLUE, TS_MAGENTA, TS_CYAN, TS_WHITE,
    TS_BOLD_BLACK, TS_BOLD_RED, TS_BOLD_GREEN, TS_BOLD_YELLOW,
    TS_BOLD_BLUE, TS_BOLD_MAGENTA, TS_BOLD_CYAN, TS_BOLD_WHITE,
    TS_COUNT
};

class TerminalPanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    bool HasFocus() const;
    void Hide();

    // launch the shell (idempotent); returns false if ConPTY unavailable.
    bool StartShell();
    // stop the shell and clear the transcript (used when the panel hides so
    // reopening starts a fresh session instead of re-rendering stale output).
    void StopShell();
    void RestartShell();

    void ApplyTheme(const ThemeDef& t);
    void Layout(int w, int h);
    // change the terminal monospace font (output + input line), DPI-aware.
    void SetFont(const std::wstring& name, int size);
    // read back the current terminal font (for settings persistence)
    std::wstring FontName() const { return fontName_; }
    int FontSize() const { return fontSize_; }

    // automation/testing hook: send a command line to the shell.
    bool TypeLine(const std::wstring& line);

    std::function<void()> onClose;
    std::function<void(int)> onHeightChange;

    // re-apply localized texts after a language switch
    void Retranslate();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SciProcThunk(HWND, UINT, WPARAM, LPARAM,
                                         UINT_PTR, DWORD_PTR);
    LRESULT SciProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProcThunk(HWND, UINT, WPARAM, LPARAM,
                                           UINT_PTR, DWORD_PTR);
    LRESULT InputProc(HWND, UINT, WPARAM, LPARAM);

    void OnPtyOutput(const std::string& chunk);
    void OnPtyEof();
    void AppendPty(const std::string& text, const std::vector<VtStyle>& styles);
    void UpdateLabel();
    void SendLine(const std::wstring& line);

    HWND hwnd_ = nullptr;
    HWND sci_ = nullptr;
    HWND input_ = nullptr;
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HFONT font_ = nullptr;      // chrome font
    HFONT mono_ = nullptr;      // terminal/output font
    std::wstring fontName_ = L"Consolas";
    int fontSize_ = 11;
    HINSTANCE inst_ = nullptr;

    ConPTY pty_;
    std::string utf8Pending_;   // partial UTF-8 across reader chunks
    bool running_ = false;
    int lineStart_ = -1;        // VT parser: position to overwrite on \r

    // splitter state (screen coords; see HexPanel)
    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;

    // theme colours
    struct {
        COLORREF bg = RGB(0x1E, 0x1E, 0x1E), fg = RGB(0xCC, 0xCC, 0xCC);
        COLORREF cursor = RGB(0xFF, 0xFF, 0xFF);
        COLORREF palette[16] = {
            RGB(0x00,0x00,0x00), RGB(0xCD,0x31,0x31), RGB(0x0F,0x9F,0x0F),
            RGB(0xC4,0xA0,0x00), RGB(0x24,0x69,0xA4), RGB(0x92,0x50,0x80),
            RGB(0x00,0x9D,0x9D), RGB(0xC0,0xC0,0xC0),
            RGB(0x55,0x55,0x55), RGB(0xEC,0x33,0x33), RGB(0x00,0xD1,0x00),
            RGB(0xED,0xF1,0x00), RGB(0x32,0x8F,0xD3), RGB(0xBF,0x6B,0xAF),
            RGB(0x00,0xD5,0xD5), RGB(0xE0,0xE0,0xE0),
        };
    } colors_;
};

} // namespace xfs
