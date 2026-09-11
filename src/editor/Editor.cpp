#include "Editor.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../language/LanguageMap.h"
#include "../settings/Settings.h"
#include "../theme/Theme.h"
#include "../theme/Styler.h"
#include "LineOps.h"
#include "Wordscan.h"
#include <commctrl.h>

#include <ILexer.h>
#include <Lexilla.h>
#include <Scintilla.h>
#include <SciLexer.h>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <set>

using namespace Scintilla;

namespace xfs {

EolMode DetectEol(const char* data, size_t len) {
    size_t crlf = 0, lf = 0, cr = 0;
    size_t sampleEnd = (std::min)(len, (size_t)1048576);
    for (size_t i = 0; i < sampleEnd; ++i) {
        char c = data[i];
        if (c == '\r') { (i + 1 < len && data[i+1] == '\n') ? ++crlf : ++cr; }
        else if (c == '\n') ++lf;
    }
    if (lf > crlf && lf > cr) return EolMode::LF;
    if (cr > crlf && cr > lf) return EolMode::CR;
    return EolMode::CRLF;
}
EolMode DetectEol(const std::string& text) {
    return DetectEol(text.data(), text.size());
}

namespace {
constexpr int MARGIN_LINE = 0;
constexpr int MARGIN_SYMBOL = 1;
constexpr int MARGIN_FOLD = 2;
constexpr int MARK_BOOKMARK = 2;   // app-defined marker (folder uses 25-31)
constexpr int MARK_MARKED   = 3;   // 标记页 line marker
constexpr int MARK_DIFF_CHANGED = 5;   // diff: line differs between docs
constexpr int MARK_DIFF_ADDED   = 6;   // diff: line only in one doc

sptr_t SendTo(HWND h, unsigned int msg, uptr_t wp = 0, LPARAM lp = 0) {
    return ::SendMessageW(h, msg, (WPARAM)wp, lp);
}

// ASCII 标识符字符（双击高亮做整词边界判断用；CJK 按连续串整串匹配）
bool AsciiIdByte(int b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
           (b >= '0' && b <= '9') || b == '_';
}
} // namespace

bool Editor::Create(HWND parent, HINSTANCE hInst) {
    hwnd_ = ::CreateWindowExW(0, L"Scintilla", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 100, 100, parent, nullptr, hInst, nullptr);
    if (!hwnd_) {
        Logger::Error("Scintilla CreateWindowEx failed, gle=" + std::to_string(::GetLastError()));
        return false;
    }
    ::SetWindowSubclass(hwnd_, EditorKeyProc, kEditorSubclassId, (DWORD_PTR)this);
    ApplyDefaultStyle(::GetDpiForWindow(parent));
    return true;
}

// --- editor key subclass: intercept Enter for brace expansion -------------------

LRESULT CALLBACK Editor::EditorKeyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR idSub, DWORD_PTR ref) {
    auto* self = (Editor*)ref;
    if (!self) return DefSubclassProc(h, msg, wp, lp);

    // macro recording hook (before any handling so all input is captured)
    if (self->keyHookFn_) {
        if (msg == WM_CHAR) self->keyHookFn_(self->keyHookCtx_, WM_CHAR, wp);
        else if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_TAB || wp == VK_BACK || wp == VK_DELETE))
            self->keyHookFn_(self->keyHookCtx_, WM_KEYDOWN, wp);
    }

    // Auto-close brackets/quotes on WM_CHAR
    if (msg == WM_CHAR) {
        wchar_t ch = (wchar_t)wp;
        const wchar_t* closer = nullptr;
        switch (ch) {
            case L'(': closer = L")"; break;
            case L'[': closer = L"]"; break;
            case L'{': closer = L"}"; break;
            case L'"': closer = L"\""; break;
            case L'\'': closer = L"'"; break;
        }
        if (closer && self->autoClose_) {
            // Pass the opening char to Scintilla first
            DefSubclassProc(h, msg, wp, lp);
            // Then insert the closing char at current position
            self->Send(SCI_ADDTEXT, (uptr_t)wcslen(closer), (LPARAM)closer);
            // Move cursor back between the pair
            sptr_t pos = self->Send(SCI_GETCURRENTPOS);
            self->Send(SCI_GOTOPOS, pos - (sptr_t)wcslen(closer));
            return 0;   // fully handled
        }
    }

    // Auto-indent on Enter (respects the Preferences > 编辑 switch)
    if (msg == WM_KEYDOWN && wp == VK_RETURN && self->autoIndent_ &&
        !self->Send(SCI_GETREADONLY)) {
        LRESULT ret = DefSubclassProc(h, msg, wp, lp);   // newline inserted
        sptr_t pos = self->Send(SCI_GETCURRENTPOS);
        int line = (int)self->Send(SCI_LINEFROMPOSITION, pos);
        std::string indent = self->GetLineIndentText(line - 1);
        if (!indent.empty())
            self->Send(SCI_ADDTEXT, (uptr_t)indent.size(), (LPARAM)indent.c_str());
        return ret;
    }

    // Right-click: hand the screen point to the host context menu
    if (msg == WM_CONTEXTMENU && self->onContextMenu_) {
        POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
        if (pt.x == -1 && pt.y == -1) {          // keyboard-invoked: use caret
            sptr_t pos = self->Send(SCI_GETCURRENTPOS);
            pt.x = (int)self->Send(SCI_POINTXFROMPOSITION, 0, (LPARAM)pos);
            pt.y = (int)self->Send(SCI_POINTYFROMPOSITION, 0, (LPARAM)pos);
            ::ClientToScreen(h, &pt);
        }
        self->onContextMenu_(pt.x, pt.y);
        return 0;   // 不让 Scintilla 再弹内置菜单
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void Editor::Destroy() {
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void Editor::ApplyDefaultStyle(int dpi) {
    (void)dpi;
    Send(SCI_SETCODEPAGE, SC_CP_UTF8);
    Send(SCI_SETEOLMODE, SC_EOL_CRLF);

    // Default font / colors
    Send(SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Consolas");
    Send(SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    Send(SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0x1E, 0x1E, 0x1E));
    Send(SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0xFF, 0xFF, 0xFF));
    Send(SCI_STYLECLEARALL);

    // Caret & current line
    Send(SCI_SETCARETPERIOD, 530);
    Send(SCI_SETCARETWIDTH, 2);
    Send(SCI_SETCARETLINEVISIBLE, 1);
    Send(SCI_SETCARETLINEVISIBLEALWAYS, 1);   // 无焦点也绘制（首选项实时预览需要）
    Send(SCI_SETCARETLINEBACK, RGB(0xF2, 0xF6, 0xFC));
    Send(SCI_SETCARETLINEFRAME, 0);           // 实心色块填充；1px 线框几乎不可见

    // Selection
    Send(SCI_SETSELECTIONLAYER, SC_LAYER_BASE);
    Send(SCI_SETMOUSESELECTIONRECTANGULARSWITCH, 1);

    // Indentation + indent guides
    Send(SCI_SETUSETABS, 1);
    Send(SCI_SETTABWIDTH, 4);
    Send(SCI_SETINDENT, 4);
    Send(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);

    // Brace matching: indicator 8 (style-independent, always visible)
    Send(SCI_INDICSETSTYLE, 8, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, 8, RGB(255, 180, 0));
    Send(SCI_INDICSETALPHA, 8, 80);

    // Double-click word highlight: indicator 9 (background tint)
    Send(SCI_INDICSETSTYLE, 9, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, 9, RGB(255, 230, 0));
    Send(SCI_INDICSETALPHA, 9, 70);

    // Multiple selection
    Send(SCI_SETMULTIPLESELECTION, 1);
    Send(SCI_SETADDITIONALSELECTIONTYPING, 1);
    Send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);

    // 右键菜单由宿主接管（禁用 Scintilla 内置 popup，WM_CONTEXTMENU 走
    // EditorKeyProc → onContextMenu_ 回调）
    Send(SCI_USEPOPUP, 0);

    SetupMargins(dpi);
    // Folding behavior
    Send(SCI_SETPROPERTY, (uptr_t)"fold", (LPARAM)"1");
    Send(SCI_SETPROPERTY, (uptr_t)"fold.compact", (LPARAM)"0");
    Send(SCI_SETPROPERTY, (uptr_t)"fold.comment", (LPARAM)"1");
    Send(SCI_SETPROPERTY, (uptr_t)"fold.html", (LPARAM)"1");
}

// 双击选词 → 全文相同单词/中文短语背景高亮（indicator 9）。
// Scintilla 在 SCN_DOUBLECLICK 前已完成整词选中，这里直接取当前选区做整词搜索。
void Editor::HighlightOccurrences() {
    sptr_t docLen = Send(SCI_GETLENGTH);
    Send(SCI_SETINDICATORCURRENT, 9);
    Send(SCI_INDICATORCLEARRANGE, 0, docLen);
    occActive_ = false;

    const sptr_t selStart = Send(SCI_GETSELECTIONSTART);
    const sptr_t selEnd = Send(SCI_GETSELECTIONEND);
    if (selStart >= selEnd || selEnd - selStart > 512) return;   // 空/超长选区忽略

    sptr_t selLen = Send(SCI_GETSELTEXT, 0, 0);   // 选中文本字节数（不含 NUL）
    if (selLen <= 1) return;
    std::string word(selLen + 1, '\0');           // +1 容纳 GETSELTEXT 写入的结尾 NUL
    Send(SCI_GETSELTEXT, 0, (LPARAM)&word[0]);
    word.resize(selLen);

    const size_t a = word.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return;   // 双击空白 → 不处理
    const size_t b = word.find_last_not_of(" \t\r\n");
    word = word.substr(a, b - a + 1);

    bool asciiOnly = true;   // 含 ≥0x80 字节按 UTF-8 中文整串匹配（CJK 无词界）
    for (unsigned char c : word)
        if (c >= 0x80) { asciiOnly = false; break; }

    Send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
    const size_t n = word.size();
    sptr_t start = 0;
    while (start < docLen) {
        Send(SCI_SETTARGETSTART, start);
        Send(SCI_SETTARGETEND, docLen);
        const sptr_t found = Send(SCI_SEARCHINTARGET, (uptr_t)n, (LPARAM)word.data());
        if (found < 0) break;
        bool boundary = !asciiOnly;
        if (asciiOnly) {
            const int prev = (found > 0) ? (int)Send(SCI_GETCHARAT, found - 1, 0) : 0;
            const int next = (found + (sptr_t)n < docLen)
                ? (int)Send(SCI_GETCHARAT, found + (sptr_t)n, 0) : 0;
            boundary = !AsciiIdByte(prev) && !AsciiIdByte(next);
        }
        if (boundary) {
            Send(SCI_INDICATORFILLRANGE, found, (sptr_t)n);
            occActive_ = true;   // 至少命中一处才算激活
        }
        start = found + 1;
    }
}

void Editor::ClearOccurrenceHighlight() {
    if (!occActive_) return;   // 无高亮时跳过清扫（大文档省一次全文遍历）
    occActive_ = false;
    Send(SCI_SETINDICATORCURRENT, 9);
    Send(SCI_INDICATORCLEARRANGE, 0, Send(SCI_GETLENGTH));
}

void Editor::SetupMargins(int dpi) {
    Send(SCI_SETMARGINTYPEN, MARGIN_LINE, SC_MARGIN_NUMBER);
    Send(SCI_SETMARGINWIDTHN, MARGIN_LINE, 36);
    Send(SCI_SETMARGINSENSITIVEN, MARGIN_LINE, 0);

    Send(SCI_SETMARGINTYPEN, MARGIN_SYMBOL, SC_MARGIN_COLOUR);
    Send(SCI_SETMARGINWIDTHN, MARGIN_SYMBOL, MulDiv(14, dpi, 96));
    Send(SCI_SETMARGINBACKN, MARGIN_SYMBOL, RGB(0xF0, 0xF0, 0xF0));
    Send(SCI_SETMARGINMASKN, MARGIN_SYMBOL, ~SC_MASK_FOLDERS);
    Send(SCI_SETMARGINSENSITIVEN, MARGIN_SYMBOL, 1);

    Send(SCI_SETMARGINTYPEN, MARGIN_FOLD, SC_MARGIN_SYMBOL);
    Send(SCI_SETMARGINMASKN, MARGIN_FOLD, SC_MASK_FOLDERS);
    Send(SCI_SETMARGINWIDTHN, MARGIN_FOLD, 14);
    Send(SCI_SETMARGINSENSITIVEN, MARGIN_FOLD, 1);

    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_MINUS);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_PLUS);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_EMPTY);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_EMPTY);
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDEROPEN, RGB(0x60, 0x60, 0x60));
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDEROPEN, RGB(0xF0, 0xF0, 0xF0));
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDER, RGB(0x60, 0x60, 0x60));
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDER, RGB(0xF0, 0xF0, 0xF0));

    // Bookmark (marker 24) drawn in the symbol margin
    Send(SCI_MARKERDEFINE, MARK_BOOKMARK, SC_MARK_BOOKMARK);
    Send(SCI_MARKERSETFORE, MARK_BOOKMARK, RGB(0x00, 0x66, 0xCC));
    Send(SCI_MARKERSETBACK, MARK_BOOKMARK, RGB(0x00, 0x66, 0xCC));

    DefineMarkMarker();

    lastLineCountDigits_ = 0;
}

