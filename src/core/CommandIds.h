#pragma once
// xfsWinPad - command identifier registry (menu / accelerator shared ids)

namespace xfs {

enum Cmd : unsigned int {
    // File
    FileNew = 100,
    FileOpen,
    FileSave,
    FileSaveAs,
    FileSaveAll,
    FilePrint,
    FileClose,
    FileCloseAll,
    FileReload,
    FileExit,
    FileOpenFolder,     // 110: open a folder as project workspace
    FileCloseFolder,    // 111
    FileNewWindow = 112, // 批次 67 独立新窗口（--new --no-restore）
    FileRecentFirst = 200,       // 200..219 reserved for recent file entries
    FileRecentFolderFirst = 230, // 230..239 reserved for recent folder entries

    // Edit
    EditUndo = 300,
    EditRedo,
    EditCut,
    EditCopy,
    EditPaste,
    EditDelete,
    EditSelectAll,
    EditTimeDate,
    EditDuplicateLine,
    EditMoveLineUp,
    EditMoveLineDown,
    EditDeleteLine,
    EditUpperCase,
    EditLowerCase,
    EditSortAsc,
    EditSortDesc,
    EditRemoveDupLines,
    EditTrimTrailingSpace,
    EditToggleComment,       // 批次 29：切换行注释（lexer 感知）
    EditJoinLines,           // 批次 29：连接行
    EditSplitLines,          // 批次 29：拆分行
    EditRemoveEmptyLines,    // 批次 29：删除空行
    EditReverseLines,        // 批次 29：行序反转

    // Search
    SearchFind = 400,
    SearchFindNext,
    SearchFindPrev,
    SearchReplace,
    SearchGotoLine,

    // View
    ViewWordWrap = 450,
    ViewZoomIn,
    ViewZoomOut,
    ViewZoomReset,
    ViewMoveToOtherView = 456,   // move active document to the other split view
    ViewMoveToNewView = 457,     // open the active document in a brand-new window
    ThemeLight = 460,
    ThemeDark,

    // Help
    HelpAbout = 500,

    // Command palette
    PaletteShow = 530,

    // Plugins > 插件管理
    PluginAdmin = 535,

    // View > Explorer
    ViewExplorer = 540,

    // Code folding
    FoldAll = 550,
    UnfoldAll = 551,

    // Hex viewer
    ViewHexView = 560,
    ViewLogPanel = 561,

    // STDF viewer
    ViewStdfView = 576,
    ViewBigFile = 577,

    // CSV table view
    ViewCsvView = 589,

    // Chroma 3380 静态检查（批次 87）
    ViewDiagnostics = 590,   // View > 诊断面板（列表 + 双击跳转）
    ViewChromaCheck = 591,   // View > Chroma 静态检查（勾选：是否标红）

    // 批次 104：转到定义。光标下的符号（pin / pin 组 / 时序名）→ 它在本工程
    // `SET_DEC_FILE` 引用的 `.dec` 里的声明处。词源口径与悬停提示、跨文件补全
    // 共用同一个抽取函数（Chroma3380Diagnostics），三处不会各说各话。
    GotoDefinition = 596,    // View > 转到定义（F12）

    // 批次 105：导航历史。转到定义是"跳出去"，这两条是"跳回来" —— 只有 F12 而
    // 没有回退，用户跳一次就得自己找原来的位置，功能其实只做了一半。
    // 历史只记**显式跳转**（跳行 / 转到定义 / 诊断面板双击），不记光标移动。
    NavBack = 597,           // View > 上一处（Alt+Left）
    NavForward = 598,        // View > 下一处（Alt+Right）

    // 批次 107：查找所有引用（F12 的反方向）。F12 回答"这个词在哪儿定义的"，
    // 这条回答"这个词在哪些地方被用过" —— 改一个 pin 之前得先知道要动多少处。
    // 词边界 / 注释与字符串的判定复用内核同一个函数（FindSymbolReferences），
    // 与转到定义、状态栏提示、补全共口径，不会各说各话。
    FindAllReferences = 599, // View > 查找所有引用（Shift+F12）

    // CRAFT 编译集成（批次 96，方向 D）。与上面两项**刻意分开**：那是我们自建的
    // 静态规则，这里是**编译器自己的结论**（见 CompilePanel.h 头部说明）。
    //
    // 本批**刻意不做「重新编译」**：它的真正语义是"删掉中间目录 `.<stem>` 再编"，
    // 而那是一个**删除目录的破坏性动作**；`patcmp -c` 本来就会重编，所以"编译"
    // 已经覆盖了日常需求。等有 CRAFT 可验证时再加，且必须带"列出将被删除的目录 +
    // 明确确认"。
    BuildCompile = 592,       // 工具 > 编译工程
    ViewCompileOutput = 593,  // 工具 > 编译输出面板（勾选态）
    BuildClearOutput = 594,   // 工具 > 清除编译输出
    BuildChooseToolDir = 595, // 工具 > 指定 CRAFT 工具目录…

    // Diff compare
    DiffCompare = 570,
    DiffExitCompare = 571,

    // Terminal
    ViewTerminal = 572,

    // AI (opencode 右栏对话面板)
    ViewAiPanel = 578,      // AI > 打开/关闭 opencode
    AiNewSession,           // AI > 新会话
    AiToggleContext,        // AI > 附带编辑器上下文（勾选态）

    // Tab recovery (恢复关闭的标签)
    ReopenClosedTab = 588,

    // Window menu (N++ style): list dialog + per-doc entries
    WindowList = 575,
    WindowDocFirst = 7200,   // 7200..7299 reserved for open-document entries

    // Encoding (indexes follow encoding::EncodingType order; 11 targets now)
    EncConvertFirst  = 700,  // 700..710 convert-to targets
    EncReloadAsFirst = 720,  // 720..730 reload-as targets

    // Language top-level menu (index into LanguageMenuCatalog())
    LangFirst = 2600,

    // Bookmarks
    BookmarkToggle = 800,
    BookmarkNext,
    BookmarkPrev,
    BookmarkClearAll,

    // Macro
    MacroStart = 850,
    MacroStop,
    MacroPlayback,
    MacroSave,
    MacroLoad,

    // Settings 菜单（设置系统设计笔记 阶段 1..3）
    Preferences = 880,       // 首选项…（阶段 1）
    StyleConfigurator,       // 语言样式配置器…（阶段 2，先占位灰显）
    ShortcutMapper,          // 管理快捷键…（阶段 3，先占位灰显）
    ThemeExport,             // 导出主题 JSON…（阶段 4）
    ThemeImport,             // 导入主题 JSON…（阶段 4）
    ConfigExport,            // 导出配置 profile…（换机迁移/备份）
    ConfigImport,            // 导入配置 profile…（重启生效）

    // Tab bar control id (WM_NOTIFY source)
    TabBarId = 9000,
    TabBarId2 = 9001,   // right/other view tab bar
};

} // namespace xfs
