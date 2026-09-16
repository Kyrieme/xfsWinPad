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
};

} // namespace xfs
