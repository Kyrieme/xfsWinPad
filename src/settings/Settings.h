#pragma once
// xfsWinPad - application settings persisted as flat JSON
// (%APPDATA%\xfsWinPad\settings.json)

#include <string>

namespace xfs {

struct AppSettings {
    // editor preferences
    std::wstring fontName = L"Consolas";
    int fontSize = 10;
    int tabWidth = 4;
    bool wrapOn = false;

    // editing behavior (Preferences > 编辑)
    int caretWidth = 2;               // 1..3 px (SCI_SETCARETWIDTH)
    bool currentLineHighlight = true; // SCI_SETCARETLINEVISIBLE
    bool showLineNumber = true;       // line-number margin visibility
    bool autoIndent = true;           // Editor::SetAutoIndent
    bool autoComplete = true;         // Editor::SetAutoComplete（关键词+文档词汇）

    // new-document defaults (Preferences > 新建文档)
    int defaultEol = 0;               // 0=CRLF 1=CR 2=LF (Editor::EolMode order)

    // crash-safe autosave (Preferences > 备份)
    bool autosaveEnabled = true;
    int autosaveSeconds = 30;         // clamped to 5..600 at use site

    // ui
    std::wstring theme = L"light";   // light | dark
    bool autoCloseBrackets = true;

    // ui visibility (Preferences > 界面)
    bool showStatusBar = true;   // 状态栏显隐
    bool showTabBar = true;      // 标签栏显隐
    bool showToolbar = true;     // 工具栏显隐

    // recent files (Preferences > 最近文件)
    int recentFilesMax = 10;     // 1..30，使用处钳制

    // highlighting (Preferences > 高亮)
    bool braceMatch = true;      // 括号匹配高亮（indicator 8）
    bool indentGuides = true;    // 缩进参考线 SCI_SETINDENTATIONGUIDES
    bool showWhitespace = false; // 显示空白符 SCI_SETVIEWWS

    // Chroma 3380 静态检查（批次 87）：.pln/.dec/.pat 编辑时自动跑我们自建的
    // 校验规则，命中处画波浪线（indicator 10/11）+ 底部诊断面板列出。
    // 关掉即完全不标红、不校验（面板显示"已关闭"）。默认开。
    bool chromaDiagnostics = true;

    // searching defaults (Preferences > 搜索)
    bool searchMatchCase = false;   // FindState 初始 matchCase
    bool searchWholeWord = false;   // FindState 初始 wholeWord

    // misc (Preferences > 杂项)
    bool fullPathTitle = false;  // 标题栏显示完整路径
    bool autoDetectLang = true;  // 按扩展名自动检测语言（Editor::SetLexerForFile）

    // terminal panel
    std::wstring termFontName = L"Consolas";
    int termFontSize = 11;

    // window geometry (outer rect); hasWindow=false until first successful save
    bool hasWindow = false;
    int winX = 0;
    int winY = 0;
    int winW = 0;
    int winH = 0;
    bool winMax = false;

    // folder workspace (project mode)
    std::wstring projectRoot;   // empty = no folder open
    bool explorerVisible = false;   // File Explorer panel shown on last exit
    bool aiPanelVisible = false;    // AI 右栏（opencode）上次退出时是否可见

    // dock panel heights at 96 dpi (0 = use built-in default)
    int hexPanelH = 0;
    int resultsPanelH = 0;
    int logPanelH = 0;
    int terminalPanelH = 0;
    int diagPanelH = 0;          // 诊断面板高度 at 96 dpi (0 = default)
    int aiPanelW = 0;            // AI 右栏宽度 at 96 dpi (0 = default)
    bool aiAttachContext = true; // AI 发送时自动附带编辑器上下文（路径/光标/选区）
    bool aiAutoApprove = true;   // 自动放行 AI 权限请求（false = 用户在 opencode 侧手动确认）
    std::wstring aiModel;        // AI 模型偏好 "provider/model"（空 = 自动择优）
    std::wstring aiBackend = L"opencode";  // opencode | openai（本地/第三方直连）
    std::wstring aiEndpoint;     // openai 模式端点（空 = http://127.0.0.1:11434/v1）
    std::wstring aiApiKey;       // openai 模式 Bearer key（本地 Ollama 通常留空）
    std::wstring aiLocalModel;   // openai 模式模型 ID（如 qwen2.5-coder:7b）

    // UI language code (e.g. "zh-CN", "en"); empty = zh-CN
    std::wstring uiLang = L"zh-CN";
};

std::wstring SettingsFilePath();

// Loads into `out`; fields keep their defaults when a key is missing or the
// file is unreadable/corrupt. Returns false when no usable file existed.
bool SettingsLoad(const std::wstring& path, AppSettings* out);
bool SettingsSave(const std::wstring& path, const AppSettings& s);

} // namespace xfs
