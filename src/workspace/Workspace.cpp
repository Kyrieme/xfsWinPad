#include "Workspace.h"
#include "../app/MainWindow.h"
#include "../core/CommandIds.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../core/I18n.h"
#include "../encoding/Encoding.h"
#include "../encoding/StreamDecoder.h"
#include "../encoding/StreamEncoder.h"
#include "../language/LanguageMap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#pragma comment(lib, "comdlg32.lib")

namespace xfs {

// DetectEol 实现移至 Editor.cpp（xfs 命名空间自由函数，声明在
// Editor.h）——AI 未保存文档回填等路径共用，原匿名空间版本已删除。

Workspace::Workspace(MainWindow& host) : host_(host) {}

void Workspace::Init(HWND editorHost, HINSTANCE hInst,
                     const AppSettings& prefs, const ThemeDef& theme,
                     bool createInitialDoc) {
    editorHost_ = editorHost;
    inst_ = hInst;
    prefs_ = &prefs;
    theme_ = &theme;

    TabBar::Callbacks cb;
    cb.onSelect = [this](int i) { Activate(i); };
    cb.onCloseRequest = [this](int i) { Close(i); };
    cb.onNewRequest = [this]() { NewDocument(); };
    cb.onReorder = [this](int from, int to) { MoveDocument(from, to); };
    cb.onTooltipText = [this](int i) -> std::wstring {
        Document* d = At(i);
        if (!d) return {};
        return d->HasPath() ? d->path.wstring() : Tr(L"tab.unsaved");
    };
    cb.onDetachRequest = [this](int idx) {
        // N++-style: drag the tab out of the strip -> new window
        Activate(idx);
        host_.MoveCurrentToNewWindow();
    };
    cb.onContextMenu = [this](int idx, int sx, int sy) -> void {
        HMENU menu = ::CreatePopupMenu();
        if (!menu) return;
        Document* d = At(idx);
        bool hasPath = d && d->HasPath();
        ::AppendMenuW(menu, MF_STRING | (closedTabs_.empty() ? MF_GRAYED : 0),
                      1010, Tr(L"tab.reopenclosed"));
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, 1001, Tr(L"tab.close"));
        ::AppendMenuW(menu, MF_STRING, 1011, Tr(L"tab.closeunmodified"));
        ::AppendMenuW(menu, MF_STRING, 1002, Tr(L"tab.closeothers"));
        ::AppendMenuW(menu, MF_STRING, 1003, Tr(L"tab.closeleft"));
        ::AppendMenuW(menu, MF_STRING, 1004, Tr(L"tab.closeright"));
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // 批次 38：锁定文案区分锁型（用户锁=解锁/文件只读=注明/普通=锁定）
        ::AppendMenuW(menu, MF_STRING, 1012,
                      d ? (d->locked ? Tr(L"tab.unlock")
                                     : (d->readOnly ? Tr(L"tab.lockro")
                                                    : Tr(L"tab.lock")))
                        : Tr(L"tab.lock"));
        ::AppendMenuW(menu, MF_STRING, 1008, Tr(L"tab.copypath"));
        if (hasPath)
            ::AppendMenuW(menu, MF_STRING, 1005, Tr(L"tab.openlocation"));
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, 1006, Tr(L"tab.moveother"));
        ::AppendMenuW(menu, MF_STRING, 1007, Tr(L"tab.movenew"));
        HWND fw = host_.Hwnd();
        UINT cmd = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                      sx, sy, fw, nullptr);
        ::DestroyMenu(menu);
        switch (cmd) {
            case 1010: ReopenClosedTab(); break;
            case 1001: Close(idx); break;
            case 1011: CloseUnmodified(0); break;
            case 1002: {   // 关闭其他（锁定跳过，脏文件先提示）
                for (int i = (int)docs_.size()-1; i >= 0; --i) {
                    if (i == idx) continue;
                    Document* o = docs_[i].get();
                    if (o && o->locked) continue;
                    if (!PromptSaveIfDirty(i)) return;   // user cancelled
                    docs_[i]->editor.Destroy(); RemoveAt(i);
                    if (i < idx) --idx;
                }
                idx = (std::min)(idx, (int)docs_.size()-1);
                if(idx>=0) Activate(idx); UpdateUi();
                break;
            }
            case 1003: {   // 关闭左侧（锁定跳过）
                for (int i = idx-1; i >= 0; --i) {
                    Document* o = docs_[i].get();
                    if (o && o->locked) continue;
                    if (!PromptSaveIfDirty(i)) return;
                    docs_[i]->editor.Destroy(); RemoveAt(i); --idx;
                }
                if(idx<0) idx=0;
                Activate(idx); UpdateUi();
                break;
            }
            case 1004: {   // 关闭右侧（锁定跳过）
                for (int i = (int)docs_.size()-1; i > idx; --i) {
                    Document* o = docs_[i].get();
                    if (o && o->locked) continue;
                    if (!PromptSaveIfDirty(i)) return;
                    docs_[i]->editor.Destroy(); RemoveAt(i);
                }
                UpdateUi();
                break;
            }
            case 1012: {   // 锁定/解锁切换（锁定=禁关闭+只读）
                if (!d) break;
                d->locked = !d->locked;
                d->editor.SetReadOnly(d->locked || d->readOnly);
                Logger::Info(std::string("Tab lock toggled: ") +
                             (d->locked ? "on" : "off"));
                break;
            }
            case 1008:
                if (d && d->HasPath())
                    SetClipboardText(host_.Hwnd(), d->path.wstring());
                break;
            case 1005:
                if (d && d->HasPath()) OpenInExplorer(d->path.wstring());
                break;
            case 1006:
                // move the tab under the context menu to the other view
                Activate(idx);
                MoveActiveToOtherView();
                break;
            case 1007:
                Activate(idx);
                host_.MoveCurrentToNewWindow();
                break;
        }
    };
    tabs_.Create(editorHost, hInst, xfs::Cmd::TabBarId);
    tabs_.SetCallbacks(std::move(cb));
    // full custom painting: strip background + per-tab themed drawing, both
    // driven from MainWindow (theme + workspace data).
    tabs_.SetDrawHandler(
        [this]() -> COLORREF {
            return theme_ ? theme_->tabInactiveBg : RGB(0xDE, 0xE1, 0xE6);
        },
        [this](HDC hdc, int index, const RECT& rc, bool selected) {
            host_.DrawTabItem(hdc, tabs_, index, rc, selected);
        });

    if (createInitialDoc)
        NewDocument(); // start with one empty document unless files arrive
}

