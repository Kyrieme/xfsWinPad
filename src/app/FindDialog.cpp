#include "FindDialog.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace xfs {

// ---- control ids -------------------------------------------------------------
constexpr WORD IDC_FIND_TEXT     = 1100;  // find page combo
constexpr WORD IDC_FIND_TEXT_R   = 1130;  // replace page find combo
constexpr WORD IDC_REPLACE_TEXT  = 1110;
constexpr WORD IDC_MATCH_CASE    = 1101;
constexpr WORD IDC_WHOLE_WORD    = 1102;
constexpr WORD IDC_TABS          = 1120;

// find page buttons
constexpr WORD IDC_BTN_NEXT      = IDOK;  // 1
constexpr WORD IDC_BTN_PREV      = 1104;
constexpr WORD IDC_BTN_COUNT     = 1113;
constexpr WORD IDC_BTN_ALLCUR    = 1114;
constexpr WORD IDC_BTN_ALLOPEN   = 1115;
// replace page buttons
constexpr WORD IDC_BTN_REPLACE   = 1111;
constexpr WORD IDC_BTN_REPLALL   = 1112;
constexpr WORD IDC_BTN_REPLOPEN  = 1116;
// 文件中查找 (page 2)
constexpr WORD IDC_DIR_EDIT      = 1140;
constexpr WORD IDC_DIR_BROWSE    = 1141;
constexpr WORD IDC_FIF_FILTERS   = 1142;
constexpr WORD IDC_FIND_TEXT_FIF = 1144;
constexpr WORD IDC_BTN_FIF_FIND  = 1145;
constexpr WORD IDC_FIF_SUB       = 1146;
constexpr WORD IDC_FIF_REPLACE   = 1147;
constexpr WORD IDC_BTN_FIF_REPL  = 1148;
constexpr WORD IDC_REGEXP        = 1103;
// 项目中查找 (page 3)
constexpr WORD IDC_PROJ_FILTERS  = 1150;
constexpr WORD IDC_FIND_TEXT_PRJ = 1151;
constexpr WORD IDC_BTN_PROJ_FIND = 1152;
// 标记 (page 4)
constexpr WORD IDC_MARK_TEXT     = 1160;
constexpr WORD IDC_MARK_OPEN     = 1161;
constexpr WORD IDC_MARK_BOOKMARK = 1162;
constexpr WORD IDC_BTN_MARK_ALL  = 1164;
constexpr WORD IDC_BTN_MARK_CLR  = 1165;

constexpr wchar_t kDlgClass[] = L"xfsWinPadFindDlg";

namespace {

HFONT g_dlgFont = nullptr;

HFONT DlgFont(HWND refWindow) {
    if (!g_dlgFont) {
        int dpi = ::GetDpiForWindow(refWindow);
        g_dlgFont = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                  L"Segoe UI");
    }
    return g_dlgFont;
}

HWND MakeControl(HWND parent, HINSTANCE inst, const wchar_t* cls, const wchar_t* text,
                 DWORD style, DWORD exStyle, int x, int y, int w, int h, WORD id, HFONT font) {
    // WS_VISIBLE here is safe: the dialog itself is still hidden during
    // construction, and SwitchPage() hides inactive pages before it is shown.
    HWND c = ::CreateWindowExW(exStyle, cls, text,
                               WS_CHILD | WS_VISIBLE | style | WS_CLIPSIBLINGS,
                               x, y, w, h, parent,
                               (HMENU)(UINT_PTR)id, inst, nullptr);
    if (c) ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

struct BtnDef { const wchar_t* t; WORD id; int y; bool def; };

void MakeButtons(HWND parent, HINSTANCE inst, const BtnDef* btns, size_t n,
                 int x, int w, int h, HFONT font) {
    for (size_t i = 0; i < n; ++i)
        MakeControl(parent, inst, L"BUTTON", btns[i].t,
                    BS_PUSHBUTTON | (btns[i].def ? BS_DEFPUSHBUTTON : 0) | WS_TABSTOP,
                    0, x, btns[i].y, w, h, btns[i].id, font);
}

WORD FocusComboFor(int page) {
    switch (page) {
        case 0:  return IDC_FIND_TEXT;
        case 1:  return IDC_FIND_TEXT_R;
        case 2:  return IDC_FIND_TEXT_FIF;
        case 3:  return IDC_FIND_TEXT_PRJ;
        default: return IDC_MARK_TEXT;
    }
}

void BrowseForFolder(HWND owner, HWND targetEdit) {
    IFileDialog* dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IFileDialog, (void**)&dlg);
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    ::SetWindowTextW(targetEdit, path);
                    ::CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
}

} // namespace

