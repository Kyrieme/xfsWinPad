#include "FileExplorer.h"
#include "InputBox.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <algorithm>

namespace xfs {

namespace fs = std::filesystem;

namespace {
constexpr wchar_t kFeClass[] = L"xfsWinPadFileExplorer";
constexpr wchar_t kTreeClass[] = L"SysTreeView32";
constexpr int ID_TREE = 1400;
constexpr UINT_PTR kTreeSubclassId = 4;
} // namespace

LRESULT CALLBACK FeWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

bool FileExplorer::Create(HWND parent, HINSTANCE hInst) {
    if (hwnd_) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FeWndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kFeClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    int dpi = ::GetDpiForWindow(parent);
    int w = MulDiv(260, dpi, 96);

    hwnd_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, kFeClass, nullptr,
                              WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, w, 600, parent, (HMENU)(INT_PTR)1104,
                              hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL,
                          FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Segoe UI");
    hInst_ = hInst;

    tree_ = ::CreateWindowExW(0, WC_TREEVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                              TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS |
                              TVS_SHOWSELALWAYS | WS_TABSTOP,
                              0, 0, w - 8, 590,
                              hwnd_, (HMENU)(UINT_PTR)ID_TREE, hInst, nullptr);
    if (tree_) ::SendMessageW(tree_, WM_SETFONT, (WPARAM)font_, TRUE);
    return true;
}

void FileExplorer::Destroy() {
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    tree_ = nullptr;
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
}

void FileExplorer::SetRoot(const std::wstring& dir) {
    rootDir_ = dir;
    if (!tree_) return;
    ::SendMessageW(tree_, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    if (dir.empty()) return;
    fs::path p(dir);
    auto name = p.filename().wstring();
    if (name.empty()) name = dir;

    TVINSERTSTRUCTW tvi{};
    tvi.hParent = TVI_ROOT;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    tvi.item.pszText = const_cast<LPWSTR>(name.c_str());
    tvi.item.lParam = (LPARAM)new std::wstring(dir);
    tvi.item.cChildren = 1;
    HTREEITEM root = (HTREEITEM)::SendMessageW(tree_, TVM_INSERTITEMW, 0, (LPARAM)&tvi);

    // expand root and load first level
    PopulateTree(root, p);
    ::SendMessageW(tree_, TVM_EXPAND, TVE_EXPAND, (LPARAM)root);
}

void FileExplorer::PopulateTree(HTREEITEM parent, const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        try {
            auto entry = *it;
            auto fname = entry.path().filename().wstring();
            if (fname.starts_with(L".")) continue;   // hidden

            bool isDir = entry.is_directory(ec);
            bool isFile = entry.is_regular_file(ec);
            if (!isDir && !isFile) continue;

            // filter: only common text/code extensions for files
            if (isFile) {
                auto ext = entry.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                static const wchar_t* ok[] = { L".txt", L".md", L".cpp", L".h", L".hpp",
                    L".c", L".py", L".js", L".ts", L".html", L".css", L".json",
                    L".xml", L".yaml", L".yml", L".sql", L".sh", L".bat", L".ps1",
                    L".rs", L".go", L".java", L".rb", L".lua", L".log", L".cfg",
                    L".ini", L".toml", L".csv", L".svg", L".pgs", L".ldf",
                    L".pat", L".dat", L".stil", L".vcproj", L".vcxproj",
                    L".xfm", nullptr };
                bool match = false;
                for (int j = 0; ok[j]; ++j) if (ext == ok[j]) { match = true; break; }
                if (!match) continue;
            }

            TVINSERTSTRUCTW tvi{};
            tvi.hParent = parent;
            tvi.hInsertAfter = TVI_LAST;
            tvi.item.mask = TVIF_TEXT | TVIF_PARAM | (isDir ? TVIF_CHILDREN : 0);
            tvi.item.pszText = const_cast<LPWSTR>(fname.c_str());
            tvi.item.lParam = (LPARAM)new std::wstring(entry.path().wstring());
            tvi.item.cChildren = isDir ? 1 : 0;
            HTREEITEM item = TreeView_InsertItem(tree_, &tvi);

            // lazy: directories get expanded on demand via TVN_ITEMEXPANDING
        } catch (...) {}
    }
}

std::wstring FileExplorer::GetItemFullPath(HTREEITEM hti) const {
    TVITEMW ti{};
    ti.hItem = hti;
    ti.mask = TVIF_PARAM | TVIF_TEXT;
    wchar_t buf[MAX_PATH * 2]{};
    ti.pszText = buf;
    ti.cchTextMax = MAX_PATH * 2;
    ::SendMessageW(tree_, TVM_GETITEMW, 0, (LPARAM)&ti);
    if (ti.lParam) return *(std::wstring*)ti.lParam;
    return buf;
}

COLORREF FileExplorer::GitTextColorOf(HTREEITEM hti) const {
    if (!gitStates_ || gitStates_->empty() || !hti) return CLR_NONE;
    std::wstring key = GetItemFullPath(hti);
    for (auto& ch : key) ch = towlower(ch);
    auto it = gitStates_->find(key);
    if (it == gitStates_->end()) return CLR_NONE;
    switch (it->second) {
        case git::FileState::Modified:
        case git::FileState::Renamed:   return RGB(214, 120, 0);   // orange
        case git::FileState::Untracked:
        case git::FileState::Added:     return RGB(51, 153, 51);   // green
        case git::FileState::Deleted:
        case git::FileState::Conflict:  return RGB(204, 51, 51);   // red
    }
    return CLR_NONE;
}

void FileExplorer::Refresh() {
    if (!rootDir_.empty()) SetRoot(rootDir_);
}

void FileExplorer::CloseFolder() {
    rootDir_.clear();
    if (tree_) ::SendMessageW(tree_, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
}

void FileExplorer::ShowContextMenu(POINT screenPt) {
    HTREEITEM sel = TreeView_GetSelection(tree_);
    std::wstring path = sel ? GetItemFullPath(sel) : L"";
    std::error_code ec;
    bool isDir = !path.empty() && fs::is_directory(path, ec);
    bool isFile = !path.empty() && fs::is_regular_file(path, ec);

    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING, 1, Tr(L"fe.open"));
    ::AppendMenuW(menu, MF_STRING, 2, Tr(L"fe.openlocation"));
    ::AppendMenuW(menu, MF_STRING, 3, Tr(L"fe.copypath"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 4, Tr(L"fe.newfile"));
    ::AppendMenuW(menu, MF_STRING, 5, Tr(L"fe.newfolder"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 6, Tr(L"fe.rename"));
    ::AppendMenuW(menu, MF_STRING, 7, Tr(L"fe.delete"));
    {
        git::FileState gst{};
        bool hasState = false;
        if (gitStates_ && !path.empty()) {
            std::wstring key = path;
            for (auto& ch : key) ch = towlower(ch);
            auto it = gitStates_->find(key);
            if (it != gitStates_->end()) { gst = it->second; hasState = true; }
        }
        bool inRepo = gitStates_ && !gitStates_->empty();
        bool any = inRepo && (isFile && onGitCompare && hasState) ||
                   (onGitStage && hasState && gst != git::FileState::Added) ||
                   (onGitUnstage && hasState && gst == git::FileState::Added) ||
                   (onGitRevert && isFile && hasState &&
                    gst != git::FileState::Untracked) ||
                   (inRepo && onGitCommit) || (inRepo && onGitBranch) ||
                   (inRepo && onGitBranchNew) || (inRepo && onGitPush) ||
                   (inRepo && onGitFetch) || (inRepo && onGitPull);
        if (any) {
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            if (isFile && onGitCompare && hasState)
                ::AppendMenuW(menu, MF_STRING, 9, Tr(L"git.compare"));
            if (onGitStage && hasState && gst != git::FileState::Added)
                ::AppendMenuW(menu, MF_STRING, 10, Tr(L"git.stage"));
            if (onGitUnstage && hasState && gst == git::FileState::Added)
                ::AppendMenuW(menu, MF_STRING, 11, Tr(L"git.unstage"));
            if (onGitRevert && isFile && hasState &&
                gst != git::FileState::Untracked)
                ::AppendMenuW(menu, MF_STRING, 18, Tr(L"git.revert"));
            if (inRepo && onGitCommit)
                ::AppendMenuW(menu, MF_STRING, 12, Tr(L"git.commit"));
            if (inRepo && onGitBranch)
                ::AppendMenuW(menu, MF_STRING, 13, Tr(L"git.branch"));
            if (inRepo && onGitBranchNew)
                ::AppendMenuW(menu, MF_STRING, 14, Tr(L"git.branch.new"));
            if (inRepo && onGitFetch)
                ::AppendMenuW(menu, MF_STRING, 15, Tr(L"git.fetch"));
            if (inRepo && onGitPull)
                ::AppendMenuW(menu, MF_STRING, 17, Tr(L"git.pull"));
            if (inRepo && onGitPush)
                ::AppendMenuW(menu, MF_STRING, 16, Tr(L"git.push"));
        }
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 8, Tr(L"fe.refresh"));

    int cmd = (int)::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                      screenPt.x, screenPt.y, hwnd_, nullptr);
    ::DestroyMenu(menu);
    if (cmd == 0) return;
    switch (cmd) {
        case 1:
            if (!path.empty()) {
                if (isDir)
                    ::SendMessageW(tree_, TVM_EXPAND, TVE_TOGGLE, (LPARAM)sel);
                else if (onOpenFile) onOpenFile(path);
            }
            break;
        case 2:
            if (!path.empty()) OpenInExplorer(path);
            break;
        case 3:
            if (!path.empty() && ::OpenClipboard(hwnd_)) {
                ::EmptyClipboard();
                SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
                HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (g) {
                    void* p = ::GlobalLock(g);
                    if (p) { memcpy(p, path.c_str(), bytes); ::GlobalUnlock(g); }
                    ::SetClipboardData(CF_UNICODETEXT, g);
                }
                ::CloseClipboard();
            }
            break;
        case 4: NewFile(sel); break;
        case 5: NewFolder(sel); break;
        case 6: RenameItem(sel); break;
        case 7: DeleteItem(sel); break;
        case 8: Refresh(); break;
        case 9: if (!path.empty() && onGitCompare) onGitCompare(path); break;
        case 10: if (!path.empty() && onGitStage) onGitStage(path); break;
        case 11: if (!path.empty() && onGitUnstage) onGitUnstage(path); break;
        case 12: if (onGitCommit) onGitCommit(); break;
        case 13: if (onGitBranch) onGitBranch(); break;
        case 14: if (onGitBranchNew) onGitBranchNew(); break;
        case 15: if (onGitFetch) onGitFetch(); break;
        case 16: if (onGitPush) onGitPush(); break;
        case 17: if (onGitPull) onGitPull(); break;
        case 18: if (!path.empty() && onGitRevert) onGitRevert(path); break;
    }
}

std::wstring FileExplorer::TargetDir(HTREEITEM sel) const {
    if (!sel) return L"";
    std::wstring p = GetItemFullPath(sel);
    if (p.empty()) return L"";
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) p = fs::path(p).parent_path().wstring();
    return p;
}

void FileExplorer::NewFile(HTREEITEM sel) {
    std::wstring dir = TargetDir(sel);
    if (dir.empty()) return;
    std::wstring name;
    if (!InputBox(hwnd_, hInst_, Tr(L"fe.newfile.title"), Tr(L"fe.newfile.name"), name)) return;
    if (name.empty()) return;
    fs::path p(fs::path(dir) / name);
    std::error_code ec;
    if (fs::exists(p, ec)) {
        ::MessageBoxW(hwnd_, Tr(L"fe.newfile.exists"), Tr(L"fe.newfile.title"), MB_ICONWARNING);
        return;
    }
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::MessageBoxW(hwnd_, Tr(L"fe.newfile.fail"), Tr(L"fe.newfile.title"), MB_ICONERROR);
        return;
    }
    ::CloseHandle(h);
    Refresh();
    if (onOpenFile) onOpenFile(p.wstring());   // open the freshly created file
}

