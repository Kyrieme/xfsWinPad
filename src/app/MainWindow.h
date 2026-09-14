#pragma once
// xfsWinPad - MainWindow: frame window, menus, accelerators, layout

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include "StatusBar.h"
#include "FindDialog.h"
#include "ResultsPanel.h"
#include "CommandPalette.h"
#include "FileExplorer.h"
#include "PreferencesDialog.h"
#include "StyleConfiguratorDialog.h"
#include "ShortcutMapperDialog.h"
#include "StdfPanel.h"
#include "CsvPanel.h"
#include "../bigfile/BigFileView.h"
#include "../git/GitClient.h"
#include "../hex/HexPanel.h"
#include "../log/LogPanel.h"
#include "../terminal/TerminalPanel.h"
#include "../ai/AiPanel.h"
#include "../plugin/PluginManager.h"
#include "../plugin/Workshop.h"
#include "../plugin/DockManager.h"
#include "../plugin/PluginAdminDialog.h"
#include "../macro/MacroRecorder.h"
#include "../settings/Settings.h"
#include "../settings/Profile.h"
#include "../theme/Theme.h"
#include "../workspace/Workspace.h"

namespace xfs {

struct StartupOptions {
    std::vector<std::wstring> files;
    int gotoLine = -1;
    bool readOnly = false;
    std::wstring search;
    bool autoDiff = false;   // --diff: compare the first two opened files
    std::wstring logFile;    // --log <file>: open in the log analyzer panel
    bool forceNew = false;   // --new: bypass single-instance forwarding
};

// 解析命令行（wWinMain 与 WM_COPYDATA 单实例转发共用同一实现）
StartupOptions ParseCommandLine(LPCWSTR cmd);

class MainWindow : public IPrefsApplier, public IStyleApplier, public IShortcutChange {
public:
    ~MainWindow();

    bool Create(HINSTANCE hInst, const StartupOptions& opts);
    int  RunMessageLoop();
    HWND Hwnd() const { return hwnd_; }
    Workspace& GetWorkspace() { return *workspace_; }
    // installs the macro key-hook onto a freshly created editor
    void AttachMacroHook(class Editor* ed);
    // AI 快捷命令（编辑器右键菜单/斜杠命令共用）：kind = explain/fix/
    // refactor/translate/summary。面板未开时先开；busy 时丢弃并提示。
    void AiQuickCommand(const std::wstring& kind);
    // 编辑器右键菜单（基础编辑 + AI 快捷组）；Editor::SetContextMenu 回调进来
    void ShowEditorContextMenu(class Editor* ed, int sx, int sy);

    // IPrefsApplier（首选项对话框实时应用通道）
    void ApplyAll(const AppSettings& s) override;
    // IStyleApplier（样式配置器实时应用通道）
    void RestyleAll() override;
    bool SwitchThemeByName(const wchar_t* name) override;
    const wchar_t* CurrentThemeName() override;
    // IShortcutChange（快捷键映射器变更后重建加速器并落盘）
    void OnShortcutChanged() override;

