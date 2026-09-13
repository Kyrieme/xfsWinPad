#include "InputBox.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <windowsx.h>

namespace xfs {

namespace {

constexpr wchar_t kInputClass[] = L"xfsWinPadInputBox";
constexpr int IDC_INPUT_EDIT = 3001;

struct InputState {
    std::wstring label;
    std::wstring initial;
    std::wstring result;
    bool ok = false;
};

LRESULT CALLBACK InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (InputState*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    wchar_t buf[2048]{};
                    ::GetWindowTextW(::GetDlgItem(h, IDC_INPUT_EDIT), buf, 2048);
                    st->result = buf;
                    st->ok = true;
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
                    return 0;
                }
                case IDCANCEL:
                    st->ok = false;
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
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

int TextWidth(HFONT font, const std::wstring& s) {
    HDC dc = ::GetDC(nullptr);
    HFONT old = (HFONT)::SelectObject(dc, font);
    SIZE sz{};
    ::GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    ::SelectObject(dc, old);
    ::ReleaseDC(nullptr, dc);
    return sz.cx;
}

} // namespace

bool InputBox(HWND parent, HINSTANCE hInst, const std::wstring& title,
              const std::wstring& label, std::wstring& value, bool multiline) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InputProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kInputClass;
    ::RegisterClassExW(&wc);

    int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return MulDiv(px, dpi, 96); };

    InputState st;
    st.label = label;
    st.initial = value;
    st.result = value;

    HFONT font = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Segoe UI");

    RECT wr{}; ::GetWindowRect(parent, &wr);
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    int contentW = TextWidth(font, label) + u(24);
    if (contentW < u(300)) contentW = u(300);
    if (contentW > u(520)) contentW = u(520);
    int contentH = multiline ? u(180) : u(108);
    RECT rc{0, 0, contentW, contentH};
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    int x = wr.left + ((wr.right - wr.left) - (rc.right - rc.left)) / 2;
    int y = wr.top + u(90);

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kInputClass, title.c_str(),
                                 gstyle, x, y, rc.right - rc.left, rc.bottom - rc.top,
                                 parent, nullptr, hInst, &st);
    if (!dlg) { ::DeleteObject(font); return false; }

    int innerW = contentW - u(24);
    HWND lbl = ::CreateWindowExW(0, L"STATIC", label.c_str(),
        WS_CHILD | WS_VISIBLE, u(12), u(14), innerW, u(16),
        dlg, (HMENU)(UINT_PTR)3002, hInst, nullptr);
    ::SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
    DWORD estyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (multiline
        ? (ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL)
        : (ES_AUTOHSCROLL | ES_LEFT));
    int editH = multiline ? u(94) : u(22);
    HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        estyle,
        u(12), u(34), innerW, editH, dlg, (HMENU)(UINT_PTR)IDC_INPUT_EDIT, hInst, nullptr);
    ::SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    int btnY = multiline ? u(136) : u(66);
    struct Bt { const wchar_t* t; WORD id; int x; };
    const Bt bs[] = {{Tr(L"input.ok"), IDOK, contentW - u(150)},
                     {Tr(L"input.cancel"), IDCANCEL, contentW - u(80)}};
    for (const Bt& b : bs) {
        HWND bb = ::CreateWindowExW(0, L"BUTTON", b.t,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            b.x, btnY, u(66), u(24), dlg, (HMENU)(UINT_PTR)b.id, hInst, nullptr);
        ::SendMessageW(bb, WM_SETFONT, (WPARAM)font, TRUE);
    }
    ::SetWindowTextW(edit, st.initial.c_str());
    ::SendMessageW(edit, EM_SETSEL, 0, -1);

    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);
    ::SetForegroundWindow(dlg);
    ::SetFocus(edit);

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
    value = st.result;
    return true;
}

} // namespace xfs
