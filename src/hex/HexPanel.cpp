#include "HexPanel.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../theme/Theme.h"

#include <commctrl.h>
#include <windowsx.h>
#include <cstdio>
#include <algorithm>


namespace xfs {
namespace {
constexpr wchar_t kPanelClass[] = L"xfsWinPadHexPanel";
constexpr UINT_PTR kSciSubclassId  = 0x48585032; // 'HXP2'
constexpr int ID_LABEL = 1300;
constexpr int ID_CLOSE = 1301;
constexpr size_t kMaxBytes = 1u * 1024u * 1024u;   // 1MB view cap

sptr_t Send(HWND h, UINT m, uptr_t wp = 0, LPARAM lp = 0) {
    return ::SendMessageW(h, m, (WPARAM)wp, lp);
}

bool IsHexDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}
int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return c - L'A' + 10;
}
char HexChar(int v) { return "0123456789ABCDEF"[v & 0xF]; }
char AsciiChar(BYTE b) { return (b >= 0x20 && b < 0x7F) ? (char)b : '.'; }
} // namespace

// ---------------------------------------------------------------- panel frame

bool HexPanel::Create(HWND parent, HINSTANCE hInst) {
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

    hwnd_ = ::CreateWindowExW(0, kPanelClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 600, 200, parent,
                              (HMENU)(INT_PTR)1103, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", Tr(L"panel.hex"),
                               WS_CHILD | WS_VISIBLE | SS_NOTIFY, 6, 8, 520, 18,
                               hwnd_, (HMENU)(INT_PTR)ID_LABEL, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.hex.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 2, 76, 22,
                                  hwnd_, (HMENU)(INT_PTR)ID_CLOSE, hInst, nullptr);
    if (label_) ::SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (closeBtn_) ::SendMessageW(closeBtn_, WM_SETFONT, (WPARAM)font_, TRUE);

    sci_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"Scintilla", nullptr,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             6, 26, 588, 160,
                             hwnd_, nullptr, hInst, nullptr);
    if (!sci_) return false;
    ::SetWindowSubclass(sci_, SciProcThunk, kSciSubclassId, (DWORD_PTR)this);

    // monospace, no margins, no wrap; stays read-only - all edits are
    // programmatic single-byte operations from the key interceptor
    Send(sci_, SCI_SETREADONLY, 1);
    Send(sci_, SCI_SETCODEPAGE, SC_CP_UTF8);
    Send(sci_, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Consolas");
    Send(sci_, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    Send(sci_, SCI_STYLECLEARALL);
    Send(sci_, SCI_SETWRAPMODE, SC_WRAP_NONE);
    Send(sci_, SCI_SETCARETLINEVISIBLE, 1);
    Send(sci_, SCI_SETCARETLINEBACK, RGB(0xF2, 0xF6, 0xFC));
    Send(sci_, SCI_SETMARGINWIDTHN, 0, 0);
    Send(sci_, SCI_SETMARGINWIDTHN, 1, 0);
    Send(sci_, SCI_SETMARGINWIDTHN, 2, 0);
    Send(sci_, SCI_SETHSCROLLBAR, 0);
    Send(sci_, SCI_SETUNDOCOLLECTION, 0);
    Send(sci_, SCI_SETCARETPERIOD, 530);
    return true;
}

void HexPanel::Destroy() {
    if (sci_) {
        ::RemoveWindowSubclass(sci_, SciProcThunk, kSciSubclassId);
        ::DestroyWindow(sci_);
        sci_ = nullptr;
    }
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
}

bool HexPanel::HasFocus() const {
    return sci_ && ::GetFocus() == sci_;
}

void HexPanel::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
}

void HexPanel::Layout(int w, int h) {
    if (!hwnd_) return;
    int dpi = ::GetDpiForWindow(hwnd_);
    ::MoveWindow(label_, 8, 9, std::max(0, w - MulDiv(110, dpi, 96)), 18, TRUE);
    ::MoveWindow(closeBtn_, std::max(0, w - MulDiv(90, dpi, 96)), 3,
                 MulDiv(84, dpi, 96), MulDiv(22, dpi, 96), TRUE);
    ::MoveWindow(sci_, 6, 26, std::max(0, w - 12), std::max(0, h - 32), TRUE);
}

