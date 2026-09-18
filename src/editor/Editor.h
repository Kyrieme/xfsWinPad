#pragma once
#include <functional>
// xfsWinPad - Editor: owns one Scintilla control and its per-document state.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Scintilla.h>
#include <string>
#include <set>
#include <functional>

namespace xfs {
namespace encoding { enum class EncodingType : int; }
struct AppSettings;
struct ThemeDef;

enum class EolMode : int { CRLF = 0, CR = 1, LF = 2 };

// 按内容启发式判断换行风格（CRLF 计数多→CRLF；否则 CR/LF）。
// Workspace 加载与大文件路径已有内部实现，提升至此供 AI 回填等共用。
EolMode DetectEol(const char* data, size_t len);
EolMode DetectEol(const std::string& text);

struct EditorStatus {
    int line = 1;        // 1-based
    int column = 1;      // 1-based
    int selection = 0;   // chars selected
    int lines = 0;
    long long length = 0;
};

class Editor {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();

    static LRESULT CALLBACK EditorKeyProc(HWND, UINT, WPARAM, LPARAM,
                                          UINT_PTR, DWORD_PTR);
    static constexpr UINT_PTR kEditorSubclassId = 10;

    HWND Hwnd() const { return hwnd_; }
    bool Valid() const { return hwnd_ != nullptr; }

    // --- configuration ---
    void ApplyDefaultStyle(int dpi);
    void ApplyPrefs(const AppSettings& prefs);   // user font/tab/wrap overrides
    void ApplyTheme(const ThemeDef& t);          // base colors: bg/fg/caret/margins
    // Attaches the Lexilla lexer for this file name; when `t` is given the
    // syntax styles are colored immediately after the lexer switch.
    void SetLexerForFile(const std::wstring& fileName, const ThemeDef* t = nullptr);
    // Applies an explicit Lexilla lexer (by name) regardless of file extension.
    // `keywords` is a 2-element array of c-strings (may hold nullptr); pass
    // nullptr for the whole array when none. lexerName == nullptr => plain text.
    void SetLexerByName(const char* lexerName, const char* const* keywords,
                        const ThemeDef* t = nullptr);

    // --- text / document ---
    void SetTextUtf8(const std::string& utf8);
    // Like SetTextUtf8 but streams an external buffer (e.g. mmap view) in
    // chunks, avoiding a second full-size std::string copy. The buffer must
    // stay valid for the duration of the call.
    void SetTextUtf8Buffer(const char* data, size_t size);
    // Append-only fast path for chunked loading of huge documents.
    void AppendTextUtf8(const char* data, size_t size);
    // Preset the horizontal scroll range from the longest line so the
    // h-scrollbar is correct on load (Scintilla measures line widths lazily).
    void UpdateScrollWidthEstimate();
    std::string GetTextUtf8() const;              // O(n) full copy
    // Copy [pos, pos+len) into `out` (resized); for chunked streaming saves.
    void GetTextRangeUtf8(long long pos, size_t len, std::string& out) const;
    void SetSavePoint();
    bool Modified() const;

    void SetReadOnly(bool ro);
    bool ReadOnly() const;

    EolMode Eol() const;
    void SetEol(EolMode eol);
    void ConvertEols(EolMode eol);
    static const wchar_t* EolDisplayName(EolMode eol);

    // --- view ---
    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    bool WordWrap() const;
    void SetWordWrap(bool on);

    // --- editing helpers ---
    void SelectAll()       { Send(SCI_SELECTALL); }
    void Undo()            { Send(SCI_UNDO); }
    void Redo()            { Send(SCI_REDO); }
    bool CanUndo()         { return Send(SCI_CANUNDO) != 0; }
    bool CanRedo()         { return Send(SCI_CANREDO) != 0; }
    void Cut()             { Send(SCI_CUT); }
    void Copy()            { Send(SCI_COPY); }
    void Paste()           { Send(SCI_PASTE); }
    void ClearSelection()  { Send(SCI_CLEAR); }
    void DuplicateLine()   { Send(SCI_SELECTIONDUPLICATE); }
    void MoveLineUp()      { Send(SCI_MOVESELECTEDLINESUP); }
    void MoveLineDown()    { Send(SCI_MOVESELECTEDLINESDOWN); }
    void DeleteLine()      { Send(SCI_LINEDELETE); }
    void UpperCase()       { Send(SCI_UPPERCASE); }
    void LowerCase()       { Send(SCI_LOWERCASE); }
    void SortSelectedLines(bool ascending);
    void RemoveDuplicateLines();
    void TrimTrailingSpace();
    // 批次 29 行变换（逻辑在 LineOps.{h,cpp}，纯函数可单测）
    void JoinSelectedLines();       // 选中行并一行（无选择 = 当前行的下一行）
    void SplitLineAtCaret();        // 光标处断行（保留缩进）
    void RemoveEmptyLines();        // 选区（无选择 = 全文）删空白行
    void ReverseLines();            // 选区（无选择 = 全文）行序反转
    void ToggleLineComment();       // 按当前 lexer 选注释符，选区行切换