void Editor::UpdateLineNumberWidth() {
    if (!showLineNumber_) {
        Send(SCI_SETMARGINWIDTHN, MARGIN_LINE, 0);   // 首选项关行号：保持 0 宽
        return;
    }
    int lines = (int)Send(SCI_GETLINECOUNT);
    int digits = 1;
    while (lines >= 10) { lines /= 10; ++digits; }
    if (digits == lastLineCountDigits_) return;
    lastLineCountDigits_ = digits;

    std::string sample((size_t)digits, '9');
    sptr_t w = Send(SCI_TEXTWIDTH, STYLE_LINENUMBER, (LPARAM)sample.c_str());
    Send(SCI_SETMARGINWIDTHN, MARGIN_LINE, w + 10);
}

void Editor::ApplyPrefs(const AppSettings& prefs) {
    if (!hwnd_) return;
    Send(SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)xfs::WideToUtf8(prefs.fontName).c_str());
    Send(SCI_STYLESETSIZE, STYLE_DEFAULT, prefs.fontSize);
    Send(SCI_STYLECLEARALL);
    // 记住默认字体：无全局/语言级字体覆盖时，SetLexerByName 用它在切换语言后
    // 重置 STYLE_DEFAULT，避免残留上一语言的整行字体（2026-08-30 用户报告）。
    defaultFont_ = xfs::WideToUtf8(prefs.fontName);
    defaultFontSize_ = prefs.fontSize;
    if (prefs.tabWidth >= 1 && prefs.tabWidth <= 64)
        Send(SCI_SETTABWIDTH, prefs.tabWidth);
    SetWordWrap(prefs.wrapOn);
    autoClose_ = prefs.autoCloseBrackets;
    SetAutoIndent(prefs.autoIndent);
    SetAutoComplete(prefs.autoComplete);
    Send(SCI_SETCARETWIDTH, (prefs.caretWidth >= 1 && prefs.caretWidth <= 3)
                                ? prefs.caretWidth : 2);
    Send(SCI_SETCARETLINEVISIBLE, prefs.currentLineHighlight ? 1 : 0);
    showLineNumber_ = prefs.showLineNumber;
    if (showLineNumber_) {
        // 强制重算宽度：UpdateLineNumberWidth 的"位数未变即跳过"优化会在
        // 边距刚被清零（此前关闭行号）时挡住重新展开（2026-08-29 用户报告）。
        lastLineCountDigits_ = 0;
        UpdateLineNumberWidth();
    } else {
        Send(SCI_SETMARGINWIDTHN, MARGIN_LINE, 0);
    }
    Logger::Info("ApplyPrefs: autoCloseBrackets=" +
                 std::to_string(prefs.autoCloseBrackets ? 1 : 0) +
                 " showLineNumber=" + std::to_string(prefs.showLineNumber ? 1 : 0) +
                 " caretW=" + std::to_string(prefs.caretWidth) +
                 " autoIndent=" + std::to_string(prefs.autoIndent ? 1 : 0));
    Send(SCI_SETINDENTATIONGUIDES, prefs.indentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE);
    Send(SCI_SETVIEWWS, prefs.showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
    autoDetect_ = prefs.autoDetectLang;
}

