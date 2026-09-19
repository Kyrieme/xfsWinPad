#include "CompilePanel.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>

namespace xfs {
namespace {
constexpr wchar_t kClass[] = L"xfsWinPadCompilePanel";
constexpr int ID_LIST  = 1310;
constexpr int ID_LABEL = 1311;
constexpr int ID_CLOSE = 1312;
constexpr int kTopBand = 6;      // splitter drag strip

// 列：步骤 / 位置 / 级别 / 输出（输出列在 LayoutChildren 里吃掉剩余宽度）
enum Col { kColStep = 0, kColWhere, kColLevel, kColText, kColCount };

// 级别列的取值：0 普通 / 1 警告 / 2 错误
enum Kind { kPlain = 0, kWarn = 1, kError = 2 };
} // namespace

bool CompilePanel::Create(HWND parent, HINSTANCE hInst) {
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
                              0, 0, 600, 220, parent,
                              (HMENU)(INT_PTR)1109 /*frame child id*/, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", Tr(L"panel.compile.title"),
                               WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 6, 4, 500, 18,
                               hwnd_, (HMENU)(INT_PTR)ID_LABEL, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.compile.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                  0, 2, 80, 22,
                                  hwnd_, (HMENU)(INT_PTR)ID_CLOSE, hInst, nullptr);
    list_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                              LVS_NOSORTHEADER,
                              6, 24, 588, 180,
                              hwnd_, (HMENU)(INT_PTR)ID_LIST, hInst, nullptr);
    if (list_) {
        ::SendMessageW(list_, WM_SETFONT, (WPARAM)font_, TRUE);
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = LVCFMT_LEFT;
        c.pszText = const_cast<LPWSTR>(Tr(L"panel.compile.col.step"));
        c.cx = 84;  ::SendMessageW(list_, LVM_INSERTCOLUMNW, kColStep, (LPARAM)&c);
        c.pszText = const_cast<LPWSTR>(Tr(L"panel.compile.col.where"));
        c.cx = 200; ::SendMessageW(list_, LVM_INSERTCOLUMNW, kColWhere, (LPARAM)&c);
        c.pszText = const_cast<LPWSTR>(Tr(L"panel.compile.col.level"));
        c.cx = 56;  ::SendMessageW(list_, LVM_INSERTCOLUMNW, kColLevel, (LPARAM)&c);
        c.pszText = const_cast<LPWSTR>(Tr(L"panel.compile.col.text"));
        c.cx = 380; ::SendMessageW(list_, LVM_INSERTCOLUMNW, kColText, (LPARAM)&c);
        DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
        ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, (LPARAM)ex);
    }
    if (label_) ::SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (closeBtn_) ::SendMessageW(closeBtn_, WM_SETFONT, (WPARAM)font_, TRUE);
    return true;
}

void CompilePanel::Destroy() {
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
    list_ = nullptr; label_ = nullptr; closeBtn_ = nullptr;
}

void CompilePanel::Show() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_SHOW);
}

void CompilePanel::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
}

void CompilePanel::Layout(int /*w*/, int /*h*/) {
    // 面板自己按客户区排（WM_SIZE 也走这里）；w/h 只为与其它底部面板同签名
    LayoutChildren();
}