// ------------------------------------------------------------------ load/save

bool HexPanel::Load(const std::wstring& filePath) {
    if (!hwnd_) return false;
    std::string raw;
    unsigned long long size = 0;
    HANDLE h = ::CreateFileW(filePath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Logger::Error("HexPanel: cannot open " + WideToUtf8(filePath));
        return false;
    }
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h, &li)) { ::CloseHandle(h); return false; }
    size = (unsigned long long)li.QuadPart;
    DWORD toRead = (DWORD)std::min<unsigned long long>(size, kMaxBytes);
    raw.resize(toRead);
    DWORD got = 0;
    BOOL rd = toRead == 0 || ::ReadFile(h, raw.data(), toRead, &got, nullptr);
    ::CloseHandle(h);
    if (!rd || got != toRead) {
        Logger::Error("HexPanel: read failed " + WideToUtf8(filePath));
        return false;
    }

    path_ = filePath;
    raw_ = std::move(raw);
    fileSize_ = size;
    truncated_ = size > raw_.size();
    overlay_.clear();
    undo_.clear();
    redo_.clear();

    RenderFull();
    UpdateLabel();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetFocus(sci_);
    Logger::Info("HexPanel load: " + WideToUtf8(filePath) + " size=" +
                 std::to_string(size) + (truncated_ ? " (TRUNCATED)" : ""));
    return true;
}