void Workspace::SetRightHost(HWND h) {
    editorHost1_ = h;
    TabBar::Callbacks cb1;
    cb1.onSelect = [this](int i) { ActivateView1(i); };
    cb1.onCloseRequest = [this](int i) { Close1(i); };
    cb1.onNewRequest = [this]() {
        // new document opens in the right/other view
        auto doc = std::make_unique<Document>();
        if (!doc->editor.Create(editorHost1_, inst_)) return;
        host_.AttachMacroHook(&doc->editor);
        if (prefs_) {
            doc->editor.ApplyPrefs(*prefs_);
            doc->editor.SetEol((EolMode)prefs_->defaultEol);
        }
        if (theme_) doc->editor.ApplyTheme(*theme_);
        wchar_t name[32];
        swprintf_s(name, L"new %d", ++untitledCounter1_);
        doc->SetUntitledName(name);
        doc->view = 1;
        InsertDoc1(std::move(doc), true);
        OnSplitToggledOn();
        Logger::Info("New right-view document: " + WideToUtf8(name));
    };
    cb1.onReorder = [this](int from, int to) { MoveDocument1(from, to); };
    cb1.onDetachRequest = [this](int idx) {
        ActivateView1(idx);
        host_.MoveCurrentToNewWindow();
    };
    cb1.onTooltipText = [this](int i) -> std::wstring {
        Document* d = At1(i);
        if (!d) return {};
        return d->HasPath() ? d->path.wstring() : Tr(L"tab.unsaved");
    };
    cb1.onContextMenu = [this](int idx, int sx, int sy) -> void {
        HMENU menu = ::CreatePopupMenu();
        if (!menu) return;
        Document* d = At1(idx);
        bool hasPath = d && d->HasPath();
        ::AppendMenuW(menu, MF_STRING | (closedTabs_.empty() ? MF_GRAYED : 0),
                      1010, Tr(L"tab.reopenclosed"));
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, 1001, Tr(L"tab.close"));
        ::AppendMenuW(menu, MF_STRING, 1011, Tr(L"tab.closeunmodified"));
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // 批次 38：锁定文案区分锁型（用户锁=解锁/文件只读=注明/普通=锁定）
        ::AppendMenuW(menu, MF_STRING, 1012,
                      d ? (d->locked ? Tr(L"tab.unlock")
                                     : (d->readOnly ? Tr(L"tab.lockro")
                                                    : Tr(L"tab.lock")))
                        : Tr(L"tab.lock"));
        ::AppendMenuW(menu, MF_STRING, 1008, Tr(L"tab.copypath"));
        if (hasPath)
            ::AppendMenuW(menu, MF_STRING, 1005, Tr(L"tab.openlocation"));
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, 1006, Tr(L"tab.moveother"));
        ::AppendMenuW(menu, MF_STRING, 1007, Tr(L"tab.movenew"));
        UINT cmd = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                      sx, sy, host_.Hwnd(), nullptr);
        ::DestroyMenu(menu);
        switch (cmd) {
            case 1010: ReopenClosedTab(); break;
            case 1001: Close1(idx); break;
            case 1011: CloseUnmodified(1); break;
            case 1012: {   // 锁定/解锁切换
                if (!d) break;
                d->locked = !d->locked;
                d->editor.SetReadOnly(d->locked || d->readOnly);
                break;
            }
            case 1008:
                if (d && d->HasPath())
                    SetClipboardText(host_.Hwnd(), d->path.wstring());
                break;
            case 1005:
                if (d && d->HasPath()) OpenInExplorer(d->path.wstring());
                break;
            case 1006:
                ActivateView1(idx);
                MoveActiveToOtherView();
                break;
            case 1007:
                ActivateView1(idx);
                host_.MoveCurrentToNewWindow();
                break;
        }
    };
    tabs1_.Create(h, inst_, xfs::Cmd::TabBarId2);
    tabs1_.SetCallbacks(std::move(cb1));
    tabs1_.SetDrawHandler(
        [this]() -> COLORREF {
            return theme_ ? theme_->tabInactiveBg : RGB(0xDE, 0xE1, 0xE6);
        },
        [this](HDC hdc, int index, const RECT& rc, bool selected) {
            host_.DrawTabItem(hdc, tabs1_, index, rc, selected);
        });
}

void Workspace::Layout(int x, int y, int w, int h) {
    int dpi = ::GetDpiForWindow(editorHost_);
    bool showTabs = prefs_ ? prefs_->showTabBar : true;
    int tabH = showTabs ? tabs_.HeightForDpi(dpi) : 0;
    ::ShowWindow(tabs_.Hwnd(), showTabs ? SW_SHOW : SW_HIDE);
    ::MoveWindow(tabs_.Hwnd(), x, y, w, tabH, TRUE);
    if (diffView_.Active()) {
        diffView_.Layout(x, y + tabH, w, (std::max)(0, h - tabH));
        return;
    }
    LayoutView(0);
    // when split, also lay out the right view inside its own host
    if (SplitActive() && editorHost1_)
        LayoutRight(w, h);
}

void Workspace::LayoutRight(int w, int h) {
    if (!editorHost1_) return;
    int dpi = ::GetDpiForWindow(editorHost1_);
    bool showTabs = prefs_ ? prefs_->showTabBar : true;
    int tabH = showTabs ? tabs1_.HeightForDpi(dpi) : 0;
    ::ShowWindow(tabs1_.Hwnd(), showTabs ? SW_SHOW : SW_HIDE);
    ::MoveWindow(tabs1_.Hwnd(), 0, 0, w, tabH, TRUE);
    // position active right-view editor to fill the host below its tab strip
    if (Document* d = Active1()) {
        ::MoveWindow(d->editor.Hwnd(), 0, tabH, w, (std::max)(0, h - tabH), TRUE);
        ::ShowWindow(d->editor.Hwnd(), SW_SHOW);
    }
    for (auto& d : docs1_)
        if (d.get() != Active1()) ::ShowWindow(d->editor.Hwnd(), SW_HIDE);
}

void Workspace::SetActiveDoc(Document* d) {
    if (d && (d->view == 0 || d->view == 1)) currentView_ = d->view;
}

Document* Workspace::ActiveIn(int view) {
    return (view == 1) ? Active1() : At(active_);
}

Document* Workspace::Active() {
    return (currentView_ == 1) ? Active1() : At(active_);
}

Document* Workspace::At(int index) {
    return (index >= 0 && index < (int)docs_.size()) ? docs_[index].get() : nullptr;
}

Document* Workspace::FindByEditor(HWND editorHwnd) {
    for (auto& d : docs_)
        if (d->editor.Hwnd() == editorHwnd) return d.get();
    for (auto& d : docs1_)
        if (d->editor.Hwnd() == editorHwnd) return d.get();
    return nullptr;
}

Document* Workspace::FindByPath(const std::wstring& absPath) const {
    for (const auto& d : docs_)
        if (d->HasPath() && d->path.wstring() == absPath) return d.get();
    for (const auto& d : docs1_)
        if (d->HasPath() && d->path.wstring() == absPath) return d.get();
    return nullptr;
}

int Workspace::IndexOf(const Document* d) const {
    for (size_t i = 0; i < docs_.size(); ++i)
        if (docs_[i].get() == d) return (int)i;
    return -1;
}

int Workspace::IndexOf1(const Document* d) const {
    for (size_t i = 0; i < docs1_.size(); ++i)
        if (docs1_[i].get() == d) return (int)i;
    return -1;
}

