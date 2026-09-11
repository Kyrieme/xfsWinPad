// ShortcutMapperDialog.cpp — 管理快捷键实现（阶段 3）。
// 复用 PreferencesDialog/StyleConfigurator 的配方。修改… 用捕获式录入：
// 捕获框子类化 WM_KEYDOWN，忽略纯修饰键，实时显示 "Ctrl+Shift+F3" 形态。
#include "ShortcutMapperDialog.h"
#include "../shortcut/ShortcutTable.h"
#include "../core/CommandIds.h"
#include "../core/Log.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <utility>
#include <vector>

namespace xfs {

namespace {

constexpr unsigned IDC_TAB     = 3401;
constexpr unsigned IDC_FILTER  = 3402;
constexpr unsigned IDC_LIST    = 3403;
constexpr unsigned IDC_MODIFY  = 3404;
constexpr unsigned IDC_CLEAR   = 3405;
constexpr unsigned IDC_RESET   = 3406;
constexpr unsigned IDC_CLOSE   = 3407;

struct Entry {
    unsigned id;
    std::wstring name;
    ShortcutInfo info;        // 当前生效值
};

struct State {
    HINSTANCE inst = nullptr;
    HWND hTab = nullptr, hFilter = nullptr, hList = nullptr;
    HWND hModify = nullptr, hClear = nullptr, hReset = nullptr, hClose = nullptr;
    std::vector<ShortcutEntry> all;      // 宿主传入的全量
    int page = 0;                        // 0=主菜单 1=内部 2=插件
    std::wstring filter;
    IShortcutChange* change = nullptr;
    // 捕获框状态（Modify 子对话框期间有效）
    HWND capture = nullptr;
    WNDPROC captureOrig = nullptr;
    ShortcutInfo pending{};
    unsigned pendingId = 0;

    static State* Get(HWND h) { return (State*)::GetWindowLongPtrW(h, GWLP_USERDATA); }
};

int U(HWND h, int px) { return ::MulDiv(px, ::GetDpiForWindow(h), 96); }

// ---- 列表构建 ----------------------------------------------------------------

bool GroupMatches(const ShortcutEntry& e, int page) {
    if (page == 0) return e.group == 0;
    if (page == 1) return e.group == 1;
    return e.group == 2;
}

int SelectedId(State& st);   // 定义在后（Refill 需要读取当前选中项）

void Refill(State& st) {
    // 保留当前选中项：修改/过滤/翻页重建列表后不得丢选，
    // 否则紧接着点「恢复默认」会因 target=0 静默失效（用户视角=不生效）。
    const unsigned prevSel = SelectedId(st);
    ::SendMessageW(st.hList, LB_RESETCONTENT, 0, 0);
    int selIdx = -1;
    for (const ShortcutEntry& e : st.all) {
        if (!GroupMatches(e, st.page)) continue;
        if (!st.filter.empty()) {
            if (_wcsnicmp(e.name.c_str(), st.filter.c_str(), st.filter.size()) != 0)
                continue;
        }
        // 单串格式：名称（快捷键）——避免 ListBox 制表单位的 DPI 折腾
        std::wstring display = e.name;
        const ShortcutInfo info = GlobalShortcuts().Get(e.id);
        if (info.Valid()) {
            display += L"（";
            display += info.ToString();
            display += L"）";
        } else {
            display += Tr(L"sc.none");
        }
        int idx = (int)::SendMessageW(st.hList, LB_ADDSTRING, 0, (LPARAM)display.c_str());
        ::SendMessageW(st.hList, LB_SETITEMDATA, idx, e.id);
        if (prevSel && e.id == prevSel) selIdx = idx;
    }
    if (selIdx >= 0) {
        ::SendMessageW(st.hList, LB_SETCURSEL, selIdx, 0);
        ::SendMessageW(st.hList, LB_SETCARETINDEX, selIdx, TRUE);   // 滚动到可见
    }
}

int SelectedId(State& st) {
    int sel = (int)::SendMessageW(st.hList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return 0;
    return (unsigned)::SendMessageW(st.hList, LB_GETITEMDATA, sel, 0);
}

const ShortcutEntry* EntryById(State& st, unsigned id) {
    for (const ShortcutEntry& e : st.all)
        if (e.id == id) return &e;
    return nullptr;
}

// ---- 捕获框子类 ----------------------------------------------------------------

// ---- 修改… 子对话框 ------------------------------------------------------------

constexpr unsigned IDC_M_CTRL  = 3501;
constexpr unsigned IDC_M_ALT   = 3502;
constexpr unsigned IDC_M_SHIFT = 3503;
constexpr unsigned IDC_M_CAP   = 3504;
constexpr unsigned IDC_M_OK    = 3505;
constexpr unsigned IDC_M_CANCEL= 3506;

struct ModifyState {
    HWND hCap = nullptr;
    bool ctrl = false, alt = false, shift = false;
    ShortcutInfo pending{};
    unsigned targetId = 0;
    HWND owner = nullptr;
    static ModifyState* Get(HWND h) { return (ModifyState*)::GetWindowLongPtrW(h, GWLP_USERDATA); }
};

LRESULT CALLBACK CapProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        UINT vk = (UINT)wp;
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS)
            return 0;
        ModifyState* m = ModifyState::Get(::GetParent(h));
        if (m) {
            ShortcutInfo info;
            info.vk = vk;
            info.ctrl  = m->ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            info.alt   = m->alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
            info.shift = m->shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            m->pending = info;
            wchar_t buf[64];
            lstrcpyW(buf, info.ToString().c_str());
            ::SetWindowTextW(h, buf);
        }
        return 0;
    }
    if (msg == WM_CHAR) return 0;   // 捕获框不接受文本输入
    WNDPROC orig = (WNDPROC)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return ::CallWindowProcW(orig, h, msg, wp, lp);
}