void Editor::ApplyTheme(const ThemeDef& t) {
    if (!hwnd_) return;
    const GlobalOverride& go = GlobalStyler().Global();
    Send(SCI_STYLESETFORE, STYLE_DEFAULT, go.enableFg ? go.fg : t.editorFg);
    Send(SCI_STYLESETBACK, STYLE_DEFAULT, go.enableBg ? go.bg : t.editorBg);
    Send(SCI_STYLECLEARALL);
    Send(SCI_SETCARETFORE, t.caret);
    Send(SCI_SETCARETLINEBACK, t.currentLineBack);
    Send(SCI_SETSELBACK, 1, t.selectionBack);
    Send(SCI_SETSELFORE, 1, t.editorFg);
    Send(SCI_STYLESETFORE, STYLE_LINENUMBER, t.lineNumFg);
    Send(SCI_STYLESETBACK, STYLE_LINENUMBER, t.lineNumBack);
    Send(SCI_SETMARGINBACKN, MARGIN_SYMBOL, t.lineNumBack);
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDEROPEN, t.foldArrow);
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDEROPEN, t.lineNumBack);
    Send(SCI_MARKERSETFORE, SC_MARKNUM_FOLDER, t.foldArrow);
    Send(SCI_MARKERSETBACK, SC_MARKNUM_FOLDER, t.lineNumBack);
    Send(SCI_MARKERSETFORE, MARK_BOOKMARK, t.bookmark);
    Send(SCI_MARKERSETBACK, MARK_BOOKMARK, t.bookmark);
    // STYLECLEARALL 清掉了 indicator 定义，重新建立
    Send(SCI_INDICSETSTYLE, 8, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, 8, RGB(255, 180, 0));
    Send(SCI_INDICSETALPHA, 8, 80);
    Send(SCI_INDICSETSTYLE, 9, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, 9, RGB(255, 230, 0));
    Send(SCI_INDICSETALPHA, 9, 70);
}

// --- syntax theme tables -------------------------------------------------------

namespace {

enum Role { RComment = SR_Comment, RString = SR_String, RNumber = SR_Number,
            RKeyword = SR_Keyword, RKeyword2 = SR_Keyword2, ROperator = SR_Operator,
            RClass = SR_Class, RPreproc = SR_Preproc, RSpecial = SR_Special };

struct StyleRole { int style; Role role; };

const StyleRole kCppStyles[] = {
    {SCE_C_COMMENT, RComment}, {SCE_C_COMMENTLINE, RComment},
    {SCE_C_COMMENTDOC, RComment}, {SCE_C_COMMENTLINEDOC, RComment},
    {SCE_C_NUMBER, RNumber}, {SCE_C_WORD, RKeyword}, {SCE_C_WORD2, RKeyword2},
    {SCE_C_STRING, RString}, {SCE_C_CHARACTER, RString},
    {SCE_C_STRINGEOL, RString}, {SCE_C_VERBATIM, RString},
    {SCE_C_STRINGRAW, RString}, {SCE_C_HASHQUOTEDSTRING, RString},
    {SCE_C_REGEX, RSpecial}, {SCE_C_ESCAPESEQUENCE, RSpecial},
    {SCE_C_OPERATOR, ROperator}, {SCE_C_GLOBALCLASS, RClass},
    {SCE_C_PREPROCESSOR, RPreproc},
};

const StyleRole kPythonStyles[] = {
    {SCE_P_COMMENTLINE, RComment}, {SCE_P_COMMENTBLOCK, RComment},
    {SCE_P_NUMBER, RNumber}, {SCE_P_WORD, RKeyword}, {SCE_P_WORD2, RKeyword2},
    {SCE_P_STRING, RString}, {SCE_P_CHARACTER, RString},
    {SCE_P_TRIPLE, RString}, {SCE_P_TRIPLEDOUBLE, RString},
    {SCE_P_FSTRING, RString}, {SCE_P_FCHARACTER, RString},
    {SCE_P_FTRIPLE, RString}, {SCE_P_FTRIPLEDOUBLE, RString},
    {SCE_P_STRINGEOL, RString}, {SCE_P_DECORATOR, RKeyword2},
    {SCE_P_OPERATOR, ROperator}, {SCE_P_CLASSNAME, RClass},
    {SCE_P_DEFNAME, RClass},
};

const StyleRole kHtmlStyles[] = {
    {SCE_H_TAG, RKeyword}, {SCE_H_TAGUNKNOWN, RKeyword},
    {SCE_H_TAGEND, RKeyword}, {SCE_H_XMLSTART, RKeyword}, {SCE_H_XMLEND, RKeyword},
    {SCE_H_ATTRIBUTE, RClass}, {SCE_H_ATTRIBUTEUNKNOWN, RClass},
    {SCE_H_NUMBER, RNumber}, {SCE_H_DOUBLESTRING, RString},
    {SCE_H_SINGLESTRING, RString}, {SCE_H_OTHER, RString},
    {SCE_H_COMMENT, RComment}, {SCE_H_ENTITY, RSpecial},
    {SCE_H_XCCOMMENT, RComment},
    // embedded JavaScript
    {SCE_HJ_COMMENT, RComment}, {SCE_HJ_COMMENTLINE, RComment},
    {SCE_HJ_COMMENTDOC, RComment}, {SCE_HJ_NUMBER, RNumber},
    {SCE_HJ_WORD, RKeyword}, {SCE_HJ_KEYWORD, RKeyword2},
    {SCE_HJ_DOUBLESTRING, RString}, {SCE_HJ_SINGLESTRING, RString},
    {SCE_HJ_TEMPLATELITERAL, RString}, {SCE_HJ_STRINGEOL, RString},
    {SCE_HJ_REGEX, RSpecial}, {SCE_HJ_SYMBOLS, ROperator},
    // VB script in HTML
    {SCE_HB_COMMENTLINE, RComment}, {SCE_HB_NUMBER, RNumber},
    {SCE_HB_WORD, RKeyword}, {SCE_HB_STRING, RString},
    // embedded PHP
    {SCE_HPHP_SIMPLESTRING, RString}, {SCE_HPHP_HSTRING, RString},
    {SCE_HPHP_WORD, RKeyword}, {SCE_HPHP_NUMBER, RNumber},
    {SCE_HPHP_COMMENT, RComment}, {SCE_HPHP_COMMENTLINE, RComment},
    {SCE_HPHP_VARIABLE, RSpecial}, {SCE_HPHP_OPERATOR, ROperator},
};

const StyleRole kCssStyles[] = {
    {SCE_CSS_TAG, RKeyword}, {SCE_CSS_CLASS, RClass}, {SCE_CSS_ID, RClass},
    {SCE_CSS_PSEUDOCLASS, RClass}, {SCE_CSS_UNKNOWN_PSEUDOCLASS, RClass},
    {SCE_CSS_OPERATOR, ROperator}, {SCE_CSS_COMMENT, RComment},
    {SCE_CSS_DOUBLESTRING, RString}, {SCE_CSS_SINGLESTRING, RString},
    {SCE_CSS_IMPORTANT, RNumber}, {SCE_CSS_VARIABLE, RSpecial},
};

const StyleRole kJsonStyles[] = {
    {SCE_JSON_NUMBER, RNumber}, {SCE_JSON_STRING, RString},
    {SCE_JSON_PROPERTYNAME, RClass}, {SCE_JSON_ESCAPESEQUENCE, RSpecial},
    {SCE_JSON_LINECOMMENT, RComment}, {SCE_JSON_BLOCKCOMMENT, RComment},
    {SCE_JSON_OPERATOR, ROperator}, {SCE_JSON_KEYWORD, RKeyword},
};

const StyleRole kYamlStyles[] = {
    {SCE_YAML_COMMENT, RComment}, {SCE_YAML_KEYWORD, RKeyword},
    {SCE_YAML_NUMBER, RNumber}, {SCE_YAML_REFERENCE, RClass},
    {SCE_YAML_OPERATOR, ROperator},
};

const StyleRole kSqlStyles[] = {
    {SCE_SQL_COMMENT, RComment}, {SCE_SQL_COMMENTLINE, RComment},
    {SCE_SQL_COMMENTDOC, RComment}, {SCE_SQL_COMMENTLINEDOC, RComment},
    {SCE_SQL_SQLPLUS, RComment}, {SCE_SQL_SQLPLUS_COMMENT, RComment},
    {SCE_SQL_NUMBER, RNumber}, {SCE_SQL_WORD, RKeyword},
    {SCE_SQL_STRING, RString}, {SCE_SQL_CHARACTER, RString},
    {SCE_SQL_OPERATOR, ROperator}, {SCE_SQL_IDENTIFIER, RClass},
};

const StyleRole kBashStyles[] = {
    {SCE_SH_COMMENTLINE, RComment}, {SCE_SH_NUMBER, RNumber},
    {SCE_SH_WORD, RKeyword}, {SCE_SH_STRING, RString},
    {SCE_SH_CHARACTER, RString}, {SCE_SH_BACKTICKS, RString},
    {SCE_SH_SCALAR, RSpecial}, {SCE_SH_PARAM, RSpecial},
    {SCE_SH_OPERATOR, ROperator},
};

const StyleRole kPsStyles[] = {
    {SCE_POWERSHELL_COMMENT, RComment}, {SCE_POWERSHELL_COMMENTSTREAM, RComment},
    {SCE_POWERSHELL_STRING, RString}, {SCE_POWERSHELL_CHARACTER, RString},
    {SCE_POWERSHELL_HERE_STRING, RString}, {SCE_POWERSHELL_HERE_CHARACTER, RString},
    {SCE_POWERSHELL_NUMBER, RNumber}, {SCE_POWERSHELL_VARIABLE, RSpecial},
    {SCE_POWERSHELL_OPERATOR, ROperator}, {SCE_POWERSHELL_KEYWORD, RKeyword},
    {SCE_POWERSHELL_CMDLET, RKeyword2}, {SCE_POWERSHELL_FUNCTION, RClass},
};

const StyleRole kBatStyles[] = {
    {SCE_BAT_COMMENT, RComment}, {SCE_BAT_WORD, RKeyword},
    {SCE_BAT_LABEL, RClass}, {SCE_BAT_COMMAND, RKeyword2},
    {SCE_BAT_OPERATOR, ROperator},
};

struct FamilyMap { const char* lexer; const StyleRole* styles; size_t n; };
#define MAP(name) {name, name##Styles, ARRAYSIZE(name##Styles)}
const FamilyMap kFamilies[] = {
    {"cpp",        kCppStyles,   ARRAYSIZE(kCppStyles)},
    {"python",     kPythonStyles, ARRAYSIZE(kPythonStyles)},
    {"hypertext",  kHtmlStyles,  ARRAYSIZE(kHtmlStyles)},
    {"xml",        kHtmlStyles,  ARRAYSIZE(kHtmlStyles)},
    {"css",        kCssStyles,   ARRAYSIZE(kCssStyles)},
    {"json",       kJsonStyles,  ARRAYSIZE(kJsonStyles)},
    {"yaml",       kYamlStyles,  ARRAYSIZE(kYamlStyles)},
    {"sql",        kSqlStyles,   ARRAYSIZE(kSqlStyles)},
    {"bash",       kBashStyles,  ARRAYSIZE(kBashStyles)},
    {"powershell", kPsStyles,    ARRAYSIZE(kPsStyles)},
    {"batch",      kBatStyles,   ARRAYSIZE(kBatStyles)},
};

} // namespace

