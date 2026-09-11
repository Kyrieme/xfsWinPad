#include "CommandPalette.h"
#include "../core/CommandIds.h"
#include "../core/Log.h"
#include "../core/I18n.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <string>
#include <vector>

namespace xfs {

namespace {

constexpr wchar_t kClass[] = L"xfsWinPadCmdPal";
constexpr WORD IDC_EDIT = 1300;
constexpr WORD IDC_LIST = 1301;
constexpr int LIST_VISIBLE_ROWS = 12;

// command registry for the palette (label-id, cmd); labels resolve via Tr()
// at every Refill so a language switch restyles the list without a rebuild.
using Item = CommandPalette::Item;
const Item kItems[] = {
    {L"pal.file.new",              Cmd::FileNew},
    {L"pal.file.open",             Cmd::FileOpen},
    {L"pal.file.save",             Cmd::FileSave},
    {L"pal.file.saveas",           Cmd::FileSaveAs},
    {L"pal.file.saveall",          Cmd::FileSaveAll},
    {L"pal.file.reload",           Cmd::FileReload},
    {L"pal.file.close",            Cmd::FileClose},
    {L"pal.file.closeall",         Cmd::FileCloseAll},
    {L"pal.edit.undo",             Cmd::EditUndo},
    {L"pal.edit.redo",             Cmd::EditRedo},
    {L"pal.edit.cut",              Cmd::EditCut},
    {L"pal.edit.copy",             Cmd::EditCopy},
    {L"pal.edit.paste",            Cmd::EditPaste},
    {L"pal.edit.selectall",        Cmd::EditSelectAll},
    {L"pal.edit.datetime",         Cmd::EditTimeDate},
    {L"pal.edit.upper",            Cmd::EditUpperCase},
    {L"pal.edit.lower",            Cmd::EditLowerCase},
    {L"pal.edit.comment",          Cmd::EditToggleComment},
    {L"pal.edit.join",             Cmd::EditJoinLines},
    {L"pal.edit.split",            Cmd::EditSplitLines},
    {L"pal.edit.removeempty",      Cmd::EditRemoveEmptyLines},
    {L"pal.edit.reverse",          Cmd::EditReverseLines},
    {L"pal.search.find",           Cmd::SearchFind},
    {L"pal.search.replace",        Cmd::SearchReplace},
    {L"pal.search.findnext",       Cmd::SearchFindNext},
    {L"pal.search.findprev",       Cmd::SearchFindPrev},
    {L"pal.search.gotoline",       Cmd::SearchGotoLine},
    {L"pal.view.wrap",             Cmd::ViewWordWrap},
    {L"pal.view.terminal",         Cmd::ViewTerminal},
    {L"pal.view.zoomin",           Cmd::ViewZoomIn},
    {L"pal.view.zoomout",          Cmd::ViewZoomOut},
    {L"pal.view.zoomreset",        Cmd::ViewZoomReset},
};
constexpr int kItemCount = sizeof(kItems) / sizeof(kItems[0]);

HFONT g_palFont = nullptr;

HFONT PalFont(HWND ref) {
    if (!g_palFont) {
        int dpi = ::GetDpiForWindow(ref);
        g_palFont = ::CreateFontW(-MulDiv(10, dpi, 96), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                  L"Segoe UI");
    }
    return g_palFont;
}

bool ContainsCI(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return hay.find(needle) != std::wstring::npos;   // CJK exact-substring
}

} // namespace

void CommandPalette::SetExtraItems(const std::vector<Item>& items) {
    extraItems_ = items;
    // copy all labels into owned storage so the wchar_t* stay valid
    extraLabels_.resize(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        extraLabels_[i] = items[i].label;
        extraItems_[i].label = extraLabels_[i].c_str();
    }
}

void CommandPalette::Refill() {
    wchar_t buf[256] = L"";
    ::GetWindowTextW(edit_, buf, 256);
    std::wstring filter(buf);

    visible_.clear();
    for (const Item& it : kItems) {
        std::wstring label(Tr(it.label));
        if (ContainsCI(label, filter)) visible_.push_back(it);
    }
    for (const Item& it : extraItems_) {
        std::wstring label(it.label);
        if (ContainsCI(label, filter)) visible_.push_back(it);
    }

    ::SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (const Item& it : visible_) {
        int idx = (int)::SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)Tr(it.label));
        ::SendMessageW(list_, LB_SETITEMDATA, idx, (LPARAM)(INT_PTR)it.cmd);
    }
    if (!visible_.empty())
        ::SendMessageW(list_, LB_SETCURSEL, 0, 0);
}