void FileExplorer::NewFolder(HTREEITEM sel) {
    std::wstring dir = TargetDir(sel);
    if (dir.empty()) return;
    std::wstring name;
    if (!InputBox(hwnd_, hInst_, Tr(L"fe.newfolder.title"), Tr(L"fe.newfolder.name"), name)) return;
    if (name.empty()) return;
    fs::path p(fs::path(dir) / name);
    std::error_code ec;
    if (fs::exists(p, ec)) {
        ::MessageBoxW(hwnd_, Tr(L"fe.newfolder.exists"), Tr(L"fe.newfolder.title"), MB_ICONWARNING);
        return;
    }
    if (!::CreateDirectoryW(p.c_str(), nullptr)) {
        ::MessageBoxW(hwnd_, Tr(L"fe.newfolder.fail"), Tr(L"fe.newfolder.title"), MB_ICONERROR);
        return;
    }
    Refresh();
}

void FileExplorer::RenameItem(HTREEITEM sel) {
    if (!sel) return;
    std::wstring old = GetItemFullPath(sel);
    if (old.empty()) return;
    fs::path op(old);
    std::wstring name = op.filename().wstring();
    if (!InputBox(hwnd_, hInst_, Tr(L"fe.rename.title"), Tr(L"fe.rename.name"), name)) return;
    if (name.empty() || name == op.filename().wstring()) return;
    fs::path np = op.parent_path() / name;
    std::error_code ec;
    if (fs::exists(np, ec)) {
        ::MessageBoxW(hwnd_, Tr(L"fe.rename.exists"), Tr(L"fe.rename.title"), MB_ICONWARNING);
        return;
    }
    fs::rename(op, np, ec);
    if (ec) {
        ::MessageBoxW(hwnd_, Tr(L"fe.rename.fail"), Tr(L"fe.rename.title"), MB_ICONERROR);
        return;
    }
    Refresh();
}