void Editor::SetLexerByName(const char* lexerName, const char* const* keywords,
                            const ThemeDef* t) {
    ILexer5* lexer = lexerName ? ::CreateLexer(lexerName) : nullptr;
    lexerName_ = lexerName ? lexerName : "";
    wordCacheValid_ = false;   // 词法器切换 → 关键词来源变化，词汇缓存重建
    Send(SCI_CLEARDOCUMENTSTYLE);
    if (lexer) {
        Send(SCI_SETILEXER, 0, (LPARAM)lexer);   // Scintilla takes ownership
        for (int i = 0; i < 2; ++i) {
            const char* kw = (keywords && keywords[i]) ? keywords[i] : nullptr;
            if (kw && kw[0]) Send(SCI_SETKEYWORDS, (uptr_t)i, (LPARAM)kw);
        }
        // keep fold margin working after lexer switch
        Send(SCI_SETPROPERTY, (uptr_t)"fold", (LPARAM)"1");
        Send(SCI_SETPROPERTY, (uptr_t)"fold.compact", (LPARAM)"0");
    } else {
        Send(SCI_SETILEXER, 0, (LPARAM)(ILexer5*)nullptr);
    }
    Send(SCI_STYLECLEARALL);
    if (t) {
        // 先解析全局/语言级字体链并写入 STYLE_DEFAULT，让 ApplyTheme 内部的
        // STYLECLEARALL 把字体/字形传播到全部样式；否则全局字体只影响默认样式
        // （2026-08-30 用户报告：粗体/斜体/下划线不生效、语言整行字体残留）。
        // 纯文本（lexerName==nullptr）也解析：全局字体/字形应作用于普通文本。
        std::wstring gf; int gs = 0;
        bool gb = false, gi = false, gu = false;
        GlobalStyler().ResolveFont(lexerName, *t, &gf, &gs, &gb, &gi, &gu);
        // 总是重置 STYLE_DEFAULT 字体/字号：无覆盖时回退默认字体，避免切换语言后
        // 残留上一语言的整行字体（2026-08-30 用户报告）。
        const std::string fontName = gf.empty() ? defaultFont_ : WideToUtf8(gf);
        Send(SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)fontName.c_str());
        Send(SCI_STYLESETSIZE, STYLE_DEFAULT, gs > 0 ? gs : defaultFontSize_);
        // 字形必须显式 0/1：若只在开启时写 1，取消后 STYLE_DEFAULT 仍残留加粗，
        // STYLECLEARALL 又会把它传播到全部样式（2026-08-30 用户报告）。
        Send(SCI_STYLESETBOLD, STYLE_DEFAULT, gb ? 1 : 0);
        Send(SCI_STYLESETITALIC, STYLE_DEFAULT, gi ? 1 : 0);
        Send(SCI_STYLESETUNDERLINE, STYLE_DEFAULT, gu ? 1 : 0);

        ApplyTheme(*t);   // re-asserts default fore/back after STYLECLEARALL
        if (lexerName) {
            const ThemeDef& th = *t;
            StylerStore& styler = GlobalStyler();
            for (const FamilyMap& fam : kFamilies) {
                if (strcmp(fam.lexer, lexerName) != 0) continue;
                for (size_t i = 0; i < fam.n; ++i) {
                    const int st = fam.styles[i].style;
                    const int role = fam.styles[i].role;
                    // 配色解析链：Global override > stylers.json 覆盖 > 主题角色色
                    Send(SCI_STYLESETFORE, st, styler.ResolveFg(lexerName, role, th));
                    Send(SCI_STYLESETBACK, st, styler.ResolveBg(lexerName, role, th));
                    const StyleOverride& so = styler.Override(lexerName, role);
                    // 字形：per-style 显式开启 或 全局覆盖开启（取或）。
                    // 必须显式 0/1：只写 1 时取消勾选后残留加粗（2026-08-30 用户报告）。
                    const bool bld = (so.defined && so.bold) || gb;
                    const bool itl = (so.defined && so.italic) || gi;
                    const bool und = (so.defined && so.underline) || gu;
                    Send(SCI_STYLESETBOLD, st, bld ? 1 : 0);
                    Send(SCI_STYLESETITALIC, st, itl ? 1 : 0);
                    Send(SCI_STYLESETUNDERLINE, st, und ? 1 : 0);
                    if (so.defined) {
                        if (!so.font.empty())
                            Send(SCI_STYLESETFONT, st,
                                 (LPARAM)WideToUtf8(so.font).c_str());
                        if (so.fontSize > 0) Send(SCI_STYLESETSIZE, st, so.fontSize);
                    }
                }
                break;
            }
        }
    }
    // re-apply brace highlight indicator (survives STYLECLEARALL via indicators)
    Send(SCI_INDICSETSTYLE, 8, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, 8, RGB(255, 180, 0));
    Send(SCI_INDICSETALPHA, 8, 80);
    // re-apply double-click word highlight indicator (same STYLECLEARALL reason)
    Send(SCI_INDICSETSTYLE, 9, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, 9, RGB(255, 230, 0));
    Send(SCI_INDICSETALPHA, 9, 70);
    Send(SCI_COLOURISE, 0, -1);
    UpdateLineNumberWidth();
}

void Editor::SetLexerForFile(const std::wstring& fileName, const ThemeDef* t) {
    const LanguageInfo* lang = nullptr;
    if (autoDetect_) {
        // 先查用户扩展名映射（userExts），再 fallback 到内置 DetectLanguage
        std::wstring ext;
        size_t dot = fileName.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < fileName.size()) {
            ext = fileName.substr(dot + 1);
            for (auto& c : ext) c = (wchar_t)towlower(c);
        }
        if (!ext.empty()) {
            std::string extA(ext.begin(), ext.end());
            const char* userLex = GlobalStyler().UserLexerForExt(extA.c_str());
            if (userLex) {
                const LanguageMenuItem* cat = LanguageMenuCatalog();
                for (int i = 0; cat[i].label; ++i) {
                    if (cat[i].lexerName && strcmp(cat[i].lexerName, userLex) == 0) {
                        const char* kw[2] = { cat[i].keywords[0], cat[i].keywords[1] };
                        SetLexerByName(userLex, kw, t);
                        return;
                    }
                }
                // 在 catalog 中找不到词法器名时直接用名字（内置 Lexilla 词法器名）
                const char* kw[2] = { nullptr, nullptr };
                SetLexerByName(userLex, kw, t);
                return;
            }
        }
        lang = DetectLanguage(fileName);
    }
    const char* kw[2] = { lang ? lang->keywords[0] : nullptr,
                          lang ? lang->keywords[1] : nullptr };
    SetLexerByName(lang ? lang->lexerName : nullptr, kw, t);
}