// ------------------------------------------------------------------------------

LRESULT CALLBACK FindDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (FindDialog*)::GetWindowLongPtrW(hdlg, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = (FindDialog*)cs->lpCreateParams;
            ::SetWindowLongPtrW(hdlg, GWLP_USERDATA, (LONG_PTR)self);
            ::SetWindowLongPtrW(hdlg, GWLP_HWNDPARENT, (LONG_PTR)self->parent_);
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (nm->idFrom == IDC_TABS && nm->code == TCN_SELCHANGE && self) {
                int sel = (int)::SendMessageW(nm->hwndFrom, TCM_GETCURSEL, 0, 0);
                self->SwitchPage(sel);
                return 0;
            }
            break;
        }
        case WM_APP_SETPAGE:
            if (self) self->SwitchPage((int)wp);
            return 0;
        case WM_COMMAND:
            if (!self) break;
            switch (LOWORD(wp)) {
                case IDC_MATCH_CASE:
                case IDC_WHOLE_WORD:
                    self->ApplyFromControls();
                    return 0;
                case IDC_BTN_NEXT:    self->ApplyFromControls(); self->Request(SearchAction::FindNext);       return 0;
                case IDC_BTN_PREV:    self->ApplyFromControls(); self->Request(SearchAction::FindPrev);       return 0;
                case IDC_BTN_COUNT:   self->ApplyFromControls(); self->Request(SearchAction::CountCurrent);   return 0;
                case IDC_BTN_ALLCUR:  self->ApplyFromControls(); self->Request(SearchAction::FindAllCurrent); return 0;
                case IDC_BTN_ALLOPEN: self->ApplyFromControls(); self->Request(SearchAction::FindAllOpen);    return 0;
                case IDC_BTN_REPLACE: self->ApplyFromControls(); self->Request(SearchAction::Replace);        return 0;
                case IDC_BTN_REPLALL: self->ApplyFromControls(); self->Request(SearchAction::ReplaceAll);     return 0;
                case IDC_BTN_REPLOPEN:self->ApplyFromControls(); self->Request(SearchAction::ReplaceAllOpen); return 0;
                case IDC_DIR_BROWSE:  self->BrowseFolder();                                                   return 0;
                case IDC_BTN_FIF_FIND:
                    self->ApplyFromControls(); self->Request(SearchAction::FindInFiles);    return 0;
                case IDC_BTN_FIF_REPL:
                    self->ApplyFromControls(); self->Request(SearchAction::ReplaceInFiles); return 0;
                case IDC_REGEXP:
                    self->ApplyFromControls(); return 0;
                case IDC_BTN_PROJ_FIND:
                    self->ApplyFromControls(); self->Request(SearchAction::FindInProjects); return 0;
                case IDC_BTN_MARK_ALL:
                    self->ApplyFromControls(); self->Request(SearchAction::MarkAll);        return 0;
                case IDC_BTN_MARK_CLR:
                    self->ApplyFromControls(); self->Request(SearchAction::MarkClear);      return 0;
                case IDCANCEL:
                    self->Close();
                    return 0;
            }
            break;

        case WM_CLOSE:
            if (self) self->Close();
            return 0;

        case WM_NCDESTROY:
            if (self) self->hwnd_ = nullptr;
            ::SetWindowLongPtrW(hdlg, GWLP_USERDATA, 0);
            return 0;
    }
    return ::DefWindowProcW(hdlg, msg, wp, lp);
}

