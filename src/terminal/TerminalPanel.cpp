#include "TerminalPanel.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../theme/Theme.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>

namespace xfs {

namespace {
constexpr wchar_t kPanelClass[] = L"xfsWinPadTerminalPanel";
constexpr UINT_PTR kSciSubclassId = 0x54455332;   // 'TES2'
constexpr UINT_PTR kInputSubclassId = 0x54455349; // 'TESI'
constexpr int ID_LABEL = 1400;
constexpr int ID_CLOSE = 1401;
constexpr int ID_INPUT = 1402;

sptr_t Send(HWND h, UINT m, uptr_t wp = 0, LPARAM lp = 0) {
    return ::SendMessageW(h, m, (WPARAM)wp, lp);
}

// ANSI 16 colour -> terminal style index.
// palette 0..7 normal  -> TS_RED..TS_WHITE (indices 3..10)
// palette 0..7 bold    -> TS_BOLD_RED..TS_BOLD_WHITE (11..18)
// palette 8..15 act as bright variants of the same base -> bold slot.
int FgStyle(bool bold, int c) {
    if (c < 0) return bold ? TS_BOLD : TS_DEFAULT;
    int base = c % 8;                 // 0..7 base colour
    int idx = TS_RED + base;          // 3..10 normal
    if (bold || c >= 8) idx = TS_BOLD_RED + base;   // 11..18
    return idx;
}

// Place the edit caret at the end of the current text (after the "> " prompt).
void PlaceCaretEnd(HWND hEdit) {
    int len = ::GetWindowTextLengthW(hEdit);
    ::SendMessageW(hEdit, EM_SETSEL, len, len);
}

// Take CF_UNICODETEXT from the clipboard (flattened to one line — a terminal
// command is a single line) and drop it at the end of the input edit, moving
// focus there. Used by Ctrl+V and the output-area context menu.
void PasteClipboardToInput(HWND panel, HWND input) {
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!::OpenClipboard(panel)) return;
    std::wstring s;
    HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
    if (h) {
        if (const wchar_t* txt = (const wchar_t*)::GlobalLock(h)) {
            s = txt;
            std::replace(s.begin(), s.end(), L'\r', L' ');
            std::replace(s.begin(), s.end(), L'\n', L' ');
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    if (!s.empty()) {
        int len = ::GetWindowTextLengthW(input);
        ::SendMessageW(input, EM_SETSEL, len, len);
        ::SendMessageW(input, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
    }
    ::SetFocus(input);
}
} // namespace

bool TerminalPanel::Create(HWND parent, HINSTANCE hInst) {
    if (hwnd_) return true;
    inst_ = hInst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPanelClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    int dpi = ::GetDpiForWindow(parent);
    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    // mono_ comes from SetFont() below so settings can drive it
    mono_ = nullptr;

    hwnd_ = ::CreateWindowExW(0, kPanelClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 600, 240, parent,
                              (HMENU)(INT_PTR)1105, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", Tr(L"panel.terminal"),
                               WS_CHILD | WS_VISIBLE | SS_NOTIFY, 8, 8, 480, 18,
                               hwnd_, (HMENU)(INT_PTR)ID_LABEL, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.terminal.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 2, 76, 22,
                                  hwnd_, (HMENU)(INT_PTR)ID_CLOSE, hInst, nullptr);
    if (label_) ::SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (closeBtn_) ::SendMessageW(closeBtn_, WM_SETFONT, (WPARAM)font_, TRUE);

    sci_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"Scintilla", nullptr,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             6, 26, 588, 170, hwnd_, nullptr, hInst, nullptr);
    if (!sci_) return false;
    ::SetWindowSubclass(sci_, SciProcThunk, kSciSubclassId, (DWORD_PTR)this);

    input_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"> ",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               6, 200, 588, 26, hwnd_, (HMENU)(INT_PTR)ID_INPUT,
                               hInst, nullptr);
    if (!input_) return false;
    ::SetWindowSubclass(input_, InputProcThunk, kInputSubclassId, (DWORD_PTR)this);
    if (mono_) ::SendMessageW(input_, WM_SETFONT, (WPARAM)mono_, TRUE);
    // caret starts after the "> " prompt, not before it
    PlaceCaretEnd(input_);