void CompilePanel::Update(const std::wstring& summary, std::vector<CompileRow> rows) {
    rows_ = std::move(rows);
    hasRun_ = true;
    if (!hwnd_) return;
    ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    int i = 0;
    for (const CompileRow& r : rows_) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;

        // 步骤列**只在分隔行上填** —— 每一行都重复一遍步骤号纯属噪声，
        // 分隔行本身就是分节线。
        it.pszText = const_cast<LPWSTR>(r.header ? r.step.c_str() : L"");
        ::SendMessageW(list_, LVM_INSERTITEMW, i, (LPARAM)&it);

        std::wstring where;
        if (!r.header && !r.file.empty()) {
            where = r.file;
            if (r.line > 0) {
                where += L":";
                where += std::to_wstring(r.line);
                if (r.column > 0) {
                    where += L":";
                    where += std::to_wstring(r.column);
                }
            }
        }
        it.iSubItem = kColWhere;  it.pszText = const_cast<LPWSTR>(where.c_str());
        ::SendMessageW(list_, LVM_SETITEMW, i, (LPARAM)&it);

        const wchar_t* lv = L"";
        if (!r.header) {
            if (r.kind == kError) lv = Tr(L"panel.compile.level.error");
            else if (r.kind == kWarn) lv = Tr(L"panel.compile.level.warning");
        }
        it.iSubItem = kColLevel;  it.pszText = const_cast<LPWSTR>(lv);
        ::SendMessageW(list_, LVM_SETITEMW, i, (LPARAM)&it);

        it.iSubItem = kColText;   it.pszText = const_cast<LPWSTR>(r.text.c_str());
        ::SendMessageW(list_, LVM_SETITEMW, i, (LPARAM)&it);
        ++i;
    }
    ::SetWindowTextW(label_, summary.c_str());
    Show();
}

void CompilePanel::ShowBusy(const std::wstring& text) {
    if (!hwnd_) return;
    rows_.clear();
    ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    ::SetWindowTextW(label_, text.c_str());
    Show();
}

void CompilePanel::Clear() {
    rows_.clear();
    hasRun_ = false;
    if (!hwnd_) return;
    ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    ::SetWindowTextW(label_, Tr(L"panel.compile.title"));
    Hide();
}

void CompilePanel::Retranslate() {
    if (closeBtn_) ::SetWindowTextW(closeBtn_, Tr(L"panel.compile.close"));
    if (label_ && !hasRun_) ::SetWindowTextW(label_, Tr(L"panel.compile.title"));
    if (!list_) return;
    static const wchar_t* keys[kColCount] = {
        L"panel.compile.col.step", L"panel.compile.col.where",
        L"panel.compile.col.level", L"panel.compile.col.text",
    };
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT;
    for (int i = 0; i < kColCount; ++i) {
        c.pszText = const_cast<LPWSTR>(Tr(keys[i]));
        ::SendMessageW(list_, LVM_SETCOLUMNW, i, (LPARAM)&c);
    }
    // 级别列的文案会随语言变（"错误"/"warning"），已填的行也要重刷一遍
    if (hasRun_) {
        const std::wstring summary = [&] {
            wchar_t buf[512] = {0};
            ::GetWindowTextW(label_, buf, 512);
            return std::wstring(buf);
        }();
        Update(summary, rows_);
    }
}

void CompilePanel::LayoutChildren() {
    if (!hwnd_) return;
    RECT rc; ::GetClientRect(hwnd_, &rc);
    int dpi = ::GetDpiForWindow(hwnd_);
    int pad = MulDiv(6, dpi, 96);
    int labelH = MulDiv(20, dpi, 96);
    int closeW = MulDiv(84, dpi, 96);
    if (closeBtn_)
        ::MoveWindow(closeBtn_, rc.right - closeW - pad, kTopBand + 3, closeW, labelH + 2, TRUE);
    if (label_)
        ::MoveWindow(label_, pad, kTopBand + 4, rc.right - closeW - 3 * pad, labelH, TRUE);
    if (list_) {
        const int listW = rc.right - 2 * pad;
        ::MoveWindow(list_, pad, kTopBand + 4 + labelH + 2,
                     listW, rc.bottom - labelH - 10 - kTopBand, TRUE);
        // 输出列吃掉剩余宽度（最后一列不会自动延伸，必须自己算）
        int used = 0;
        for (int i = 0; i < kColText; ++i)
            used += (int)::SendMessageW(list_, LVM_GETCOLUMNWIDTH, i, 0);
        const int rest = listW - used - MulDiv(GetSystemMetrics(SM_CXVSCROLL), dpi, 96)
                                 - MulDiv(4, dpi, 96);
        if (rest > MulDiv(120, dpi, 96))
            ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, kColText, rest);
    }
}