void CommandPalette::ExecuteSelected() {
    int sel = (int)::SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)visible_.size()) sel = 0;
    if (visible_.empty()) { Close(); return; }
    unsigned cmd = visible_[sel].cmd;
    Logger::Info("Palette execute cmd=" + std::to_string(cmd));
    Close();
    if (onExecute) onExecute(cmd);
}

LRESULT CALLBACK PalWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (CommandPalette*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = (CommandPalette*)cs->lpCreateParams;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            ::SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)self->parent_);
            return 0;
        }
        case WM_SIZE: {
            RECT rc; ::GetClientRect(h, &rc);
            int dpi = ::GetDpiForWindow(h);
            int pad = MulDiv(8, dpi, 96);
            int editH = MulDiv(26, dpi, 96);
            if (self->edit_)
                ::MoveWindow(self->edit_, pad, pad,
                             rc.right - 2 * pad, editH, TRUE);
            if (self->list_)
                ::MoveWindow(self->list_, pad, editH + pad + MulDiv(4, dpi, 96),
                             rc.right - 2 * pad,
                             rc.bottom - editH - 3 * pad - MulDiv(4, dpi, 96), TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (!self) break;
            if (LOWORD(wp) == IDC_EDIT && HIWORD(wp) == EN_CHANGE) {
                self->Refill();
                return 0;
            }
            if (LOWORD(wp) == IDC_LIST && HIWORD(wp) == LBN_DBLCLK) {
                self->ExecuteSelected();
                return 0;
            }
            break;
        case WM_CLOSE:
            if (self) self->Close();
            return 0;
        case WM_NCDESTROY:
            if (self) self->hwnd_ = nullptr;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK PalEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR, DWORD_PTR ref) {
    auto* self = (CommandPalette*)ref;
    switch (msg) {
        case WM_KEYDOWN:
            if (!self) break;
            switch (wp) {
                case VK_RETURN:  self->ExecuteSelected(); return 0;
                case VK_ESCAPE:  self->Close();           return 0;
                case VK_DOWN: {
                    int n = (int)::SendMessageW(self->list_, LB_GETCOUNT, 0, 0);
                    int cur = (int)::SendMessageW(self->list_, LB_GETCURSEL, 0, 0);
                    if (n > 0 && cur < n - 1)
                        ::SendMessageW(self->list_, LB_SETCURSEL, cur + 1, 0);
                    return 0;
                }
                case VK_UP: {
                    int cur = (int)::SendMessageW(self->list_, LB_GETCURSEL, 0, 0);
                    if (cur > 0)
                        ::SendMessageW(self->list_, LB_SETCURSEL, cur - 1, 0);
                    return 0;
                }
            }
            break;
        case WM_CHAR:
            if (wp == L'\r' && self) { self->ExecuteSelected(); return 0; }
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void CommandPalette::Show(HWND parent, HINSTANCE hInst) {
    if (hwnd_) { ::SetForegroundWindow(hwnd_); return; }
    parent_ = parent;
    inst_ = hInst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PalWndProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        if (!::RegisterClassExW(&wc)) return;
        registered = true;
    }

    int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return MulDiv(px, dpi, 96); };
    RECT wr{}; ::GetWindowRect(parent, &wr);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, u(430), u(318)};
    ::AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);
    int x = wr.left + ((wr.right - wr.left) - (rc.right - rc.left)) / 2;
    int y = wr.top + u(60);

    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                              kClass, Tr(L"pal.title"),
                              style, x, y, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, hInst, this);
    if (!hwnd_) { hwnd_ = nullptr; return; }
    font_ = PalFont(hwnd_);

    edit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
        u(8), u(8), u(406), u(24), hwnd_, (HMENU)(UINT_PTR)IDC_EDIT, hInst, nullptr);
    list_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS |
        LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
        u(8), u(38), u(406), u(240), hwnd_, (HMENU)(UINT_PTR)IDC_LIST, hInst, nullptr);
    ::SendMessageW(edit_, WM_SETFONT, (WPARAM)font_, TRUE);
    ::SendMessageW(list_, WM_SETFONT, (WPARAM)font_, TRUE);

    // arrow/enter/esc handling for the edit box
    ::SetWindowSubclass(edit_, PalEditProc, 3, (DWORD_PTR)this);

    Refill();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(edit_);
}

void CommandPalette::Close() {
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        ::DestroyWindow(h);
    }
}

} // namespace xfs