    // terminal colours + monospace (font comes from SetFont)
    Send(sci_, SCI_SETCODEPAGE, SC_CP_UTF8);
    Send(sci_, SCI_SETWRAPMODE, SC_WRAP_NONE);
    Send(sci_, SCI_SETCARETLINEVISIBLE, 1);
    Send(sci_, SCI_SETCARETPERIOD, 530);
    Send(sci_, SCI_SETREADONLY, 1);
    // hide all margins
    Send(sci_, SCI_SETMARGINWIDTHN, 0, 0);
    Send(sci_, SCI_SETMARGINWIDTHN, 1, 0);
    Send(sci_, SCI_SETMARGINWIDTHN, 2, 0);
    Send(sci_, SCI_SETUNDOCOLLECTION, 0);

    SetFont(fontName_, fontSize_);
    ApplyTheme(*theme::Find(L"dark"));
    return true;
}

void TerminalPanel::SetFont(const std::wstring& name, int size) {
    fontName_ = name.empty() ? L"Consolas" : name;
    fontSize_ = size > 0 ? size : 11;
    HWND parent = hwnd_ ? ::GetParent(hwnd_) : nullptr;
    int dpi = parent ? ::GetDpiForWindow(parent) : 96;
    HFONT nf = ::CreateFontW(-MulDiv(fontSize_, dpi, 96), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                             fontName_.c_str());
    if (!nf) return;
    if (mono_) ::DeleteObject(mono_);
    mono_ = nf;
    if (input_) {
        ::SendMessageW(input_, WM_SETFONT, (WPARAM)mono_, TRUE);
        PlaceCaretEnd(input_);
    }
    if (sci_) {
        Send(sci_, SCI_STYLESETFONT, STYLE_DEFAULT,
             (LPARAM)WideToUtf8(fontName_).c_str());
        Send(sci_, SCI_STYLESETSIZE, STYLE_DEFAULT, fontSize_);
        Send(sci_, SCI_STYLECLEARALL);
        ApplyTheme(*theme::Find(L"dark"));   // STYLECLEARALL resets colours
    }
}

void TerminalPanel::Destroy() {
    pty_.Stop();
    if (sci_) { ::RemoveWindowSubclass(sci_, SciProcThunk, kSciSubclassId);
                ::DestroyWindow(sci_); sci_ = nullptr; }
    if (input_) { ::RemoveWindowSubclass(input_, InputProcThunk, kInputSubclassId);
                  ::DestroyWindow(input_); input_ = nullptr; }
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
    if (mono_) { ::DeleteObject(mono_); mono_ = nullptr; }
}

bool TerminalPanel::HasFocus() const {
    if (!hwnd_) return false;
    HWND f = ::GetFocus();
    return f == sci_ || f == input_;
}

void TerminalPanel::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
}

void TerminalPanel::Layout(int w, int h) {
    if (!hwnd_) return;
    int dpi = ::GetDpiForWindow(hwnd_);
    auto u = [dpi](int px) { return MulDiv(px, dpi, 96); };
    int topBar = u(24);
    int inH = u(28);
    int gap = u(8);
    int inW = std::max(0, w - 16);
    ::MoveWindow(label_, u(8), u(6), std::max(0, w - u(110)), u(18), TRUE);
    ::MoveWindow(closeBtn_, std::max(0, w - u(90)), u(2), u(84), u(22), TRUE);
    ::MoveWindow(sci_, u(6), topBar, std::max(0, w - 12),
                 std::max(0, h - topBar - inH - gap), TRUE);
    ::MoveWindow(input_, u(6), std::max(0, h - inH - u(4)), inW, inH, TRUE);
    // resize the console buffer to a sensible grid using real font metrics
    if (pty_.IsRunning()) {
        int wpx = std::max(0, w - 16);
        int charW = (int)Send(sci_, SCI_TEXTWIDTH, STYLE_DEFAULT, (LPARAM)"M");
        int lineH = (int)Send(sci_, SCI_TEXTHEIGHT, 0, 0);
        if (charW <= 0) charW = u(7);
        if (lineH <= 0) lineH = u(16);
        int cols = std::max(20, wpx / charW);
        int rows = std::max(10, std::max(0, h - topBar - inH) / lineH);
        pty_.Resize(cols, rows);
    }
}

// ------------------------------------------------------------------- shell

