#include "BigFileView.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../theme/Theme.h"

#include <algorithm>
#include <cctype>
#include <windowsx.h>

namespace xfs {

namespace {

constexpr wchar_t kClass[] = L"xfsWinPadBigFileView";
constexpr wchar_t kRenderClass[] = L"xfsWinPadBigFileRender";
constexpr int kSplitBand = 6;
constexpr unsigned long long kMaxHChars = 100000;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(::towlower(c));
    return s;
}

// expand tabs to 4-column stops
std::wstring ExpandTabs(const std::wstring& in) {
    if (in.find(L'\t') == std::wstring::npos) return in;
    std::wstring out;
    out.reserve(in.size() + 16);
    for (wchar_t c : in) {
        if (c == L'\t') {
            int col = (int)(out.size() % 4);
            out.append((size_t)(4 - col), L' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

bool BigFileView::Create(HWND parent, HINSTANCE hInst) {
    inst_ = hInst;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::CreateSolidBrush(gutterBg_);
    wc.lpszClassName = kClass;
    ::RegisterClassW(&wc);

    WNDCLASSW wc2{};
    wc2.lpfnWndProc = RenderProcThunk;
    wc2.hInstance = hInst;
    wc2.hCursor = ::LoadCursorW(nullptr, IDC_IBEAM);
    wc2.style = CS_HREDRAW | CS_VREDRAW;
    wc2.lpszClassName = kRenderClass;
    ::RegisterClassW(&wc2);

    hwnd_ = ::CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, nullptr, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    render_ = ::CreateWindowExW(0, kRenderClass, L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
                                0, 0, 0, 0, hwnd_, nullptr, hInst, nullptr);
    if (!render_) return false;
    ::SetWindowLongPtrW(render_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", L"",
                               WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                               0, 0, 0, 0, hwnd_, nullptr, hInst, nullptr);
    gotoEdit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_LEFT,
                                  0, 0, 0, 0, hwnd_, nullptr, hInst, nullptr);
    gotoBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"bigfile.goto"),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)1, hInst, nullptr);
    findEdit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_LEFT,
                                  0, 0, 0, 0, hwnd_, nullptr, hInst, nullptr);
    findBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"bigfile.find"),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)2, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"bigfile.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)3, hInst, nullptr);

    uiFont_ = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
    ::SendMessageW(label_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    ::SendMessageW(gotoEdit_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    ::SendMessageW(gotoBtn_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    ::SendMessageW(findEdit_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    ::SendMessageW(findBtn_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    ::SendMessageW(closeBtn_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
    return true;
}

void BigFileView::Destroy() {
    if (model_) { model_->Close(); model_.reset(); }
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
}

void BigFileView::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
}

bool BigFileView::HasFocus() const {
    return render_ && ::GetFocus() == render_;
}

bool BigFileView::LoadFile(const std::wstring& path) {
    if (!model_) model_ = std::make_unique<BigFileModel>();
    model_->Close();
    topLine_ = 0;
    hChar_ = 0;
    if (!model_->Open(path)) {
        ::SetWindowTextW(label_, model_->Error().c_str());
        Logger::Error("BigFileView open failed: " + WideToUtf8(path) + " - " +
                      WideToUtf8(model_->Error()));
        return false;
    }
    Logger::Info("BigFileView opened bytes=" +
                 std::to_string(model_->FileSize()));
    EnsureFont();
    SyncScrollbars();
    SyncProgress();
    ::ShowWindow(hwnd_, SW_SHOW);
    UINT ms = ::SetTimer(hwnd_, kTimerId, 250, nullptr);
    (void)ms;
    return true;
}

void BigFileView::EnsureFont() {
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
    UINT dpi = render_ ? ::GetDpiForWindow(render_) : 96;
    int px = -MulDiv(10, (int)dpi, 72);
    font_ = ::CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    ::SendMessageW(render_, WM_SETFONT, (WPARAM)font_, TRUE);
    UpdateMetrics();
}

void BigFileView::UpdateMetrics() {
    if (!font_ || !render_) return;
    HDC hdc = ::GetDC(render_);
    HFONT old = (HFONT)::SelectObject(hdc, font_);
    TEXTMETRICW tm{};
    ::GetTextMetricsW(hdc, &tm);
    lineH_ = tm.tmHeight + tm.tmExternalLeading;
    if (lineH_ < 1) lineH_ = 1;
    SIZE sz{};
    ::GetTextExtentPoint32W(hdc, L"0", 1, &sz);
    charW_ = sz.cx ? sz.cx : 8;
    ::SelectObject(hdc, old);
    ::ReleaseDC(render_, hdc);
}

void BigFileView::SyncScrollbars() {
    if (!model_ || !render_) return;
    RECT rc;
    ::GetClientRect(render_, &rc);
    int vis = (std::max)(1, (int)((rc.bottom - rc.top) / lineH_));

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    si.nMin = 0;
    si.nMax = (int)std::min<unsigned long long>(model_->LineCount(), 0x7FFFFFF0);
    si.nPage = (UINT)vis;
    ::SetScrollInfo(render_, SB_VERT, &si, TRUE);

    unsigned long long longest = model_->LongestLine();
    SCROLLINFO sh{};
    sh.cbSize = sizeof(sh);
    sh.fMask = SIF_RANGE | SIF_PAGE;
    sh.nMin = 0;
    sh.nMax = (int)std::min<unsigned long long>(longest + 16, kMaxHChars);
    sh.nPage = (UINT)(std::max)(1, (int)((rc.right - rc.left - gutterW_) / charW_));
    ::SetScrollInfo(render_, SB_HORZ, &sh, TRUE);
}

void BigFileView::SyncProgress() {
    if (!model_) return;
    wchar_t buf[512];
    if (model_->FileSize() == 0) {
        swprintf_s(buf, L"%s — 0 " L"%s", model_->Path().c_str(),
                   Tr(L"bigfile.lines"));
    } else if (model_->ScanDone()) {
        swprintf_s(buf, L"%s — %llu %s", model_->Path().c_str(),
                   model_->LineCount(), Tr(L"bigfile.lines"));
    } else {
        swprintf_s(buf, L"%s — %llu %s · %s %d%%", model_->Path().c_str(),
                   model_->LineCount(), Tr(L"bigfile.lines"),
                   Tr(L"bigfile.scanning"),
                   (int)(model_->ScanProgress() * 100.0));
    }
    ::SetWindowTextW(label_, buf);
}

void BigFileView::SetTopLine(unsigned long long line) {
    if (!model_) return;
    unsigned long long count = std::max<unsigned long long>(model_->LineCount(), 1);
    if (line >= count) line = count ? count - 1 : 0;
    topLine_ = line;
    RECT rc;
    ::GetClientRect(render_, &rc);
    ::ScrollWindow(render_, 0, 0, nullptr, nullptr);
    ::InvalidateRect(render_, nullptr, FALSE);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = (int)std::min<unsigned long long>(topLine_, 0x7FFFFFF0);
    ::SetScrollInfo(render_, SB_VERT, &si, TRUE);
}

void BigFileView::Paint(HDC hdc) {
    if (!model_) return;
    RECT rc;
    ::GetClientRect(render_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int vis = h / lineH_ + 1;

    HFONT old = (HFONT)::SelectObject(hdc, font_);
    ::SetBkMode(hdc, TRANSPARENT);

    // gutter
    RECT gut = rc;
    gut.right = gutterW_;
    HBRUSH gutBrush = ::CreateSolidBrush(gutterBg_);
    ::FillRect(hdc, &gut, gutBrush);
    ::DeleteObject(gutBrush);
    ::SetTextColor(hdc, numFg_);

    unsigned long long count = model_->LineCount();
    int maxChars = (std::max)(1, (w - gutterW_) / charW_);
    int digits = 1;
    for (unsigned long long v = count; v >= 10; v /= 10) ++digits;
    wchar_t num[32];

    for (int r = 0; r < vis; ++r) {
        unsigned long long line = topLine_ + (unsigned long long)r;
        if (line >= count) break;
        int y = r * lineH_;

        swprintf_s(num, L"%llu", line + 1);
        SIZE nsz{};
        ::GetTextExtentPoint32W(hdc, num, (int)wcslen(num), &nsz);
        ::TextOutW(hdc, gutterW_ - nsz.cx - 6, y, num, (int)wcslen(num));

        std::string utf8;
        if (!model_->GetLine(line, utf8)) continue;
        std::wstring wide = ExpandTabs(Utf8ToWide(utf8));

        // horizontal clipping window
        if (hChar_ >= wide.size()) continue;
        const wchar_t* start = wide.c_str() + hChar_;
        size_t avail = wide.size() - hChar_;
        size_t draw = std::min<size_t>(avail, (size_t)maxChars + 1);

        RECT clip;
        clip.left = gutterW_;
        clip.top = y;
        clip.right = rc.right;
        clip.bottom = y + lineH_;
        ::SetTextColor(hdc, fg_);
        ::ExtTextOutW(hdc, gutterW_, y, ETO_CLIPPED, &clip, start, (UINT)draw, nullptr);
    }
    ::SelectObject(hdc, old);
}

void BigFileView::GotoLine() {
    wchar_t buf[32]{};
    ::GetWindowTextW(gotoEdit_, buf, 32);
    unsigned long long n = _wcstoui64(buf, nullptr, 10);
    if (n == 0) return;
    SetTopLine(n ? n - 1 : 0);
    ::SetFocus(render_);
}

void BigFileView::FindNext() {
    if (!model_) return;
    wchar_t buf[256]{};
    ::GetWindowTextW(findEdit_, buf, 256);
    std::wstring needle = Lower(buf);
    if (needle.empty()) return;

    unsigned long long count = model_->LineCount();
    for (unsigned long long i = topLine_ + 1; i < count; ++i) {
        std::string utf8;
        if (!model_->GetLine(i, utf8)) break;
        std::wstring wide = Lower(Utf8ToWide(utf8));
        if (wide.find(needle) != std::wstring::npos) {
            SetTopLine(i);
            ::SetFocus(render_);
            return;
        }
    }
    ::SetWindowTextW(label_, Tr(L"bigfile.notfound"));
}

void BigFileView::Layout(int w, int h) {
    (void)w;
    curH_ = h;
    // container WM_SIZE does the real child layout; just record height
}
void BigFileView::ApplyTheme(const ThemeDef& t) {
    bg_ = t.editorBg;
    fg_ = t.editorFg;
    numFg_ = t.lineNumFg;
    gutterBg_ = t.lineNumBack;
    if (hwnd_) ::InvalidateRect(render_, nullptr, TRUE);
}

void BigFileView::Retranslate() {
    if (!hwnd_) return;
    ::SetWindowTextW(gotoBtn_, Tr(L"bigfile.goto"));
    ::SetWindowTextW(findBtn_, Tr(L"bigfile.find"));
    ::SetWindowTextW(closeBtn_, Tr(L"bigfile.close"));
    SyncProgress();
}

LRESULT CALLBACK BigFileView::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (BigFileView*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->WndProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

LRESULT BigFileView::WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE: {
        int w = LOWORD(lp), hgt = HIWORD(lp);
        UINT dpi = ::GetDpiForWindow(h);
        int barH = MulDiv(30, (int)dpi, 96);
        int pad = MulDiv(4, (int)dpi, 96);
        int edH = MulDiv(24, (int)dpi, 96);
        int btnW = MulDiv(64, (int)dpi, 96);
        int edGW = MulDiv(80, (int)dpi, 96);
        int edFW = MulDiv(120, (int)dpi, 96);
        int x = pad;
        int y = (barH - edH) / 2;
        ::MoveWindow(label_, x, y, w - (btnW * 3 + edGW + edFW + pad * 7), edH, TRUE);
        x += w - (btnW * 3 + edGW + edFW + pad * 7) + pad;
        ::MoveWindow(gotoEdit_, x, y, edGW, edH, TRUE); x += edGW + pad;
        ::MoveWindow(gotoBtn_, x, y, btnW, edH, TRUE); x += btnW + pad;
        ::MoveWindow(findEdit_, x, y, edFW, edH, TRUE); x += edFW + pad;
        ::MoveWindow(findBtn_, x, y, btnW, edH, TRUE); x += btnW + pad;
        ::MoveWindow(closeBtn_, x, y, btnW, edH, TRUE);
        ::MoveWindow(render_, 0, barH, w, hgt - barH, TRUE);
        SyncScrollbars();
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerId) {
            if (!model_ || model_->ScanDone()) {
                ::KillTimer(h, kTimerId);
                SyncScrollbars();
                SyncProgress();
            } else {
                SyncScrollbars();
                SyncProgress();
            }
            ::InvalidateRect(render_, nullptr, FALSE);
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1) GotoLine();
        else if (id == 2) FindNext();
        else if (id == 3 && onClose) onClose();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.y < kSplitBand) {
            resizing_ = true;
            dragStartScreenY_ = pt.y;
            RECT rc; ::GetWindowRect(h, &rc);
            dragStartScreenY_ += rc.top;
            dragStartH_ = curH_;
            ::SetCapture(h);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (resizing_ && (wp & MK_LBUTTON)) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc; ::GetWindowRect(h, &rc);
            int screenY = pt.y + rc.top;
            int nh = dragStartH_ + (dragStartScreenY_ - screenY);
            nh = (std::max)(120, (std::min)(nh, 1400));
            if (nh != curH_ && onHeightChange) {
                curH_ = nh;
                onHeightChange(nh);
            }
            return 0;
        }
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        bool hot = pt.y < kSplitBand;
        if (hot != splitHot_) {
            splitHot_ = hot;
            ::SetCursor(::LoadCursorW(nullptr, hot ? IDC_SIZENS : IDC_ARROW));
        }
        break;
    }
    case WM_LBUTTONUP:
        if (resizing_) {
            resizing_ = false;
            ::ReleaseCapture();
            return 0;
        }
        break;
    case WM_SETCURSOR: {
        DWORD pos = ::GetMessagePos();
        POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
        ::ScreenToClient(h, &pt);
        if (pt.y < kSplitBand) {
            ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }
        break;
    }
    case WM_NCDESTROY:
        if (model_) { model_->Close(); model_.reset(); }
        hwnd_ = nullptr;
        return 0;
    }
    return ::DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK BigFileView::RenderProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (BigFileView*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->RenderProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

LRESULT BigFileView::RenderProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(h, &ps);
        RECT rc;
        ::GetClientRect(h, &rc);
        HBRUSH bg = ::CreateSolidBrush(bg_);
        ::FillRect(hdc, &rc, bg);
        ::DeleteObject(bg);
        Paint(hdc);
        ::EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_VSCROLL: {
        if (!model_) return 0;
        RECT rc;
        ::GetClientRect(h, &rc);
        int vis = (std::max)(1, (int)((rc.bottom - rc.top) / lineH_));
        unsigned long long newPos = topLine_;
        switch (LOWORD(wp)) {
        case SB_TOP: newPos = 0; break;
        case SB_BOTTOM:
            newPos = model_->LineCount() ? model_->LineCount() - 1 : 0; break;
        case SB_LINEUP: newPos = topLine_ ? topLine_ - 1 : 0; break;
        case SB_LINEDOWN: newPos = topLine_ + 1; break;
        case SB_PAGEUP: newPos = topLine_ >= (unsigned long long)vis ? topLine_ - vis : 0; break;
        case SB_PAGEDOWN: newPos = topLine_ + vis; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            ::GetScrollInfo(h, SB_VERT, &si);
            newPos = (unsigned long long)si.nTrackPos;
            break;
        }
        }
        SetTopLine(newPos);
        return 0;
    }
    case WM_HSCROLL: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        ::GetScrollInfo(h, SB_HORZ, &si);
        int delta = 8;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: hChar_ = hChar_ > (unsigned long long)delta ? hChar_ - delta : 0; break;
        case SB_LINERIGHT: hChar_ += delta; break;
        case SB_PAGELEFT: hChar_ = hChar_ > 40 ? hChar_ - 40 : 0; break;
        case SB_PAGERIGHT: hChar_ += 40; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: hChar_ = (unsigned long long)si.nTrackPos; break;
        default: return 0;
        }
        if (hChar_ > kMaxHChars) hChar_ = kMaxHChars;
        si.fMask = SIF_POS;
        si.nPos = (int)hChar_;
        ::SetScrollInfo(h, SB_HORZ, &si, TRUE);
        ::InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int notches = delta / WHEEL_DELTA;
        if (notches == 0) return 0;
        unsigned long long step = 3 * (unsigned long long)(notches < 0 ? -notches : notches);
        SetTopLine(notches < 0 ? topLine_ + step :
                   (topLine_ >= step ? topLine_ - step : 0));
        return 0;
    }
    case WM_KEYDOWN: {
        switch (wp) {
        case VK_UP: SetTopLine(topLine_ ? topLine_ - 1 : 0); return 0;
        case VK_DOWN: SetTopLine(topLine_ + 1); return 0;
        case VK_PRIOR: {
            RECT rc; ::GetClientRect(h, &rc);
            int vis = (std::max)(1, (int)((rc.bottom - rc.top) / lineH_));
            SetTopLine(topLine_ >= (unsigned long long)vis ? topLine_ - vis : 0);
            return 0;
        }
        case VK_NEXT: {
            RECT rc; ::GetClientRect(h, &rc);
            int vis = (std::max)(1, (int)((rc.bottom - rc.top) / lineH_));
            SetTopLine(topLine_ + (unsigned long long)vis);
            return 0;
        }
        case VK_HOME: SetTopLine(0); return 0;
        case VK_END:
            if (model_) SetTopLine(model_->LineCount() ? model_->LineCount() - 1 : 0);
            return 0;
        case 'G':
            if (::GetKeyState(VK_CONTROL) & 0x8000) {
                ::SetFocus(gotoEdit_);
                ::PostMessageW(gotoEdit_, EM_SETSEL, 0, -1);
                return 0;
            }
            break;
        case 'F':
            if (::GetKeyState(VK_CONTROL) & 0x8000) {
                ::SetFocus(findEdit_);
                ::PostMessageW(findEdit_, EM_SETSEL, 0, -1);
                return 0;
            }
            break;
        }
        break;
    }
    case WM_SETFOCUS:
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_DESTROY:
        return 0;
    }
    return ::DefWindowProcW(h, m, wp, lp);
}

} // namespace xfs