void Workspace::InsertDoc(std::unique_ptr<Document> doc, bool activate) {
    doc->view = 0;
    docs_.push_back(std::move(doc));
    int idx = (int)docs_.size() - 1;
    tabs_.Insert(idx, docs_[idx]->TitleForTab());
    WireExtWords(docs_[idx].get());
    if (activate) Activate(idx);
}

void Workspace::InsertDoc1(std::unique_ptr<Document> doc, bool activate) {
    doc->view = 1;
    docs1_.push_back(std::move(doc));
    int idx = (int)docs1_.size() - 1;
    tabs1_.Insert(idx, docs1_[idx]->TitleForTab());
    WireExtWords(docs1_[idx].get());
    if (activate) ActivateView1(idx);
}

// 批次 31：跨标签词汇源。unique_ptr 挪动不影响 Document* 指向，捕获安全。
void Workspace::WireExtWords(Document* d) {
    d->editor.SetExtWordsProvider([this, d](std::set<std::string>& out) {
        CollectOpenTabWords(out, d);
        // 批次 89：被引用 .dec 的符号（pin / 组 / 时序名）。.dec 通常**不在**
        // 打开的标签里，上面的跨标签词汇源覆盖不到 —— 这正是这层缓存的增量。
        // 缓存由宿主随打开/切换/编辑防抖刷新，这里只读，不碰盘。
        for (const std::string& s : d->decSymbols) out.insert(s);
    });
}

void Workspace::CollectOpenTabWords(std::set<std::string>& out,
                                    const Document* exclude) {
    int budget = 3000;
    auto scan = [&](const std::unique_ptr<Document>& up) {
        const Document* d = up.get();
        if (d == exclude || budget <= 0) return;
        const sptr_t len = d->editor.Send(SCI_GETLENGTH);
        if (len <= 0 || len > 512 * 1024) return;   // 大文档不拖累弹出
        std::set<std::string> w;
        d->editor.ExtractWords(w);
        for (const std::string& s : w) {
            if (budget <= 0) break;
            if (out.insert(s).second) --budget;
        }
    };
    for (const std::unique_ptr<Document>& up : docs_) scan(up);
    for (const std::unique_ptr<Document>& up : docs1_) scan(up);
}

void Workspace::RemoveAt1(int index) {
    if (index < 0 || index >= (int)docs1_.size()) return;
    docs1_.erase(docs1_.begin() + index);
    tabs1_.Remove(index);
    for (int i = 0; i < (int)docs1_.size(); ++i)
        tabs1_.Rename(i, docs1_[i]->TitleForTab());
}

void Workspace::MoveDocument1(int from, int to) {
    if (from == to || from < 0 || to < 0 ||
        from >= (int)docs1_.size() || to >= (int)docs1_.size())
        return;
    auto doc = std::move(docs1_[from]);
    docs1_.erase(docs1_.begin() + from);
    docs1_.insert(docs1_.begin() + to, std::move(doc));
    active1_ = to;
    UpdateUi();
}

void Workspace::RemoveAt(int index) {
    if (index < 0 || index >= (int)docs_.size()) return;
    docs_.erase(docs_.begin() + index);
    tabs_.Remove(index);
    // rebuild remaining tab titles/indices
    for (int i = 0; i < (int)docs_.size(); ++i)
        tabs_.Rename(i, docs_[i]->TitleForTab());
}

void Workspace::SyncTabTitles() {
    for (int i = 0; i < (int)docs_.size(); ++i) {
        if (tabs_.Count() > i)
            tabs_.Rename(i, docs_[i]->TitleForTab());
    }
    for (int i = 0; i < (int)docs1_.size(); ++i) {
        if (tabs1_.Count() > i)
            tabs1_.Rename(i, docs1_[i]->TitleForTab());
    }
}

void Workspace::Activate(int index) {
    Document* d = At(index);
    if (!d) return;

    // In compare mode both panes stay visible; clicking either compared tab
    // just moves focus. Switching to an unrelated tab exits compare mode.
    if (diffView_.Active()) {
        if (diffView_.Contains(d)) {
            active_ = index;
            tabs_.SetCurrent(index);
            ::SetFocus(d->editor.Hwnd());
            currentView_ = 0;
            SyncTabTitles();
            if (onDocumentActivated) onDocumentActivated(d);
            return;
        }
        ExitCompareMode();
    }

    active_ = index;
    currentView_ = 0;
    tabs_.SetCurrent(index);
    LayoutView(0);
    if (Document* cur = Active())
        SetFocus(cur->editor.Hwnd());
    SyncTabTitles();
    UpdateUi();
    if (onDocumentActivated) onDocumentActivated(d);
}

void Workspace::ActivateView1(int index) {
    Document* d = At1(index);
    if (!d) return;
    active1_ = index;
    currentView_ = 1;
    tabs1_.SetCurrent(index);
    LayoutView(1);
    if (Document* cur = Active())
        SetFocus(cur->editor.Hwnd());
    // sync the left tab titles as well (dirty markers)
    SyncTabTitles();
    UpdateUi();
    if (onDocumentActivated) onDocumentActivated(d);
}

void Workspace::LayoutView(int view) {
    HWND host = (view == 1) ? editorHost1_ : editorHost_;
    if (!host) return;
    int dpi = ::GetDpiForWindow(host);
    bool showTabs = prefs_ ? prefs_->showTabBar : true;
    int tabH = showTabs ? ((view == 1) ? tabs1_.HeightForDpi(dpi) : tabs_.HeightForDpi(dpi)) : 0;
    RECT rc; GetClientRect(host, &rc);
    int w = rc.right - rc.left;
    int h = std::max<int>(0, rc.bottom - rc.top - tabH);
    if (view == 1) {
        Document* d = Active1();
        if (d) {
            ::MoveWindow(d->editor.Hwnd(), 0, tabH, w, h, TRUE);
            ::ShowWindow(d->editor.Hwnd(), SW_SHOW);
        }
        for (auto& doc : docs1_)
            if (doc.get() != d) ::ShowWindow(doc->editor.Hwnd(), SW_HIDE);
    } else {
        Document* d = At(active_);
        if (active_ >= 0 && d) {
            ::MoveWindow(d->editor.Hwnd(), 0, tabH, w, h, TRUE);
            ::ShowWindow(d->editor.Hwnd(), SW_SHOW);
        }
        for (auto& doc : docs_)
            if (doc.get() != d) ::ShowWindow(doc->editor.Hwnd(), SW_HIDE);
    }
}

void Workspace::SetParentReparent(Document* d) {
    if (!d) return;
    HWND host = (d->view == 1) ? editorHost1_ : editorHost_;
    if (host) ::SetParent(d->editor.Hwnd(), host);
}

void Workspace::OnSplitToggledOn() {
    // after the split appears all existing view-0 editors must snap back to
    // the left host (they may have been reparented when moved away earlier)
    for (auto& d : docs_) SetParentReparent(d.get());
    host_.Relayout();
}

void Workspace::OnSplitToggledOff() {
    // right view is gone; drop the split and refresh layout
    currentView_ = 0;
    host_.Relayout();
}

