#include "TabBar.h"
#include "../core/Log.h"
#include <windowsx.h>

namespace xfs {

namespace {
// Subclass id is a constant tag; the instance travels in dwRefData.
constexpr UINT_PTR kTabSubclassId = 0x58465354; // 'X F S T'
} // namespace

bool TabBar::Create(HWND parent, HINSTANCE hInst, int id) {
    // Deliberately NOT owner-drawn (no TCS_OWNERDRAWFIXED): the stock tab
    // control only supports equal-width owner-drawn tabs (WM_MEASUREITEM is
    // sent once, every tab gets the same size). Keeping the control in its
    // normal auto-layout mode makes each tab width track its own text, so a
    // short file name gets a narrow tab and a long one gets a wide tab. We
    // paint every tab ourselves from WM_PAINT (see Handle) and only use the
    // control for layout, hit-testing, tooltips and drag reordering.
    hwnd_ = ::CreateWindowExW(0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN |
        TCS_FOCUSNEVER | TCS_TABS,
        0, 0, 600, 30, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
    if (!hwnd_) return false;

    ::SetWindowSubclass(hwnd_, WndProcThunk, kTabSubclassId, (DWORD_PTR)this);

    int dpi = ::GetDpiForWindow(hwnd_);
    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    ::SendMessageW(hwnd_, WM_SETFONT, (WPARAM)font_, TRUE);
    ::SendMessageW(hwnd_, TCM_SETPADDING, 0, MAKELPARAM(16, 6));

    // hover tooltip for full file paths (text supplied via TTN_GETDISPINFO)
    tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 0, 0, 0, 0, hwnd_, nullptr, hInst, nullptr);
    if (tooltip_) {
        TOOLINFO ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = hwnd_;
        ti.uFlags = TTF_SUBCLASS;
        ti.uId = 1;
        ti.lpszText = LPSTR_TEXTCALLBACK;
        ::GetClientRect(hwnd_, &ti.rect);
        ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        int dpi2 = ::GetDpiForWindow(hwnd_);
        ::SendMessageW(tooltip_, TTM_SETDELAYTIME,
                       TTDT_AUTOPOP, MAKELPARAM(8000, 0));
        ::SendMessageW(tooltip_, TTM_SETDELAYTIME,
                       TTDT_INITIAL, MAKELPARAM(350, 0));
        ::SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, dpi2, 96));
    }

    return true;
}