// --- text ------------------------------------------------------------------

void Editor::SetTextUtf8(const std::string& utf8) {
    SetTextUtf8Buffer(utf8.data(), utf8.size());
}

void Editor::AppendTextUtf8(const char* data, size_t size) {
    if (!data || size == 0) return;
    constexpr size_t kChunkSize = 8 * 1024 * 1024;
    size_t offset = 0;
    while (offset < size) {
        size_t chunkLen = (std::min)(kChunkSize, size - offset);
        Send(SCI_APPENDTEXT, (uptr_t)chunkLen, (LPARAM)(data + offset));
        offset += chunkLen;
    }
    // 流式加载（大文件）路径没有经过 SetTextUtf8Buffer，行号栏宽度不会
    // 随行数增长重算（2026-09-04 用户报告：13M 行文件行号只显示两位数）。
    UpdateLineNumberWidth();
}

void Editor::SetTextUtf8Buffer(const char* data, size_t size) {
    Send(SCI_SETREADONLY, 0);
    Send(SCI_CANCEL, 0, 0);

    // For large texts, use chunked SCI_APPENDTEXT to avoid single huge allocation
    constexpr size_t kChunkSize = 8 * 1024 * 1024;
    if (size > kChunkSize) {
        size_t offset = 0;
        while (offset < size) {
            size_t chunkLen = (std::min)(kChunkSize, size - offset);
            Send(SCI_APPENDTEXT, (uptr_t)chunkLen, (LPARAM)(data + offset));
            offset += chunkLen;
        }
    } else {
        Send(SCI_SETTEXT, 0, (LPARAM)data);
    }

    Send(SCI_EMPTYUNDOBUFFER);
    Send(SCI_SETSAVEPOINT);
    Send(SCI_GOTOPOS, 0);
    Send(SCI_SETUNDOCOLLECTION, 1);
    UpdateLineNumberWidth();
    UpdateScrollWidthEstimate();
}

// Scintilla measures line widths lazily: scrollWidth only grows when a wider
// line actually gets styled/rendered (trackLineWidth). A document whose
// longest lines are far off-screen therefore starts with a tiny scroll range
// and the h-scrollbar "grows into shape" while the user pages right
// (2026-09-11 user report). Preset the width from the longest line's byte
// length; tracking keeps refining afterwards.
void Editor::UpdateScrollWidthEstimate() {
    if (!hwnd_ || largeFile_) return;   // large-file mode pins a fixed 65536 cap
    Send(SCI_SETSCROLLWIDTHTRACKING, 1);
    const sptr_t lines = Send(SCI_GETLINECOUNT);
    if (lines <= 0) return;
    // SCI_LINELENGTH is an O(1) line-index lookup. Scan every line for normal
    // docs; for huge line counts sample head+tail (estimate is close enough).
    constexpr sptr_t kFullScanLimit = 400000;
    constexpr sptr_t kSamplePerSide = 100000;
    size_t maxLen = 0;
    auto scan = [&](sptr_t from, sptr_t to) {
        for (sptr_t i = from; i < to; ++i) {
            sptr_t l = Send(SCI_LINELENGTH, i);
            if ((size_t)l > maxLen) maxLen = (size_t)l;
        }
    };
    if (lines <= kFullScanLimit) {
        scan(0, lines);
    } else {
        scan(0, kSamplePerSide);
        scan(lines - kSamplePerSide, lines);
    }
    if (maxLen == 0) return;
    // Average glyph width from a digit+lowercase sample in the default style.
    // UTF-8 byte count >= char count, so CJK docs overshoot (harmless — the
    // thumb is just a bit small); ASCII is near-exact with the 12% margin.
    const char* sample = "0123456789abcdefghijklmnopqrstuvwxyz";
    sptr_t sampleW = Send(SCI_TEXTWIDTH, STYLE_DEFAULT, (LPARAM)sample);
    if (sampleW <= 0) return;
    const double avg = (double)sampleW / 36.0;
    int width = (int)((double)maxLen * avg * 1.12) + 100;
    width = std::max(width, 2000);          // Scintilla default floor
    width = std::min(width, 4000000);       // int-safe layout bound
    if (width != (int)Send(SCI_GETSCROLLWIDTH))
        Send(SCI_SETSCROLLWIDTH, width);
}

std::string Editor::GetTextUtf8() const {
    sptr_t len = Send(SCI_GETLENGTH);
    if (len <= 0) return {};
    std::string out((size_t)len + 1, '\0');
    Send(SCI_GETTEXT, len + 1, (LPARAM)out.data());
    out.resize((size_t)len);
    return out;
}

void Editor::GetTextRangeUtf8(long long pos, size_t len, std::string& out) const {
    out.clear();
    if (len == 0 || pos < 0) return;
    // clamp to the document: Scintilla may not bound cpMax itself and the
    // caller's std::string would otherwise carry uninitialized bytes
    long long avail = Send(SCI_GETLENGTH) - pos;
    if (avail <= 0) return;
    if ((long long)len > avail) len = (size_t)avail;
    out.resize(len);
    Sci_TextRangeFull tr{};
    tr.chrg.cpMin = (Sci_Position)pos;
    tr.chrg.cpMax = (Sci_Position)(pos + (long long)len);
    tr.lpstrText = out.data();
    sptr_t got = Send(SCI_GETTEXTRANGEFULL, 0, (LPARAM)&tr);
    if (got < 0) got = 0;
    if ((size_t)got < len) out.resize((size_t)got);
}

void Editor::SetSavePoint() { Send(SCI_SETSAVEPOINT); }
bool Editor::Modified() const { return Send(SCI_GETMODIFY) != 0; }

void Editor::SetReadOnly(bool ro) { Send(SCI_SETREADONLY, ro ? 1 : 0); }
bool Editor::ReadOnly() const { return Send(SCI_GETREADONLY) != 0; }

EolMode Editor::Eol() const {
    switch (Send(SCI_GETEOLMODE)) {
        case SC_EOL_CR: return EolMode::CR;
        case SC_EOL_LF: return EolMode::LF;
        default:        return EolMode::CRLF;
    }
}
void Editor::SetEol(EolMode eol) {
    Send(SCI_SETEOLMODE, (int)eol == (int)EolMode::CR ? SC_EOL_CR
        : (int)eol == (int)EolMode::LF ? SC_EOL_LF : SC_EOL_CRLF);
}
void Editor::ConvertEols(EolMode eol) {
    Send(SCI_CONVERTEOLS, (int)eol == (int)EolMode::CR ? SC_EOL_CR
        : (int)eol == (int)EolMode::LF ? SC_EOL_LF : SC_EOL_CRLF);
}
const wchar_t* Editor::EolDisplayName(EolMode eol) {
    switch (eol) {
        case EolMode::CR: return L"Macintosh CR";
        case EolMode::LF: return L"Unix LF";
        default:          return L"Windows CRLF";
    }
}

// --- view --------------------------------------------------------------------

void Editor::ZoomIn()  { Send(SCI_ZOOMIN); }
void Editor::ZoomOut() { Send(SCI_ZOOMOUT); }
void Editor::ZoomReset() { Send(SCI_SETZOOM, 100); }

bool Editor::WordWrap() const { return wordWrap_; }
void Editor::SetWordWrap(bool on) {
    wordWrap_ = on;
    Send(SCI_SETWRAPMODE, on ? SC_WRAP_WHITESPACE : SC_WRAP_NONE);
    Send(SCI_SETWRAPVISUALFLAGS, on ? SC_WRAPVISUALFLAG_END : SC_WRAPVISUALFLAG_NONE);
}

// --- editing -----------------------------------------------------------------

void Editor::GotoLine(int line1based) {
    int max = (int)Send(SCI_GETLINECOUNT) - 1;
    if (line1based > max) line1based = max;
    if (line1based < 1) line1based = 1;
    Send(SCI_ENSUREVISIBLE, line1based - 1);
    Send(SCI_GOTOLINE, line1based - 1);
}
void Editor::EnsureVisibleCurrent() {
    Send(SCI_ENSUREVISIBLEENFORCEPOLICY, Send(SCI_LINEFROMPOSITION, Send(SCI_GETCURRENTPOS)));
}