void Workspace::MoveActiveToOtherView() {
    if (currentView_ == 0) {
        Document* d = At(active_);
        if (!d) return;
        auto moved = std::move(docs_[active_]);
        docs_.erase(docs_.begin() + active_);
        tabs_.Remove(active_);
        for (int i = 0; i < (int)docs_.size(); ++i)
            tabs_.Rename(i, docs_[i]->TitleForTab());
        if (active_ >= (int)docs_.size()) active_ = (int)docs_.size() - 1;

        moved->view = 1;
        SetParentReparent(moved.get());
        bool wasEmpty = docs1_.empty();
        active1_ = (int)docs1_.size();
        docs1_.push_back(std::move(moved));
        tabs1_.Insert(active1_, docs1_[active1_]->TitleForTab());
        currentView_ = 1;
        tabs1_.SetCurrent(active1_);
        if (active_ < 0) {
            // left became empty: keep an empty/new doc on the left
            NewDocument();
            currentView_ = 1;   // NewDocument's Activate resets to view 0
        }
        if (wasEmpty) OnSplitToggledOn(); else host_.Relayout();
        if (Document* cur = Active1()) SetFocus(cur->editor.Hwnd());
        if (Document* movedDoc = Active1()) {
            if (onDocumentActivated) onDocumentActivated(movedDoc);
        }
        UpdateUi();
        return;
    } else {
        Document* d = At1(active1_);
        if (!d) return;
        auto moved = std::move(docs1_[active1_]);
        docs1_.erase(docs1_.begin() + active1_);
        tabs1_.Remove(active1_);
        for (int i = 0; i < (int)docs1_.size(); ++i)
            tabs1_.Rename(i, docs1_[i]->TitleForTab());
        if (active1_ >= (int)docs1_.size()) active1_ = (int)docs1_.size() - 1;

        bool willBeEmpty = docs1_.empty();
        moved->view = 0;
        SetParentReparent(moved.get());
        docs_.push_back(std::move(moved));
        active_ = (int)docs_.size() - 1;
        tabs_.Insert(active_, docs_[active_]->TitleForTab());
        currentView_ = 0;
        tabs_.SetCurrent(active_);
        if (willBeEmpty) OnSplitToggledOff(); else host_.Relayout();
        if (Document* cur = Active()) SetFocus(cur->editor.Hwnd());
        UpdateUi();
        if (onDocumentActivated && Active()) onDocumentActivated(Active());
    }
}

void Workspace::EnterCompareMode(Document* a, Document* b,
                                 std::vector<int> mapA, std::vector<int> mapB) {
    diffView_.Enter(a, b, std::move(mapA), std::move(mapB));
    // relayout with the current host rect so both panes appear immediately
    RECT rc; GetClientRect(editorHost_, &rc);
    Layout(0, 0, rc.right, rc.bottom);
    if (a) SetFocus(a->editor.Hwnd());
}

void Workspace::ExitCompareMode() {
    if (!diffView_.Active()) return;
    // clear the orange/blue diff markers from both panes
    Document* a = diffView_.Left();
    Document* b = diffView_.Right();
    if (a) a->editor.ClearDiffMarks();
    if (b) b->editor.ClearDiffMarks();
    // keep whichever doc is active visible; hide the other pane
    Document* keep = Active();
    diffView_.Exit();
    if (keep) ShowWindow(keep->editor.Hwnd(), SW_SHOW);
    for (auto& d : docs_)
        if (d.get() != keep) ShowWindow(d->editor.Hwnd(), SW_HIDE);
    RECT rc; GetClientRect(editorHost_, &rc);
    Layout(0, 0, rc.right, rc.bottom);
    if (keep) { SetFocus(keep->editor.Hwnd()); UpdateUi(); }
}

void Workspace::MoveDocument(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= (int)docs_.size() || to >= (int)docs_.size())
        return;
    auto doc = std::move(docs_[from]);
    docs_.erase(docs_.begin() + from);
    docs_.insert(docs_.begin() + to, std::move(doc));
    active_ = to;
    UpdateUi();
}

// --- new / open ---------------------------------------------------------------

void Workspace::NewDocument() {
    auto doc = std::make_unique<Document>();
    if (!doc->editor.Create(editorHost_, inst_)) {
        Logger::Error("Failed to create editor control");
        return;
    }
    host_.AttachMacroHook(&doc->editor);
    if (prefs_) {
        doc->editor.ApplyPrefs(*prefs_);
        doc->editor.SetEol((EolMode)prefs_->defaultEol);
    }
    if (theme_) doc->editor.ApplyTheme(*theme_);
    wchar_t name[32];
    swprintf_s(name, L"new %d", ++untitledCounter_);
    doc->SetUntitledName(name);
    InsertDoc(std::move(doc), true);
    Logger::Info("New document: " + WideToUtf8(name));
}

void Workspace::ApplyPrefsToAll(const AppSettings& prefs) {
    // ApplyPrefs 的 STYLECLEARALL 会抹掉主题基础色 AND 词法分析器的配色表，
    // 故随后 ApplyTheme 重刷基础色 + ReapplyLexer 重挂词法器恢复语法配色。
    for (auto& d : docs_) {
        d->editor.ApplyPrefs(prefs);
        if (theme_) d->editor.ApplyTheme(*theme_);
        ReapplyLexer(d.get());
    }
    for (auto& d : docs1_) {
        d->editor.ApplyPrefs(prefs);
        if (theme_) d->editor.ApplyTheme(*theme_);
        ReapplyLexer(d.get());
    }
    UpdateUi();
}

void Workspace::OpenFileDialog() {
    wchar_t fileBuf[32768] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host_.Hwnd();
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0Text Files (*.txt)\0*.txt\0Source Code\0*.c;*.cpp;*.h;*.py;*.js;*.json;*.xml;*.html;*.sql\0\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&ofn)) return;

    // Parse multi-select result
    const wchar_t* p = fileBuf;
    std::wstring dir = p;
    p += dir.size() + 1;
    if (*p) {
        while (*p) {
            std::wstring f = dir + L"\\" + p;
            OpenPath(f);
            p += wcslen(p) + 1;
        }
    } else {
        OpenPath(dir);
    }
}

