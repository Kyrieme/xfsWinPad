#pragma once
// xfsWinPad - Workspace: owns all open documents and tab/file operations

#include "../document/Document.h"
#include "../tabs/TabBar.h"
#include "../search/SearchService.h"
#include "../diff/DiffView.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <set>
#include <vector>
#include <memory>

namespace xfs {

struct AppSettings;
struct ThemeDef;
class MainWindow;

class Workspace {
public:
    explicit Workspace(MainWindow& host);

    // 恢复关闭的标签（undo 栈，最近优先；untitled 文档不入栈）
    struct ClosedTab { std::wstring path; int line; int view; };

    void Init(HWND editorHost, HINSTANCE hInst,
              const AppSettings& prefs, const ThemeDef& theme,
              bool createInitialDoc = true);
    void ApplyThemeToAll(const ThemeDef& theme);
    // 首选项实时应用：对两个视图的全部编辑器 ApplyPrefs(+主题重刷)。
    void ApplyPrefsToAll(const AppSettings& prefs);
    void Layout(int x, int y, int w, int h);
    void LayoutRight(int w, int h);   // layout the right/other view's tab+editor

    // --- document lifecycle ---
    void NewDocument();
    void OpenFileDialog();
    void OpenPath(const std::wstring& path, int gotoLine = -1,
                  bool readOnly = false, bool addRecent = true);
    bool Save();                       // active document
    bool SaveAs();
    bool SaveAt(int index);            // specific document (view 0), untitled skipped
    bool SaveAt1(int index);           // specific document (view 1)
    bool SaveAll();
    bool ReloadActive();               // re-read file from disk (auto encoding)
    void ReloadActiveAs(encoding::EncodingType forced);   // reload with forced decoding
    // NPP 兼容通道用：按索引重载。alert=true 时脏文档先确认；false 时静默丢弃未保存修改。
    bool ReloadDocument(int index, bool alert);
    // 静默回填：读盘 → 解码 → 直接写入该文档编辑器（不激活 tab、不切视图、
    // 保持光标行）。AI 改盘自动重载用——view 0/1 统一路径，不抢焦点。
    bool ReloadDocumentDirect(Document* d);
    void SetActiveEncoding(encoding::EncodingType t);     // applies on next save
    bool Close(int index);
    bool CloseActive() { return Close(active_); }
    bool CloseAll(bool keepOneDoc = true);
    void Activate(int index);
    void MoveDocument(int from, int to);

    // --- tab recovery / batch close（阶段 3.5 Tab 菜单补全）---
    bool ReopenClosedTab();                    // 弹出 undo 栈顶并按原视图重开
    bool HasClosedTabs() const { return !closedTabs_.empty(); }
    bool CloseUnmodified(int view);            // 关闭当前视图所有未修改（锁定跳过）

    // --- split view (move-to-other-view) ---
    void SetRightHost(HWND h);              // host for the right/other view
    void MoveActiveToOtherView();           // active doc 0 <-> 1
    bool SplitActive() const { return !docs1_.empty(); }
    int  CurrentView() const { return currentView_; }
    void SetActiveDoc(Document* d);         // track which view is active
    void ActivateView1(int index);          // activate tab in the right view
    bool Close1(int index);                 // close a tab in the right view
    int  Count1() const { return (int)docs1_.size(); }
    int  ActiveIndex1() const { return active1_; }
    Document* ActiveIn(int view);
    Document* FindByTabIndex1(int index) { return At1(index); }
    TabBar& Tabs1() { return tabs1_; }
    Document* Active1() { return At1(active1_); }

    Document* Active();
    int ActiveIndex() const { return active_; }
    // 批次 31：其它已开文档（两视图，排除 exclude）词汇注入 out。
    // 单文档 >512KB 跳过、总量 3000 词封顶——自动补全触发路径上的开销护栏。
    void CollectOpenTabWords(std::set<std::string>& out, const Document* exclude);
    Document* FindByEditor(HWND editorHwnd);
    Document* FindByPath(const std::wstring& absPath) const;   // 全路径匹配（两视图）
    int IndexOf(const Document* d) const;    // view-0 index or -1
    int IndexOf1(const Document* d) const;   // view-1 index or -1
    Document* FindByTabIndex(int index) { return At(index); }
    Document* DocumentAt(int index) { return At(index); }   // results panel access
    const std::wstring& LastError() const { return lastError_; }

    int Count() const { return (int)docs_.size(); }
    TabBar& Tabs() { return tabs_; }
    void SyncTabTitles();

    // invoked after a document has been closed (pointer no longer valid)
    // fired for files beyond the editable load cap (kMaxLoadBytes) so the host
    // can route them to the read-only big-file viewer instead of Scintilla
    std::function<void(const std::wstring& path, unsigned long long size)> onOversizeFile;

    std::function<void(const Document*)> onDocumentClosed;
    // lifecycle event sinks (wired by MainWindow to the plugin host)
    std::function<void(const Document*)> onDocumentOpened;
    std::function<void(const Document*)> onDocumentActivated;
    std::function<void(const Document*)> onDocumentSaved;

    // --- side-by-side diff compare ---
    void EnterCompareMode(Document* a, Document* b,
                          std::vector<int> mapA, std::vector<int> mapB);
    void ExitCompareMode();
    DiffView& Diff() { return diffView_; }

    FindState& Find() { return findState_; }

private:
    Document* At(int index);
    // 主题切换后重挂词法分析器（尊重语言菜单的手动选择 langIndex）。
    void ReapplyLexer(Document* d);
    Document* At1(int index) {
        return (index >= 0 && index < (int)docs1_.size())
                   ? docs1_[index].get() : nullptr;
    }
    void InsertDoc(std::unique_ptr<Document> doc, bool activate);
    void RemoveAt(int index);
    void InsertDoc1(std::unique_ptr<Document> doc, bool activate);
    void RemoveAt1(int index);
    void MoveDocument1(int from, int to);
    void OnSplitToggledOn();    // first doc entered the right view
    void OnSplitToggledOff();   // right view became empty
    void UpdateUi();
    bool PromptSaveIfDirty(int index);   // returns false when cancelled
    bool PromptSaveIfDirty1(int index);
    // 批次 31：给新文档编辑器挂跨标签词汇源
    void WireExtWords(Document* d);
    bool WriteDocument(Document* doc, const std::filesystem::path& path);
    void AddRecentFile(const std::wstring& path);
    bool DoReloadActive(const encoding::EncodingType* forced,
                        bool allowPrompt = true);
    // Position each view's active editor to fill its host and hide its siblings.
    void LayoutView(int view);
    void SetParentReparent(Document* d);

    MainWindow& host_;
    HWND editorHost_ = nullptr;
    HWND editorHost1_ = nullptr;   // right view host (null until split ready)
    HINSTANCE inst_ = nullptr;
    const AppSettings* prefs_ = nullptr;
    const ThemeDef* theme_ = nullptr;
    TabBar tabs_;
    TabBar tabs1_;
    std::vector<std::unique_ptr<Document>> docs_;
    std::vector<std::unique_ptr<Document>> docs1_;   // right/other view docs
    DiffView diffView_;
    int active_ = -1;
    int active1_ = -1;
    int untitledCounter_ = 0;
    int untitledCounter1_ = 0;
    int currentView_ = 0;   // which view Active() resolves against
    FindState findState_;
    std::vector<ClosedTab> closedTabs_;   // 恢复关闭标签 undo 栈（≤10）
    std::wstring lastError_;
};

} // namespace xfs
