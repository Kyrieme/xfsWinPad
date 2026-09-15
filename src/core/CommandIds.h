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

    // Settings 菜单（docs/settings-plan.md 阶段 1..3）
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