void Workspace::OpenPath(const std::wstring& pathRaw, int gotoLine, bool readOnly, bool addRecent) {
    std::wstring path = NormalizePath(pathRaw);

    // a directory is not a document; open it as the project root instead
    std::error_code fec;
    if (std::filesystem::is_directory(path, fec)) {
        Logger::Info("OpenPath: '" + WideToUtf8(path) + "' is a directory; ignored");
        return;
    }

    // already open? just focus it
    for (int i = 0; i < (int)docs_.size(); ++i) {
        if (docs_[i]->path == path) {
            if (gotoLine > 0) docs_[i]->editor.GotoLine(gotoLine);
            Activate(i);
            return;
        }
    }

    auto t0 = std::chrono::steady_clock::now();

    // --- decide load strategy -----------------------------------------------
    // mmap every file >100MB and decide per encoding:
    //   UTF-8/UTF-8 BOM  -> feed raw bytes straight to Scintilla (chunked)
    //   UTF-16LE/BE/ANSI -> StreamDecoder converts chunk-by-chunk to UTF-8
    //   (<100MB files take the small path: full read + DecodeToUtf8)
    std::error_code fsEc;
    auto fsSz = std::filesystem::file_size(path, fsEc);
    bool isLarge = !fsEc && fsSz > 100ULL * 1024 * 1024;

    // files beyond the editable cap go to the read-only big-file viewer:
    // Scintilla would either truncate (UTF-8 path) or try to materialize the
    // whole document and die (UTF-16/ANSI stream paths)
    if (!fsEc && fsSz > kMaxLoadBytes && onOversizeFile) {
        Logger::Info("OpenPath oversize -> BigFileView bytes=" +
                     std::to_string(fsSz));
        onOversizeFile(path, fsSz);
        return;
    }


    MappedFile mapped;
    const char* textData = nullptr;
    size_t textSize = 0;
    encoding::EncodingType enc = encoding::EncodingType::UTF8;
    bool useMmap = false;
    bool streamDecode = false;      // mmap + UTF-16/ANSI chunked transcoding
    bool truncated = false;
    size_t truncatedAt = 0;

    std::string raw;
    DecodedText dec;

    auto openFail = [&](const char* what) {
        Logger::Error(std::string("OpenPath ") + what + " FAILED '" +
                      WideToUtf8(path) + "' gle=" + std::to_string(::GetLastError()));
        lastError_ = L"Cannot open file:\n" + path;
        MessageBoxW(host_.Hwnd(), lastError_.c_str(), L"xfsWinPad", MB_OK | MB_ICONERROR);
    };

    if (isLarge && mapped.Open(path)) {
        encoding::EncodingType det = encoding::DetectEncoding(mapped.Data(), mapped.Size());
        if (det == encoding::EncodingType::UTF8 ||
            det == encoding::EncodingType::UTF8BOM) {
            size_t skip = encoding::BomSkip(det);
            textData = mapped.Data() + skip;
            textSize = mapped.Size() - skip;
            enc = det;
            useMmap = true;
        } else if (det == encoding::EncodingType::UTF16LE ||
                   det == encoding::EncodingType::UTF16BE ||
                   det == encoding::EncodingType::ANSI) {
            enc = det;
            useMmap = true;
            streamDecode = true;
        }
    }

    if (useMmap && !streamDecode) {
        // plain UTF-8 path: cap the load at a line boundary
        size_t capped = TruncateAtLine(textData, textSize, kMaxLoadBytes);
        truncated = capped < textSize;
        truncatedAt = capped;
        textSize = capped;
    }

    if (!useMmap) {
        if (!ReadFileBytes(path, raw)) {
            openFail("READ");
            return;
        }
        Logger::Info("OpenPath READ OK bytes=" + std::to_string(raw.size()));
        dec = encoding::DecodeToUtf8(raw);
        enc = dec.encoding;
        textData = dec.utf8.data();
        textSize = dec.utf8.size();
        Logger::Info("OpenPath decoded utf8_size=" + std::to_string(dec.utf8.size()) +
                     " enc=" + WideToUtf8(encoding::DisplayName(dec.encoding)));
    }
    else {
        Logger::Info("OpenPath mmap bytes=" + std::to_string(mapped.Size()) +
                     " enc=" + WideToUtf8(encoding::DisplayName(enc)) +
                     " stream=" + std::to_string(streamDecode ? 1 : 0));
    }

    // binary detection (first 8KB null-byte scan) on whichever buffer we have.
    // For UTF-16 the NUL code-unit bytes would false-positive; skip it there.
    bool isBinary = false;
    if (enc != encoding::EncodingType::UTF16LE &&
        enc != encoding::EncodingType::UTF16BE) {
        size_t scanLen = (std::min)(textSize, (size_t)8192);
        for (size_t i = 0; i < scanLen; ++i) {
            if (textData[i] == '\0') { isBinary = true; break; }
        }
    }
    Logger::Info("OpenPath binary=" + std::to_string(isBinary ? 1 : 0));

    auto doc = std::make_unique<Document>();
    doc->path = path;
    doc->encoding = enc;
    doc->readOnly = readOnly;
    if (!doc->editor.Create(editorHost_, inst_)) {
        Logger::Error("Failed to create editor for " + WideToUtf8(path));
        return;
    }

    // EOL / binary decisions must not depend on decoding the whole file:
    // sample the head (mmap) or the decoded buffer (small path).
    EolMode eol;
    if (useMmap && streamDecode) {
        // sample = first raw chunk converted by a throwaway decoder
        StreamDecoder probe(mapped.Data(), (std::min)(mapped.Size(), (size_t)(4 << 20)), enc);
        std::string head = probe.Next();
        eol = DetectEol(head);
        // UTF-16 binary check on the converted head
        if (!isBinary) {
            size_t scanLen = (std::min)(head.size(), (size_t)8192);
            for (size_t i = 0; i < scanLen; ++i)
                if (head[i] == '\0') { isBinary = true; break; }
        }
    } else {
        eol = DetectEol(textData, textSize);
    }

    doc->editor.SetEol(eol);
    host_.AttachMacroHook(&doc->editor);
    if (prefs_) doc->editor.ApplyPrefs(*prefs_);

    // enable large-file optimizations BEFORE loading text
    if (isLarge)
        doc->editor.EnableLargeFileMode();

    // --- load text ------------------------------------------------------------
    // Large loads block the UI thread for hundreds of ms to seconds. Keep the
    // shell responsive: disable input (prevents re-entrancy and ghosting),
    // dispatch paint messages between chunks and show progress in the title.
    struct LoadUiGuard {
        HWND main = nullptr;
        bool on = false;
        void Begin(HWND m) {
            main = m;
            on = true;
            ::EnableWindow(m, FALSE);
        }
        void Pump(const std::wstring& title) {
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, WM_PAINT, WM_PAINT, PM_REMOVE))
                ::DispatchMessageW(&msg);
            if (!title.empty()) ::SetWindowTextW(main, title.c_str());
        }
        ~LoadUiGuard() {
            if (on) ::EnableWindow(main, TRUE);
        }
    } ui;
    if (isLarge) ui.Begin(host_.Hwnd());
    std::filesystem::path loadPath(path);
    const std::wstring loadName = loadPath.filename().wstring();

    if (useMmap && streamDecode) {
        StreamDecoder sd(mapped.Data(), mapped.Size(), enc);
        bool first = true;
        long long done = 0;
        for (;;) {
            std::string chunk = sd.Next();
            if (chunk.empty()) break;
            if (first) {
                // UTF-16 BOM conversion: BOM code unit becomes U+FEFF text;
                // strip it from the front of the first chunk
                if (chunk.compare(0, 3, "\xEF\xBB\xBF") == 0)
                    chunk.erase(0, 3);
                first = false;
            }
            doc->editor.AppendTextUtf8(chunk.data(), chunk.size());
            done += (long long)chunk.size();
            wchar_t pct[64];
            swprintf_s(pct, L" %lld%%", done * 100 / (long long)mapped.Size());
            ui.Pump(L"Opening " + loadName + pct);
        }
    } else if (useMmap) {
        const size_t kChunk = 8u << 20;
        for (size_t off = 0; off < textSize; off += kChunk) {
            size_t n = (std::min)(kChunk, textSize - off);
            doc->editor.AppendTextUtf8(textData + off, n);
            wchar_t pct[64];
            swprintf_s(pct, L" %lld%%", (long long)((off + n) * 100 / textSize));
            ui.Pump(L"Opening " + loadName + pct);
        }
    } else {
        doc->editor.SetTextUtf8(dec.utf8);
    }
    if (isLarge) ui.Pump(L"Opening " + loadName);
    // streaming appends count as modifications; mark the fresh load as an
    // unmodified savepoint with no undo history (SetTextUtf8Buffer does this
    // internally for the small path)
    doc->editor.Send(SCI_EMPTYUNDOBUFFER);
    doc->editor.SetSavePoint();

    // Apply the syntax lexer AFTER ApplyPrefs and the text load: ApplyPrefs
    // calls SCI_STYLECLEARALL (resets every style to STYLE_DEFAULT and wipes
    // the lexer's colour assignments), and SetLexerByName() ends with
    // SCI_COLOURISE which needs the real buffer to restyle. Otherwise a .cpp
    // file stays looking like plain text even though the Language menu
    // auto-detects C/C++.
    if (!isLarge && !isBinary)
        doc->editor.SetLexerForFile(path, theme_);
    Logger::Info("OpenPath SetTextUtf8 done mmap=" + std::to_string(useMmap ? 1 : 0) +
                 " stream=" + std::to_string(streamDecode ? 1 : 0) +
                 (truncated ? (" truncated_at=" + std::to_string(truncatedAt)) : ""));
    doc->editor.SetReadOnly(readOnly);

    if (isBinary) {
        doc->editor.SetReadOnly(true);
        doc->editor.Send(SCI_SETVIEWEOL, 0);
        Logger::Info("Binary file detected: " + WideToUtf8(path));
    }
    if (truncated) {
        Logger::Info("OpenPath truncated to " + std::to_string(truncatedAt) + " bytes");
    }

    {
        sptr_t scintillaLen = doc->editor.Send(SCI_GETLENGTH);
        sptr_t scintillaLines = doc->editor.Send(SCI_GETLINECOUNT);
        Logger::Info("OpenPath VERIFY len=" + std::to_string(scintillaLen) +
                     " lines=" + std::to_string(scintillaLines));
        // single-line huge file: force char wrap so content is visible
        if (scintillaLines <= 2 && scintillaLen > 1024 * 1024)
            doc->editor.Send(SCI_SETWRAPMODE, 2 /*SC_WRAP_CHAR*/);
    }

    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();

    wchar_t info[256];
    swprintf_s(info, L"%s (%.1f ms)", path.c_str(), ms);
    Logger::Info("Opened " + WideToUtf8(info));

    bool activate = true;
    InsertDoc(std::move(doc), activate);
    if (gotoLine > 0 && At(active_)) At(active_)->editor.GotoLine(gotoLine);

    if (addRecent) AddRecentFile(path);
    // InsertDoc activated the tab already; report OPENED last, after the
    // document is fully constructed, so receivers can query editor state.
    if (onDocumentOpened) {
        Document* opened = At(active_);
        if (opened) onDocumentOpened(opened);
    }
}

