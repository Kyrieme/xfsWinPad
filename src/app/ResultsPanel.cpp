#include "ResultsPanel.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <string>

namespace xfs {
namespace {
constexpr wchar_t kClass[] = L"xfsWinPadResultsPanel";
constexpr int ID_LIST = 1200;
constexpr int ID_LABEL = 1201;
constexpr int ID_CLOSE = 1202;
} // namespace

bool ResultsPanel::Create(HWND parent, HINSTANCE hInst) {
    if (hwnd_) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    int dpi = ::GetDpiForWindow(parent);
    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    hwnd_ = ::CreateWindowExW(0, kClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 600, 180, parent,
                              (HMENU)(INT_PTR)1102 /*frame child id*/, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", Tr(L"panel.results"),
                               WS_CHILD | WS_VISIBLE, 6, 4, 500, 18,
                               hwnd_, (HMENU)(INT_PTR)ID_LABEL, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.results.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                  0, 2, 80, 22,
                                  hwnd_, (HMENU)(INT_PTR)ID_CLOSE, hInst, nullptr);
    list_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                              LVS_NOSORTHEADER,
                              6, 24, 588, 140,
                              hwnd_, (HMENU)(INT_PTR)ID_LIST, hInst, nullptr);
    if (list_) {
        ::SendMessageW(list_, WM_SETFONT, (WPARAM)font_, TRUE);
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = LVCFMT_LEFT;
        c.pszText = const_cast<LPWSTR>(L"File");
        c.cx = 200; ::SendMessageW(list_, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
        c.pszText = const_cast<LPWSTR>(L"Line");
        c.cx = 56;  ::SendMessageW(list_, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
        c.pszText = const_cast<LPWSTR>(L"Content");
        c.cx = 600; ::SendMessageW(list_, LVM_INSERTCOLUMNW, 2, (LPARAM)&c);
        DWORD ex = LVS_EX_FULLROWSELECT;
        ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, (LPARAM)ex);
    }
    if (label_) ::SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (closeBtn_) ::SendMessageW(closeBtn_, WM_SETFONT, (WPARAM)font_, TRUE);
    return true;
}

void ResultsPanel::Destroy() {
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
    list_ = nullptr; label_ = nullptr;
}

void ResultsPanel::SetResults(std::vector<SearchHit> hits, const std::wstring& summary) {
    hits_ = std::move(hits);
    if (!hwnd_) return;
    ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    int i = 0;
    for (const SearchHit& h : hits_) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = const_cast<LPWSTR>(h.file.c_str());
        ::SendMessageW(list_, LVM_INSERTITEMW, i, (LPARAM)&it);

        wchar_t ln[32];
        swprintf_s(ln, L"%d", h.line);
        it.iSubItem = 1; it.pszText = ln;
        ::SendMessageW(list_, LVM_SETITEMW, i, (LPARAM)&it);

        it.iSubItem = 2; it.pszText = const_cast<LPWSTR>(h.lineText.c_str());
        ::SendMessageW(list_, LVM_SETITEMW, i, (LPARAM)&it);
        ++i;
    }
    ::SetWindowTextW(label_, summary.c_str());
    ::ShowWindow(hwnd_, SW_SHOW);
    if (hits_.empty()) ::SetForegroundWindow(::GetParent(hwnd_));
}

void ResultsPanel::Clear() {
    hits_.clear();
    if (hwnd_) {
        ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
        ::ShowWindow(hwnd_, SW_HIDE);
    }
}

const SearchHit* ResultsPanel::HitAt(int i) const {
    return (i >= 0 && i < (int)hits_.size()) ? &hits_[i] : nullptr;
}

void ResultsPanel::Retranslate() {
    if (closeBtn_) ::SetWindowTextW(closeBtn_, Tr(L"panel.results.close"));
    if (label_ && hits_.empty())
        ::SetWindowTextW(label_, Tr(L"panel.results"));
}

LRESULT CALLBACK ResultsPanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    auto* self = (ResultsPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->Handle(m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

LRESULT ResultsPanel::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETCURSOR: {
            POINT pt; ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd_, &pt);
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
                RECT rc; ::GetWindowRect(hwnd_, &rc);
                dragStartH_ = rc.bottom - rc.top;
                ::SetCapture(hwnd_);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (resizing_) {
                POINT sp; ::GetCursorPos(&sp);
                int newH = dragStartH_ + (dragStartScreenY_ - sp.y);
                newH = std::max(90, std::min(1200, newH));
                if (onHeightChange) onHeightChange(newH);
                return 0;
            }
            // splitter hover highlight
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool hot = pt.y < 6;
            if (hot) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
                TrackMouseEvent(&tme);
            }
            if (hot != splitHot_) {
                splitHot_ = hot;
                RECT rc; ::GetClientRect(hwnd_, &rc);
                rc.bottom = 6;
                ::InvalidateRect(hwnd_, &rc, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (splitHot_) {
                splitHot_ = false;
                RECT rc; ::GetClientRect(hwnd_, &rc);
                rc.bottom = 6;
                ::InvalidateRect(hwnd_, &rc, FALSE);
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
            ::DefWindowProcW(hwnd_, msg, wp, lp);
            HDC dc = ::GetDC(hwnd_);
            RECT rc; ::GetClientRect(hwnd_, &rc);
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
            ::ReleaseDC(hwnd_, dc);
            return 0;
        }
        case WM_SIZE: {
            RECT rc; ::GetClientRect(hwnd_, &rc);
            int dpi = ::GetDpiForWindow(hwnd_);
            int pad = MulDiv(6, dpi, 96);
            int labelH = MulDiv(20, dpi, 96);
            int closeW = MulDiv(84, dpi, 96);
            int topBand = 6;   // splitter drag strip
            if (closeBtn_) ::MoveWindow(closeBtn_, rc.right - closeW - pad, topBand + 3,
                                        closeW, labelH + 2, TRUE);
            if (label_) ::MoveWindow(label_, pad, topBand + 4,
                                     rc.right - closeW - 3 * pad, labelH, TRUE);
            if (list_) ::MoveWindow(list_, pad, topBand + 4 + labelH + 2,
                                    rc.right - 2 * pad, rc.bottom - labelH - 10 - topBand, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == ID_CLOSE && onClose) { onClose(); return 0; }
            break;
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (nm->idFrom == ID_LIST && nm->code == NM_DBLCLK) {
                NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lp;
                Logger::Info("ResultsPanel double-click row=" +
                             std::to_string(ia->iItem));
                if (ia->iItem >= 0 && onActivateRow) onActivateRow(ia->iItem);
                return 0;
            }
            break;
        }
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace xfs