bool TerminalPanel::StartShell() {
    if (pty_.IsRunning()) return true;
    std::wstring comspec;
    wchar_t b[MAX_PATH]; ::GetEnvironmentVariableW(L"COMSPEC", b, MAX_PATH);
    comspec = b ? b : L"cmd.exe";
    std::wstring cmd = comspec + L" /K chcp 65001 >nul";
    pty_.onOutput = [this](const std::string& chunk) {
        // Runs on the ConPTY reader thread. Post a heap copy to the panel so
        // Sci mutations happen on the UI thread. Empty chunk = EOF.
        if (chunk.empty()) {
            ::PostMessageW(hwnd_, WM_APP_PTY_OUTPUT, 0, 1);   // lp=1 => EOF
            return;
        }
        auto* copy = new std::string(chunk);
        ::PostMessageW(hwnd_, WM_APP_PTY_OUTPUT, (WPARAM)copy, 0);
    };
    if (!pty_.Start(cmd, 120, 30)) {
        Logger::Error("TerminalPanel: shell start failed");
        return false;
    }
    running_ = true;
    UpdateLabel();
    ::SetFocus(input_);
    Logger::Info("TerminalPanel shell started");
    return true;
}

void TerminalPanel::StopShell() {
    pty_.Stop();
    running_ = false;
    Send(sci_, SCI_SETREADONLY, 0);
    Send(sci_, SCI_SETTEXT, 0, (LPARAM)"");
    Send(sci_, SCI_SETREADONLY, 1);
    lineStart_ = -1;
    UpdateLabel();
}

void TerminalPanel::RestartShell() {
    StopShell();
    StartShell();
}

bool TerminalPanel::TypeLine(const std::wstring& line) {
    if (!running_) return false;
    std::string utf8 = WideToUtf8(line);
    // Windows console treats \r (CR) as Enter. \n alone is not processed
    // as a line terminator by the console input handler.
    utf8 += "\r";
    pty_.Write(utf8);
    return true;
}

void TerminalPanel::SendLine(const std::wstring& line) {
    TypeLine(line);
}

// ----------------------------------------------------------------- output

void TerminalPanel::OnPtyOutput(const std::string& chunk) {
    std::string text;
    std::vector<VtStyle> styles;
    // base = absolute position where this chunk's output begins, so the
    // parser can record overwrite positions against the real buffer.
    sptr_t base = Send(sci_, SCI_GETLENGTH);
    vt::Parse(chunk, text, styles, lineStart_, (int)base);
    if (!text.empty()) AppendPty(text, styles);
}

void TerminalPanel::OnPtyEof() {
    running_ = false;
    UpdateLabel();
}

void TerminalPanel::AppendPty(const std::string& text, const std::vector<VtStyle>& styles) {
    if (!sci_ || text.empty()) return;

    sptr_t bufLen = Send(sci_, SCI_GETLENGTH);
    Send(sci_, SCI_SETREADONLY, 0);

    sptr_t start;
    if (lineStart_ >= 0 && lineStart_ < bufLen) {
        // Overwrite mode: a bare \r earlier placed the cursor mid-buffer;
        // replace from there to the current end with the new text.
        start = lineStart_;
        Send(sci_, SCI_SETTARGETSTART, start, 0);
        Send(sci_, SCI_SETTARGETEND, bufLen, 0);
        Send(sci_, SCI_REPLACETARGET, (uptr_t)text.size(), (LPARAM)text.c_str());
    } else {
        // Append mode: either there is no pending \r, or the \r fell at/after
        // the end of the buffer (e.g. a CRLF split across chunks). Appending
        // is equivalent to overwriting there, so consume the pending position.
        start = bufLen;
        Send(sci_, SCI_APPENDTEXT, (uptr_t)text.size(), (LPARAM)text.c_str());
    }
    // Pending overwrite position consumed (append or overwrite both place
    // text at the cursor); the next chunk starts in append mode unless the
    // parser sets lineStart again.
    lineStart_ = -1;

    // Re-apply styles for the range we just wrote.
    Send(sci_, SCI_STARTSTYLING, start, 0xFF);
    for (size_t i = 0; i < text.size(); ++i)
        Send(sci_, SCI_SETSTYLING, 1, (uptr_t)FgStyle(styles[i].bold, styles[i].fg));

    Send(sci_, SCI_SETREADONLY, 1);
    Send(sci_, SCI_GOTOPOS, Send(sci_, SCI_GETLENGTH));
}

void TerminalPanel::Retranslate() {
    if (closeBtn_) ::SetWindowTextW(closeBtn_, Tr(L"panel.terminal.close"));
    UpdateLabel();
}