void Editor::SelectRange(sptr_t start, sptr_t end) {
    if (!hwnd_) return;
    Send(SCI_ENSUREVISIBLEENFORCEPOLICY, Send(SCI_LINEFROMPOSITION, start));
    Send(SCI_SETSELECTION, end, start);
    Send(SCI_SCROLLCARET);
}

// --- bookmarks ----------------------------------------------------------------

namespace {
constexpr unsigned int kBookmarkMask = 1u << MARK_BOOKMARK;
}

void Editor::ToggleBookmark() {
    int line = (int)Send(SCI_LINEFROMPOSITION, Send(SCI_GETCURRENTPOS));
    if (Send(SCI_MARKERGET, line) & kBookmarkMask)
        Send(SCI_MARKERDELETE, line, MARK_BOOKMARK);
    else
        Send(SCI_MARKERADD, line, MARK_BOOKMARK);
}

void Editor::NextBookmark() {
    int lines = (int)Send(SCI_GETLINECOUNT);
    int cur = (int)Send(SCI_LINEFROMPOSITION, Send(SCI_GETCURRENTPOS));
    int line = (int)Send(SCI_MARKERNEXT, cur + 1, kBookmarkMask);
    if (line < 0) line = (int)Send(SCI_MARKERNEXT, 0, kBookmarkMask);
    if (line >= 0 && line < lines) {
        Send(SCI_ENSUREVISIBLE, line);
        Send(SCI_GOTOLINE, line);
    }
}

void Editor::PreviousBookmark() {
    int lines = (int)Send(SCI_GETLINECOUNT);
    int cur = (int)Send(SCI_LINEFROMPOSITION, Send(SCI_GETCURRENTPOS));
    int line = (int)Send(SCI_MARKERPREVIOUS, cur - 1, kBookmarkMask);
    if (line < 0) line = (int)Send(SCI_MARKERPREVIOUS, lines - 1, kBookmarkMask);
    if (line >= 0) {
        Send(SCI_ENSUREVISIBLE, line);
        Send(SCI_GOTOLINE, line);
    }
}

void Editor::ClearBookmarks() {
    Send(SCI_MARKERDELETEALL, MARK_BOOKMARK);
}

// --- 鏍囪椤?line marks ----------------------------------------------------------

void Editor::DefineMarkMarker() {
    Send(SCI_MARKERDEFINE, MARK_MARKED, SC_MARK_ROUNDRECT);
    Send(SCI_MARKERSETFORE, MARK_MARKED, RGB(0xE8, 0x3A, 0x3A));
    Send(SCI_MARKERSETBACK, MARK_MARKED, RGB(0xFF, 0xD0, 0xD0));

    // diff markers: changed (orange) / added-removed (blue)
    // NOTE: SC_MARK_ROUNDRECT is drawn filled with BACK colour and outlined with FORE.
    // These must be defined BEFORE any SCI_MARKERADD - an undefined marker renders
    // as a hollow default circle.
    Send(SCI_MARKERDEFINE, MARK_DIFF_CHANGED, SC_MARK_ROUNDRECT);
    Send(SCI_MARKERSETFORE, MARK_DIFF_CHANGED, RGB(0xB4, 0x5F, 0x06));
    Send(SCI_MARKERSETBACK, MARK_DIFF_CHANGED, RGB(0xFF, 0x9F, 0x2E));
    Send(SCI_MARKERDEFINE, MARK_DIFF_ADDED, SC_MARK_ROUNDRECT);
    Send(SCI_MARKERSETFORE, MARK_DIFF_ADDED, RGB(0x00, 0x51, 0x9E));
    Send(SCI_MARKERSETBACK, MARK_DIFF_ADDED, RGB(0x2E, 0x8B, 0xF0));
}

bool Editor::MarkLine(int line1based) {
    int line = line1based - 1;
    if (line < 0) return false;
    DefineMarkMarker();
    if (Send(SCI_MARKERGET, line) & (1 << MARK_MARKED)) return false;
    Send(SCI_MARKERADD, line, MARK_MARKED);
    return true;
}

void Editor::ClearLineMarks() {
    Send(SCI_MARKERDELETEALL, MARK_MARKED);
}

// --- auto-indent / auto-close -------------------------------------------------

void Editor::SetAutoIndent(bool on) { autoIndent_ = on; }
void Editor::SetAutoCloseBrackets(bool on) { autoClose_ = on; }

void Editor::HandleCharAdded(SCNotification* sn) {
    // Auto-close brackets/quotes is handled in EditorKeyProc (WM_CHAR path).
    // Auto-complete v1: 输入到第 3 个构词字符时弹出（关键词+文档词汇）。
    if (sn) HandleAutocompleteChar((unsigned int)sn->ch);
}

// ---- 自动补全 v1（docs/settings-plan.md 之外的编辑器缺口批次） ----------------

