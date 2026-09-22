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
#include "DiagnosticsPanel.h"
#include "CompilePanel.h"
#include "../language/CraftRunner.h"   // craft::BuildResult / Toolchain / BuildStep
#include "../core/NavHistory.h"        // 批次 105：导航历史（上一处/下一处）
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
    bool firstInstance = true;  // set by wWinMain: owns the legacy session.json
    bool noRestore = false;  // --no-restore: start blank, skip session restore
    std::wstring restoreFile;   // --restore <file>: load this session slot only
};

// 解析命令行（wWinMain 与 WM_COPYDATA 单实例转发共用同一实现）
StartupOptions ParseCommandLine(LPCWSTR cmd);

// 会话恢复跳行（wParam/lParam=0）：RestoreSession 攒好 PendingJump 后 Post，
// 消息循环侧 ApplyRestoreJumps 统一应用，避免编辑器未挂载时 SCI 跳行丢失。
constexpr UINT WM_APP_RESTOREJUMP = WM_APP + 90;
// 批次 96：后台编译线程跑完 → lParam = craft::BuildResult*（接收方 delete）。
// 用堆指针而不是塞进 WPARAM 的原因是结果里有整个输出文本与逐行解析，
// 不是几个整数能装下的（与 WM_APP_GIT_DONE 同口径）。
constexpr UINT WM_APP_COMPILE_DONE = WM_APP + 91;

class MainWindow : public IPrefsApplier, public IStyleApplier, public IShortcutChange {
public:
    ~MainWindow();

    bool Create(HINSTANCE hInst, const StartupOptions& opts);
    int  RunMessageLoop();
    HWND Hwnd() const { return hwnd_; }
    bool StartMaximized() const { return settings_.winMax; }
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
    void ApplyRestoreJumps();   // WM_APP_RESTOREJUMP：窗口消息循环开始后再跳恢复行
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
    // --- 批次 87：Chroma 3380 静态检查 --------------------------------------
    // 内核在 src/language/Chroma3380Diagnostics.{h,cpp}（纯函数、可单测），
    // 这里只负责三件事：什么时候跑、把结果画到编辑器（波浪线）、把列表交给面板。
    void ToggleDiagnostics();          // View > 诊断面板（底部列表）
    void ToggleChromaCheck();          // View > Chroma 静态检查（勾选：是否校验+标红）
    void RefreshDiagnostics();         // 重扫活动文档：标红 + 面板 + 状态栏计数
    void ScheduleDiagnostics();        // 编辑防抖：延迟合并一次重扫
    void ClearDiagnosticsEverywhere(); // 关功能时清掉所有已打开文档的标记
    void OnDiagActivate(int row);      // 面板双击：跳到那行并选中被判错的片段
    // 批次 96 补：跨文件补全词源（被引用 .dec 的符号）。
    // **与静态检查开关无关**，也**不等防抖** —— 见 .cpp 里的说明。
    // 返回读到的 .dec 文本（规则 3 复用，同一轮不读两遍盘）。
    std::vector<std::string> RefreshDecSymbols();

    // --- 批次 104：转到定义（F12 / View 菜单）-------------------------------
    // 光标下的符号（pin / pin 组 / 时序名）→ 本工程 `SET_DEC_FILE` 引用的 `.dec`
    // 里那条声明。**不新增词源**：挑文件用 ExtractDecSymbols，落点用
    // ExtractDecSymbolLocations —— 与悬停提示、跨文件补全同一口径。
    void GotoChromaDefinition();
    // 把已经打开的文档（任一视图）激活；没打开就按路径打开。返回最终的活动文档
    // （失败给 nullptr）。抽出来是因为"先看两视图再开文件"这套逻辑在别处也写过，
    // 而转到定义**必须**优先落到已打开的那份上（用户可能在右边视图改过它）。
    Document* ActivateOrOpenDocument(const std::wstring& full);

    // 状态栏"一次性消息"通道。复用 [5]（语言段）——这是本程序既有的瞬态消息位
    // （宏回放完成提示也用它），下一次 UpdateStatusBar 会把语言名写回去。
    // 没有独立段落是**刻意**的：为了不挤掉 Ln/Col、编码、EOL 这些常驻信息。
    void SetTransientStatus(const std::wstring& text);

    // --- 批次 106：状态栏「定义」提示（转到定义的"看得见"那一半）-------------
    // 批次 104 加了 F12 跳转、批次 105 加了 Alt+Left 回程，但**没有任何东西告诉
    // 用户脚下这个词是可以跳的** —— 一次单向旅程。这个提示把"跳到哪去"提前显示
    // 在状态栏上：光标停在被引用 .dec 里声明过的符号上时，[7] 段显示
    // `<.dec 名> 第 N 行  <那一行原文>`；离开符号就清空。
    //
    // 【为什么不是悬停气泡】原计划是把提示接到批次 103 的悬停气泡上，但本沙箱
    //   里鼠标悬停路径**不可验证**（桌面被一个全屏窗口覆盖，光标永远落不到本程序
    //   窗口上，见 MEMORY 里那次实测）。按"没真机跑过的 e2e 断言等于未验证"的
    //   纪律，宁可选一个**不用鼠标**就能断言的观测面：状态栏是标准控件，
    //   段的长度与文本都能跨进程读到。
    //
    // 【为什么挂在 SCN_UPDATEUI】它本来就是"光标动了"的通知，同一处理里已经有
    //   UpdateStatusBar()。挂在定时器上会让提示慢半拍，还得自己造一套"光标变了"
    //   的判定。
    //
    // 【为什么不查盘】只查 RefreshDecSymbols 填好的缓存（defHints_）—— 光标每动
    //   一格都重读一次 .dec 是不可接受的。
    void RefreshDefinitionHint();