void FindDialog::Show(HWND parent, HINSTANCE hInst, int pageIndex) {
    if (hwnd_) {
        ::SetForegroundWindow(hwnd_);
        SwitchPage(pageIndex);
        return;
    }
    parent_ = parent;
    inst_ = hInst;

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FindDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDlgClass;
        if (!::RegisterClassExW(&wc)) return;
        classRegistered = true;
    }

    int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return MulDiv(px, dpi, 96); };
    HFONT font = DlgFont(parent);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    RECT rc{0, 0, u(500), u(248)};
    ::AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    // center over the editor window itself: SPI_GETWORKAREA always returns
    // the primary monitor, which dumped the dialog on the wrong screen for
    // multi-monitor users (2026-09-15 report). Clamp into the monitor the
    // parent lives on so it stays fully visible when the parent is maximized
    // or partly off-screen.
    const int dlgW = rc.right - rc.left;
    const int dlgH = rc.bottom - rc.top;
    RECT pr{};
    ::GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - dlgW) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - dlgH) / 2;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(::MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT wa = mi.rcWork;
        if (x + dlgW > wa.right)  x = wa.right - dlgW;
        if (x < wa.left)          x = wa.left;
        if (y + dlgH > wa.bottom) y = wa.bottom - dlgH;
        if (y < wa.top)           y = wa.top;
    }

    hwnd_ = ::CreateWindowExW(exStyle | WS_EX_CONTROLPARENT, kDlgClass, Tr(L"find.title"),
                              style, x, y, dlgW, dlgH,
                              nullptr, nullptr, hInst, this);
    if (!hwnd_) { hwnd_ = nullptr; return; }

    // tab strip — five pages like Notepad++.
    // NOTE: created here but pushed to the BOTTOM of the child Z-order right
    // before the dialog is shown (child windows enter the stack at the bottom,
    // so an early move would leave later controls underneath it).
    HWND tabs = MakeControl(hwnd_, inst_, WC_TABCONTROLW, L"",
                            WS_TABSTOP | WS_CLIPSIBLINGS, 0,
                            u(6), u(6), u(488), u(202), IDC_TABS, font);
    const wchar_t* kPages[] = { Tr(L"find.tab1"), Tr(L"find.tab2"), Tr(L"find.tab3"),
                                Tr(L"find.tab4"), Tr(L"find.tab5") };
    for (int i = 0; i < 5; ++i) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(kPages[i]);
        ::SendMessageW(tabs, TCM_INSERTITEMW, i, (LPARAM)&ti);
    }

    // ---------------- Page 0: 查找 ----------------
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.what"), 0, 0, u(16), u(42), u(74), u(14), 1001, font);
    MakeControl(hwnd_, inst_, L"COMBOBOX", L"",
        CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL | WS_GROUP,
        WS_EX_CLIENTEDGE, u(96), u(38), u(216), u(150), IDC_FIND_TEXT, font);
    const BtnDef fbtns[] = {
        {Tr(L"find.next"), IDC_BTN_NEXT, u(36), true},
        {Tr(L"find.prev"), IDC_BTN_PREV, u(66), false},
        {Tr(L"find.count"), IDC_BTN_COUNT, u(96), false},
        {Tr(L"find.allcur"), IDC_BTN_ALLCUR, u(126), false},
        {Tr(L"find.allopen"), IDC_BTN_ALLOPEN, u(156), false},
    };
    MakeButtons(hwnd_, inst_, fbtns, 5, u(326), u(106), u(25), font);

    // ---------------- Page 1: 替换 ----------------
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.what"), 0, 0, u(16), u(42), u(74), u(14), 1003, font);
    MakeControl(hwnd_, inst_, L"COMBOBOX", L"",
        CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL,
        WS_EX_CLIENTEDGE, u(96), u(38), u(216), u(150), IDC_FIND_TEXT_R, font);
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.replacewith"), 0, 0, u(16), u(76), u(78), u(14), 1002, font);
    MakeControl(hwnd_, inst_, L"COMBOBOX", L"",
        CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL,
        WS_EX_CLIENTEDGE, u(96), u(72), u(216), u(150), IDC_REPLACE_TEXT, font);
    const BtnDef rbtns[] = {
        {Tr(L"find.replace"), IDC_BTN_REPLACE, u(36), false},
        {Tr(L"find.replaceall"), IDC_BTN_REPLALL, u(66), false},
        {Tr(L"find.replaceallopen"), IDC_BTN_REPLOPEN, u(96), false},
    };
    MakeButtons(hwnd_, inst_, rbtns, 3, u(326), u(106), u(25), font);

    // ---------------- Page 2: 文件中查找 ----------------
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.dir"), 0, 0, u(12), u(40), u(88), u(14), 1010, font);
    MakeControl(hwnd_, inst_, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                WS_EX_CLIENTEDGE, u(104), u(37), u(226), u(21), IDC_DIR_EDIT, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.browse"), BS_PUSHBUTTON | WS_TABSTOP, 0,
                u(370), u(36), u(110), u(24), IDC_DIR_BROWSE, font);
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.filters"), 0, 0, u(12), u(70), u(88), u(14), 1011, font);
    MakeControl(hwnd_, inst_, L"EDIT", L"*.txt;*.log;*.ini;*.cfg", ES_AUTOHSCROLL | WS_TABSTOP,
                WS_EX_CLIENTEDGE, u(104), u(67), u(226), u(21), IDC_FIF_FILTERS, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.subdirs"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 0, u(374), u(68), u(100), u(18), IDC_FIF_SUB, font);
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.what"), 0, 0, u(12), u(100), u(88), u(14), 1012, font);
    MakeControl(hwnd_, inst_, L"COMBOBOX", L"",
        CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL,
        WS_EX_CLIENTEDGE, u(104), u(97), u(226), u(150), IDC_FIND_TEXT_FIF, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.findall"),
        BS_PUSHBUTTON | BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
        u(104), u(130), u(120), u(26), IDC_BTN_FIF_FIND, font);
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.replacewith"), 0, 0, u(12), u(170), u(88), u(14), 1017, font);
    MakeControl(hwnd_, inst_, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                WS_EX_CLIENTEDGE, u(104), u(167), u(226), u(21), IDC_FIF_REPLACE, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.replaceallbtn"), BS_PUSHBUTTON | WS_TABSTOP, 0,
                u(370), u(166), u(110), u(25), IDC_BTN_FIF_REPL, font);
    ::CheckDlgButton(hwnd_, IDC_FIF_SUB, BST_CHECKED);

    // ---------------- Page 3: 项目中查找 ----------------
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.filters"), 0, 0, u(12), u(40), u(88), u(14), 1013, font);
    MakeControl(hwnd_, inst_, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                WS_EX_CLIENTEDGE, u(104), u(37), u(260), u(21), IDC_PROJ_FILTERS, font);
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.what"), 0, 0, u(12), u(70), u(88), u(14), 1014, font);
    MakeControl(hwnd_, inst_, L"COMBOBOX", L"",
        CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL,
        WS_EX_CLIENTEDGE, u(104), u(67), u(260), u(150), IDC_FIND_TEXT_PRJ, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.findall"),
        BS_PUSHBUTTON | BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
        u(104), u(100), u(120), u(26), IDC_BTN_PROJ_FIND, font);
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.scope"),
        0, 0, u(12), u(140), u(420), u(16), 1015, font);

    // ---------------- Page 4: 标记 ----------------
    MakeControl(hwnd_, inst_, L"STATIC", Tr(L"find.what"), 0, 0, u(16), u(42), u(74), u(14), 1016, font);
    MakeControl(hwnd_, inst_, L"COMBOBOX", L"",
        CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL,
        WS_EX_CLIENTEDGE, u(96), u(38), u(216), u(150), IDC_MARK_TEXT, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.markbookmark"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 0, u(16), u(72), u(110), u(18), IDC_MARK_BOOKMARK, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.markopen"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 0, u(140), u(72), u(130), u(18), IDC_MARK_OPEN, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.markall"), BS_PUSHBUTTON | WS_TABSTOP, 0,
                u(16), u(104), u(120), u(26), IDC_BTN_MARK_ALL, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.markclear"), BS_PUSHBUTTON | WS_TABSTOP, 0,
                u(146), u(104), u(120), u(26), IDC_BTN_MARK_CLR, font);
    ::CheckDlgButton(hwnd_, IDC_MARK_BOOKMARK, BST_CHECKED);

    // ---------------- common options ----------------
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.matchcase"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 0, u(10), u(216), u(110), u(18), IDC_MATCH_CASE, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.wholeword"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 0, u(130), u(216), u(110), u(18), IDC_WHOLE_WORD, font);
    MakeControl(hwnd_, inst_, L"BUTTON", Tr(L"find.regexp"),
        BS_AUTOCHECKBOX | WS_TABSTOP, 0, u(250), u(216), u(120), u(18), IDC_REGEXP, font);

    ::SendMessageW(hwnd_, WM_SETFONT, (WPARAM)font, TRUE);

    page_ = pageIndex;
    SwitchPage(page_);

    // push the tab strip under every page widget now that all exist
    ::SetWindowPos(tabs, HWND_BOTTOM, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (state_) {
        if (!state_->text.empty()) {
            for (WORD id : {IDC_FIND_TEXT, IDC_FIND_TEXT_R, IDC_FIND_TEXT_FIF,
                            IDC_FIND_TEXT_PRJ, IDC_MARK_TEXT})
                ::SetWindowTextW(::GetDlgItem(hwnd_, id), state_->text.c_str());
        }
        if (!state_->replace.empty())
            ::SetWindowTextW(::GetDlgItem(hwnd_, IDC_REPLACE_TEXT), state_->replace.c_str());
        ::CheckDlgButton(hwnd_, IDC_MATCH_CASE, state_->matchCase ? BST_CHECKED : BST_UNCHECKED);
        ::CheckDlgButton(hwnd_, IDC_WHOLE_WORD, state_->wholeWord ? BST_CHECKED : BST_UNCHECKED);
    }

    ::ShowWindow(hwnd_, SW_SHOW);
    HWND focusTarget = ::GetDlgItem(hwnd_, FocusComboFor(page_));
    BOOL focusOk = focusTarget ? ::SetFocus(focusTarget) != nullptr : FALSE;
    if (state_ && !state_->text.empty())
        ::SendDlgItemMessageW(hwnd_, FocusComboFor(page_), EM_SETSEL, 0, -1);

    // belt-and-braces: force a synchronous full repaint of frame + all children
    ::RedrawWindow(hwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);

    // diagnostics: one compact line describing what actually exists
    {
        int visCount = 0;
        for (WORD id : {IDC_FIND_TEXT, IDC_BTN_NEXT, IDC_TABS,
                        IDC_MATCH_CASE, IDC_DIR_EDIT})
            if (::GetDlgItem(hwnd_, id) && ::IsWindowVisible(::GetDlgItem(hwnd_, id)))
                ++visCount;
        Logger::Info("SearchDlg open page=" + std::to_string(page_) +
                     " dpi=" + std::to_string(dpi) +
                     " keyControlsVisible=" + std::to_string(visCount) + "/5" +
                     " setFocus=" + std::to_string(focusOk ? 1 : 0));
    }
}

