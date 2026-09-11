// WindowsListDialog.cpp — N++ WindowsDlg 风格的窗口列表对话框。
// 过滤框 + 多选 ListView（名称/路径/视图/大小）+ 激活/保存/关闭窗口按钮。
// (view, index) 在 Refill 时捕获；Save/Close 批量操作从后往前执行避免索引位移，
// 每轮操作后重新收集快照再刷新列表。
#include "WindowsListDialog.h"
#include "MainWindow.h"
#include "../workspace/Workspace.h"
#include "../document/Document.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include <commctrl.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xfs {

namespace {

constexpr unsigned IDC_FILTER   = 4001;
constexpr unsigned IDC_LIST     = 4002;
constexpr unsigned IDC_ACTIVATE = 4003;
constexpr unsigned IDC_SAVE     = 4004;
constexpr unsigned IDC_CLOSE    = 4005;
constexpr unsigned IDC_CANCEL   = 4006;
constexpr unsigned IDC_INFO     = 4007;

constexpr int kDlgW = 640, kDlgH = 430;

struct DlgState {
    HWND hFilter = nullptr, hList = nullptr, hInfo = nullptr;
    HINSTANCE inst = nullptr;
    HWND parent = nullptr;
    MainWindow* host = nullptr;
    std::vector<WindowsListDialog::Entry> all;   // full snapshot
    std::vector<int> shown;                      // indices into all (post-filter)
    bool changed = false;