    // 状态栏「定义」提示的一行数据。名字 + 来自哪个 .dec（只留文件名：状态栏那段
    // 只有 250px，路径会把行号挤没）+ 行号 + 那一行原文（UTF-8）。
    struct DefHintEntry {
        std::string  name;
        std::wstring decFile;
        int          line = 0;
        std::string  lineText;
    };
    // 与 decSymbols 同一批 .dec 抽出来，所以**只在 RefreshDecSymbols 里重建**。
    // 重建点与消费点分离，是为了让"光标移动"这条热路径上零 IO。
    std::vector<DefHintEntry> defHints_;
    // 上一次写进状态栏的提示文本。用来避免重复 SB_SETTEXTW —— 同一行内左右移动
    // 时提示内容不变，反复写既白白跨进程发消息、又会让那段闪烁。
    std::wstring defHintShown_;

    // --- 批次 105：导航历史（上一处 / 下一处，Alt+Left / Alt+Right）-----------
    // 游标语义在 NavHistory.h 里（纯逻辑、有单测）；这里只做"翻译"：
    // 把编辑器位置翻译成 NavPoint，再把 NavPoint 还原成"激活文档 + 光标落点"。
    //
    // 【记两次是刻意的】每次跳转**前后各记一次**。因为 NavHistory::Push 对"与当前
    //   项相同"的点是空操作，"跳转前"那次在常见情形下自动退化成 no-op；而用户
    //   在跳转前手动移动过的那一段不会丢（它被这一次记下来）。少记一次会退化成
    //   "只能跳回上一次跳转的落点"，多记一次不会（去重在 NavHistory 里）。
    void RememberNavPoint();
    // 取"活动文档此刻的光标位置"当一个导航点。返回 false 表示这一点不入历史
    // （没有活动文档 / 大文件查看器没有 Scintilla 光标 / 未命名文档没有路径）。
    bool NavPointHere(NavPoint* out) const;
    // 回到一个导航点。目标文档可能已经关了 ⇒ 走 ActivateOrOpenDocument 重新打开
    // （与转到定义同一条路径，包括"优先落到右边视图里已打开的那份"）。
    // 位置会夹到文件长度以内 —— 文件可能在这期间被外部改短了。
    bool GoToNavPoint(const NavPoint& p);
    void NavBack();
    void NavForward();
    NavHistory navHistory_;

    // --- 批次 107：查找所有引用（Shift+F12）---------------------------------
    // 扫描范围 = 所有打开的 Chroma 文档（左右两视图）+ 当前文档 SET_DEC_FILE
    // 引用的 .dec（那里的声明也算一处，与 VS Code 的语义一致：改一个 pin 之前
    // 要同时看到"谁在用"和"它在哪儿定义"）。
    // 命中直接喂给既有的结果面板，双击跳转沿用 OnResultActivate —— 不新造一套。
    void FindAllReferences();

    // --- 批次 96：CRAFT 编译集成（方向 D 第三刀）-----------------------------
    // 内核分三层，这里只负责"何时跑、结果给谁看"：
    //   CraftProject（纯函数：该跑什么）→ CraftHost（读盘/读环境）→ CraftRunner（真的跑）
    // 三条硬约束（见 CompilePanel.h）：① 没 CRAFT 也能用，缺工具链是"灰掉 + 说明"
    // 不是报错弹窗；② 编译输出面板与静态检查面板**必须分开**；③ 解析不出位置就
    // 退化为纯文本，绝不猜着跳。
    // 后台编译线程的产物。用堆指针 PostMessage 回 UI 线程，接收方 delete
    // （与 WM_APP_GIT_DONE 同口径）——结果里有整个输出文本与逐行解析，
    // 不是几个整数能装下的。
    struct CompileJob {
        craft::BuildResult result;
        std::wstring       projectRoot;   // 解析输出里的相对路径要用
        std::wstring       plnName;
    };