    // called by Workspace
    void OnWorkspaceChanged();                       // title + status bar refresh
    void Relayout();                                 // reposition all children
    void MoveSplitter(int xAbs);                     // drag the split view divider
    // hover state for the split divider (highlighted while hovered/dragging)
    bool SplitterHot() const { return splitterHot_; }
    void SetSplitterHot(bool b) { splitterHot_ = b; }
    void RebuildRecentMenu(const std::vector<std::wstring>& items);
    HMENU Menu() const { return menu_; }
    // recent folders (project roots) — owned here since it drives explorer
    void RebuildRecentFolderMenu(const std::vector<std::wstring>& items);
    void AddRecentFolder(const std::wstring& dir);
    // open the active document in a brand-new xfsWinPad window (used by tab menus)
    void MoveCurrentToNewWindow();
    // per-tab painting callback wired from Workspace (see TabBar::SetDrawHandler)
    void DrawTabItem(HDC hdc, class TabBar& tabs, int itemID, const RECT& rc,
                     bool selected);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    bool RegisterWindowClass();
    void BuildMenus();
    void BuildAccelerators();
    void ApplyLanguage();
    void CreateToolbar();
    void LayoutChildren();
    void UpdateTitleBar();
    void UpdateStatusBar();
    void UpdateUndoRedoState();   // 工具栏撤销/重做按钮禁用态（无操作/到底变灰）
    void DoGotoLine();
    void HandleSearchAction(SearchAction act);
    void ShowSearchResults(std::vector<SearchHit> hits);
    void OnResultActivate(int row);
    void ShowFindDialog(const std::wstring& prefill = L"", int pageIndex = 0);
    void InsertDateTime();
    void ToggleWordWrap();
    void ToggleExplorer();
    void OpenFolderDialog();
    void SetProjectRoot(const std::wstring& dir, bool persist);
    void CloseFolder();
    void ToggleHexView();
    void ToggleStdfView();
    void ToggleCsvView();
    void ToggleBigFileView(const std::wstring& forcedPath = std::wstring());
    void ToggleLogPanel();
    void ToggleTerminal();
    void ToggleAiPanel();
    AiContext BuildAiContext();
    // StdfPanel「AI 分析」（批次 28）：统计块喂给 AiPanel 问失效原因
    void AiAskStdf();
    void SnapDiskState();       // AI 发送前：记录活跃文档磁盘 mtime/size
    void AutoReloadChanged();   // AI 响应后：延迟 3s 对比（等 write 收尾）
    void AutoReloadChangedNow();
    // AI 插件工场：/plugin <描述> 指令编排（脚手架+提示词+安装钩子）
    void RunPluginWorkshop(const std::wstring& taskDesc);
    std::unique_ptr<Workshop> workshop_;
    std::wstring workshopPending_;   // 进行中的工场项目目录（响应完成后安装）
    int workshopRetries_ = 0;        // 安装失败自动回喂修复的次数（上限 3）
    void RebuildPluginMenu();
    void DoDiffCompare();
    void RunDiffCompare(Document* a, Document* b);
    // git 集成 v1（批次 46）：分支进标题 + 树着色 + 与 HEAD 比较
    void GitCompareWithHead(const std::wstring& absPath);
    void SyncGitUi();                 // explorer colors + title after a refresh
    void OnGitDone(GitSnapshot* snap);    // WM_APP_GIT_DONE
    void OnGitBlob(GitBlobResult* res);   // WM_APP_GIT_BLOB
    // 批次 48：暂存/取消暂存/提交
    void WireExplorerGit();               // set git callbacks on a fresh explorer
    void GitStagePath(const std::wstring& absPath, bool unstage);
    void GitCommitDialog();
    void OnGitOp(GitOpResult* res);       // WM_APP_GIT_OP
    void SwitchTheme(const ThemeDef* t);
    void DoAutoSave();
    void CheckAutoSaveRecovery();
    void PrintDocument();
    void EditCmd(unsigned int cmd);
    void ExecuteCommand(unsigned int id);