bool HexPanel::Save() {
    if (path_.empty()) return false;
    if (truncated_) {
        ::MessageBoxW(::GetAncestor(hwnd_, GA_ROOT),
                      Tr(L"msg.hextrunc"),
                      L"xfsWinPad Hex", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (overlay_.empty()) return true;

    std::string out = raw_;
    for (auto& kv : overlay_) {
        if (kv.first < out.size()) out[kv.first] = (char)kv.second;
    }
    if (!WriteFileBytes(path_, out.data(), out.size())) {
        Logger::Error("HexPanel save FAILED: " + WideToUtf8(path_));
        ::MessageBoxW(::GetAncestor(hwnd_, GA_ROOT),
                      Tr(L"msg.hexsavefail"), L"xfsWinPad Hex",
                      MB_OK | MB_ICONERROR);
        return false;
    }
    size_t n = overlay_.size();
    overlay_.clear();
    undo_.clear();
    redo_.clear();
    StyleAll();
    UpdateLabel();
    Logger::Info("HexPanel save: " + std::to_string(n) + " modified bytes -> " +
                 WideToUtf8(path_));
    return true;
}

BYTE HexPanel::ByteAt(size_t off) const {
    auto it = overlay_.find(off);
    if (it != overlay_.end()) return it->second;
    return off < raw_.size() ? (BYTE)raw_[off] : 0;
}

void HexPanel::SetByte(size_t off, BYTE v) {
    if (off >= raw_.size()) return;
    BYTE old = ByteAt(off);
    if (old == v) return;             // no-op edit: keep undo history clean
    undo_.push_back({off, old, v});
    if (undo_.size() > 10000) undo_.erase(undo_.begin());
    redo_.clear();
    if (v == (BYTE)raw_[off])
        overlay_.erase(off);          // edited back to original file value
    else
        overlay_[off] = v;
    Logger::Debug("HexPanel SetByte: off=" + std::to_string(off) +
                  " " + std::to_string((int)old) + "->" + std::to_string((int)v));
    RenderLine((int)(off / kBytesPerLine));
    UpdateLabel();
}

void HexPanel::Undo() {
    if (undo_.empty()) return;
    HexUndo u = undo_.back();
    undo_.pop_back();
    if (u.before == (BYTE)raw_[u.off])
        overlay_.erase(u.off);
    else
        overlay_[u.off] = u.before;
    redo_.push_back(u);
    RenderLine((int)(u.off / kBytesPerLine));
    UpdateLabel();
    Logger::Debug("HexPanel Undo: off=" + std::to_string(u.off) +
                  " -> " + std::to_string((int)u.before) +
                  " (undo depth " + std::to_string(undo_.size()) + ")");
}

void HexPanel::Redo() {
    if (redo_.empty()) return;
    HexUndo u = redo_.back();
    redo_.pop_back();
    if (u.after == (BYTE)raw_[u.off])
        overlay_.erase(u.off);
    else
        overlay_[u.off] = u.after;
    undo_.push_back(u);
    RenderLine((int)(u.off / kBytesPerLine));
    UpdateLabel();
    Logger::Debug("HexPanel Redo: off=" + std::to_string(u.off) +
                  " -> " + std::to_string((int)u.after));
}

// ------------------------------------------------------------------ rendering

std::string HexPanel::BuildLine(int line) const {
    size_t base = (size_t)line * kBytesPerLine;
    std::string s;
    s.reserve(kLineStride);
    char addr[9];
    sprintf_s(addr, "%08X", (unsigned)(base & 0xFFFFFFFFu));
    s.append(addr);
    s.append("  ");
    for (int i = 0; i < kBytesPerLine; ++i) {
        size_t off = base + i;
        if (off < raw_.size()) {
            BYTE b = ByteAt(off);
            s += HexChar(b >> 4);
            s += HexChar(b & 0xF);
        } else {
            s += "  ";
        }
        s += ' ';
    }
    s += " |";
    for (int i = 0; i < kBytesPerLine; ++i) {
        size_t off = base + i;
        s += off < raw_.size() ? AsciiChar(ByteAt(off)) : ' ';
    }
    s += '|';
    return s;
}

void HexPanel::RenderFull() {
    Send(sci_, SCI_SETREADONLY, 0);
    Send(sci_, SCI_SETTEXT, 0, (LPARAM)"");
    int lines = (int)((raw_.size() + kBytesPerLine - 1) / kBytesPerLine);
    const int kChunkLines = 4096;
    std::string chunk;
    for (int start = 0; start < lines; start += kChunkLines) {
        chunk.clear();
        int end = std::min(lines, start + kChunkLines);
        for (int l = start; l < end; ++l) {
            chunk += BuildLine(l);
            if (l + 1 < lines) chunk += "\r\n";
        }
        Send(sci_, SCI_APPENDTEXT, chunk.size(), (LPARAM)chunk.c_str());
    }
    Send(sci_, SCI_SETREADONLY, 1);
    Send(sci_, SCI_GOTOPOS, 0);
    // caret starts on the first hex nibble (position 0 is the address column)
    Send(sci_, SCI_SETSELECTION, (uptr_t)kHexStart, (LPARAM)kHexStart);
    StyleAll();
}

void HexPanel::RenderLine(int line) {
    sptr_t docLines = Send(sci_, SCI_GETLINECOUNT);
    if (line < 0 || line >= docLines) return;
    sptr_t start = Send(sci_, SCI_POSITIONFROMLINE, line);
    sptr_t end = (line + 1 < docLines)
        ? Send(sci_, SCI_POSITIONFROMLINE, line + 1)
        : Send(sci_, SCI_GETLENGTH);
    std::string s = BuildLine(line);
    // target range includes this line's CRLF; keep it or the line merges
    // with the next one and the whole dump shifts left per edit
    if (line + 1 < docLines) s += "\r\n";
    sptr_t caret = Send(sci_, SCI_GETCURRENTPOS);
    Logger::Debug("HexPanel RenderLine: line=" + std::to_string(line) +
                  " range=" + std::to_string(end - start) +
                  " newLen=" + std::to_string(s.size()));

    Send(sci_, SCI_SETREADONLY, 0);
    Send(sci_, SCI_SETTARGETSTART, start);
    Send(sci_, SCI_SETTARGETEND, end);
    Send(sci_, SCI_REPLACETARGET, s.size(), (LPARAM)s.c_str());
    Send(sci_, SCI_SETREADONLY, 1);

    // keep the caret on the same byte/nibble it was on
    ByteRef ref = PosToByte(caret);
    caret = ref.valid ? ByteToPos(ref.line, ref.index, ref.ascii, ref.nibble)
                      : std::min(caret, Send(sci_, SCI_GETLENGTH));
    Send(sci_, SCI_SETSELECTION, caret, caret);
    StyleRange(start, start + (sptr_t)s.size());
}

void HexPanel::StyleRange(sptr_t start, sptr_t end) {
    sptr_t len = Send(sci_, SCI_GETLENGTH);
    end = std::min(end, len);
    if (start >= end) return;
    int firstLine = (int)Send(sci_, SCI_LINEFROMPOSITION, start);
    int lastLine = (int)Send(sci_, SCI_LINEFROMPOSITION, end - 1);
    sptr_t total = Send(sci_, SCI_GETLINECOUNT);

    for (int l = firstLine; l <= lastLine; ++l) {
        sptr_t ls = Send(sci_, SCI_POSITIONFROMLINE, l);
        size_t base = (size_t)l * kBytesPerLine;
        Send(sci_, SCI_STARTSTYLING, ls, 0xFF);

        Send(sci_, SCI_SETSTYLING, kHexStart, ST_ADDR);        // offset + 2sp

        // hex bytes in normal/modified runs
        int i = 0;
        while (i < kBytesPerLine) {
            if (base + (size_t)i >= raw_.size()) {
                Send(sci_, SCI_SETSTYLING, (kBytesPerLine - i) * 3, ST_HEX);
                break;
            }
            bool mod = overlay_.count(base + (size_t)i) > 0;
            int n = 1;
            while (i + n < kBytesPerLine && base + (size_t)(i + n) < raw_.size() &&
                   (overlay_.count(base + (size_t)(i + n)) > 0) == mod)
                ++n;
            Send(sci_, SCI_SETSTYLING, n * 3, mod ? ST_HEX_MOD : ST_HEX);
            i += n;
        }

        Send(sci_, SCI_SETSTYLING, 2, ST_ADDR);                // space + '|'

        // ascii column runs
        i = 0;
        while (i < kBytesPerLine) {
            if (base + (size_t)i >= raw_.size()) {
                Send(sci_, SCI_SETSTYLING, kBytesPerLine - i, ST_ASCII);
                break;
            }
            bool mod = overlay_.count(base + (size_t)i) > 0;
            int n = 1;
            while (i + n < kBytesPerLine && base + (size_t)(i + n) < raw_.size() &&
                   (overlay_.count(base + (size_t)(i + n)) > 0) == mod)
                ++n;
            Send(sci_, SCI_SETSTYLING, n, mod ? ST_ASCII_MOD : ST_ASCII);
            i += n;
        }

        Send(sci_, SCI_SETSTYLING, 1, ST_ADDR);                // trailing '|'
        if (l + 1 < total)
            Send(sci_, SCI_SETSTYLING, 2, ST_HEX);             // CRLF
    }
}

void HexPanel::StyleAll() {
    sptr_t len = Send(sci_, SCI_GETLENGTH);
    StyleRange(0, len);
}

void HexPanel::Retranslate() {
    if (closeBtn_) ::SetWindowTextW(closeBtn_, Tr(L"panel.hex.close"));
    UpdateLabel();
}

void HexPanel::UpdateLabel() {
    if (!label_) return;
    const std::wstring title =
        I18n::Instance().Fmt(L"panel.hex.title",
                             {path_.empty() ? Tr(L"panel.hex.nofile")
                                            : std::wstring(path_),
                              std::to_wstring(fileSize_),
                              truncated_ ? Tr(L"panel.hex.truncated") : L"",
                              std::to_wstring((unsigned)overlay_.size())});
    ::SetWindowTextW(label_, title.c_str());
}

// ------------------------------------------------------------------ mapping

HexPanel::ByteRef HexPanel::PosToByte(sptr_t pos) const {
    ByteRef r;
    int line = (int)Send(sci_, SCI_LINEFROMPOSITION, pos);
    sptr_t lineStart = Send(sci_, SCI_POSITIONFROMLINE, line);
    int col = (int)(pos - lineStart);
    r.line = line;
    if (col >= kHexStart && col < kHexStart + kBytesPerLine * 3 - 1) {
        int rel = col - kHexStart;
        r.index = rel / 3;
        r.nibble = (rel % 3) == 0 ? 0 : 1;   // 0 = high nibble col, 1 = low
        r.ascii = false;
        r.valid = true;
    } else if (col >= kAsciiStart && col < kAsciiStart + kBytesPerLine) {
        r.index = col - kAsciiStart;
        r.ascii = true;
        r.valid = true;
    }
    // positions past end-of-data are padding: not editable, caret snaps away
    if (r.valid &&
        (size_t)r.line * kBytesPerLine + (size_t)r.index >= raw_.size())
        r.valid = false;
    return r;
}

sptr_t HexPanel::ByteToPos(int line, int index, bool ascii, int nibble) const {
    int col = ascii ? (kAsciiStart + index)
                    : (kHexStart + index * 3 + (nibble ? 1 : 0));
    sptr_t ls = Send(sci_, SCI_POSITIONFROMLINE, line);
    return ls + col;
}

// ------------------------------------------------------------------ editing

LRESULT HexPanel::SciProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CHAR: {
            wchar_t ch = (wchar_t)wp;
            sptr_t pos = Send(h, SCI_GETCURRENTPOS);
            ByteRef ref = PosToByte(pos);
            if (!ref.valid) {
                // Caret sits in the address column, a gap, or the padding area:
                // route the edit to the NEAREST byte on that line instead of
                // swallowing it (click-a-line-edit-that-line behaviour).
                int line = (int)Send(h, SCI_LINEFROMPOSITION, pos);
                size_t lineStart = (size_t)line * kBytesPerLine;
                if (lineStart >= raw_.size()) return 0;   // no data on this line
                sptr_t ls = Send(h, SCI_POSITIONFROMLINE, line);
                int col = (int)(pos - ls);
                int idx;
                bool ascii;
                if (col < kHexStart + kBytesPerLine * 3) {
                    int rel = std::max(0, col - kHexStart);
                    idx = rel / 3;
                    ascii = false;
                } else {
                    idx = col - kAsciiStart;
                    ascii = true;
                }
                int dataOnLine = (int)std::min<size_t>(kBytesPerLine,
                                                       raw_.size() - lineStart);
                idx = std::min(std::max(0, idx), dataOnLine - 1);
                ref.line = line; ref.index = idx; ref.nibble = 0;
                ref.ascii = ascii; ref.valid = true;
                sptr_t np = ByteToPos(line, idx, ascii, 0);
                Send(h, SCI_SETSELECTION, np, np);
            }

            if (!ref.ascii && IsHexDigit(ch)) {
                int v = HexVal(ch);
                size_t off = (size_t)ref.line * kBytesPerLine + ref.index;
                if (off < raw_.size()) {
                    BYTE old = ByteAt(off);
                    BYTE nv = ref.nibble == 0
                        ? (BYTE)((old & 0x0F) | (v << 4))
                        : (BYTE)((old & 0xF0) | v);
                    SetByte(off, nv);
                }
                // advance: hi->low, low->next byte hi (wraps to next line)
                int nextNibble = ref.nibble == 0 ? 1 : 0;
                int nextIndex = ref.index + (ref.nibble == 0 ? 0 : 1);
                int nextLine = ref.line;
                if (nextIndex >= kBytesPerLine) {
                    nextIndex = 0;
                    ++nextLine;
                    nextNibble = 0;
                }
                sptr_t np = ByteToPos(nextLine, nextIndex, false, nextNibble);
                Send(h, SCI_SETSELECTION, np, np);
                return 0;
            }
            if (ref.ascii && ch >= 0x20 && ch < 0x7F) {
                size_t off = (size_t)ref.line * kBytesPerLine + ref.index;
                if (off < raw_.size())
                    SetByte(off, (BYTE)ch);
                int nextIndex = ref.index + 1;
                int nextLine = ref.line;
                if (nextIndex >= kBytesPerLine) { nextIndex = 0; ++nextLine; }
                sptr_t np = ByteToPos(nextLine, nextIndex, true, 0);
                Send(h, SCI_SETSELECTION, np, np);
                return 0;
            }
            return 0;   // swallow everything else: buffer integrity by design
        }
        case WM_KEYDOWN: {
            if (wp == VK_TAB) {
                sptr_t pos = Send(h, SCI_GETCURRENTPOS);
                ByteRef ref = PosToByte(pos);
                if (ref.valid) {
                    sptr_t np = ByteToPos(ref.line, ref.index, !ref.ascii, ref.nibble);
                    Send(h, SCI_SETSELECTION, np, np);
                }
                return 0;
            }
            if (wp == VK_ESCAPE) {
                ::SetFocus(::GetAncestor(hwnd_, GA_ROOT));   // back to main editor
                return 0;
            }
            if (wp == VK_BACK || wp == VK_DELETE)
                return 0;   // overwrite-only editing (data safety)
            break;
        }
        case WM_SYSCHAR:   // block Alt-typing from mutating the dump
            return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK HexPanel::SciProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR ref) {
    auto* self = (HexPanel*)ref;
    return self->SciProc(h, m, wp, lp);
}

LRESULT HexPanel::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            // splitter strip hit-test: claim the top 6px so mouse messages
            // route here even if a child control overlaps the drag band
            POINT htpt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ::ScreenToClient(h, &htpt);
            if (htpt.y < 6) return HTCLIENT;   // handled by WM_SETCURSOR/WM_LBUTTONDOWN
            break;
        }
        case WM_SETCURSOR: {
            // size cursor over the top splitter strip
            POINT pt; ::GetCursorPos(&pt);
            ::ScreenToClient(h, &pt);
            if (pt.y < 6) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.y < 6) {
                // screen coordinates: the panel moves under the cursor while
                // resizing, so client-relative deltas cancel themselves out
                resizing_ = true;
                POINT sp; ::GetCursorPos(&sp);
                dragStartScreenY_ = sp.y;
                RECT rc; ::GetWindowRect(h, &rc);
                dragStartH_ = rc.bottom - rc.top;
                ::SetCapture(h);
                Logger::Debug("HexPanel splitter down: screenY=" +
                              std::to_string(sp.y) + " h=" +
                              std::to_string(dragStartH_));
                return 0;
            }
            if (sci_) ::SetFocus(sci_);   // clicking anywhere focuses the dump
            break;
        }
        case WM_MOUSEMOVE: {
            if (resizing_) {
                POINT sp; ::GetCursorPos(&sp);
                int newH = dragStartH_ + (dragStartScreenY_ - sp.y);
                newH = std::max(90, std::min(1400, newH));
                if (onHeightChange) onHeightChange(newH);
                return 0;
            }
            // splitter hover highlight
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool hot = pt.y < 6;
            if (hot) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
            }
            if (hot != splitHot_) {
                splitHot_ = hot;
                RECT rc; ::GetClientRect(h, &rc);
                rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (splitHot_) {
                splitHot_ = false;
                RECT rc; ::GetClientRect(h, &rc);
                rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (resizing_) {
                resizing_ = false;
                ::ReleaseCapture();
                return 0;
            }
            break;
        }
        case WM_CAPTURECHANGED:
            resizing_ = false;
            break;
        case WM_PAINT: {
            DefWindowProcW(h, msg, wp, lp);
            // splitter band: light fill + accent bottom line (blue when hot)
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
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            (void)nm;
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wp) == ID_CLOSE && onClose) onClose();
            break;
        }
        case WM_CTLCOLORSTATIC:
            return (LRESULT)::GetSysColorBrush(COLOR_BTNFACE);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK HexPanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (HexPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->WndProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

void HexPanel::ApplyTheme(const ThemeDef& t) {
    colors_.bg = t.editorBg;
    colors_.fg = t.editorFg;
    colors_.addr = t.lineNumFg;
    colors_.dim = t.lineNumFg;
    colors_.modFg = t.editorBg;
    colors_.modBg = t.tabAccent;
    if (!sci_) return;
    struct S { int idx; COLORREF fg; COLORREF bg; };
    S styles[] = {
        {STYLE_DEFAULT, colors_.fg,    colors_.bg},
        {ST_ADDR,       colors_.addr,  colors_.bg},
        {ST_HEX,        colors_.fg,    colors_.bg},
        {ST_HEX_MOD,    colors_.modFg, colors_.modBg},
        {ST_ASCII,      colors_.dim,   colors_.bg},
        {ST_ASCII_MOD,  colors_.modFg, colors_.modBg},
    };
    for (auto& s : styles) {
        Send(sci_, SCI_STYLESETFORE, s.idx, s.fg);
        Send(sci_, SCI_STYLESETBACK, s.idx, s.bg);
    }
    Send(sci_, SCI_SETCARETLINEBACK, t.currentLineBack);
    Send(sci_, SCI_STYLECLEARALL);
    if (!raw_.empty()) StyleAll();
}

} // namespace xfs