namespace {

bool IsWordCharW(unsigned ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

// 语言关键词集合（按 lexerName 缓存；keywords 是空格分隔的大字符串）
const std::set<std::string>& KeywordSetFor(const std::string& lexerName) {
    static std::map<std::string, std::set<std::string>> cache;
    auto it = cache.find(lexerName);
    if (it != cache.end()) return it->second;
    std::set<std::string> out;
    for (const LanguageMenuItem* e = LanguageMenuCatalog(); e->label; ++e) {
        if (!e->lexerName || strcmp(lexerName.c_str(), e->lexerName) != 0) continue;
        for (int k = 0; k < 2; ++k) {          // 两套关键词集都收集
            const char* kw = e->keywords[k];
            if (!kw) continue;
            const char* p = kw;
            while (*p) {
                const char* q = p;
                while (*q && *q != ' ') ++q;
                if (q > p) out.emplace(p, q);
                p = (*q) ? q + 1 : q;
            }
        }
        break;                                  // 目录里每个词法器只有一个条目
    }
    return cache.emplace(lexerName, std::move(out)).first->second;
}

} // namespace

void Editor::HandleAutocompleteChar(unsigned int ch) {
    if (!autoComplete_ || !hwnd_) return;
    if (!IsWordCharW(ch)) {
        if (Send(SCI_AUTOCACTIVE)) Send(SCI_AUTOCCANCEL);
        return;
    }
    const sptr_t pos = Send(SCI_GETCURRENTPOS);
    const sptr_t start = Send(SCI_WORDSTARTPOSITION, pos, 1);
    const int len = (int)(pos - start);
    if (len < 3 || len > 64) {
        if (Send(SCI_AUTOCACTIVE)) Send(SCI_AUTOCCANCEL);
        return;
    }
    if (Send(SCI_AUTOCACTIVE)) return;   // 已在补全中，Scintilla 自动继续过滤
    // 读前缀（caret 前 len 字节）
    std::string prefix((size_t)len, '\0');
    Sci_TextRange tr{{(sptr_t)(pos - len), pos}, prefix.data()};
    Send(SCI_GETTEXTRANGE, 0, (LPARAM)&tr);
    ShowAutocomplete(prefix);
}

void Editor::ShowAutocomplete(const std::string& prefix) {
    // 1) 关键词（按当前词法器缓存拆分）
    const std::set<std::string>& kws = KeywordSetFor(lexerName_);
    // 2) 文档词汇（>1MB 文档只出关键词；每次触发重建——1MB 内扫描毫秒级，
    //    且仅在词首第 3 字符触发一次；节流会让新词不可见，已踩坑）
    //    批次 31：lexer 有注释/字符串风格表 → GETSTYLEDTEXT 过滤扫描，
    //    注释里「只」出现一次的词不进候选；否则退回纯文本扫描。
    const sptr_t docLen = Send(SCI_GETLENGTH);
    wordCache_.clear();
    if (docLen > 0 && docLen <= 1024 * 1024) {
        const WordStyleFilter* f = WordStyleFilterFor(lexerName_.c_str());
        if (f && f->Any()) {
            std::string buf((size_t)docLen * 2 + 2, '\0');
            Sci_TextRange tr{{0, docLen}, buf.data()};
            Send(SCI_GETSTYLEDTEXT, 0, (LPARAM)&tr);
            ScanWordsStyled((const unsigned char*)buf.data(), (size_t)docLen,
                            *f, wordCache_);
        } else {
            std::string buf((size_t)docLen + 1, '\0');
            Send(SCI_GETTEXT, docLen, (LPARAM)buf.data());
            ScanWordsPlain(buf.data(), (size_t)docLen, wordCache_);
        }
        if (wordCache_.size() > 5000) {   // 超大词汇量截断，防列表爆炸
            auto it = wordCache_.begin();
            std::advance(it, 5000);
            wordCache_.erase(it, wordCache_.end());
        }
    }
    // 2b) 批次 31：跨标签词汇（Workspace 注入；每次触发重建，Workspace 端
    //     限单文档 512KB / 总量 3000 词，控制弹出延迟）
    std::set<std::string> ext;
    if (extWords_) extWords_(ext);
    // 3) 按前缀过滤（大小写不敏感）合并去重，上限 100 条
    //    （优先级：关键词 > 本档词汇 > 其它标签词汇）
    std::set<std::string> cands;
    auto match = [&](const std::string& w) {
        return w.size() >= prefix.size() &&
               _strnicmp(w.c_str(), prefix.c_str(), prefix.size()) == 0 &&
               _stricmp(w.c_str(), prefix.c_str()) != 0;
    };
    for (const std::string& w : kws)
        if (match(w) && cands.size() < 100) cands.insert(w);
    for (const std::string& w : wordCache_)
        if (match(w) && cands.size() < 100) cands.insert(w);
    for (const std::string& w : ext)
        if (match(w) && cands.size() < 100) cands.insert(w);
    if (cands.empty()) return;

    std::string list;
    for (const std::string& w : cands) {
        if (!list.empty()) list += ' ';
        list += w;
    }
    Send(SCI_AUTOCSETSEPARATOR, ' ');
    Send(SCI_AUTOCSETIGNORECASE, 1);
    Send(SCI_AUTOCSETAUTOHIDE, 1);
    Send(SCI_AUTOCSETDROPRESTOFWORD, 0);
    Send(SCI_AUTOCSETORDER, SC_ORDER_PERFORMSORT);
    Send(SCI_AUTOCSETMAXHEIGHT, 8);
    Send(SCI_AUTOCSHOW, (uptr_t)prefix.size(), (LPARAM)list.c_str());
}

void Editor::CollectDocWords(std::set<std::string>& out) {
    out = wordCache_;   // 保留接口供测试/后续扩展
}

// 批次 31：本档词汇抽取（纯文本口径；跨标签收集由 Workspace 复用此函数）
void Editor::ExtractWords(std::set<std::string>& out) const {
    const sptr_t docLen = Send(SCI_GETLENGTH);
    if (docLen <= 0 || docLen > 1024 * 1024) return;
    std::string buf((size_t)docLen + 1, '\0');
    Send(SCI_GETTEXT, docLen, (LPARAM)buf.data());
    ScanWordsPlain(buf.data(), (size_t)docLen, out);
}

void Editor::SortSelectedLines(bool ascending) {
    sptr_t selStart = Send(SCI_GETSELECTIONSTART);
    sptr_t selEnd = Send(SCI_GETSELECTIONEND);
    int lineStart = (int)Send(SCI_LINEFROMPOSITION, selStart);
    int lineEnd = (int)Send(SCI_LINEFROMPOSITION, selEnd);
    if (lineStart >= lineEnd) return;   // need at least 2 lines

    // collect line texts
    std::vector<std::string> lines;
    for (int i = lineStart; i <= lineEnd; ++i) {
        char buf[4096];
        sptr_t len = Send(SCI_GETLINE, i, (LPARAM)buf);
        if (len > 0 && len < 4095) {
            buf[len] = '\0';
            std::string s(buf);
            // strip EOL
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            lines.push_back(s);
        } else {
            lines.push_back("");
        }
    }
    if (ascending) std::sort(lines.begin(), lines.end());
    else std::sort(lines.begin(), lines.end(), std::greater<std::string>());

    // build replacement text
    std::string repl;
    for (auto& s : lines) { repl += s; repl += "\r\n"; }

    // replace range
    Send(SCI_BEGINUNDOACTION);
    Send(SCI_SETTARGETSTART, Send(SCI_POSITIONFROMLINE, lineStart));
    Send(SCI_SETTARGETEND, Send(SCI_POSITIONFROMLINE, lineEnd + 1));
    Send(SCI_REPLACETARGET, (uptr_t)repl.size(), (LPARAM)repl.c_str());
    Send(SCI_ENDUNDOACTION);
}

void Editor::RemoveDuplicateLines() {
    sptr_t total = Send(SCI_GETLINECOUNT);
    std::vector<std::string> unique;
    std::set<std::string> seen;
    for (sptr_t i = 0; i < total; ++i) {
        char buf[4096];
        sptr_t len = Send(SCI_GETLINE, i, (LPARAM)buf);
        if (len > 4094) continue;
        buf[len > 0 ? len : 0] = '\0';
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty() || !seen.count("")) {
            if (!seen.count(s)) { seen.insert(s); unique.push_back(s); }
        }
    }
    std::string repl;
    for (auto& s : unique) { repl += s; repl += "\r\n"; }
    Send(SCI_BEGINUNDOACTION);
    Send(SCI_TARGETWHOLEDOCUMENT);
    Send(SCI_REPLACETARGET, (uptr_t)repl.size(), (LPARAM)repl.c_str());
    Send(SCI_ENDUNDOACTION);
}

void Editor::TrimTrailingSpace() {
    sptr_t total = Send(SCI_GETLINECOUNT);
    Send(SCI_BEGINUNDOACTION);
    for (int i = (int)total - 1; i >= 0; --i) {
        sptr_t lineStart = Send(SCI_POSITIONFROMLINE, i);
        sptr_t lineEnd = Send(SCI_GETLINEENDPOSITION, i);
        // scan backwards from end for first non-space char
        sptr_t lastNonSpace = lineEnd;
        for (sptr_t p = lineEnd - 1; p >= lineStart; --p) {
            char c = (char)Send(SCI_GETCHARAT, p, 0);
            if (c != ' ' && c != '\t') break;
            --lastNonSpace;
        }
        if (lastNonSpace < lineEnd)
            Send(SCI_DELETERANGE, lastNonSpace, lineEnd - lastNonSpace);
    }
    Send(SCI_ENDUNDOACTION);
}

// ---------------------------------------------------------------- 批次 29 行变换

namespace {
const char* DocEolStr(sptr_t eolMode) {
    // SC_EOL_CRLF=0 SC_EOL_LF=1 SC_EOL_CR=2
    return eolMode == 0 ? "\r\n" : (eolMode == 2 ? "\r" : "\n");
}
} // namespace

// 目标行范围读取：无选择=当前行；选择末行只到行首则不含该行。
// 返回目标文本与 [start,end) 位置（Scintilla target 不污染）。
bool Editor::CollectLineRange(std::string* out, sptr_t* outStart,
                              sptr_t* outEnd) {
    sptr_t selStart = Send(SCI_GETSELECTIONSTART);
    sptr_t selEnd = Send(SCI_GETSELECTIONEND);
    int ls = (int)Send(SCI_LINEFROMPOSITION, selStart);
    int le = (int)Send(SCI_LINEFROMPOSITION, selEnd);
    if (le < ls) le = ls;
    if (selEnd > selStart && selEnd == Send(SCI_POSITIONFROMLINE, le)) {
        if (--le < ls) return false;
    }
    sptr_t ts = Send(SCI_POSITIONFROMLINE, ls);
    sptr_t te = Send(SCI_POSITIONFROMLINE, le + 1);
    if (te <= ts) return false;
    std::vector<char> buf((size_t)(te - ts) + 1, 0);
    Send(SCI_SETTARGETSTART, ts);
    Send(SCI_SETTARGETEND, te);
    Send(SCI_GETTARGETTEXT, 0, (LPARAM)buf.data());
    *out = std::string(buf.data(), (size_t)(te - ts));
    *outStart = ts;
    *outEnd = te;
    return true;
}

void Editor::ReplaceLineRange(sptr_t start, sptr_t end,
                              const std::string& repl) {
    Send(SCI_BEGINUNDOACTION);
    Send(SCI_SETTARGETSTART, start);
    Send(SCI_SETTARGETEND, end);
    Send(SCI_REPLACETARGET, (uptr_t)repl.size(), (LPARAM)repl.c_str());
    Send(SCI_ENDUNDOACTION);
}

void Editor::RemoveEmptyLines() {
    std::string text;
    sptr_t ts = 0, te = 0;
    if (!CollectLineRange(&text, &ts, &te)) return;
    std::string out = LineOps::RemoveEmptyLines(text,
                                                DocEolStr(Send(SCI_GETEOLMODE)));
    ReplaceLineRange(ts, te, out);
}

void Editor::ReverseLines() {
    std::string text;
    sptr_t ts = 0, te = 0;
    if (!CollectLineRange(&text, &ts, &te)) return;
    std::string out = LineOps::ReverseLines(text,
                                            DocEolStr(Send(SCI_GETEOLMODE)));
    ReplaceLineRange(ts, te, out);
}