    static DlgState* Get(HWND h) {
        return (DlgState*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    }
};

std::vector<WindowsListDialog::Entry> CollectEntries(Workspace& ws);

std::wstring FormatSize(unsigned long long bytes) {
    wchar_t buf[64]{};
    if (bytes >= 1024ULL * 1024ULL)
        swprintf_s(buf, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        swprintf_s(buf, L"%llu KB", bytes / 1024ULL);
    return buf;
}

std::wstring ViewText(int view) {
    return Tr(view == 1 ? L"winlist.view2" : L"winlist.view1");
}

void Refill(DlgState& st) {
    wchar_t buf[128]{};
    ::GetWindowTextW(st.hFilter, buf, 128);
    std::wstring q = buf;
    CharLowerBuffW(q.data(), (DWORD)q.size());

    ::SendMessageW(st.hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st.hList);
    st.shown.clear();

    int selIdx = -1;
    for (size_t i = 0; i < st.all.size(); ++i) {
        const WindowsListDialog::Entry& e = st.all[i];
        if (!q.empty()) {
            std::wstring hay = e.name + L" " + e.path;
            CharLowerBuffW(hay.data(), (DWORD)hay.size());
            if (hay.find(q) == std::wstring::npos) continue;
        }
        st.shown.push_back((int)i);

        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = INT_MAX;
        it.lParam = (LPARAM)i;
        std::wstring name = e.name;
        if (e.active) name = L"\x2713 " + name;
        it.pszText = name.data();
        int idx = (int)::SendMessageW(st.hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        std::wstring path = e.path.empty() ? std::wstring(Tr(L"winlist.untitled"))
                                           : e.path;
        ListView_SetItemText(st.hList, idx, 1, path.data());
        std::wstring vw = ViewText(e.view);
        ListView_SetItemText(st.hList, idx, 2, vw.data());
        std::wstring sz = e.path.empty() ? L"" : FormatSize(e.size);
        ListView_SetItemText(st.hList, idx, 3, sz.data());
        if (e.active && selIdx < 0) selIdx = idx;
    }
    if (selIdx >= 0)
        ListView_SetItemState(st.hList, selIdx, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    ::SendMessageW(st.hList, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(st.hList, nullptr, TRUE);

    std::wstring info = I18n::Instance().Fmt(L"winlist.count",
            {std::to_wstring(st.shown.size()), std::to_wstring(st.all.size())});
    ::SetWindowTextW(st.hInfo, info.c_str());
}

// 选中文档（快照下标），从后往前排序——批量关闭/保存时先动尾部，避免索引位移。
std::vector<int> SelectedDocsDescending(DlgState& st) {
    std::vector<int> docs;
    int i = -1;
    while ((i = (int)::SendMessageW(st.hList, LVM_GETNEXTITEM, (WPARAM)-1,
                    LVNI_SELECTED)) != -1) {
        if ((size_t)i < st.shown.size()) docs.push_back(st.shown[i]);
        if (docs.size() > 9999) break;
    }
    std::sort(docs.begin(), docs.end(), std::greater<int>());
    return docs;
}

// 选中单个文档（激活用）：多选时取第一个聚焦行。
int SingleSelectedRow(DlgState& st) {
    int i = (int)::SendMessageW(st.hList, LVM_GETNEXTITEM, (WPARAM)-1,
                                LVNI_SELECTED | LVNI_FOCUSED);
    if (i < 0) i = (int)::SendMessageW(st.hList, LVM_GETNEXTITEM, (WPARAM)-1,
                                       LVNI_SELECTED);
    if (i < 0 || (size_t)i >= st.shown.size()) return -1;
    return i;
}

void Resnapshot(DlgState& st) {
    st.changed = true;
    st.all = CollectEntries(st.host->GetWorkspace());
    Refill(st);
}

void DoActivate(DlgState& st) {
    int row = SingleSelectedRow(st);
    if (row < 0) return;
    const WindowsListDialog::Entry& e = st.all[st.shown[row]];
    if (e.view == 1) st.host->GetWorkspace().ActivateView1(e.index);
    else             st.host->GetWorkspace().Activate(e.index);
    ::DestroyWindow(::GetParent(st.hList));
}

void DoSave(DlgState& st) {
    Workspace& ws = st.host->GetWorkspace();
    std::vector<int> docs = SelectedDocsDescending(st);
    if (docs.empty()) return;
    for (int di : docs) {
        const WindowsListDialog::Entry& e = st.all[di];
        if (e.view == 1) ws.SaveAt1(e.index);
        else             ws.SaveAt(e.index);
    }
    Resnapshot(st);
}

void DoClose(DlgState& st) {
    Workspace& ws = st.host->GetWorkspace();
    std::vector<int> docs = SelectedDocsDescending(st);
    if (docs.empty()) return;
    for (int di : docs) {
        const WindowsListDialog::Entry& e = st.all[di];
        if (e.view == 1) ws.Close1(e.index);
        else             ws.Close(e.index);
    }
    Resnapshot(st);
}

std::vector<WindowsListDialog::Entry> CollectEntries(Workspace& ws) {
    std::vector<WindowsListDialog::Entry> out;
    auto push = [&](Document* d, int view, int index) {
        if (!d) return;
        WindowsListDialog::Entry e;
        e.name = d->TitleForTab();
        e.path = d->path.wstring();
        e.view = view;
        e.index = index;
        e.active = (ws.ActiveIn(view) == d);
        e.dirty = d->editor.Modified();
        e.size = 0;
        if (!e.path.empty()) {
            std::error_code ec;
            auto sz = std::filesystem::file_size(e.path, ec);
            if (!ec) e.size = (unsigned long long)sz;
        }
        out.push_back(std::move(e));
    };
    for (int i = 0; i < ws.Count(); ++i) push(ws.DocumentAt(i), 0, i);
    for (int i = 0; i < ws.Count1(); ++i) push(ws.FindByTabIndex1(i), 1, i);
    return out;
}

LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    DlgState* st = DlgState::Get(h);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_COMMAND: {
            if (!st) return 0;
            switch (LOWORD(wp)) {
                case IDC_FILTER:
                    if (HIWORD(wp) == EN_CHANGE) Refill(*st);
                    return 0;
                case IDC_ACTIVATE: DoActivate(*st); return 0;
                case IDC_SAVE:     DoSave(*st); return 0;
                case IDC_CLOSE:    DoClose(*st); return 0;
                case IDC_CANCEL:
                case IDCANCEL:
                    ::DestroyWindow(h);
                    return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            if (!st) break;
            const NMHDR* nm = (const NMHDR*)lp;
            if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK) {
                if (((NMITEMACTIVATE*)nm)->iItem >= 0) { DoActivate(*st); return 0; }
            }
            break;
        }
        case WM_CLOSE:
            ::DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            delete st;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

bool WindowsListDialog::Run(HWND parent, HINSTANCE inst, MainWindow& host) {
    static const wchar_t kClass[] = L"xfsWinPadWindowList";
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = inst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        reg = true;
    }

    DlgState* st = new DlgState();
    st->inst = inst;
    st->parent = parent;
    st->host = &host;
    st->all = CollectEntries(host.GetWorkspace());

    const int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return ::MulDiv(px, dpi, 96); };
    HFONT font = ::CreateFontW(-u(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const int W = u(kDlgW), H = u(kDlgH);
    RECT rc{0, 0, W, H};
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr{}; ::GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, Tr(L"winlist.title"),
                                 gstyle, x, y, rc.right - rc.left,
                                 rc.bottom - rc.top, parent, nullptr, inst, st);
    if (!dlg) { delete st; ::DeleteObject(font); return false; }
    const int w = kDlgW;   // logical client width

    auto MkLabel = [&](const wchar_t* txt, int X, int Y, int Wd, int Ht) {
        HWND c = ::CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE,
                                   u(X), u(Y), u(Wd), u(Ht), dlg, nullptr, inst, nullptr);
        ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    auto MkEdit = [&](unsigned id, int X, int Y, int Wd, int Ht) {
        HWND c = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   u(X), u(Y), u(Wd), u(Ht), dlg,
                                   (HMENU)(UINT_PTR)id, inst, nullptr);
        ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    auto MkButton = [&](unsigned id, const wchar_t* txt, int X, int Y, int Wd,
                        int Ht, DWORD style) {
        HWND c = ::CreateWindowExW(0, L"BUTTON", txt,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                   u(X), u(Y), u(Wd), u(Ht), dlg,
                                   (HMENU)(UINT_PTR)id, inst, nullptr);
        ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    // 过滤行
    MkLabel(Tr(L"winlist.filter"), 10, 13, 60, 20);
    st->hFilter = MkEdit(IDC_FILTER, 74, 10, w - 84, 24);

    // 列表
    st->hList = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
        u(10), u(42), u(w - 20), u(kDlgH - 42 - 66), dlg,
        (HMENU)(UINT_PTR)IDC_LIST, inst, nullptr);
    ::SendMessageW(st->hList, WM_SETFONT, (WPARAM)font, TRUE);
    ListView_SetExtendedListViewStyle(st->hList, LVS_EX_FULLROWSELECT);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.iSubItem = 0; col.cx = u(200); col.pszText = const_cast<LPWSTR>(Tr(L"winlist.col.name"));
    ListView_InsertColumn(st->hList, 0, &col);
    col.iSubItem = 1; col.cx = u(250); col.pszText = const_cast<LPWSTR>(Tr(L"winlist.col.path"));
    ListView_InsertColumn(st->hList, 1, &col);
    col.iSubItem = 2; col.cx = u(60);  col.pszText = const_cast<LPWSTR>(Tr(L"winlist.col.view"));
    ListView_InsertColumn(st->hList, 2, &col);
    col.iSubItem = 3; col.cx = u(74);  col.pszText = const_cast<LPWSTR>(Tr(L"winlist.col.size"));
    ListView_InsertColumn(st->hList, 3, &col);

    // 状态行 + 底部按钮
    st->hInfo = MkLabel(L"", 10, kDlgH - 52, w - 480, 18);
    MkButton(IDC_ACTIVATE, Tr(L"winlist.activate"), w - 470, kDlgH - 40, 100, 28,
             BS_DEFPUSHBUTTON);
    MkButton(IDC_SAVE, Tr(L"winlist.save"), w - 360, kDlgH - 40, 86, 28, BS_PUSHBUTTON);
    MkButton(IDC_CLOSE, Tr(L"winlist.close"), w - 264, kDlgH - 40, 110, 28, BS_PUSHBUTTON);
    MkButton(IDC_CANCEL, Tr(L"input.cancel"), w - 144, kDlgH - 40, 134, 28, BS_PUSHBUTTON);

    Refill(*st);
    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);
    ::SetFocus(st->hFilter);
    MSG m;
    while (::IsWindow(dlg) && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &m)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    ::EnableWindow(parent, TRUE);
    ::SetForegroundWindow(parent);
    ::DeleteObject(font);
    return st->changed;
}

} // namespace xfs