void TerminalPanel::UpdateLabel() {
    if (!label_) return;
    std::wstring s = Tr(L"panel.terminal");
    s += L" — ";
    s += running_ ? Tr(L"panel.terminal.running") : Tr(L"panel.terminal.notrunning");
    s += L"（";
    s += running_ ? Tr(L"panel.terminal.hintrun") : Tr(L"panel.terminal.hintstop");
    s += L"）";
    ::SetWindowTextW(label_, s.c_str());
}

// ------------------------------------------------------------------ input

LRESULT TerminalPanel::InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                wchar_t buf[8192];
                ::GetWindowTextW(input_, buf, 8192);
                std::wstring line = buf;
                // Strip the "> " prompt prefix that the edit control holds.
                if (line.size() >= 2 && line[0] == L'>' && line[1] == L' ')
                    line = line.substr(2);
                ::SetWindowTextW(input_, L"> ");
                // reset caret after the prompt for the next command
                PlaceCaretEnd(input_);
                SendLine(line);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                ::SetFocus(sci_);
                return 0;
            }
            break;
        case WM_SETFOCUS:
            // terminal input edits at the end of the line; park the caret
            // there unless the user deliberately clicks elsewhere (a later
            // WM_LBUTTONDOWN overrides this).
            PlaceCaretEnd(input_);
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK TerminalPanel::InputProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                               UINT_PTR, DWORD_PTR ref) {
    auto* self = (TerminalPanel*)ref;
    return self->InputProc(h, m, wp, lp);
}

// ------------------------------------------------------------------- Sci

LRESULT TerminalPanel::SciProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // We keep Sci read-only; the input EDIT holds the typed command line.
    // Copy works natively on a selection; Ctrl+V routes to the input line.
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == 'V' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
                PasteClipboardToInput(hwnd_, input_);
                return 0;
            }
            if (wp == VK_INSERT && (::GetKeyState(VK_SHIFT) & 0x8000)) {
                PasteClipboardToInput(hwnd_, input_);
                return 0;
            }
            break;
        case WM_CONTEXTMENU: {
            bool sel = Send(sci_, SCI_GETSELECTIONEMPTY) == 0;
            HMENU m = ::CreatePopupMenu();
            ::AppendMenuW(m, MF_STRING | (sel ? MF_ENABLED : MF_GRAYED), 1,
                          Tr(L"term.copy"));
            ::AppendMenuW(m, MF_STRING, 2, Tr(L"term.pasteinput"));
            ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(m, MF_STRING, 3, Tr(L"term.selectall"));
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) ::GetCursorPos(&pt);
            int cmd = ::TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                       pt.x, pt.y, 0, h, nullptr);
            ::DestroyMenu(m);
            switch (cmd) {
                case 1: ::SendMessageW(sci_, WM_COPY, 0, 0); break;
                case 2: PasteClipboardToInput(hwnd_, input_); break;
                case 3: Send(sci_, SCI_SELECTALL); break;
            }
            return 0;
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK TerminalPanel::SciProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                             UINT_PTR, DWORD_PTR ref) {
    auto* self = (TerminalPanel*)ref;
    return self->SciProc(h, m, wp, lp);
}

// -------------------------------------------------------------- wnd proc