    void ToggleCompilePanel();          // 工具 > 编译输出面板（勾选）
    void CompileActiveProject();        // 工具 > 编译工程（后台线程，结果 PostMessage 回来）
    void OnCompileChooseToolDir();      // 工具 > 指定 CRAFT 工具目录…
    void ClearCompileOutput();          // 工具 > 清除编译输出
    void OnCompileDone(CompileJob* job);   // 接管所有权；把结果铺进面板
    void OnCompileActivate(int row);    // 面板双击：跳到编译器报出的位置
    void ShowCompileNotice(const std::wstring& summary,
                           std::vector<CompileRow> rows);   // 铺面板（不编译）
    bool JumpToCompileLocation(const CompileRow& r);
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
    // 批次 63：文件历史（log 选择器 + 任一审阅版本的只读视图/对比）
    void GitShowRevision(const std::wstring& absPath, const std::wstring& rev);
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
    void RunPreferences();     // 设置 > 首选项…（设置系统设计笔记 阶段 1）
    void RunStyleConfigurator();  // 设置 > 语言样式配置器…（阶段 2b）
    void RunShortcutMapper();     // 设置 > 管理快捷键…（阶段 3）
    void RunThemeExport();        // 设置 > 导出主题 JSON…（阶段 4）
    void RunThemeImport();        // 设置 > 导入主题 JSON…（阶段 4）
    void RunConfigExport();       // 设置 > 导出配置 profile…（换机迁移）
    void RunConfigImport();       // 设置 > 导入配置 profile…（重启生效）
    void RunWindowList();         // 窗口 > 窗口列表…（N++ WindowsDlg 风格）
    void RebuildWindowMenuItems();   // 窗口菜单动态文档条目（OnWorkspaceChanged）
    void SpawnWindow(const std::wstring& file); // launch a second xfsWinPad.exe
    void SpawnWithArgs(const std::wstring& args);
    void SpawnRestoreWindow(const std::wstring& slotFile);  // --new --restore slot
    void NewWindowProcess();   // File > New Window (blank, unshared session)
    // How many other xfsWinPad main windows are alive right now (any process).
    // Used at WM_CLOSE to tell "closing one window" from "the app is exiting".
    int CountOtherMainWindows() const;
    void StartupSession();   // CLI files / --restore / legacy restore + fan-out
    void RestoreSession(const struct SessionState& ss);
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
    enum class GitPick : int {
        None = 0, Merge, DeleteBranch, Switch, DeleteRemoteBranch
    };
    GitPick gitPick_ = GitPick::None;  // next branch picker result -> target op
    std::unique_ptr<HexPanel> hex_;
    std::unique_ptr<StdfPanel> stdf_;
    std::unique_ptr<CsvPanel> csv_;
    std::unique_ptr<DiagnosticsPanel> diag_;
    // 批次 96：编译输出面板。**与 diag_ 分开是硬约束** —— 诊断面板的摘要写着
    // "非 CRAFT 编译结果"（那是我们自建的规则），这个面板是编译器自己的结论。
    std::unique_ptr<CompilePanel> compile_;
    int compileHLogical_ = 220;          // 编译输出面板高度 at 96 dpi（批次 96）
    bool compileBusy_ = false;           // 后台编译线程在跑（防重入）
    // 最近一次编译的工程根。跳转时要用它把编译器给的**相对路径**补全 ——
    // 我们对编译器输出的路径形态**零实证**（只有编译成功的工程，没有失败输出），
    // 所以跳转必须按"绝对 → 根相对 → 按文件名匹配已打开文档"三级依次尝试。
    std::wstring compileRoot_;
    // 刻意**不加状态栏分段**：状态栏 0..7 已被占用，插第 8 段会移动分区索引，
    // 而 GUI e2e 脚本（chroma-e2e.ps1）会读状态栏。编译状态放在面板标题行里。
    // 等能跑 e2e 时再统一加。
    // 状态栏第 8 段的文本（"3 错 1 警"/"未发现问题"）。存在成员里是因为状态栏
    // 会被 UpdateStatusBar 反复重刷（改标题、切标签、光标移动），而计数只在
    // RefreshDiagnostics 时才算得出来 —— 存下来让两边不必互相知道对方何时跑。
    std::wstring diagStatusText_;
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
int diagHLogical_ = 200;             // 诊断面板高度 at 96 dpi（批次 87）
int bigfileHLogical_ = 320;             // STDF panel height at 96 dpi, splitter-adjustable
int rpHLogical_ = 190;               // results panel height at 96 dpi, splitter-adjustable
int logHLogical_ = 300;              // log panel height at 96 dpi, splitter-adjustable
int termHLogical_ = 260;             // terminal panel height at 96 dpi
int aiWLogical_ = 360;               // AI panel width at 96 dpi, splitter-adjustable

    MacroRecorder macro_;

    AppSettings settings_;
    // 恢复跳行延迟队列：编辑器此时可能尚未挂进可见窗口树，SCI_GOTOLINE 会丢，
    // 故攒到 WM_APP_RESTOREJUMP（消息循环开始、窗口已首显）再应用。
    struct PendingJump {
        std::wstring path;   // 有路径文档按全路径匹配
        std::wstring name;   // untitled 文档按显示名匹配
        int line = 1;        // 1-based
        int col = 1;         // 1-based 显示列（GETCOLUMN+1）
    };
    std::vector<PendingJump> pendingJumps_;
    const ThemeDef* theme_ = nullptr;
    StartupOptions startup_;
};

} // namespace xfs