    // diff markers (marker 5 = changed line, 6 = added/removed)
    void ClearDiffMarks();
    void MarkDiffLine(int line0, bool added);
    void InsertTextAtCaret(const std::string& utf8) { Send(SCI_REPLACESEL, 0, (LPARAM)utf8.c_str()); }
    void GotoLine(int line1based);
    void GotoPosition(int line1based, int columnDisplay);  // restore caret col too (GETCOLUMN 1-based)
    void EnsureVisibleCurrent();
    void SelectRange(sptr_t start, sptr_t end);

    // --- bookmarks ---
    void ToggleBookmark();
    void NextBookmark();        // wraps around
    void PreviousBookmark();    // wraps around
    void ClearBookmarks();

    // --- 标记页 line marks (marker 3) ---
    bool MarkLine(int line1based);   // returns false when already marked
    void ClearLineMarks();

    // --- auto-indent / auto-close / brace expansion ---
    void SetAutoIndent(bool on);
    void SetAutoCloseBrackets(bool on);
    void SetBraceExpand(bool on) { braceExpand_ = on; }
    void SetAutoComplete(bool on) { autoComplete_ = on; }
    void HandleCharAdded(SCNotification* sn);
    using KeyHookFn = void(*)(void* ctx, UINT msg, WPARAM wp);
    void SetKeyHook(void* ctx, KeyHookFn fn) { keyHookCtx_ = ctx; keyHookFn_ = fn; }
    std::string GetLineIndentText(int line);

    // --- double-click word highlight (indicator 9) ---
    void HighlightOccurrences();
    void ClearOccurrenceHighlight();
    // 双击词高亮当前是否有内容（配合「点击别处选区塌缩即清除」的判定）
    bool HasOccurrenceHighlight() const { return occActive_; }

    // --- large file mode ---
    bool IsLargeFile() const { return largeFile_; }
    void EnableLargeFileMode();

    // --- folding shortcuts ---
    void FoldAll();
    void UnfoldAll();

    // --- status ---
    EditorStatus Status() const;
    int WordCount() const;
    sptr_t Send(unsigned int msg, uptr_t wp = 0, LPARAM lp = 0) const;

    // --- 自动补全（阶段：autocomplete v1）---
    void HandleAutocompleteChar(unsigned int ch);
    void ShowAutocomplete(const std::string& prefix);
    void CollectDocWords(std::set<std::string>& out);