// --- save ---------------------------------------------------------------------

bool Workspace::Save() {
    Document* d = Active();
    if (!d) return false;
    if (!d->HasPath()) return SaveAs();
    if (!WriteDocument(d, d->path)) return false;
    SyncTabTitles();
    UpdateUi();
    return true;
}

bool Workspace::SaveAt(int index) {
    Document* d = At(index);
    if (!d || !d->HasPath()) return false;   // untitled: skip in batch ops
    if (!d->editor.Modified()) return true;
    if (!WriteDocument(d, d->path)) return false;
    SyncTabTitles();
    UpdateUi();
    return true;
}

bool Workspace::SaveAt1(int index) {
    Document* d = At1(index);
    if (!d || !d->HasPath()) return false;
    if (!d->editor.Modified()) return true;
    if (!WriteDocument(d, d->path)) return false;
    SyncTabTitles();
    UpdateUi();
    return true;
}

bool Workspace::SaveAs() {
    Document* d = Active();
    if (!d) return false;

    wchar_t fileBuf[MAX_PATH * 2] = {};
    if (d->HasPath()) {
        wcscpy_s(fileBuf, d->path.c_str());
    } else if (!d->DisplayName().empty()) {
        wcscpy_s(fileBuf, d->DisplayName().c_str());
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host_.Hwnd();
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return false;

    std::wstring newPath(fileBuf);
    if (!WriteDocument(d, NormalizePath(newPath))) return false;

    d->editor.SetLexerForFile(d->path.wstring(), theme_);
    AddRecentFile(d->path.wstring());
    SyncTabTitles();
    UpdateUi();
    Logger::Info("Saved as " + WideToUtf8(newPath));
    return true;
}

bool Workspace::WriteDocument(Document* doc, const std::filesystem::path& path) {
    // streaming save: pull UTF-8 chunks out of Scintilla and re-encode on the
    // fly — peak memory stays at one chunk instead of two full-size copies
    // (Scintilla text + encoded bytes) for documents of any size.
    HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        wchar_t msg[512];
        swprintf_s(msg, L"Cannot save file:\n%s\n(Error %lu)", path.c_str(), err);
        MessageBoxW(host_.Hwnd(), msg, L"xfsWinPad", MB_OK | MB_ICONERROR);
        Logger::Error("Save failed (open): " + WideToUtf8(path.wstring()) +
                      " gle=" + std::to_string(err));
        return false;
    }

    auto t0 = std::chrono::steady_clock::now();
    auto writeAll = [&](const char* p, size_t n) -> bool {
        while (n > 0) {
            DWORD w = 0;
            DWORD cap = (DWORD)(std::min)(n, (size_t)(64u << 20));
            if (!::WriteFile(h, p, cap, &w, nullptr) || w == 0) return false;
            p += w;
            n -= w;
        }
        return true;
    };

    bool ok = true;
    long long totalIn = 0, totalOut = 0;
    const encoding::EncodingType t = doc->encoding;
    // UTF8BOM/UTF16LE/UTF16BE: the encoder emits the BOM once (EmitBomOnce);
    // plain UTF-8 needs none.

    if (ok) {
        const size_t kChunk = 8u << 20;
        const sptr_t total = doc->editor.Send(SCI_GETLENGTH);
        sptr_t pos = 0;
        if (t == encoding::EncodingType::UTF8) {
            std::string chunk;
            while (ok && pos < total) {
                doc->editor.GetTextRangeUtf8(pos, kChunk, chunk);
                if (chunk.empty()) break;
                ok = writeAll(chunk.data(), chunk.size());
                pos += (sptr_t)chunk.size();
                totalOut += (long long)chunk.size();
            }
            totalIn = pos;
        } else {
            StreamEncoder enc(t);
            std::string chunk, encOut;
            while (ok && pos < total) {
                doc->editor.GetTextRangeUtf8(pos, kChunk, chunk);
                if (chunk.empty()) break;
                encOut.clear();
                enc.Feed(chunk.data(), chunk.size(), encOut);
                ok = writeAll(encOut.data(), encOut.size());
                pos += (sptr_t)chunk.size();
                totalIn += (long long)chunk.size();
                totalOut += (long long)encOut.size();
            }
            if (ok) {
                encOut.clear();
                enc.Flush(encOut);
                if (!encOut.empty()) {
                    ok = writeAll(encOut.data(), encOut.size());
                    totalOut += (long long)encOut.size();
                }
            }
        }
    }
    ::CloseHandle(h);

    if (!ok) {
        DWORD err = GetLastError();
        wchar_t msg[512];
        swprintf_s(msg, L"Cannot save file:\n%s\n(Error %lu)", path.c_str(), err);
        MessageBoxW(host_.Hwnd(), msg, L"xfsWinPad", MB_OK | MB_ICONERROR);
        Logger::Error("Save failed (write): " + WideToUtf8(path.wstring()) +
                      " gle=" + std::to_string(err));
        return false;
    }
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    Logger::Info("Saved streaming in=" + std::to_string(totalIn) + " out=" +
                 std::to_string(totalOut) + " enc=" +
                 WideToUtf8(encoding::DisplayName(t)) + " " +
                 std::to_string((long long)ms) + " ms");

    bool wasPathless = !doc->HasPath();
    doc->path = path;
    doc->editor.SetSavePoint();
    if (wasPathless || doc->readOnly)
        Logger::Info("Saved " + WideToUtf8(path.wstring()));
    if (onDocumentSaved) onDocumentSaved(doc);
    return true;
}