LRESULT TerminalPanel::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_PTY_OUTPUT: {
            if (lp == 1) { OnPtyEof(); return 0; }
            // wparam = std::string* (heap); we own + delete it.
            auto* chunk = reinterpret_cast<std::string*>(wp);
            if (chunk) { OnPtyOutput(*chunk); delete chunk; }
            return 0;
        }
        case WM_NCHITTEST: {
            POINT htpt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ::ScreenToClient(h, &htpt);
            if (htpt.y < 6) return HTCLIENT;
            break;
        }
        case WM_SETCURSOR: {
            POINT pt; ::GetCursorPos(&pt);
            ::ScreenToClient(h, &pt);
            if (pt.y < 6) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.y < 6) {
                resizing_ = true;
                POINT sp; ::GetCursorPos(&sp);
                dragStartScreenY_ = sp.y;
                RECT rc; ::GetWindowRect(h, &rc);
                dragStartH_ = rc.bottom - rc.top;
                ::SetCapture(h);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (resizing_) {
                POINT sp; ::GetCursorPos(&sp);
                int newH = dragStartH_ + (dragStartScreenY_ - sp.y);
                newH = std::max(120, std::min(1400, newH));
                if (onHeightChange) onHeightChange(newH);
                return 0;
            }
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool hot = pt.y < 6;
            if (hot) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
            }
            if (hot != splitHot_) {
                splitHot_ = hot;
                RECT rc; ::GetClientRect(h, &rc); rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (splitHot_) {
                splitHot_ = false;
                RECT rc; ::GetClientRect(h, &rc); rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_LBUTTONUP:
            if (resizing_) { resizing_ = false; ::ReleaseCapture(); return 0; }
            break;
        case WM_CAPTURECHANGED:
            resizing_ = false;
            break;
        case WM_PAINT: {
            DefWindowProcW(h, msg, wp, lp);
            HDC dc = ::GetDC(h);
            RECT rc; ::GetClientRect(h, &rc);
            RECT band{0, 0, rc.right, 5};
            HBRUSH b = ::CreateSolidBrush(splitHot_ || resizing_
                ? RGB(0xCC, 0xE3, 0xFF) : RGB(0xE9, 0xE9, 0xE9));
            ::FillRect(dc, &band, b);
            ::DeleteObject(b);
            HPEN pen = ::CreatePen(PS_SOLID, 1, splitHot_ || resizing_
                ? RGB(0x00, 0x78, 0xD4) : RGB(0xAC, 0xAC, 0xAC));
            HPEN old = (HPEN)::SelectObject(dc, pen);
            ::MoveToEx(dc, 0, 5, nullptr);
            ::LineTo(dc, rc.right, 5);
            ::SelectObject(dc, old);
            ::DeleteObject(pen);
            ::ReleaseDC(h, dc);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == ID_CLOSE && onClose) onClose();
            break;
        case WM_CTLCOLORSTATIC:
            return (LRESULT)::GetSysColorBrush(COLOR_BTNFACE);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK TerminalPanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (TerminalPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->WndProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

void TerminalPanel::ApplyTheme(const ThemeDef& t) {
    if (!sci_) return;
    // terminal uses its own dark palette for contrast regardless of editor theme
    colors_.bg = RGB(0x1E, 0x1E, 0x1E);
    colors_.fg = RGB(0xCC, 0xCC, 0xCC);
    struct S { int idx; COLORREF fg; COLORREF bg; bool bold; };
    S styles[] = {
        {TS_DEFAULT,            colors_.fg, colors_.bg, false},
        {TS_BOLD,               colors_.fg, colors_.bg, true },
        {TS_BLACK,              colors_.palette[0],  colors_.bg, false},
        {TS_RED,                colors_.palette[1],  colors_.bg, false},
        {TS_GREEN,              colors_.palette[2],  colors_.bg, false},
        {TS_YELLOW,             colors_.palette[3],  colors_.bg, false},
        {TS_BLUE,               colors_.palette[4],  colors_.bg, false},
        {TS_MAGENTA,            colors_.palette[5],  colors_.bg, false},
        {TS_CYAN,               colors_.palette[6],  colors_.bg, false},
        {TS_WHITE,              colors_.palette[7],  colors_.bg, false},
        {TS_BOLD_BLACK,         colors_.palette[8],  colors_.bg, true },
        {TS_BOLD_RED,           colors_.palette[9],  colors_.bg, true },
        {TS_BOLD_GREEN,         colors_.palette[10], colors_.bg, true },
        {TS_BOLD_YELLOW,        colors_.palette[11], colors_.bg, true },
        {TS_BOLD_BLUE,          colors_.palette[12], colors_.bg, true },
        {TS_BOLD_MAGENTA,       colors_.palette[13], colors_.bg, true },
        {TS_BOLD_CYAN,          colors_.palette[14], colors_.bg, true },
        {TS_BOLD_WHITE,         colors_.palette[15], colors_.bg, true },
    };
    for (auto& s : styles) {
        Send(sci_, SCI_STYLESETFORE, s.idx, s.fg);
        Send(sci_, SCI_STYLESETBACK, s.idx, s.bg);
        Send(sci_, SCI_STYLESETBOLD, s.idx, s.bold ? 1 : 0);
    }
    Send(sci_, SCI_SETCARETLINEBACK, RGB(0x2A, 0x2A, 0x2A));
    Send(sci_, SCI_STYLECLEARALL);
}

} // namespace xfs