void FindDialog::SwitchPage(int pageIndex) {
    page_ = pageIndex;
    auto showGroup = [&](const std::vector<WORD>& ids, BOOL show) {
        for (WORD id : ids) {
            HWND c = ::GetDlgItem(hwnd_, id);
            if (c) ::ShowWindow(c, show);
        }
    };
    std::vector<std::vector<WORD>> pages(5);
    pages[0] = {1001, IDC_FIND_TEXT, IDC_BTN_NEXT, IDC_BTN_PREV, IDC_BTN_COUNT, IDC_BTN_ALLCUR, IDC_BTN_ALLOPEN};
    pages[1] = {1003, IDC_FIND_TEXT_R, 1002, IDC_REPLACE_TEXT, IDC_BTN_REPLACE, IDC_BTN_REPLALL, IDC_BTN_REPLOPEN};
    pages[2] = {1010, IDC_DIR_EDIT, IDC_DIR_BROWSE, 1011, IDC_FIF_FILTERS, IDC_FIF_SUB, 1012, IDC_FIND_TEXT_FIF, IDC_BTN_FIF_FIND, IDC_FIF_REPLACE, IDC_BTN_FIF_REPL, 1017};
    pages[3] = {1013, IDC_PROJ_FILTERS, 1014, IDC_FIND_TEXT_PRJ, IDC_BTN_PROJ_FIND, 1015};
    pages[4] = {1016, IDC_MARK_TEXT, IDC_MARK_BOOKMARK, IDC_MARK_OPEN, IDC_BTN_MARK_ALL, IDC_BTN_MARK_CLR};

    for (int p = 0; p < 5; ++p)
        showGroup(pages[p], p == pageIndex ? SW_SHOW : SW_HIDE);
}