LRESULT CALLBACK CompilePanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    auto* self = (CompilePanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->Handle(m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

LRESULT CompilePanel::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETCURSOR: {
            POINT pt; ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd_, &pt);
            if (pt.y < kTopBand) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.y < kTopBand) {
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
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool hot = pt.y < kTopBand;
            if (hot) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
                TrackMouseEvent(&tme);
            }
            if (hot != splitHot_) {
                splitHot_ = hot;
                RECT rc; ::GetClientRect(hwnd_, &rc);
                rc.bottom = kTopBand;
                ::InvalidateRect(hwnd_, &rc, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (splitHot_) {
                splitHot_ = false;
                RECT rc; ::GetClientRect(hwnd_, &rc);
                rc.bottom = kTopBand;
                ::InvalidateRect(hwnd_, &rc, FALSE);
            }
            break;
        case WM_LBUTTONUP:
            if (resizing_) { resizing_ = false; ::ReleaseCapture(); return 0; }
            break;
        case WM_CAPTURECHANGED:
            resizing_ = false;
            break;
        case WM_PAINT: {
            ::DefWindowProcW(hwnd_, msg, wp, lp);
            HDC dc = ::GetDC(hwnd_);
            RECT rc; ::GetClientRect(hwnd_, &rc);
            RECT band{0, 0, rc.right, kTopBand - 1};
            HBRUSH b = ::CreateSolidBrush(splitHot_ || resizing_
                ? RGB(0xCC, 0xE3, 0xFF) : RGB(0xE9, 0xE9, 0xE9));
            ::FillRect(dc, &band, b);
            ::DeleteObject(b);
            HPEN pen = ::CreatePen(PS_SOLID, 1, splitHot_ || resizing_
                ? RGB(0x00, 0x78, 0xD4) : RGB(0xAC, 0xAC, 0xAC));
            HPEN old = (HPEN)::SelectObject(dc, pen);
            ::MoveToEx(dc, 0, kTopBand - 1, nullptr);
            ::LineTo(dc, rc.right, kTopBand - 1);
            ::SelectObject(dc, old);
            ::DeleteObject(pen);
            ::ReleaseDC(hwnd_, dc);
            return 0;
        }
        case WM_SIZE:
            LayoutChildren();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == ID_CLOSE && onClose) { onClose(); return 0; }
            break;
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (nm->idFrom != ID_LIST) break;
            if (nm->code == NM_DBLCLK) {
                NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lp;
                // 只对**可跳转**的行派发：认不出位置的行双击不动，也不弹提示
                // —— 用户从"没反应"就能明白这一行没有位置信息。
                if (ia->iItem >= 0 && ia->iItem < (int)rows_.size() &&
                    rows_[ia->iItem].Jumpable()) {
                    Logger::Info("CompilePanel double-click row=" +
                                 std::to_string(ia->iItem));
                    if (onActivateRow) onActivateRow(ia->iItem);
                }
                return 0;
            }
            if (nm->code == NM_CUSTOMDRAW) {
                // 级别列上色（错误红 / 警告橙）；分隔行整行淡蓝，一眼区分
                // "这是我们插的分节线" vs "这是编译器的输出"。
                NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                    return CDRF_NOTIFYSUBITEMDRAW;
                if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                    const int row = (int)cd->nmcd.dwItemSpec;
                    if (row >= 0 && row < (int)rows_.size()) {
                        const CompileRow& r = rows_[row];
                        if (r.header) {
                            cd->clrText = RGB(0x1F, 0x5F, 0x9F);
                        } else if (cd->iSubItem == kColLevel) {
                            if (r.kind == kError)      cd->clrText = RGB(0xC0, 0x20, 0x20);
                            else if (r.kind == kWarn)  cd->clrText = RGB(0xA0, 0x58, 0x00);
                        }
                    }
                    return CDRF_DODEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            break;
        }
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace xfs