void Editor::ToggleLineComment() {
    // lexer 名 → 行注释符（LanguageMap 批次 29 表）；无行注释语言不动作
    const char* prefix = LineCommentToken(lexerName_.c_str());
    if (!prefix) return;
    std::string text;
    sptr_t ts = 0, te = 0;
    if (!CollectLineRange(&text, &ts, &te)) return;
    bool commented = false;
    std::string out = LineOps::ToggleComment(text, prefix,
                                             DocEolStr(Send(SCI_GETEOLMODE)),
                                             &commented);
    ReplaceLineRange(ts, te, out);
    // 重新选中受影响行（注释切换后光标停回首行行首，便于连按）
    SelectRange(ts, ts + (sptr_t)out.size());
}

void Editor::JoinSelectedLines() {
    sptr_t selStart = Send(SCI_GETSELECTIONSTART);
    sptr_t selEnd = Send(SCI_GETSELECTIONEND);
    int ls = (int)Send(SCI_LINEFROMPOSITION, selStart);
    int le = (int)Send(SCI_LINEFROMPOSITION, selEnd);
    if (le < ls) le = ls;
    if (selEnd > selStart && selEnd == Send(SCI_POSITIONFROMLINE, le) && le > ls)
        --le;
    if (le == ls) {
        // 无多行选择：当前行与下一行相连；已是末行则无事可做
        if (ls + 1 >= (int)Send(SCI_GETLINECOUNT)) return;
        le = ls + 1;
    }
    // 目标末端 = 行 le 内容末（GETLINEENDPOSITION，不含 le 自身 EOL）。
    // LinesJoin 循环条件 pos < targetEnd 且 target 随删除同步收缩——若把 le
    // 的 EOL 也圈进 target（+1 / POSITIONFROMLINE(le+1)），le 会与 le+1 并掉。
    // 末行无 EOL 时 LineEnd(le)=文档长，语义一致。
    sptr_t ts = Send(SCI_POSITIONFROMLINE, ls);
    sptr_t te = Send(SCI_GETLINEENDPOSITION, le);
    if (te <= ts) return;
    Send(SCI_BEGINUNDOACTION);
    Send(SCI_SETTARGETSTART, ts);
    Send(SCI_SETTARGETEND, te);
    Send(SCI_LINESJOIN, 1);   // 行尾替换为一个空格（wp=0 则直接拼）
    Send(SCI_ENDUNDOACTION);
    Send(SCI_SETSEL, ts, ts);   // 光标收到行首，便于连按
}

void Editor::SplitLineAtCaret() {
    sptr_t selStart = Send(SCI_GETSELECTIONSTART);
    sptr_t selEnd = Send(SCI_GETSELECTIONEND);
    int ls = (int)Send(SCI_LINEFROMPOSITION, selStart);
    int le = (int)Send(SCI_LINEFROMPOSITION, selEnd);
    if (le < ls) le = ls;
    Send(SCI_BEGINUNDOACTION);
    Send(SCI_SETTARGETSTART, Send(SCI_POSITIONFROMLINE, ls));
    Send(SCI_SETTARGETEND, Send(SCI_POSITIONFROMLINE, le + 1));
    Send(SCI_LINESSPLIT, 0);   // 0 = 按窗口宽度断行
    Send(SCI_ENDUNDOACTION);
}




void Editor::ClearDiffMarks() {
    Send(SCI_MARKERDELETEALL, 5);
    Send(SCI_MARKERDELETEALL, 6);
}

void Editor::MarkDiffLine(int line0, bool added) {
    if (line0 < 0) return;
    DefineMarkMarker();   // ensure marker 5/6 appearance exists (default = hollow circle)
    Send(SCI_MARKERADD, line0, added ? MARK_DIFF_ADDED : MARK_DIFF_CHANGED);
}

void Editor::FoldAll() {
    sptr_t count = Send(SCI_GETLINECOUNT);    for (sptr_t i = 0; i < count; ++i) {
        int lvl = (int)Send(SCI_GETFOLDLEVEL, i, 0);
        if (lvl & SC_FOLDLEVELHEADERFLAG)
            Send(SCI_SETFOLDEXPANDED, i, 0);
    }
}

void Editor::UnfoldAll() {
    sptr_t count = Send(SCI_GETLINECOUNT);
    for (sptr_t i = 0; i < count; ++i) {
        int lvl = (int)Send(SCI_GETFOLDLEVEL, i, 0);
        if (lvl & SC_FOLDLEVELHEADERFLAG)
            Send(SCI_SETFOLDEXPANDED, i, 1);
    }
}

std::string Editor::GetLineIndentText(int line) {
    if (line < 0) return "";
    char buf[2048];
    sptr_t len = Send(SCI_GETLINE, line, (LPARAM)buf);
    if (len <= 0 || len > 2047) return "";
    buf[len] = '\0';
    std::string result;
    for (sptr_t i = 0; i < len; ++i) {
        if (buf[i] == ' ' || buf[i] == '\t')
            result += buf[i];
        else break;
    }
    return result;
}

// --- large file mode ------------------------------------------------------------

void Editor::EnableLargeFileMode() {
    largeFile_ = true;
    // ATE pattern files are large but editable — keep editing enabled.
    // Optimizations: skip syntax highlighting, hide fold margin,
    // disable current-line highlight, force word-wrap off.
    Send(SCI_SETILEXER, 0, (LPARAM)(ILexer5*)nullptr);
    Send(SCI_STYLECLEARALL);
    Send(SCI_SETCARETLINEVISIBLE, 0);
    Send(SCI_SETMARGINWIDTHN, 2, 0);
    Send(SCI_SETCODEPAGE, SC_CP_UTF8);
    Send(SCI_SETWRAPMODE, SC_WRAP_NONE);       // critical for single-line huge files
    Send(SCI_SETLAYOUTCACHE, SC_CACHE_PAGE);   // cache visible page layout
    Send(SCI_SETSCROLLWIDTH, 65536);           // cap horizontal scroll extent
}

EditorStatus Editor::Status() const {
    EditorStatus s;
    sptr_t pos = Send(SCI_GETCURRENTPOS);
    s.line = (int)Send(SCI_LINEFROMPOSITION, pos) + 1;
    s.column = (int)Send(SCI_GETCOLUMN, pos) + 1;
    sptr_t selStart = Send(SCI_GETSELECTIONSTART);
    sptr_t selEnd = Send(SCI_GETSELECTIONEND);
    // character count (not byte count) for CJK-aware display
    s.selection = (int)Send(SCI_COUNTCHARACTERS, selStart, selEnd);
    if (s.selection < 0) s.selection = 0;
    s.lines = (int)Send(SCI_GETLINECOUNT);
    s.length = (long long)Send(SCI_GETLENGTH);
    return s;
}

int Editor::WordCount() const {
    if (!hwnd_) return 0;
    sptr_t len = Send(SCI_GETLENGTH);
    if (len <= 0 || len > 1048576) return -1;   // skip files > 1 MB

    std::string text;
    text.resize((size_t)len);
    Send(SCI_GETTEXT, len + 1, (LPARAM)text.data());

    int words = 0;
    bool inLatinWord = false;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = (unsigned char)text[i];
        unsigned cp = c;
        size_t advance = 1;

        if (c < 0x80) {
            advance = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp = ((c & 0x1F) << 6) | (text[i+1] & 0x3F); advance = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp = ((c & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F);
            advance = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp = ((c & 0x07) << 18) | ((text[i+1] & 0x3F) << 12) |
                 ((text[i+2] & 0x3F) << 6) | (text[i+3] & 0x3F);
            advance = 4;
        }

        bool isCJK = (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
                     (cp >= 0x3400 && cp <= 0x4DBF) ||   // Ext A
                     (cp >= 0xF900 && cp <= 0xFAFF);     // CJK Compat
        bool isLatinAlnum = isalnum(c) || c == '_';

        if (isCJK) {
            ++words;              // each CJK ideograph counts as one word
            inLatinWord = false;  // CJK breaks any Latin word
        } else if (isLatinAlnum) {
            if (!inLatinWord) { ++words; inLatinWord = true; }
        } else {
            inLatinWord = false;  // whitespace/punctuation ends word
        }
        i += advance;
    }
    return words;
}

sptr_t Editor::Send(unsigned int msg, uptr_t wp, LPARAM lp) const {
    return hwnd_ ? SendTo(hwnd_, msg, wp, lp) : 0;
}

} // namespace xfs