void FileExplorer::DeleteItem(HTREEITEM sel) {
    if (!sel) return;
    std::wstring path = GetItemFullPath(sel);
    if (path.empty()) return;
    std::wstring msg = I18n::Instance().Fmt(L"fe.delete.confirm",
                        {fs::path(path).filename().wstring()});
    if (::MessageBoxW(hwnd_, msg.c_str(), Tr(L"fe.delete.title"),
                      MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    std::wstring from = path;
    from.push_back(L'\0');   // SHFileOperation requires a double-null-terminated list
    SHFILEOPSTRUCTW fo{};
    fo.hwnd = hwnd_;
    fo.wFunc = FO_DELETE;
    fo.pFrom = from.c_str();
    fo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    int res = ::SHFileOperationW(&fo);
    if (res != 0 && !fo.fAnyOperationsAborted)
        ::MessageBoxW(hwnd_, Tr(L"fe.delete.fail"), Tr(L"fe.delete.title"), MB_ICONERROR);
    Refresh();
}

void FileExplorer::OpenSelection() {
    HTREEITEM sel = TreeView_GetSelection(tree_);
    if (!sel) return;
    auto path = GetItemFullPath(sel);
    std::error_code ec;
    if (!path.empty() && onOpenFile && !fs::is_directory(path, ec))
        onOpenFile(path);   // directories only expand/collapse in the tree
}

// --- window procs ---------------------------------------------------------

LRESULT CALLBACK FeWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (FileExplorer*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            auto* cs = (CREATESTRUCTW*)lp;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)(FileExplorer*)cs->lpCreateParams);
            return 0;
        }
        case WM_SIZE:
            if (self && self->tree_)
                ::MoveWindow(self->tree_, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            return 0;
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (!self || nm->idFrom != ID_TREE) break;
            switch (nm->code) {
                case TVN_ITEMEXPANDING: {
                    NMTREEVIEW* nmtv = (NMTREEVIEW*)lp;
                    HTREEITEM child = TreeView_GetChild(nm->hwndFrom, nmtv->itemNew.hItem);
                    if (child) {   // already populated
                        // check if first child is placeholder
                        TVITEMW ti{}; ti.hItem = child; ti.mask = TVIF_PARAM;
                        TreeView_GetItem(nm->hwndFrom, &ti);
                        break;   // allow expansion
                    }
                    // populate lazily
                    auto path = self->GetItemFullPath(nmtv->itemNew.hItem);
                    self->PopulateTree(nmtv->itemNew.hItem, fs::path(path));
                    break;
                }
                case NM_DBLCLK:
                    self->OpenSelection();
                    return 1;   // suppress default
                case NM_CUSTOMDRAW: {
                    NMTVCUSTOMDRAW* cd = (NMTVCUSTOMDRAW*)lp;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                        return (LRESULT)CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        COLORREF c = self->GitTextColorOf((HTREEITEM)cd->nmcd.dwItemSpec);
                        if (c != CLR_NONE) {
                            cd->clrText = c;
                            return (LRESULT)CDRF_NEWFONT;
                        }
                    }
                    return (LRESULT)CDRF_DODEFAULT;
                }
            }
            break;
        }
        case WM_CONTEXTMENU: {
            if (!self) break;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) {   // keyboard-invoked: use selection
                RECT rc; ::GetWindowRect(self->tree_, &rc);
                pt.x = (rc.left + rc.right) / 2;
                pt.y = (rc.top + rc.bottom) / 2;
            } else {
                // Right-click does NOT change tree selection natively: hit-test
                // the item under the cursor and select it explicitly, otherwise
                // the menu acts on the previously selected (wrong) item.
                POINT cpt = pt;
                ::ScreenToClient(self->tree_, &cpt);
                TVHITTESTINFO ht{};
                ht.pt = cpt;
                if (::SendMessageW(self->tree_, TVM_HITTEST, 0, (LPARAM)&ht) &&
                    ht.hItem && (ht.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT |
                                             TVHT_ONITEMSTATEICON))) {
                    ::SendMessageW(self->tree_, TVM_SELECTITEM, TVGN_CARET,
                                   (LPARAM)ht.hItem);
                } else {
                    return 0;   // right-click on empty space: no menu
                }
            }
            self->ShowContextMenu(pt);
            return 0;
        }
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK FeTreeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                            UINT_PTR, DWORD_PTR ref) {
    return DefSubclassProc(h, msg, wp, lp);
}

} // namespace xfs