void TabBar::Destroy() {
    if (hwnd_) {
        ::RemoveWindowSubclass(hwnd_, WndProcThunk, kTabSubclassId);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (tooltip_) { ::DestroyWindow(tooltip_); tooltip_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
}

int TabBar::HeightForDpi(int dpi) const {
    return MulDiv(31, dpi, 96);
}

void TabBar::Insert(int index, const std::wstring& title) {
    // Store the text with trailing spaces (see PaddedTitle) so the control's
    // auto-computed tab width reserves our close-button area as well.
    std::wstring padded = PaddedTitle(title);
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(padded.c_str());
    SendMessageW(hwnd_, TCM_INSERTITEMW, index, (LPARAM)&ti);
}

void TabBar::Remove(int index) {
    if (hoverIndex_ == index) { hoverIndex_ = -1; }
    SendMessageW(hwnd_, TCM_DELETEITEM, index, 0);
}

void TabBar::Rename(int index, const std::wstring& title) {
    std::wstring padded = PaddedTitle(title);
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<LPWSTR>(padded.c_str());
    SendMessageW(hwnd_, TCM_SETITEMW, index, (LPARAM)&ti);
    RedrawItem(index);
}

std::wstring TabBar::PaddedTitle(const std::wstring& title) const {
    if (!hwnd_) return title;
    int dpi = ::GetDpiForWindow(hwnd_);
    // area our painter needs besides the text itself:
    //   left margin + text<->close gap + close button + right margin
    int need = MulDiv(10, dpi, 96) + MulDiv(8, dpi, 96)
             + MulDiv(16, dpi, 96) + MulDiv(4, dpi, 96);
    int padX = 16; // TCM_SETPADDING horizontal (raw px, applied on both sides)
    int extra = need - 2 * padX; // px the auto-layout must carry inside the text
    if (extra <= 0) return title;
    HDC dc = ::GetDC(hwnd_);
    HFONT f = font_ ? font_ : (HFONT)::SendMessageW(hwnd_, WM_GETFONT, 0, 0);
    HFONT old = (HFONT)::SelectObject(dc, f ? f : GetStockObject(DEFAULT_GUI_FONT));
    SIZE s{};
    ::GetTextExtentPoint32W(dc, L" ", 1, &s);
    int spaceW = s.cx > 0 ? s.cx : 7;
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd_, dc);
    int n = (extra + spaceW - 1) / spaceW;
    std::wstring t = title;
    t.append((size_t)n, L' ');
    return t;
}

void TabBar::SetCurrent(int index) {
    SendMessageW(hwnd_, TCM_SETCURSEL, index, 0);
}

int TabBar::Current() const {
    return (int)SendMessageW(hwnd_, TCM_GETCURSEL, 0, 0);
}

int TabBar::Count() const {
    return (int)SendMessageW(hwnd_, TCM_GETITEMCOUNT, 0, 0);
}

RECT TabBar::CloseRect(const RECT& r) const {
    // DPI-scaled square hit target (16 logical px), vertically centered,
    // anchored a few px from the tab's right edge.
    int dpi = ::GetDpiForWindow(hwnd_);
    int size = MulDiv(16, dpi, 96);
    int h = r.bottom - r.top;
    if (size > h - MulDiv(4, dpi, 96)) size = h - MulDiv(4, dpi, 96);
    if (size < 8) size = 8;
    int padV = (h - size) / 2;
    RECT c = r;
    c.right -= MulDiv(4, dpi, 96);
    c.left = c.right - size;
    c.top += padV;
    c.bottom = c.top + size;
    return c;
}

int TabBar::HitTest(POINT pt, bool* inClose) const {
    *inClose = false;
    TCHITTESTINFO ht{};
    ht.pt = pt;
    ht.flags = TCHT_ONITEM | TCHT_ONITEMICON | TCHT_ONITEMLABEL;
    int idx = (int)SendMessageW(hwnd_, TCM_HITTEST, 0, (LPARAM)&ht);
    if (idx < 0 || !(ht.flags & (TCHT_ONITEMICON | TCHT_ONITEMLABEL))) return idx;
    RECT rc;
    rc.left = rc.top = 0;
    if (!SendMessageW(hwnd_, TCM_GETITEMRECT, idx, (LPARAM)&rc)) return idx;
    POINT local = pt;
    MapWindowPoints(hwnd_, hwnd_, &local, 1); // already client coords
    RECT cr = CloseRect(rc);
    *inClose = PtInRect(&cr, pt) != FALSE;
    return idx;
}

void TabBar::RedrawItem(int index) {
    RECT rc;
    if (index >= 0 && SendMessageW(hwnd_, TCM_GETITEMRECT, index, (LPARAM)&rc))
        InvalidateRect(hwnd_, &rc, FALSE);
}

void TabBar::UpdateHover(POINT pt) {
    bool inClose = false;
    int idx = HitTest(pt, &inClose);
    if (idx != hoverIndex_ || inClose != hoverInClose_) {
        int old = hoverIndex_;
        hoverIndex_ = idx;
        hoverInClose_ = inClose;
        RedrawItem(old);
        RedrawItem(idx);
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
    TrackMouseEvent(&tme);
}

LRESULT CALLBACK TabBar::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                      UINT_PTR idSubclass, DWORD_PTR refData) {
    auto* self = reinterpret_cast<TabBar*>(refData);
    return self->Handle(h, m, wp, lp);
}

LRESULT TabBar::Handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            // Full custom paint: the control auto-lays out variable-width tabs
            // (text + padding) but we draw every tab so they match the theme.
            // We never call DefSubclassProc here, so the stock tab look is
            // never painted underneath.
            PAINTSTRUCT ps;
            HDC hdc = ::BeginPaint(h, &ps);
            if (stripBg_) {
                RECT cr;
                ::GetClientRect(h, &cr);
                HBRUSH b = ::CreateSolidBrush(stripBg_());
                ::FillRect(hdc, &cr, b);
                ::DeleteObject(b);
            }
            int sel = Current();
            int count = Count();
            for (int i = 0; i < count; ++i) {
                RECT rc{};
                if (!::SendMessageW(h, TCM_GETITEMRECT, i, (LPARAM)&rc)) continue;
                if (onDraw_) onDraw_(hdc, i, rc, i == sel);
            }
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            // everything is covered by WM_PAINT; skip the white erase to avoid
            // flicker during hover/selection redraws
            return 1;

        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (nm->hwndFrom == tooltip_ && nm->code == TTN_GETDISPINFOW) {
                NMTTDISPINFOW* di = (NMTTDISPINFOW*)lp;
                tipText_.clear();
                if (hoverIndex_ >= 0 && cb_.onTooltipText)
                    tipText_ = cb_.onTooltipText(hoverIndex_);
                if (!tipText_.empty()) {
                    di->lpszText = tipText_.data();
                } else {
                    di->szText[0] = L'\0';
                    di->lpszText = di->szText;
                }
                return 0;
            }
            break;
        }
        case WM_CONTEXTMENU: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            // if from keyboard, use selected tab position
            if (pt.x == -1 && pt.y == -1) {
                int sel = Current();
                if (sel < 0) break;
                RECT rc{};
                SendMessageW(hwnd_, TCM_GETITEMRECT, sel, (LPARAM)&rc);
                pt.x = rc.left; pt.y = rc.bottom;
                MapWindowPoints(hwnd_, nullptr, &pt, 1);
            }
            bool inClose = false;
            int idx = HitTest({ pt.x, pt.y }, &inClose);
            // convert screen coords to client for hit test
            POINT cpt = pt;
            MapWindowPoints(nullptr, hwnd_, &cpt, 1);
            idx = HitTest(cpt, &inClose);
            if (idx >= 0 && cb_.onContextMenu)
                cb_.onContextMenu(idx, pt.x, pt.y);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            UpdateHover(pt);

            if (dragging_ && dragIndex_ >= 0) {
                // 拖离标签条：鼠标离开条矩形（上下越界）→ 置 detach 待发，
                // 回到条内则取消（此时已做过的 reorder 不回滚，简单优先）。
                RECT strip{};
                ::GetClientRect(hwnd_, &strip);
                bool outside = pt.y < strip.top || pt.y >= strip.bottom;
                if (outside)
                    detachPending_ = true;
                else
                    detachPending_ = false;

                if (!detachPending_ &&
                    abs(pt.x - dragStart_.x) > 6) {
                    // live reorder while crossing adjacent tab midpoints
                    for (int i = Count() - 1; i >= 0; --i) {
                        if (i == dragIndex_) continue;
                        RECT rc;
                        if (!SendMessageW(hwnd_, TCM_GETITEMRECT, i, (LPARAM)&rc)) continue;
                        int mid = (rc.left + rc.right) / 2;
                        if ((pt.x < mid && i < dragIndex_) || (pt.x > mid && i > dragIndex_)) {
                            wchar_t buf[512];
                            TCITEMW ti{};
                            ti.mask = TCIF_TEXT;
                            ti.pszText = buf;
                            ti.cchTextMax = 512;
                            SendMessageW(hwnd_, TCM_GETITEMW, dragIndex_, (LPARAM)&ti);
                            SendMessageW(hwnd_, TCM_DELETEITEM, dragIndex_, 0);
                            SendMessageW(hwnd_, TCM_INSERTITEMW, i, (LPARAM)&ti);
                            if (cb_.onReorder) cb_.onReorder(dragIndex_, i);
                            hoverIndex_ = -1;
                            dragIndex_ = i;
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            break;
                        }
                    }
                }
            }
            break;
        }
        case WM_MOUSELEAVE: {
            int old = hoverIndex_;
            hoverIndex_ = -1;
            hoverInClose_ = false;
            RedrawItem(old);
            break;
        }
        case WM_LBUTTONDOWN: {
            bool inClose = false;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = HitTest(pt, &inClose);
            Logger::Info("TabBar: drag start idx=" + std::to_string(idx) +
                         " inClose=" + std::to_string(inClose ? 1 : 0) +
                         " pt=" + std::to_string(pt.x) + "," + std::to_string(pt.y));
            SetCapture(hwnd_);
            dragging_ = true;
            dragIndex_ = idx;
            dragStart_ = pt;
            break;
        }
        case WM_LBUTTONUP: {
            // capture the drag verdict BEFORE ReleaseCapture: it synchronously
            // delivers WM_CAPTURECHANGED which clears the detach flag.
            bool wasDetach = detachPending_;
            int detachIdx = wasDetach ? dragIndex_ : -1;
            if (GetCapture() == hwnd_) ReleaseCapture();
            dragging_ = false;
            detachPending_ = false;
            bool inClose = false;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = HitTest(pt, &inClose);
            if (wasDetach && detachIdx >= 0 && cb_.onDetachRequest) {
                Logger::Info("TabBar: detach fired idx=" + std::to_string(detachIdx));
                cb_.onDetachRequest(detachIdx);   // N++-style: drag-out = new window
                return 0;
            }
            if (inClose && idx >= 0 && cb_.onCloseRequest)
                cb_.onCloseRequest(idx);
            else if (idx >= 0 && cb_.onSelect)
                cb_.onSelect(idx);
            break;
        }
        case WM_CAPTURECHANGED:
            dragging_ = false;
            detachPending_ = false;
            break;

        case WM_LBUTTONDBLCLK: {
            bool inClose = false;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = HitTest(pt, &inClose);
            if (idx < 0 && cb_.onNewRequest) cb_.onNewRequest();
            return 0;
        }

        case WM_MBUTTONDOWN: {
            bool inClose = false;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = HitTest(pt, &inClose);
            if (idx >= 0 && cb_.onCloseRequest) cb_.onCloseRequest(idx);
            return 0;
        }

        default:
            break;
    }

    return DefSubclassProc(h, msg, wp, lp);
}

} // namespace xfs