void FindDialog::Close() {
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        ::DestroyWindow(h);
    }
}

void FindDialog::Retranslate() {
    if (!hwnd_) return;
    ::SetWindowTextW(hwnd_, Tr(L"find.title"));
    // tab strip: replace all five labels
    HWND tabs = ::GetDlgItem(hwnd_, IDC_TABS);
    if (tabs) {
        const wchar_t* pages[] = { Tr(L"find.tab1"), Tr(L"find.tab2"), Tr(L"find.tab3"),
                                   Tr(L"find.tab4"), Tr(L"find.tab5") };
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        for (int i = 0; i < 5; ++i) {
            ti.pszText = const_cast<LPWSTR>(pages[i]);
            ::SendMessageW(tabs, TCM_SETITEMW, i, (LPARAM)&ti);
        }
        ::InvalidateRect(tabs, nullptr, TRUE);
    }
    struct Txt { WORD id; const wchar_t* text; };
    const Txt texts[] = {
        {1001, Tr(L"find.what")},
        {1003, Tr(L"find.what")},
        {1012, Tr(L"find.what")},
        {1014, Tr(L"find.what")},
        {1016, Tr(L"find.what")},
        {1002, Tr(L"find.replacewith")},
        {1017, Tr(L"find.replacewith")},
        {1010, Tr(L"find.dir")},
        {1011, Tr(L"find.filters")},
        {1013, Tr(L"find.filters")},
        {IDC_DIR_BROWSE,      Tr(L"find.browse")},
        {IDC_FIF_SUB,         Tr(L"find.subdirs")},
        {IDC_BTN_FIF_FIND,    Tr(L"find.findall")},
        {IDC_BTN_FIF_REPL,    Tr(L"find.replaceallbtn")},
        {IDC_BTN_PROJ_FIND,   Tr(L"find.findall")},
        {1015,                Tr(L"find.scope")},
        {IDC_MARK_BOOKMARK,   Tr(L"find.markbookmark")},
        {IDC_MARK_OPEN,       Tr(L"find.markopen")},
        {IDC_BTN_MARK_ALL,    Tr(L"find.markall")},
        {IDC_BTN_MARK_CLR,    Tr(L"find.markclear")},
        {IDC_MATCH_CASE,      Tr(L"find.matchcase")},
        {IDC_WHOLE_WORD,      Tr(L"find.wholeword")},
        {IDC_REGEXP,          Tr(L"find.regexp")},
    };
    for (const Txt& t : texts) {
        HWND c = ::GetDlgItem(hwnd_, t.id);
        if (c) ::SetWindowTextW(c, t.text);
    }
    const Txt btns[] = {
        {IDC_BTN_NEXT,    Tr(L"find.next")},
        {IDC_BTN_PREV,    Tr(L"find.prev")},
        {IDC_BTN_COUNT,   Tr(L"find.count")},
        {IDC_BTN_ALLCUR,  Tr(L"find.allcur")},
        {IDC_BTN_ALLOPEN, Tr(L"find.allopen")},
        {IDC_BTN_REPLACE, Tr(L"find.replace")},
        {IDC_BTN_REPLALL, Tr(L"find.replaceall")},
        {IDC_BTN_REPLOPEN,Tr(L"find.replaceallopen")},
    };
    for (const Txt& b : btns) {
        HWND c = ::GetDlgItem(hwnd_, b.id);
        if (c) ::SetWindowTextW(c, b.text);
    }
}