bool Workspace::SaveAll() {
    bool allOk = true;
    // right/other view first, then the left/primary view
    for (int i = 0; i < (int)docs1_.size(); ++i) {
        Document* d = docs1_[i].get();
        if (!d->editor.Modified()) continue;
        if (!d->HasPath()) { ActivateView1(i); allOk = SaveAs() && allOk; continue; }
        allOk = WriteDocument(d, d->path) && allOk;
    }
    for (int i = 0; i < (int)docs_.size(); ++i) {
        Document* d = docs_[i].get();
        if (!d->editor.Modified()) continue;
        if (!d->HasPath()) { Activate(i); allOk = SaveAs() && allOk; continue; }
        allOk = WriteDocument(d, d->path) && allOk;
    }
    SyncTabTitles();
    UpdateUi();
    return allOk;
}

void Workspace::ApplyThemeToAll(const ThemeDef& theme) {
    theme_ = &theme;
    for (auto& d : docs_) {
        d->editor.ApplyTheme(theme);
        ReapplyLexer(d.get());
    }
    for (auto& d : docs1_) {
        d->editor.ApplyTheme(theme);
        ReapplyLexer(d.get());
    }
    UpdateUi();
}

// 重挂词法分析器：优先尊重用户在语言菜单里的手动选择（langIndex），
// 否则按文件名探测。主题切换会重建样式，若一律 SetLexerForFile 会把
// 用户手动选的语言悄悄重置回扩展名默认（2026-08-29 用户报告）。
void Workspace::ReapplyLexer(Document* d) {
    if (d->langIndex >= 0) {
        const LanguageMenuItem* catalog = LanguageMenuCatalog();
        const LanguageMenuItem* entry = catalog + d->langIndex;
        if (entry->label) {
            const char* kw[2] = { entry->keywords[0], entry->keywords[1] };
            d->editor.SetLexerByName(entry->lexerName, kw, theme_);
            return;
        }
    }
    d->editor.SetLexerForFile(d->HasPath() ? d->path.wstring()
                                           : std::wstring(), theme_);
}

// --- reload / encoding ---------------------------------------------------------

bool Workspace::ReloadActive() { return DoReloadActive(nullptr); }

bool Workspace::ReloadDocument(int index, bool alert) {
    Document* d = At(index);
    if (!d || !d->HasPath()) return false;
    Activate(index);
    return DoReloadActive(nullptr, alert);
}

bool Workspace::ReloadDocumentDirect(Document* d) {
    if (!d || !d->HasPath()) return false;
    std::string raw;
    if (!ReadFileBytes(d->path.wstring(), raw)) {
        Logger::Warn("ReloadDocumentDirect: read failed: " +
                     WideToUtf8(d->path.wstring()));
        return false;
    }
    DecodedText dec = encoding::DecodeToUtf8(raw);
    // 保持光标所在行（离屏文档同样有效）
    int keepLine = (int)d->editor.Send(SCI_LINEFROMPOSITION,
                                       d->editor.Send(SCI_GETCURRENTPOS));
    d->encoding = dec.encoding;
    d->editor.SetEol(DetectEol(dec.utf8));
    d->editor.SetTextUtf8(dec.utf8);
    d->editor.SetSavePoint();
    d->editor.GotoLine(keepLine + 1);
    UpdateUi();
    return true;
}

void Workspace::ReloadActiveAs(encoding::EncodingType forced) {
    DoReloadActive(&forced);
}