LRESULT CALLBACK ModifyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ModifyState* m = ModifyState::Get(h);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_COMMAND: {
            const unsigned id = LOWORD(wp);
            if (id == IDC_M_OK) {
                if (!m || !m->pending.Valid()) {
                    ::MessageBoxW(h, Tr(L"sc.modify.needkey"),
                                  Tr(L"sc.modify.title"), MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                State* st = State::Get(m->owner);
                unsigned conflict = 0;
                if (!GlobalShortcuts().SetShortcut(m->targetId, m->pending, &conflict)) {
                    const ShortcutEntry* other = EntryById(*st, conflict);
                    std::wstring msg2 = Tr(L"sc.conflict.used");
                    msg2 += other ? other->name
                                  : I18n::Instance().Fmt(L"sc.conflict.cmd",
                                        {std::to_wstring(conflict)});
                    msg2 += Tr(L"sc.conflict.hint");
                    ::MessageBoxW(h, msg2.c_str(), Tr(L"sc.conflict.title"),
                                  MB_OK | MB_ICONWARNING);
                    return 0;
                }
                ::DestroyWindow(h);
                return 0;
            }
            if (id == IDC_M_CANCEL || id == IDCANCEL) { ::DestroyWindow(h); return 0; }
            if (id == IDC_M_CTRL || id == IDC_M_ALT || id == IDC_M_SHIFT) {
                if (m) {
                    HWND c = ::GetDlgItem(h, IDC_M_CTRL);
                    HWND a = ::GetDlgItem(h, IDC_M_ALT);
                    HWND s = ::GetDlgItem(h, IDC_M_SHIFT);
                    m->ctrl  = c && ::SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    m->alt   = a && ::SendMessageW(a, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    m->shift = s && ::SendMessageW(s, BM_GETCHECK, 0, 0) == BST_CHECKED;
                }
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            delete m;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

void OpenModify(State& st, HWND parent, HINSTANCE inst, unsigned targetId,
                const std::wstring& name) {
    static const wchar_t kClass[] = L"xfsWinPadShortcutModify";
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ModifyProc;
        wc.hInstance = inst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        reg = true;
    }
    ModifyState* m = new ModifyState();
    m->targetId = targetId;
    m->owner = parent;
    const ShortcutInfo cur = GlobalShortcuts().Get(targetId);
    m->pending = cur;

    const int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return ::MulDiv(px, dpi, 96); };
    HFONT font = ::CreateFontW(-u(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    // 逻辑常量 = 唯一坐标基准（Mk 内部统一 u() 缩放）；与首选项/配置器的
    // 二次缩放教训同源——W/H 物理值只用于窗口外框。
    constexpr int kMw = 380, kMh = 190;
    const int W = u(kMw), H = u(kMh);
    RECT rc{0, 0, W, H};
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr{}; ::GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    const int y = pr.top + u(60);

    std::wstring title = std::wstring(Tr(L"sc.modify.title")) + L" - " + name;
    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(),
                                 gstyle, x, y, rc.right - rc.left,
                                 rc.bottom - rc.top, parent, nullptr, inst, m);
    if (!dlg) { delete m; ::DeleteObject(font); return; }
    auto Mk = [&](const wchar_t* cls, const wchar_t* txt, unsigned id, int X, int Y,
                  int Wd, int Ht, DWORD ex) {
        HWND c = ::CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE,
                                   u(X), u(Y), u(Wd), u(Ht), dlg,
                                   (HMENU)(UINT_PTR)id, inst, nullptr);
        ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    Mk(L"STATIC", Tr(L"sc.modify.cmdlabel"), 0, 12, 12, 60, 18, 0);
    Mk(L"STATIC", name.c_str(), 0, 80, 12, 280, 18, 0);
    Mk(L"BUTTON", L"Ctrl",  IDC_M_CTRL,  12, 40, 70, 20, WS_TABSTOP | BS_AUTOCHECKBOX);
    Mk(L"BUTTON", L"Alt",   IDC_M_ALT,   92, 40, 70, 20, WS_TABSTOP | BS_AUTOCHECKBOX);
    Mk(L"BUTTON", L"Shift", IDC_M_SHIFT, 172, 40, 70, 20, WS_TABSTOP | BS_AUTOCHECKBOX);
    Mk(L"STATIC", Tr(L"sc.modify.press"), 0, 12, 70, 300, 18, 0);
    HWND cap = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                 cur.Valid() ? cur.ToString().c_str() : L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_READONLY,
                                 u(12), u(92), u(kMw - 24), u(24), dlg,
                                 (HMENU)(UINT_PTR)IDC_M_CAP, inst, nullptr);
    ::SendMessageW(cap, WM_SETFONT, (WPARAM)font, TRUE);
    st.capture = cap;
    st.captureOrig = (WNDPROC)::GetWindowLongPtrW(cap, GWLP_WNDPROC);
    // GWLP_USERDATA 由 CapProc 用作原 WNDPROC 槽（CapProc 不读 State）
    ::SetWindowLongPtrW(cap, GWLP_USERDATA, (LONG_PTR)st.captureOrig);
    ::SetWindowLongPtrW(cap, GWLP_WNDPROC, (LONG_PTR)CapProc);
    Mk(L"BUTTON", Tr(L"input.ok"), IDC_M_OK, kMw - 190, kMh - 40, 80, 28,
       WS_TABSTOP | BS_DEFPUSHBUTTON);
    Mk(L"BUTTON", Tr(L"input.cancel"), IDC_M_CANCEL, kMw - 96, kMh - 40, 80, 28,
       WS_TABSTOP | BS_PUSHBUTTON);
    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);
    ::SetFocus(cap);
    MSG msg;
    while (::IsWindow(dlg) && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    ::EnableWindow(parent, TRUE);
    ::SetForegroundWindow(parent);
    ::DeleteObject(font);
    st.capture = nullptr;
    st.captureOrig = nullptr;
}

// ---- 主窗口过程 -----------------------------------------------------------------

LRESULT CALLBACK MapperProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = State::Get(h);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_COMMAND: {
            if (!st) return 0;
            const unsigned id = LOWORD(wp);
            switch (id) {
                case IDC_CLOSE:
                case IDCANCEL:
                    ::EnableWindow(::GetWindow(h, GW_OWNER), TRUE);
                    ::SetForegroundWindow(::GetWindow(h, GW_OWNER));
                    ::DestroyWindow(h);
                    return 0;
                case IDC_FILTER:
                    if (HIWORD(wp) == EN_CHANGE) {
                        wchar_t buf[128]{};
                        ::GetWindowTextW(st->hFilter, buf, 128);
                        st->filter = buf;
                        Refill(*st);
                    }
                    return 0;
                case IDC_MODIFY: {
                    unsigned target = SelectedId(*st);
                    const ShortcutEntry* e = EntryById(*st, target);
                    if (!e) {
                        ::MessageBoxW(h, Tr(L"sc.pickone"), Tr(L"sc.title"),
                                      MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    OpenModify(*st, h, st->inst, target, e->name);
                    if (st->change) st->change->OnShortcutChanged();
                    Refill(*st);
                    return 0;
                }
                case IDC_CLEAR: {
                    unsigned target = SelectedId(*st);
                    if (!target) {
                        ::MessageBoxW(h, Tr(L"sc.pickone"), Tr(L"sc.title"),
                                      MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    ShortcutInfo none;   // vk=0 = 显式解绑
                    unsigned conflict = 0;
                    GlobalShortcuts().SetShortcut(target, none, &conflict);
                    if (st->change) st->change->OnShortcutChanged();
                    Refill(*st);
                    return 0;
                }
                case IDC_RESET: {
                    unsigned target = SelectedId(*st);
                    if (!target) {
                        ::MessageBoxW(h, Tr(L"sc.pickone"), Tr(L"sc.title"),
                                      MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    GlobalShortcuts().ClearOverride(target);   // 回默认
                    if (st->change) st->change->OnShortcutChanged();
                    Refill(*st);
                    return 0;
                }
            }
            break;
        }
        case WM_NOTIFY: {
            const NMHDR* hdr = (const NMHDR*)lp;
            if (st && hdr->code == TCN_SELCHANGE && hdr->idFrom == IDC_TAB) {
                st->page = (int)::SendMessageW(st->hTab, TCM_GETCURSEL, 0, 0);
                Refill(*st);
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            delete st;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void ShortcutMapperDialog::Run(HWND parent, HINSTANCE hInst,
                               const std::vector<ShortcutEntry>& entries,
                               IShortcutChange* change) {
    Logger::Info("ShortcutMapper: opening, entries=" +
                 std::to_string(entries.size()));
    static const wchar_t kClass[] = L"xfsWinPadShortcutMapper";
    static bool regDone = false;
    if (!regDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MapperProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        regDone = true;
    }

    State* st = new State();
    st->inst = hInst;
    st->change = change;
    st->all = entries;

    const int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return ::MulDiv(px, dpi, 96); };
    HFONT font = ::CreateFontW(-u(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const int W = u(560), H = u(480);
    RECT rc{0, 0, W, H};
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr{}; ::GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    const int y = pr.top + u(30);

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, Tr(L"sc.title"),
                                 gstyle, x, y, rc.right - rc.left,
                                 rc.bottom - rc.top, parent, nullptr, hInst, st);
    if (!dlg) {
        Logger::Error("ShortcutMapper: CreateWindowExW failed, gle=" +
                      std::to_string(::GetLastError()));
        delete st;
        ::DeleteObject(font);
        return;
    }
    const int w = W - u(16);

    st->hTab = ::CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH,
        u(8), u(8), w, u(26), dlg, (HMENU)(UINT_PTR)IDC_TAB, hInst, nullptr);
    for (const wchar_t* t : { Tr(L"sc.tab.main"), Tr(L"sc.tab.internal"), Tr(L"sc.tab.plugin") }) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(t);
        ::SendMessageW(st->hTab, TCM_INSERTITEMW, (int)::SendMessageW(st->hTab, TCM_GETITEMCOUNT, 0, 0), (LPARAM)&ti);
    }
    ::SendMessageW(st->hTab, TCM_SETITEMSIZE, 0, MAKELPARAM((w - u(8)) / 3, u(26)));
    ::SendMessageW(st->hTab, WM_SETFONT, (WPARAM)font, TRUE);

    HWND lbl = ::CreateWindowExW(0, L"STATIC", Tr(L"sc.filter"), WS_CHILD | WS_VISIBLE,
                                 u(8), u(42), u(60), u(20), dlg, nullptr, hInst, nullptr);
    ::SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
    st->hFilter = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        u(72), u(40), w - u(80), u(22), dlg, (HMENU)(UINT_PTR)IDC_FILTER, hInst, nullptr);
    ::SendMessageW(st->hFilter, WM_SETFONT, (WPARAM)font, TRUE);

    st->hList = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        WS_VSCROLL,
        u(8), u(68), w, H - u(68) - u(56), dlg, (HMENU)(UINT_PTR)IDC_LIST, hInst, nullptr);
    ::SendMessageW(st->hList, WM_SETFONT, (WPARAM)font, TRUE);

    st->hClose = ::CreateWindowExW(0, L"BUTTON", Tr(L"sc.close"), WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | BS_DEFPUSHBUTTON, w - u(96), H - u(44), u(80), u(28),
        dlg, (HMENU)(UINT_PTR)IDC_CLOSE, hInst, nullptr);
    st->hModify = ::CreateWindowExW(0, L"BUTTON", Tr(L"sc.modify"), WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | BS_PUSHBUTTON, w - u(282), H - u(44), u(80), u(28),
        dlg, (HMENU)(UINT_PTR)IDC_MODIFY, hInst, nullptr);
    st->hClear = ::CreateWindowExW(0, L"BUTTON", Tr(L"sc.clear"), WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | BS_PUSHBUTTON, w - u(378), H - u(44), u(88), u(28),
        dlg, (HMENU)(UINT_PTR)IDC_CLEAR, hInst, nullptr);
    st->hReset = ::CreateWindowExW(0, L"BUTTON", Tr(L"sc.reset"), WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | BS_PUSHBUTTON, w - u(478), H - u(44), u(92), u(28),
        dlg, (HMENU)(UINT_PTR)IDC_RESET, hInst, nullptr);
    for (HWND c : { st->hTab, st->hFilter, st->hList, st->hClose, st->hModify,
                    st->hClear, st->hReset })
        ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);

    Refill(*st);
    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);
    MSG m;
    while (::IsWindow(dlg) && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &m)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    ::DeleteObject(font);
}

} // namespace xfs