void FindDialog::SetSearchText(const std::wstring& t) {
    if (state_) state_->text = t;
}

void FindDialog::Request(SearchAction act) {
    if (parent_) ::SendMessageW(parent_, WM_APP_SEARCHACT, 0, (LPARAM)(int)act);
}

void FindDialog::BrowseFolder() {
    HWND edit = ::GetDlgItem(hwnd_, IDC_DIR_EDIT);
    if (edit) BrowseForFolder(hwnd_, edit);
}

void FindDialog::ApplyFromControls() {
    if (!hwnd_ || !state_) return;

    WORD activeFind = FocusComboFor(page_);
    wchar_t buf[1024];
    ::GetDlgItemTextW(hwnd_, activeFind, buf, 1024);
    state_->text = buf;
    AddHistory(::GetDlgItem(hwnd_, activeFind), buf);
    for (WORD id : {IDC_FIND_TEXT, IDC_FIND_TEXT_R, IDC_FIND_TEXT_FIF,
                    IDC_FIND_TEXT_PRJ, IDC_MARK_TEXT}) {
        if (id != activeFind)
            ::SetWindowTextW(::GetDlgItem(hwnd_, id), buf);
    }

    wchar_t rbuf[1024];
    ::GetDlgItemTextW(hwnd_, IDC_REPLACE_TEXT, rbuf, 1024);
    state_->replace = rbuf;
    state_->matchCase = ::IsDlgButtonChecked(hwnd_, IDC_MATCH_CASE) == BST_CHECKED;
    state_->wholeWord = ::IsDlgButtonChecked(hwnd_, IDC_WHOLE_WORD) == BST_CHECKED;
    state_->regexp = ::IsDlgButtonChecked(hwnd_, IDC_REGEXP) == BST_CHECKED;

    wchar_t dir[1024], flt[512], rep[1024];
    ::GetDlgItemTextW(hwnd_, IDC_DIR_EDIT, dir, 1024);
    ::GetDlgItemTextW(hwnd_, IDC_FIF_FILTERS, flt, 512);
    ::GetDlgItemTextW(hwnd_, IDC_FIF_REPLACE, rep, 1024);
    fif_.directory = dir;
    fif_.filters = flt;
    fif_.replaceWith = rep;
    fif_.recursive = ::IsDlgButtonChecked(hwnd_, IDC_FIF_SUB) == BST_CHECKED;

    ::GetDlgItemTextW(hwnd_, IDC_PROJ_FILTERS, flt, 512);
    proj_.filters = flt;
}