bool Workspace::DoReloadActive(const encoding::EncodingType* forced,
                               bool allowPrompt) {
    Document* d = Active();
    if (!d) return false;
    if (!d->HasPath()) {
        MessageBoxW(host_.Hwnd(),
                    L"This document has no file on disk.\nSave it first to reload.",
                    L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (d->editor.Modified()) {
        if (!allowPrompt) {
            Logger::Info("DoReloadActive: silent reload discards unsaved changes "
                         "(plugin request)");
        } else {
            wchar_t msg[1024];
            swprintf_s(msg, L"Reloading will discard unsaved changes to:\n%s\n\nContinue?",
                       d->path.c_str());
            if (MessageBoxW(host_.Hwnd(), msg, L"xfsWinPad",
                            MB_YESNO | MB_ICONWARNING) != IDYES)
                return false;
        }
    }

    std::string raw;
    if (!ReadFileBytes(d->path.wstring(), raw)) {
        MessageBoxW(host_.Hwnd(),
                    (L"Cannot read file:\n" + d->path.wstring()).c_str(),
                    L"xfsWinPad", MB_OK | MB_ICONERROR);
        return false;
    }

    int keepLine = (int)d->editor.Send(SCI_LINEFROMPOSITION,
                                       d->editor.Send(SCI_GETCURRENTPOS));

    DecodedText dec = forced ? encoding::DecodeWithEncoding(raw, *forced)
                             : encoding::DecodeToUtf8(raw);
    d->encoding = dec.encoding;
    d->editor.SetEol(DetectEol(dec.utf8));
    d->editor.SetTextUtf8(dec.utf8);
    d->editor.SetReadOnly(d->readOnly);
    d->editor.GotoLine(keepLine + 1);

    UpdateUi();
    Logger::Info(std::string("Reloaded (") +
                 (forced ? "forced" : "auto") + ")");
    return true;
}

void Workspace::SetActiveEncoding(encoding::EncodingType t) {
    Document* d = Active();
    if (!d || d->encoding == t) return;
    d->encoding = t;   // representation used by the next save
    Logger::Info("Target encoding set: " +
                 WideToUtf8(encoding::DisplayName(t)));
    UpdateUi();
}

// --- close ----------------------------------------------------------------------

bool Workspace::PromptSaveIfDirty(int index) {
    Document* d = At(index);
    if (!d || !d->editor.Modified()) return true;
    Activate(index);
    wchar_t msg[1024];
    swprintf_s(msg, L"Save changes to %s?", d->DisplayName().c_str());
    int r = MessageBoxW(host_.Hwnd(), msg, L"xfsWinPad", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return Save();
    return true;
}

bool Workspace::Close(int index) {
    Document* d = At(index);
    if (!d) return false;
    if (d->locked) {
        Logger::Info("Close refused: tab is locked");
        return false;
    }
    if (!PromptSaveIfDirty(index)) return false;
    // 入恢复栈（恢复关闭的标签）；untitled 文档无盘路径不入栈
    if (d->HasPath()) {
        EditorStatus st = d->editor.Status();
        closedTabs_.push_back({d->path.wstring(), st.line, d->view});
        if (closedTabs_.size() > 10) closedTabs_.erase(closedTabs_.begin());
    }
    if (diffView_.Contains(d))
        ExitCompareMode();
    d->editor.Destroy();
    RemoveAt(index);
    if (onDocumentClosed) onDocumentClosed(d);

    if (docs_.empty()) {
        active_ = -1;
        NewDocument();
    } else {
        Activate((std::min)(index, (int)docs_.size() - 1));
    }
    UpdateUi();
    return true;
}

bool Workspace::PromptSaveIfDirty1(int index) {
    Document* d = At1(index);
    if (!d || !d->editor.Modified()) return true;
    ActivateView1(index);
    wchar_t msg[1024];
    swprintf_s(msg, L"Save changes to %s?", d->DisplayName().c_str());
    int r = MessageBoxW(host_.Hwnd(), msg, L"xfsWinPad", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return Save();
    return true;
}

bool Workspace::Close1(int index) {
    Document* d = At1(index);
    if (!d) return false;
    if (d->locked) {
        Logger::Info("Close1 refused: tab is locked");
        return false;
    }
    if (!PromptSaveIfDirty1(index)) return false;
    if (d->HasPath()) {
        EditorStatus st = d->editor.Status();
        closedTabs_.push_back({d->path.wstring(), st.line, 1});
        if (closedTabs_.size() > 10) closedTabs_.erase(closedTabs_.begin());
    }
    d->editor.Destroy();
    RemoveAt1(index);
    if (onDocumentClosed) onDocumentClosed(d);
    if (docs1_.empty()) {
        active1_ = -1;
        OnSplitToggledOff();
    } else {
        if (active1_ >= (int)docs1_.size()) active1_ = (int)docs1_.size() - 1;
        ActivateView1((std::min)(index, (int)docs1_.size() - 1));
    }
    UpdateUi();
    return true;
}

bool Workspace::CloseAll(bool keepOneDoc) {
    ExitCompareMode();
    // close right/other view docs first（锁定标签跳过）
    for (int i = (int)docs1_.size() - 1; i >= 0; --i) {
        Document* d1 = At1(i);
        if (d1 && d1->locked) continue;
        if (!PromptSaveIfDirty1(i)) return false;
        Document* d = At1(i);
        d->editor.Destroy();
        RemoveAt1(i);
        if (onDocumentClosed) onDocumentClosed(d);
    }
    active1_ = -1;
    for (int i = (int)docs_.size() - 1; i >= 0; --i) {
        Document* d0 = docs_[i].get();
        if (d0 && d0->locked) continue;
        if (!PromptSaveIfDirty(i)) return false;
        Document* d = At(i);
        d->editor.Destroy();
        RemoveAt(i);
        if (onDocumentClosed) onDocumentClosed(d);
    }
    active_ = -1;
    currentView_ = 0;
    if (keepOneDoc)
        NewDocument();
    host_.Relayout();
    UpdateUi();
    return true;
}

bool Workspace::ReopenClosedTab() {
    if (closedTabs_.empty()) return false;
    ClosedTab c = closedTabs_.back();
    closedTabs_.pop_back();
    OpenPath(c.path, c.line + 1);      // 恢复到关闭时的光标行
    if (c.view == 1 && SplitActive())
        MoveActiveToOtherView();       // 原在右视图：开在左视图后移回去
    Logger::Info("Reopened closed tab: " + WideToUtf8(c.path));
    return true;
}

bool Workspace::CloseUnmodified(int view) {
    bool closedAny = false;
    if (view == 1) {
        for (int i = (int)docs1_.size() - 1; i >= 0; --i) {
            Document* d = At1(i);
            if (!d || d->locked || d->editor.Modified()) continue;
            d->editor.Destroy();
            RemoveAt1(i);
            if (onDocumentClosed) onDocumentClosed(d);
            closedAny = true;
        }
        if (docs1_.empty()) OnSplitToggledOff();
        else ActivateView1((std::min)(active1_, (int)docs1_.size() - 1));
    } else {
        for (int i = (int)docs_.size() - 1; i >= 0; --i) {
            Document* d = docs_[i].get();
            if (!d || d->locked || d->editor.Modified()) continue;
            d->editor.Destroy();
            RemoveAt(i);
            if (onDocumentClosed) onDocumentClosed(d);
            closedAny = true;
        }
        if (docs_.empty()) NewDocument();
        else Activate((std::min)(active_, (int)docs_.size() - 1));
    }
    UpdateUi();
    return closedAny;
}

// --- recent files --------------------------------------------------------------

static std::wstring RecentFilePath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData;
        CoTaskMemFree(appData);
    }
    CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    return base + L"\\xfsWinPad\\recent.txt";
}

void Workspace::AddRecentFile(const std::wstring& path) {
    std::vector<std::wstring> items;
    items.push_back(path);
    int max = prefs_ ? (std::max)(1, (std::min)(30, prefs_->recentFilesMax)) : 10;
    std::string content;
    if (ReadFileBytes(RecentFilePath(), content)) {
        size_t start = 0;
        while (start < content.size()) {
            size_t end = content.find('\n', start);
            if (end == std::wstring::npos) end = content.size();
            std::string line = content.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) {
                std::wstring w = Utf8ToWide(line);
                if (w != path) items.push_back(w);
                if ((int)items.size() >= max) break;
            }
            start = end + 1;
        }
    }
    std::string out;
    for (auto& s : items) out += WideToUtf8(s) + "\r\n";
    WriteFileBytes(RecentFilePath(), out.data(), out.size());
    host_.RebuildRecentMenu(items);
}

// --- UI sync ---------------------------------------------------------------------

void Workspace::UpdateUi() {
    SyncTabTitles();
    host_.OnWorkspaceChanged();
}

} // namespace xfs