    void ToggleMacroRecord();
    void DoMacroPlayback();
    void DoMacroSave();
    void DoMacroLoad();
    void DoMacroLoadFrom(const std::wstring& path);   // CLI .xfm 双击直达共用
    std::wstring MacroDir() const;
    void RunThemeImportFrom(const std::wstring& path); // CLI 主题 json 直达共用
    bool OpenSpecialFile(const std::wstring& f);   // .xfm/theme-json 分派
    void OpenUserFile(const std::wstring& path);   // 面板双击/最近文件/拖放统一入口
    void RunPluginAdmin();
    void RunPreferences();     // 设置 > 首选项…（docs/settings-plan.md 阶段 1）
    void RunStyleConfigurator();  // 设置 > 语言样式配置器…（阶段 2b）
    void RunShortcutMapper();     // 设置 > 管理快捷键…（阶段 3）
    void RunThemeExport();        // 设置 > 导出主题 JSON…（阶段 4）
    void RunThemeImport();        // 设置 > 导入主题 JSON…（阶段 4）
    void RunConfigExport();       // 设置 > 导出配置 profile…（换机迁移）
    void RunConfigImport();       // 设置 > 导入配置 profile…（重启生效）
    void RunWindowList();         // 窗口 > 窗口列表…（N++ WindowsDlg 风格）
    void RebuildWindowMenuItems();   // 窗口菜单动态文档条目（OnWorkspaceChanged）
    void SpawnWindow(const std::wstring& file); // launch a second xfsWinPad.exe
    void OpenCliFiles(const StartupOptions& opts); // 单实例转发/启动共用：开文件进标签

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND editorHost_ = nullptr;
    HWND editorHost1_ = nullptr;    // right/other view host (split view)
    HWND splitter_ = nullptr;       // draggable divider between the two views
    int splitterPosLogical_ = 50;   // left pane width % when split active
    bool splitterHot_ = false;      // divider hovered/dragged → highlighted
    HWND toolbar_ = nullptr;
    HWND toolbarTip_ = nullptr;
    std::vector<TOOLINFOW> toolbarTipTools_;
    std::vector<std::wstring> toolbarTipTexts_;
    std::wstring toolbarTipCur_;
    bool toolbarTipShown_ = false;
    int hotBtn_ = -1;
    static LRESULT CALLBACK ToolbarMouseProc(HWND, UINT, WPARAM, LPARAM,
                                             UINT_PTR, DWORD_PTR);
    HACCEL accel_ = nullptr;
    HMENU menu_ = nullptr;
    HMENU recentMenu_ = nullptr;
    std::vector<std::wstring> recentItems_;
    HMENU recentFolderMenu_ = nullptr;
    std::vector<std::wstring> recentFolderItems_;
    HMENU pluginMenu_ = nullptr;
    HMENU windowMenu_ = nullptr;         // top-level "Window" menu
    HMENU languageMenu_ = nullptr;       // top-level "Language" menu
    std::vector<HMENU> pluginPopups_;    // NPP 分组子菜单（重建时销毁防泄漏）
    unsigned pluginEvtPending_ = 0;      // coalesced XFS_EVT_* bits for the
                                         // 250 ms plugin-host timer (id 2)

    std::unique_ptr<Workspace> workspace_;
    std::unique_ptr<StatusBar> status_;
    std::unique_ptr<FindDialog> findDlg_;
    std::unique_ptr<ResultsPanel> results_;
    std::unique_ptr<CommandPalette> palette_;
    std::unique_ptr<FileExplorer> explorer_;
    GitClient git_;
    bool gitPickMerge_ = false;   // next branch picker result -> merge target
    std::unique_ptr<HexPanel> hex_;
    std::unique_ptr<StdfPanel> stdf_;
    std::unique_ptr<CsvPanel> csv_;
    std::unique_ptr<BigFileView> bigfile_;
    std::unique_ptr<LogPanel> logPanel_;
    std::unique_ptr<TerminalPanel> terminal_;
    std::unique_ptr<AiPanel> ai_;
    // AI 改盘自动重载：发送前快照 → 响应后轮询对比（write 收尾延迟 3~5s）
    std::map<std::wstring, std::pair<unsigned long long, unsigned long long>> aiDiskSnap_;
    int aiReloadTries_ = 0;
    // 未保存文档的 AI 写入桥：untitledName → 镜像临时文件路径。
    // 无磁盘路径的缓冲区没有 write 工具可操作的目标——发送前全文镜像到
    // %TEMP%\xfsWinPad\，上下文给 AI 镜像路径，AI 改镜像，落盘轮询后读回
    // 填充缓冲区（文档保持 untitled 状态）。
    std::map<std::wstring, std::wstring> aiMirrorByDoc_;
    bool RefreshUntitledMirror(Document* d);   // 全文写镜像，返回是否成功
    std::unique_ptr<PluginManager> plugins_;
    std::unique_ptr<DockManager> dockMgr_;   // NPP 插件可停靠面板（4d）
int hexHLogical_ = 230;              // panel height at 96 dpi, splitter-adjustable
int stdfHLogical_ = 260;
int csvHLogical_ = 320;              // CSV 表格视图高度（批次 32）
int bigfileHLogical_ = 320;             // STDF panel height at 96 dpi, splitter-adjustable
int rpHLogical_ = 190;               // results panel height at 96 dpi, splitter-adjustable
int logHLogical_ = 300;              // log panel height at 96 dpi, splitter-adjustable
int termHLogical_ = 260;             // terminal panel height at 96 dpi
int aiWLogical_ = 360;               // AI panel width at 96 dpi, splitter-adjustable

    MacroRecorder macro_;

    AppSettings settings_;
    const ThemeDef* theme_ = nullptr;
    StartupOptions startup_;
};

} // namespace xfs