void FindDialog::AddHistory(HWND combo, const std::wstring& t) {
    if (!combo || t.empty()) return;
    int n = (int)::SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        wchar_t item[1024];
        int len = (int)::SendMessageW(combo, CB_GETLBTEXTLEN, i, 0);
        if (len >= 0 && len < 1024 &&
            ::SendMessageW(combo, CB_GETLBTEXT, i, (LPARAM)item) > 0) {
            if (t == item) { ::SendMessageW(combo, CB_DELETESTRING, i, 0); break; }
        }
    }
    n = (int)::SendMessageW(combo, CB_GETCOUNT, 0, 0);
    while (n >= 10) { ::SendMessageW(combo, CB_DELETESTRING, n - 1, 0); --n; }
    ::SendMessageW(combo, CB_INSERTSTRING, 0, (LPARAM)t.c_str());
    ::SetWindowTextW(combo, t.c_str());
}

bool FindDialog::MarkAllOpenScope() const {
    return hwnd_ && ::IsDlgButtonChecked(hwnd_, IDC_MARK_OPEN) == BST_CHECKED;
}

bool FindDialog::MarkBookmarkLines() const {
    return hwnd_ && ::IsDlgButtonChecked(hwnd_, IDC_MARK_BOOKMARK) == BST_CHECKED;
}