    // --- 批次 73：Chroma 3380 签名提示 ---------------------------------------
    // 光标落在已收录语句的实参里时，给两种提示，但**二选一**：
    //   下拉框   = 该参数的手册候选值（有候选值就走这条，Tab 接受）
    //   签名气泡 = 手册原文签名 + 高亮当前参数（没候选值才走这条）
    // 二选一不是取舍，是 Scintilla 的硬约束：AutoCompleteStart() 第一行
    // ct.CallTipCancel()、CallTipShow() 第一行 ac.Cancel()，两个方向互相取消，
    // 所以"气泡 + 下拉同框"根本立不住（批次 73 端到端实测抓到的）。
    // Tab 接受下拉项同样由 Scintilla 原生处理（ScintillaBase::KeyCommand 的
    // Message::Tab 分支），这里不拦 VK_TAB；自己拦会和前缀过滤/autohide 打架。
    // 返回 true 表示本次按键已由签名提示接管（下拉框已出）。
    bool HandleSignatureHint();
    // 批次 77：Chroma 族的**语句名补全**（位置敏感，只在语句起始位置触发）。
    // 候选集来自词法器自己的词表（= 会被高亮的那一批），顺序按手册章节升序，
    // 列表项右侧带章节号（`FORCE_V_MLDPS?4.9.4`，只显示不插入）。返回 true 表示
    // 下拉框已出、本次按键不再走普通词汇补全。
    bool HandleStatementCompletion(sptr_t wordStart, const std::string& prefix);
    // 批次 78：Scintilla 在**补全项已写进文档之后**发的 SCN_AUTOCCOMPLETED。
    // 语句名补全的"续动作"（补 `(` + 出签名提示）只能挂在这里 ——
    // SCN_AUTOCSELECTION(2022) 是在 NotifyParent 里、AutoCompleteInsert 之前发的，
    // 在那个回调里插字符会被随后的替换吃掉（批次 77 就卡在这一步）。
    // 这里只做便宜的判断并把续动作**投递**出去（见下），不直接改文档。
    void HandleAutocCompleted(const SCNotification* sn);
    // 续动作本身。由 EditorKeyProc 在消息循环里调用（不嵌在 Scintilla 的
    // 通知派发里跑），见 Editor.h 顶部的说明。
    void CompleteAfterStatementAccepted();
    // 投递消息号：只发给自己的 hwnd_，与 WM_APP+9/+12/+71..78 不冲突。
    static constexpr UINT kMsgStatementAccepted = WM_APP + 42;
    // 批次 31：本档词汇抽取（纯文本口径；Workspace 收集其它标签词汇用）
    void ExtractWords(std::set<std::string>& out) const;
    // 批次 31：跨标签词汇源（Workspace 注入；每次弹出候选时回调重建）
    void SetExtWordsProvider(std::function<void(std::set<std::string>&)> cb) {
        extWords_ = std::move(cb);
    }

    // --- 右键上下文菜单（宿主弹自己的菜单；屏幕坐标）---
    // Scintilla 默认右键弹内置小菜单（SCI_USEPOPUP 默认开）——Create 里已关，
    // 右键经 EditorKeyProc 转给宿主决定内容（AI 快捷组等）。
    void SetContextMenu(std::function<void(int sx, int sy)> cb) {
        onContextMenu_ = std::move(cb);
    }

private:
    void SetupMargins(int dpi);
    void UpdateLineNumberWidth();
    void DefineMarkMarker();
    bool CollectLineRange(std::string* out, sptr_t* outStart, sptr_t* outEnd);
    void ReplaceLineRange(sptr_t start, sptr_t end, const std::string& repl);
    void CancelSignatureHint();     // 批次 73：收起签名气泡（下拉框归 Scintilla 管）

    HWND hwnd_ = nullptr;
    bool wordWrap_ = false;
    bool autoIndent_ = true;
    bool autoComplete_ = true;
    std::string lexerName_;              // 当前词法器（自动补全关键词来源）
    std::set<std::string> wordCache_;    // 文档词汇缓存（每次触发重建）
    bool wordCacheValid_ = false;        // 保留字段（暂无失效路径）
    std::function<void(std::set<std::string>&)> extWords_;   // 跨标签词汇源
    bool autoClose_ = true;
    bool braceExpand_ = true;
    bool largeFile_ = false;
    bool showLineNumber_ = true;   // 首选项行号开关（UpdateLineNumberWidth 遵守）
    bool autoDetect_ = true;       // 打开文件时按扩展名自动检测语言
    bool occActive_ = false;       // 双击词高亮（indicator 9）是否有内容
    std::string defaultFont_ = "Consolas";   // 无全局/语言字体覆盖时的回退字体（UTF-8）
    int defaultFontSize_ = 10;
    int lastLineCountDigits_ = 0;
    void* keyHookCtx_ = nullptr;
    KeyHookFn keyHookFn_ = nullptr;
    std::function<void(int, int)> onContextMenu_;   // 屏幕坐标右键回调
    // 批次 73 签名气泡的当前指向：只有指向变了才重设气泡，否则逐字符重设会闪。
    bool sigTipShown_ = false;
    const void* sigTipStmt_ = nullptr;   // 指向 kStatements 里的一条
    int sigTipParam_ = -1;
    sptr_t sigTipPos_ = -1;
    // 批次 78：语句名下拉的"锚"——我们在哪个位置弹的（= 被替换区间的起点）。
    // 只有它 >= 0 时 SCN_AUTOCCOMPLETED 才认作"这是我们自己弹的语句下拉"，
    // 并校验完成通知里的 position 与它相同才续动作。用完即清（一次性）。
    sptr_t stmtCompleteStart_ = -1;
};

} // namespace xfs
