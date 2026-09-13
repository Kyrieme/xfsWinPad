#include "ListPicker.h"
#include "../core/I18n.h"
#include <windowsx.h>

namespace xfs {

namespace {

constexpr wchar_t kPickerClass[] = L"xfsWinPadListPicker";
constexpr int IDC_PICKER_LIST = 3010;

struct PickerState {
    std::vector<std::wstring> items;
    int currentSel = -1;
    int result = -1;
    bool ok = false;
};

LRESULT CALLBACK PickerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (PickerState*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    int sel = (int)::SendMessageW(::GetDlgItem(h, IDC_PICKER_LIST),
                                                  LB_GETCURSEL, 0, 0);
                    if (sel < 0) return 0;
                    st->result = sel;
                    st->ok = true;
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
                    return 0;
                }
                case IDCANCEL:
                    st->ok = false;
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
                    return 0;
                case IDC_PICKER_LIST:
                    if (HIWORD(wp) == LBN_DBLCLK)
                        ::SendMessageW(h, WM_COMMAND, IDOK, 0);
                    return 0;
            }
            break;
        case WM_CLOSE:
            ::EnableWindow(::GetWindow(h, GW_OWNER), TRUE);
            ::SetForegroundWindow(::GetWindow(h, GW_OWNER));
            ::DestroyWindow(h);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

bool ListPicker(HWND parent, HINSTANCE hInst, const std::wstring& title,
                const std::wstring& label,
                const std::vector<std::wstring>& items,
                int currentSel, int& outSel) {
    outSel = -1;
    if (items.empty()) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PickerProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPickerClass;
    ::RegisterClassExW(&wc);

    int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return MulDiv(px, dpi, 96); };

    PickerState st;
    st.items = items;
    st.currentSel = currentSel;

    HFONT font = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Segoe UI");

    RECT wr{}; ::GetWindowRect(parent, &wr);
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    int contentW = u(340);
    int listH = u(200);
    int contentH = u(34) + listH + u(40);
    RECT rc{0, 0, contentW, contentH};
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    int x = wr.left + ((wr.right - wr.left) - (rc.right - rc.left)) / 2;
    int y = wr.top + u(90);

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kPickerClass, title.c_str(),
                                 gstyle, x, y, rc.right - rc.left, rc.bottom - rc.top,
                                 parent, nullptr, hInst, &st);
    if (!dlg) { ::DeleteObject(font); return false; }

    int innerW = contentW - u(24);
    HWND lbl = ::CreateWindowExW(0, L"STATIC", label.c_str(),
        WS_CHILD | WS_VISIBLE, u(12), u(10), innerW, u(16),
        dlg, (HMENU)(UINT_PTR)3011, hInst, nullptr);
    ::SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
    HWND list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            LBS_NOTIFY | LBS_HASSTRINGS,
        u(12), u(30), innerW, listH, dlg,
        (HMENU)(UINT_PTR)IDC_PICKER_LIST, hInst, nullptr);
    ::SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
    for (const std::wstring& s : st.items)
        ::SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    if (st.currentSel >= 0 && st.currentSel < (int)st.items.size())
        ::SendMessageW(list, LB_SETCURSEL, st.currentSel, 0);
    else
        ::SendMessageW(list, LB_SETCURSEL, 0, 0);

    int btnY = u(30) + listH + u(8);
    struct Bt { const wchar_t* t; WORD id; int x; };
    const Bt bs[] = {{Tr(L"input.ok"), IDOK, contentW - u(150)},
                     {Tr(L"input.cancel"), IDCANCEL, contentW - u(80)}};
    for (const Bt& b : bs) {
        HWND bb = ::CreateWindowExW(0, L"BUTTON", b.t,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            b.x, btnY, u(66), u(24), dlg, (HMENU)(UINT_PTR)b.id, hInst, nullptr);
        ::SendMessageW(bb, WM_SETFONT, (WPARAM)font, TRUE);
    }

    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);
    ::SetForegroundWindow(dlg);
    ::SetFocus(list);

    MSG m;
    while (::IsWindow(dlg) && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &m)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    ::EnableWindow(parent, TRUE);
    ::DeleteObject(font);

    if (!st.ok) return false;
    outSel = st.result;
    return true;
}

} // namespace xfs