// --- Go To Line (hand-rolled modal) --------------------------------------------

namespace {

struct GotoState { int result; int maxLine; };

LRESULT CALLBACK GotoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (GotoState*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    BOOL ok = FALSE;
                    UINT v = ::GetDlgItemInt(h, 2001, &ok, FALSE);
                    st->result = ok ? (int)v : -1;
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
                    return 0;
                }
                case IDCANCEL:
                    st->result = -1;
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

} // namespace

int GotoDialog::Run(HWND parent, HINSTANCE hInst, int maxLine) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = GotoProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"xfsWinPadGotoDlg";
    ::RegisterClassExW(&wc);

    int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return MulDiv(px, dpi, 96); };

    GotoState st{-1, maxLine};
    RECT wr{}; ::GetWindowRect(parent, &wr);
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, u(240), u(92)};
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    int x = wr.left + ((wr.right - wr.left) - (rc.right - rc.left)) / 2;
    int y = wr.top + u(90);

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, L"xfsWinPadGotoDlg",
                                 Tr(L"goto.title"), gstyle,
                                 x, y, rc.right - rc.left, rc.bottom - rc.top,
                                 parent, nullptr, hInst, &st);
    if (!dlg) return -1;

    HFONT font = DlgFont(parent);
    HWND lbl = ::CreateWindowExW(0, L"STATIC", Tr(L"goto.line"),
        WS_CHILD | WS_VISIBLE, u(12), u(15), u(80), u(16),
        dlg, (HMENU)(UINT_PTR)2002, hInst, nullptr);
    ::SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
    HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
        u(96), u(12), u(120), u(22), dlg, (HMENU)(UINT_PTR)2001, hInst, nullptr);
    ::SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
    struct Bt { const wchar_t* t; WORD id; int x; };
    const Bt bs[] = {{Tr(L"input.ok"), IDOK, u(60)}, {Tr(L"input.cancel"), IDCANCEL, u(130)}};
    for (const Bt& b : bs) {
        HWND bb = ::CreateWindowExW(0, L"BUTTON", b.t,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            b.x, u(52), u(66), u(24), dlg, (HMENU)(UINT_PTR)b.id, hInst, nullptr);
        ::SendMessageW(bb, WM_SETFONT, (WPARAM)font, TRUE);
    }
    ::SetWindowTextW(edit, L"1");
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

    if (st.result < 1) return -1;
    if (maxLine > 0 && st.result > maxLine) return maxLine;
    return st.result;
}

} // namespace xfs
