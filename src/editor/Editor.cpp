#include "Editor.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../language/LanguageMap.h"
#include "../language/ChromaSignature.h"  // 批次 73：Chroma 3380 签名提示（纯解析器）
#include "../language/Chroma3380Complete.h" // 批次 77：语句名补全的候选生成（纯函数）
#include "../language/XfsLexer.h"        // 批次 72：自研 ATE 词法器工厂
#include "../language/XfsLexerStyles.h"  // 批次 72：ATE 族样式号（SCE_ATEP_* 等）
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

    // 批次 103：悬停气泡的触发延时。
    // ★ Scintilla 的 dwellDelay 默认是 TimeForever（Editor.cxx:147），也就是
    //   **永远不会发 SCN_DWELLSTART**。不显式设这个值，整套悬停功能会安静地完全
    //   不工作——编译过、测试过、运行时不报任何东西。这是本功能唯一的"开关"。
    //   600ms 是「确实停住了」与「只是划过」的分界；再短会在正常移动鼠标时误弹。
    Send(SCI_SETMOUSEDWELLTIME, 600);
    return true;
}

// --- editor key subclass: intercept Enter for brace expansion -------------------

LRESULT CALLBACK Editor::EditorKeyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR idSub, DWORD_PTR ref) {
    auto* self = (Editor*)ref;
    if (!self) return DefSubclassProc(h, msg, wp, lp);

    // 批次 78：语句名补全的续动作（补 `(` + 出签名提示）。它是从
    // HandleAutocCompleted 投递过来的，跑到这里时 Scintilla 的通知派发栈已经
    // 退干净了 —— 这里是普通的消息循环上下文，插文本、弹气泡、开下拉都安全。
    if (msg == kMsgStatementAccepted) {
        self->CompleteAfterStatementAccepted();
        return 0;
    }

    // 批次 103：任何键盘输入或滚轮都收起悬停气泡。
    // 滚轮这一路尤其必要——鼠标没动，Scintilla 就不会发 SCN_DWELLEND，气泡会停在
    // 原来的屏幕位置上，指着一段已经滚走的文字。
    if (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_MOUSEWHEEL)
        self->HideHoverTip();

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

    // 批次 87：静态校验诊断波浪线（indicator 10 错误 / 11 警告）
    DefineDiagIndicators();

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
    DefineDiagIndicators();
}

// --- syntax theme tables -------------------------------------------------------

namespace {

enum Role { RComment = SR_Comment, RString = SR_String, RNumber = SR_Number,
            RKeyword = SR_Keyword, RKeyword2 = SR_Keyword2, ROperator = SR_Operator,
            RClass = SR_Class, RPreproc = SR_Preproc, RSpecial = SR_Special,
            RPass = SR_Pass, RFail = SR_Fail, RDim = SR_Dim,
            // 批次 73：向量语义四色
            RVector = SR_Vector, RExpect = SR_Expect, RBoth = SR_Both,
            RCtrl = SR_Ctrl };

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

// ---- 批次 72/73：ATE 族（自研 ILexer5，样式号在 XfsLexerStyles.h）------------
//
// 配色原则：族的 *_DEFAULT 样式（普通标识符）刻意**不列入**下表，让它们沿用
// STYLECLEARALL 之后的默认前景色。ATE 文件里 pin 名/普通标识符是绝对多数派，
// 把多数派染上颜色等于没高亮 —— 颜色必须留给少数有信息量的记号。
//
// 批次 73 的向量五分类是本批核心：手册 3.4.1 的语义是「驱动与比较的组合」，
// 而「驱动+比较」(R/S/T/U) 是最容易误读的一类 —— 它两种动作都做，既不能算
// 驱动也不能算比较。批次 72 把它和掩码混在一起是错的。现在五类各占一个色：
//   drive(0/1)   -> vector  紫   数值感，但比通用 number 更抢眼（是文件主体）
//   cmp(H/L/Z)   -> expect  橙   比较侧，与驱动一眼分开
//   both(R/S/T/U)-> both    洋红 两类动作同时发生
//   mask(X)      -> dim     灰   「没驱动也没比较」，必须退到背景
//   ctrl(V/K/2)  -> ctrl    青   capture 触发与 VHH，调试时要能一眼找到
const StyleRole kAtePatternStyles[] = {
    {SCE_ATEP_COMMENT, RComment},    {SCE_ATEP_MODULE, RKeyword},
    {SCE_ATEP_MICRO, RKeyword2},     {SCE_ATEP_LABEL, RPreproc},
    // SEP 用 class 色：`*` 是向量边界，形状上要能看见，但语义上不该抢眼。
    // class 在明暗两套主题里都是低饱和的冷色，正好。
    {SCE_ATEP_SEP, RClass},          {SCE_ATEP_TIMESET, RPreproc},
    {SCE_ATEP_PIN, RClass},          {SCE_ATEP_VEC_DRIVE, RVector},
    {SCE_ATEP_VEC_CMP, RExpect},     {SCE_ATEP_VEC_DRV_CMP, RBoth},
    {SCE_ATEP_VEC_MASK, RDim},       {SCE_ATEP_VEC_CTRL, RCtrl},
    {SCE_ATEP_HEX, RNumber},         {SCE_ATEP_NUMBER, RNumber},
    {SCE_ATEP_STRING, RString},      {SCE_ATEP_OPERATOR, ROperator},
};

// .dec：只给结构列上色（块名 / pin_type / 模式值 / 通道号 / 定义位 pin 名）。
// 详见 ChromaDecLexer.cpp 的「着色策略」段：.dec 里 pin 名是多数派，不染。
const StyleRole kChromaDecStyles[] = {
    {SCE_DEC_COMMENT, RComment},     {SCE_DEC_BLOCK, RKeyword},
    {SCE_DEC_PINTYPE, RClass},       {SCE_DEC_MODEVAL, RKeyword2},
    {SCE_DEC_CHANNEL, RNumber},      {SCE_DEC_PIN, RPreproc},
    {SCE_DEC_OPERATOR, ROperator},   {SCE_DEC_STRING, RString},
};

// .pln：按「这一行在测试流程里扮演什么角色」分色。
// 测试语句(RKeyword)与 CRAFT 宏(RKeyword2)必须分开：前者是硬件动作，后者是
// 内建变量（写成 `TEST_LOT_ID()` 是错的），混色会让这类错误看不出来。
const StyleRole kChromaPlanStyles[] = {
    {SCE_PLN_COMMENT, RComment},     {SCE_PLN_BLOCK, RKeyword},
    {SCE_PLN_STMT, RKeyword2},       {SCE_PLN_MACRO, RPreproc},
    {SCE_PLN_CLIB, RSpecial},        {SCE_PLN_PINTYPE, RClass},
    {SCE_PLN_FLOW, RFail},           {SCE_PLN_LABEL, RPreproc},
    {SCE_PLN_CKEYWORD, RKeyword},    {SCE_PLN_CTYPE, RClass},
    {SCE_PLN_STRING, RString},       {SCE_PLN_NUMBER, RNumber},
    {SCE_PLN_OPERATOR, ROperator},
};

const StyleRole kStilStyles[] = {
    {SCE_STIL_COMMENT, RComment},   {SCE_STIL_BLOCK, RKeyword},
    {SCE_STIL_KEYWORD, RKeyword2},  {SCE_STIL_NAME, RClass},
    {SCE_STIL_UNIT, RNumber},       {SCE_STIL_NUMBER, RNumber},
    {SCE_STIL_EVENT, RSpecial},     {SCE_STIL_OPERATOR, ROperator},
    {SCE_STIL_STRING, RString},
};

// ATE Log：PASS/FAIL 用批次 72 新增的语义角色，在 light/dark 下分别是
// 深绿/深红与亮绿/亮红，且可被 Style Configurator 覆盖。
const StyleRole kAteLogStyles[] = {
    {SCE_ATEL_COMMENT, RComment},   {SCE_ATEL_TIMESTAMP, RComment},
    {SCE_ATEL_SITE, RClass},        {SCE_ATEL_TESTNAME, RKeyword2},
    {SCE_ATEL_NUMBER, RNumber},     {SCE_ATEL_LIMIT, RPreproc},
    {SCE_ATEL_PASS, RPass},         {SCE_ATEL_FAIL, RFail},
    {SCE_ATEL_WARN, RSpecial},      {SCE_ATEL_RECORD, RKeyword},
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
    {"ate_pattern", kAtePatternStyles, ARRAYSIZE(kAtePatternStyles)},
    {"stil",        kStilStyles,       ARRAYSIZE(kStilStyles)},
    {"ate_log",     kAteLogStyles,     ARRAYSIZE(kAteLogStyles)},
    {"chroma_dec",  kChromaDecStyles,  ARRAYSIZE(kChromaDecStyles)},
    {"chroma_plan", kChromaPlanStyles, ARRAYSIZE(kChromaPlanStyles)},
};

} // namespace

void Editor::SetLexerByName(const char* lexerName, const char* const* keywords,
                            const ThemeDef* t) {
    // 批次 72：先试自研 ATE 词法器（应用侧 ILexer5，见 language/XfsLexer.h），
    // 未命中再回退 Lexilla。两者走同一条 SCI_SETILEXER 通道，所以后面的
    // 关键字注入 / 主题角色配色 / 折叠属性逻辑完全不需要分支。
    ILexer5* lexer = nullptr;
    if (lexerName) {
        lexer = XfsCreateLexer(lexerName);
        if (!lexer) lexer = ::CreateLexer(lexerName);
    }
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
    DefineDiagIndicators();
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
            std::string extA = WideToUtf8(ext);   // 别用迭代器构造：那是 wchar_t→char 收窄
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
void Editor::GotoPosition(int line1based, int columnDisplay) {
    int max = (int)Send(SCI_GETLINECOUNT) - 1;
    if (line1based > max) line1based = max;
    if (line1based < 1) line1based = 1;
    Send(SCI_ENSUREVISIBLE, line1based - 1);
    // col 为 GETCOLUMN+1（含 tab 展开的显示列）；FINDCOLUMN 是其逆运算。
    sptr_t col0 = columnDisplay > 1 ? columnDisplay - 1 : 0;
    sptr_t pos = col0 > 0 ? Send(SCI_FINDCOLUMN, line1based - 1, col0) : -1;
    if (pos < 0) {          // 越界/无列信息 → 退化为整行恢复
        Send(SCI_GOTOLINE, line1based - 1);
        return;
    }
    Send(SCI_SETSEL, pos, pos);
    Send(SCI_SCROLLCARET);
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

namespace {

// 前置声明：定义在下面的「Chroma 3380 签名提示」一节。两个匿名命名空间块是
// 同一个命名空间，声明与定义对得上。放在**文件靠前**（而不是紧挨着定义）是因为
// 用到它的地方有三处、且都在定义之前：HandleCharAdded（锚失效）、
// HandleAutocompleteChar（语句补全分支）、HandleAutocCompleted（续动作）。
bool IsChroma3380Lexer(const std::string& n);

}  // namespace

// --- auto-indent / auto-close -------------------------------------------------

void Editor::SetAutoIndent(bool on) { autoIndent_ = on; }
void Editor::SetAutoCloseBrackets(bool on) { autoClose_ = on; }

void Editor::HandleCharAdded(SCNotification* sn) {
    // Auto-close brackets/quotes is handled in EditorKeyProc (WM_CHAR path).
    // Auto-complete v1: 输入到第 3 个构词字符时弹出（关键词+文档词汇）。
    // 批次 78：下拉已经被关掉时，语句名补全的锚就失效了（用户按了 Esc、
    // 或者打了一个非构词字符把它挤掉）。下一次 SCN_AUTOCCOMPLETED 不该再被
    // 当成"接受了一条语句名"——它可能来自普通词汇补全，位置也可能对不上。
    if (!Send(SCI_AUTOCACTIVE)) stmtCompleteStart_ = -1;
    if (sn) HandleAutocompleteChar((unsigned int)sn->ch);
}

// ---- 批次 78：接受语句名之后的续动作 ----------------------------------------
//
// 【为什么必须绕一次消息循环，不能顺手做完】
//   Scintilla 发通知的时序（ScintillaBase::AutoCompleteCompleted，本 fork 5.6.6）：
//       ac.GetValue(item) → ac.Show(false) → **NotifyParent(AutoCSelection, 2022)**
//       → ac.Cancel() → AutoCompleteInsert(...) → NotifyParent(AutoCCompleted, 2030)
//   2022 在插入**之前**发，所以在它里面插字符会被随后的 AutoCompleteInsert 连
//   区间一起替换掉。2030 在插入**之后**发，位置对了 —— 但此刻仍处在
//   AutoCompleteCompleted 的栈帧里（后面还有 SetLastXChosen()），而且
//   NotifyParent 是同步 SendMessage，从里面再 Send 回 Scintilla 会嵌进它自己的
//   通知派发。为了不依赖"Scintilla 恰好可重入"，这里只把续动作 PostMessage 出去，
//   等这条通知彻底返回、再回到消息循环时才动文档。
//
// 【这条 posted 消息不会被人抢先】
//   Windows 的取消息顺序是「已发送消息 → 投递消息 → 输入消息」，所以这条在我们
//   回来之前必然先被处理，用户的下一次击键不可能插到中间。
void Editor::HandleAutocCompleted(const SCNotification* sn) {
    // 一次性：无论后面走哪条分支，锚都用掉（避免陈旧锚在别处再触发）。
    const sptr_t armed = stmtCompleteStart_;
    stmtCompleteStart_ = -1;
    if (!sn || armed < 0 || !autoComplete_ || !hwnd_) return;
    // 只对 Chroma 三支有意义；SCN_AUTOCCOMPLETED 是**所有**补全共用的通知
    // （普通词汇补全、批次 73 的实参候选值下拉都会发），所以先按词法器短路。
    if (!IsChroma3380Lexer(lexerName_)) return;
    // 完成通知里的 position = 被替换区间的起点（ac.posStart - ac.startLen）。
    // 对语句名下拉它必然等于我们弹列表时的词首；不等就说明这次完成不是那一回。
    if ((sptr_t)sn->position != armed) return;

    const char* accepted = sn->text;
    if (!accepted || !*accepted) return;
    CancelSignatureHint();   // 下拉刚关掉，气泡记账一起清（两者在 Scintilla 里互斥）
    if (!chroma3380::WantsParenAfterName(accepted, std::strlen(accepted))) return;
    ::PostMessageW(hwnd_, kMsgStatementAccepted, 0, 0);
}

void Editor::CompleteAfterStatementAccepted() {
    if (!hwnd_ || Send(SCI_GETREADONLY)) return;
    const sptr_t caret = Send(SCI_GETCURRENTPOS);
    // 后面已经跟着左括号（用户自己敲的，或从别处粘贴过来）→ 只补提示，不补字符。
    if (caret < Send(SCI_GETLENGTH)) {
        char next = '\0';
        Sci_TextRangeFull tr{};
        tr.chrg.cpMin = (Sci_Position)caret;
        tr.chrg.cpMax = (Sci_Position)(caret + 1);
        tr.lpstrText = &next;
        Send(SCI_GETTEXTRANGEFULL, 0, (LPARAM)&tr);
        if (next == '(') {
            HandleSignatureHint();
            return;
        }
    }
    // 自动配对开着时补 `()` 并把光标放回中间 —— 与 EditorKeyProc 里手敲 `(` 的
    // 行为保持一致（那里也是"插入配对 + 光标回退一格"）。不开配对就只补 `(`。
    // 刻意不把 `)` 一次性补到实参末尾：我们不知道用户要写几个实参，也不知道
    // 后面是不是还有嵌套调用，把闭括号提前写死等于替他做决定。
    const char* ins = autoClose_ ? "()" : "(";
    Send(SCI_ADDTEXT, (uptr_t)std::strlen(ins), (LPARAM)ins);
    if (autoClose_) Send(SCI_GOTOPOS, caret + 1);
    // 光标此刻落在左括号之后，正好是批次 73 签名提示的判定条件：实参位置。
    // 所以这里不需要另写一套"刚接受完"的提示逻辑 —— 复用同一条路径，用户在
    // 这一刻看到的东西与"自己手敲 `(`"完全一致（这才是对的行为：同一个状态，
    // 同一种提示）。
    HandleSignatureHint();
}

// ---- 自动补全 v1（设置系统设计笔记 之外的编辑器缺口批次） ----------------

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
    // 批次 77：Chroma 三支的词表**不在 LanguageMap 里** —— 上面这段按目录取词对
    // 它们只会得到空集（keywords[2] 都是 nullptr，词表在 Chroma3380Db 里、由
    // 词法器构造函数直接注入，见 XfsLexer.h 的 kWordLists）。后果是这些文件里
    // 「词汇补全只剩文档里出现过的词」，新开的 .pln 打 `FORC` 什么都不弹。
    // 这里把词法器的词表并进来（语句名 / CRAFT 宏 / C 关键字 / 类型 / pin_type /
    // C 库 / 微指令 / .dec 块名）。非 Chroma 词法器返回空 → 既有行为不变。
    std::vector<const char*> extraWords;
    chroma3380::CollectExtraWords(lexerName.c_str(), extraWords);
    for (const char* w : extraWords) out.emplace(w);
    return cache.emplace(lexerName, std::move(out)).first->second;
}

} // namespace

void Editor::HandleAutocompleteChar(unsigned int ch) {
    if (!autoComplete_ || !hwnd_) return;
    // 批次 73：Chroma 语句的实参位置优先走签名提示（候选值来自手册，比文档词汇
    // 准得多）。没命中就往下走原来的词汇补全，功能不被吞掉。
    if (HandleSignatureHint()) return;
    if (!IsWordCharW(ch)) {
        if (Send(SCI_AUTOCACTIVE)) Send(SCI_AUTOCCANCEL);
        return;
    }
    const sptr_t pos = Send(SCI_GETCURRENTPOS);
    const sptr_t start = Send(SCI_WORDSTARTPOSITION, pos, 1);
    const int len = (int)(pos - start);
    // 批次 77：语句名补全（只有 Chroma 三支、只有语句起始位置）。
    // 放在下面 len<3 的既有门槛**之前**，是因为它自己的前缀门槛更低（2 字符）；
    // 但它不改下面任何一行 —— 返回 false 时词汇补全那条路一字不变，所以对
    // 另外 36 种语言是零改动（IsChroma3380Lexer 先短路）。
    if (IsChroma3380Lexer(lexerName_) && len >= 2 && len <= 64 &&
        len >= (int)chroma3380::kStmtCompleteMinPrefix &&
        !Send(SCI_AUTOCACTIVE)) {
        std::string stmtPrefix;
        GetTextRangeUtf8((long long)start, (size_t)len, stmtPrefix);
        if (HandleStatementCompletion(start, stmtPrefix)) return;
    }
    if (len < 3 || len > 64) {
        if (Send(SCI_AUTOCACTIVE)) Send(SCI_AUTOCCANCEL);
        return;
    }
    if (Send(SCI_AUTOCACTIVE)) return;   // 已在补全中，Scintilla 自动继续过滤
    // 读前缀（caret 前 len 字节）
    // 用 *FULL 版消息 + Sci_TextRangeFull：短版 Sci_TextRange 的 cpMin/cpMax 是
    // `long`（32 位），把 sptr_t 塞进去要收窄，MSVC 会报 C4244/C4838，而且文档
    // 超过 2GB 时位置会被截断。FULL 版用 Sci_Position（ptrdiff_t），与 sptr_t 同宽。
    std::string prefix((size_t)len, '\0');
    Sci_TextRangeFull tr{};
    tr.chrg.cpMin = (Sci_Position)(pos - len);
    tr.chrg.cpMax = (Sci_Position)pos;
    tr.lpstrText = prefix.data();
    Send(SCI_GETTEXTRANGEFULL, 0, (LPARAM)&tr);
    ShowAutocomplete(prefix);
}

// ---- 批次 73：Chroma 3380 签名提示 -------------------------------------------
//
// 【数据从哪来】
//   src/language/Chroma3380Db.{h,cpp} —— 从 Chroma 3380 语言手册逐节抽取并人工复核
//   的语句/参数/候选值表。**不是 CRAFT 编译器的输出**（Chroma 没有公开错误码表），
//   所以状态栏对 Chroma 族文件常驻「非 CRAFT 编译结果」标注，见 MainWindow。
//
// 【为什么下拉框只在有候选值时弹】
//   手册里大量参数是自由的数值/字符串（f_volt、pin_name、地址…），没有可选项。
//   对这类参数弹一个空下拉框或者硬塞词汇候选，只会挡住视线。所以 hasEnum 为假
//   时直接返回 false，把这一次按键交回普通的词汇补全。

namespace {

// 只有 Chroma 族的词法器才去查语句库（kStatements 覆盖手册第 3~5 章，即
// .pat/.pln）。其它语言连这段文本都不必读——省掉每次按键的文档读取。
// 批次 77：判定收进 chroma3380::IsChroma3380LexerName（与补全模块同一处口径，
// 避免"高亮算 Chroma、补全不算"这类两边各判一次的分叉）。
bool IsChroma3380Lexer(const std::string& n) {
    return chroma3380::IsChroma3380LexerName(n.c_str());
}

// 语句名补全的候选上限。手册里 .pln 的语句共 258 条，2 字符前缀最多能命中
// 几十条；给到 400 是"不会截断"的量级，不是性能阈值。
constexpr int kStmtCompleteMax = 400;

// 往回读的窗口。必须够长以覆盖「语句名 + 前面的实参」；真超过这个距离的调用
// 已经不是人手写的，判为「不在实参里」不出提示即可（ChromaSignature 同此口径）。
constexpr sptr_t kSigScanWindow = 4096;

// 批次 103：悬停气泡在 tooltip 控件里的工具号。每个编辑器一个独立窗口、只挂一个
// 工具，所以取 1 就够。
constexpr UINT_PTR kHoverTipId = 1;

}  // namespace

bool Editor::HandleSignatureHint() {
    if (!hwnd_ || !IsChroma3380Lexer(lexerName_)) return false;

    const sptr_t caret = Send(SCI_GETCURRENTPOS);
    const sptr_t docLen = Send(SCI_GETLENGTH);
    if (caret <= 0 || caret > docLen) { CancelSignatureHint(); return false; }
    const sptr_t lo = (caret > kSigScanWindow) ? caret - kSigScanWindow : 0;

    // 取光标前的一小段（不是整个文档）：签名位置只由光标附近的括号与逗号决定。
    std::string text((size_t)(caret - lo), '\0');
    Sci_TextRangeFull tr{};
    tr.chrg.cpMin = (Sci_Position)lo;
    tr.chrg.cpMax = (Sci_Position)caret;
    tr.lpstrText = text.data();
    Send(SCI_GETTEXTRANGEFULL, 0, (LPARAM)&tr);

    chroma3380::SignatureHint hint;
    if (!chroma3380::ResolveSignatureHint(text, text.size(), hint)) {
        CancelSignatureHint();      // 不在任何已收录语句的实参里
        return false;
    }

    // 槽序不可信的语句（重复组语法 `[ ... ]*`、签名被参数注释污染、签名缺失）
    // 一律不出手：param/paramIndex 只是按老口径排出来的「第 N 个」，挂候选值会
    // 挂错参数，签名本身也常是脏的。这里的「不出手」是有代价的（74/309 条语句
    // 落在此类），但这个代价换的是「绝不给出位置错位的候选值」。
    if (!hint.positional) {
        CancelSignatureHint();
        return false;
    }

    // ---- 下拉框 / 签名气泡：**二选一，不能同框** ------------------------------
    //
    // 【为什么是二选一：这不是取舍，是 Scintilla 的硬约束】
    //   查 5.6.6 源码，两个方向互相取消：
    //     ScintillaBase::AutoCompleteStart() 第一行：ct.CallTipCancel();
    //     ScintillaBase::CallTipShow()       第一行：ac.Cancel();
    //   所以"气泡在上、下拉在下、两者不打架"这个想法**立不住**：无论谁先谁后，
    //   后弹的那个都会把前一个撤掉。批次 73 端到端实测就抓到了——
    //   走到 FORCE_V_MLDPS 的 v_range 时 callTip=0 / autoC=1，即用户永远看不到
    //   气泡，而 v_range 恰恰是这个功能的主场景。
    //
    //   于是分工按"用户此刻在问什么"来切：
    //     有候选值 → 出**下拉框**。用户问的是"这里能填什么"，而候选值本身
    //                （@6V / @12V）就已经说明了这个参数，Tab 可直接接受。
    //     没候选值 → 出**气泡**。用户问的是"我在第几个参数、这参数叫什么"，
    //                这正是自由参数占多数（f_volt / pin_name / 地址…）时的需要。
    //   注意"没候选值"包含两种情况：该参数本就没有枚举；或打了前缀之后一个
    //   候选都对不上。两种都给气泡——都比什么都不给强。
    if (hint.hasEnum) {
        const char* const* vals = chroma3380::kValues + hint.param->valStart;
        const std::string prefix = text.substr((size_t)hint.argStart);
        std::string list;
        for (int i = 0; i < hint.param->valCount; ++i) {
            const char* v = vals[i];
            const size_t vl = std::strlen(v);
            if (vl < prefix.size() || _strnicmp(v, prefix.c_str(), prefix.size()) != 0)
                continue;                  // 前缀不匹配，Scintilla 也会自己滤掉
            if (vl == prefix.size()) continue;   // 已经打全了，没有可补的部分
            if (!list.empty()) list += ' ';
            list += v;
        }

        if (!list.empty()) {
            // 下拉框要顶掉气泡，先把我们的记账清掉，免得 CancelSignatureHint()
            // 之后以为气泡还在（那会让下一次重新弹出的去重判断失效）。
            if (sigTipShown_) {
                Send(SCI_CALLTIPCANCEL);
                sigTipShown_ = false;
                sigTipStmt_ = nullptr;
                sigTipParam_ = -1;
                sigTipPos_ = -1;
            }
            if (Send(SCI_AUTOCACTIVE)) return true;   // 已在收窄候选中，交给 Scintilla

            Send(SCI_AUTOCSETSEPARATOR, ' ');
            Send(SCI_AUTOCSETIGNORECASE, 1);
            Send(SCI_AUTOCSETAUTOHIDE, 1);
            Send(SCI_AUTOCSETDROPRESTOFWORD, 0);
            // 用 Custom 而不是 ShowAutocomplete 那边的 PerformSort：档位表要按手册
            // 顺序（@6V 在 @12V 之前、电流档从小到大），字母序会把它倒过来。
            Send(SCI_AUTOCSETORDER, SC_ORDER_CUSTOM);
            Send(SCI_AUTOCSETMAXHEIGHT, 8);
            // lenEntered = 已输入的实参片段长度：Tab/回车选中后 Scintilla 用它算出
            // 替换区间，正好把 `@6` 换成 `@6V`。scintilla 之后按
            // RangeText(posStart - startLen, caret) 继续做前缀过滤，所以逐字符
            // 输入会自动收窄候选——不必自己重弹。
            Send(SCI_AUTOCSHOW, (uptr_t)prefix.size(), (LPARAM)list.c_str());
            return true;   // 下拉框已出，本次按键不再走词汇补全
        }
        // 落到这里说明一个候选都对不上：要么用户在写别的东西，要么打错了。
        // 不弹空列表（挡住视线），往下走气泡分支。
    }

    // ---- 签名气泡：手册原文签名 + 高亮当前参数 -------------------------------
    const sptr_t argDocPos = lo + hint.argStart;
    if (!sigTipShown_ || sigTipStmt_ != hint.stmt ||
        sigTipParam_ != hint.paramIndex || sigTipPos_ != argDocPos) {
        const char* sig = hint.stmt->signature ? hint.stmt->signature : "";
        if (*sig) {
            // 先设位置再显示：SCI_CALLTIPSETPOSITION 只改内部标志 + 触发重画，
            // 窗口位置是 CALLTIPSHOW 那一刻算的，反过来设不生效。
            // 气泡放光标上方：下拉框要占光标下方，虽然两者不会同框（见上），
            // 但保持上方能让"气泡 → 下拉框"的切换在视觉上原地不动。
            Send(SCI_CALLTIPSETPOSITION, 1);
            Send(SCI_CALLTIPSHOW, argDocPos, (LPARAM)sig);
            int hs = 0, he = 0;
            if (chroma3380::SignatureArgRange(sig, hint.paramIndex, hs, he))
                Send(SCI_CALLTIPSETHLT, (uptr_t)hs, (LPARAM)he);
            sigTipShown_ = true;
            sigTipStmt_ = hint.stmt;
            sigTipParam_ = hint.paramIndex;
            sigTipPos_ = argDocPos;
        }
    }
    return false;   // 没下拉框 → 交回普通的词汇补全
}

void Editor::CancelSignatureHint() {
    // 下拉框的活动状态由 Scintilla 管，这里只负责签名气泡。
    if (sigTipShown_) {
        Send(SCI_CALLTIPCANCEL);
        sigTipShown_ = false;
    }
    sigTipStmt_ = nullptr;
    sigTipParam_ = -1;
    sigTipPos_ = -1;
}

// 把气泡文案按**像素**折行，用的是气泡自己的字体。
//
// 【为什么不能只靠 TTM_SETMAXTIPWIDTH 让它自己折】
//   实测（批次 103 真机截图）：设了 maxwidth = 700 物理像素，
//   气泡**确实**折成了两行，但第一行是从**词中间被裁掉**的——
//   `…sets the Per-pin PMU in voltag`，后面 `e ` 直接不见了。
//   即"折行宽度"和"窗口宽度"在原生 tracking tooltip 上不是同一个数，靠它折不可靠，
//   而且**裁掉的是内容、不报任何错**。309 条语句的文案中位数 160 字符、79% 超过
//   96 字符 ⇒ 靠不住就等于大多数气泡都缺字。
//   所以：自己折好、用 \r\n 显式给出行边界，再把 TTM_SETMAXTIPWIDTH 设得足够大
//   让它**不要**二次折行（二次折行正是裁切的来源）。
//
// 【为什么按像素而不是按字符数】
//   字符数只有在等宽字体下才等于像素。气泡用的是系统 tooltip 字体，它随 DPI 缩放；
//   按字符数折，换个 DPI 就重新出现裁切。量一次宽最稳，代价是几行 GDI。
static std::wstring WrapBalloonText(HWND tip, const std::wstring& text, int maxPx)
{
    if (maxPx <= 0) return text;
    HDC hdc = ::GetDC(tip);
    if (!hdc) return text;
    HFONT font = (HFONT)::SendMessageW(tip, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? ::SelectObject(hdc, font) : nullptr;

    auto width = [&](const std::wstring& s) -> int {
        SIZE sz{};
        if (!::GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz)) return 0;
        return (int)sz.cx;
    };

    std::wstring out, line;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i;
        while (j < text.size() && text[j] != L' ') ++j;
        std::wstring word = text.substr(i, j - i);
        i = j;
        while (i < text.size() && text[i] == L' ') ++i;   // 吃掉词间空格

        // 整段没有空格的超长"词"（例如一串参数）：按像素硬切，别让它撑破屏幕。
        while (width(word) > maxPx && word.size() > 1) {
            std::size_t k = word.size();
            while (k > 1 && width(word.substr(0, k)) > maxPx) --k;
            if (!line.empty()) { out += line; out += L"\r\n"; line.clear(); }
            out += word.substr(0, k);
            out += L"\r\n";
            word = word.substr(k);
        }
        if (word.empty()) continue;

        if (line.empty()) {
            line = word;
        } else if (width(line + L" " + word) > maxPx) {
            out += line;
            out += L"\r\n";
            line = word;
        } else {
            line += L" ";
            line += word;
        }
    }
    out += line;
    if (oldFont) ::SelectObject(hdc, oldFont);
    ::ReleaseDC(tip, hdc);
    return out;
}

// ---- 批次 103：Chroma 3380 语句的悬停气泡 ------------------------------------
//
// 【为什么不用 Scintilla 自带的 calltip（SCI_CALLTIPSHOW）】
//   1) 在 Scintilla 里 calltip 与补全下拉**互斥**（AutoCompleteStart() 第一行
//      ct.CallTipCancel()、CallTipShow() 第一行 ac.Cancel()，见 HandleSignatureHint
//      里的说明）。悬停气泡虽然多半出现在没打字的时候，但只要用户在下拉还开着时
//      把鼠标停住，就会把下拉顶掉——那等于在惩罚"移动鼠标"这个动作。
//   2) calltip 的宽度按最长行算。手册说明中位数 99 字符、用户点名的 FORCE_V_PPMU
//      是 166 字符，会拉出一条横跨屏幕的窄带；原生 tooltip 可以自己折行（但**不能
//      指望 TTM_SETMAXTIPWIDTH**，见 WrapBalloonText 的说明），那才是"气泡"的样子。
//   3) 原生 tooltip 不抢焦点、不参与编辑器的键盘状态，SCN_DWELLEND 一到就收，
//      语义正好是"悬停"。
//
// 【触发链】SCI_SETMOUSEDWELLTIME（在 Create 里设，见那里的说明）→ Scintilla 的
//   dwell 计时器 → SCN_DWELLSTART → MainWindow 的 WM_NOTIFY 分发 → 这里。
void Editor::HandleDwellStart(int x, int y) {
    if (!hwnd_ || !IsChroma3380Lexer(lexerName_)) return;

    const sptr_t pos = Send(SCI_POSITIONFROMPOINTCLOSE, (uptr_t)x, (LPARAM)y);
    if (pos < 0) { HideHoverTip(); return; }

    // 取词交给 Scintilla 自己的词边界：`FORCE_V_PPMU` 里的下划线、以及紧跟其后的
    // `(`，只有 Scintilla 的词表口径说得准。判据那一半在 chroma3380::BuildHoverTip
    // 里（纯函数、有单测），这里只负责"鼠标压着的是哪个词"。
    const sptr_t ws = Send(SCI_WORDSTARTPOSITION, (uptr_t)pos, 1);
    const sptr_t we = Send(SCI_WORDENDPOSITION, (uptr_t)pos, 1);
    // 64 = FindStatement 的长度上限。超过它一定不是语句名（长路径 / 长数字之类），
    // 先挡掉可以省一次取文本。
    if (we <= ws || (we - ws) > 64) { HideHoverTip(); return; }

    std::string word((size_t)(we - ws), '\0');
    Sci_TextRangeFull tr{};
    tr.chrg.cpMin = (Sci_Position)ws;
    tr.chrg.cpMax = (Sci_Position)we;
    tr.lpstrText = word.data();
    Send(SCI_GETTEXTRANGEFULL, 0, (LPARAM)&tr);

    std::string tip;
    if (!chroma3380::BuildHoverTip(word.c_str(), word.size(), tip)) {
        HideHoverTip();     // 不是已收录的语句名 → 不出气泡（也别弹空的）
        return;
    }

    const int dpi = ::GetDpiForWindow(hwnd_);
    if (!hoverTip_) {
        // 懒创建：非 Chroma 语言的文档永远走不到这一行，没必要给每个标签都建窗口。
        hoverTip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                      WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                      0, 0, 0, 0, hwnd_, nullptr,
                                      ::GetModuleHandleW(nullptr), nullptr);
        if (!hoverTip_) return;
        // 给控件的 maxwidth 要**足够大**：折行由我们自己做（WrapBalloonText）。
        // 这里要是设成"我们希望的宽度"，控件会二次折行，而二次折行会把词从中间裁掉。
        ::SendMessageW(hoverTip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(1600, dpi, 96));
        // 悬停时不要自己消失（默认几秒就没了，用户还没读完）
        ::SendMessageW(hoverTip_, TTM_SETDELAYTIME, TTDT_AUTOPOP,
                       MAKELPARAM(60000, 0));
    }

    hoverText_ = WrapBalloonText(hoverTip_, Utf8ToWide(tip), MulDiv(560, dpi, 96));
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = hwnd_;
    ti.uId = kHoverTipId;
    ti.lpszText = hoverText_.data();   // 每次都要重设：上面那行可能已让缓冲搬家
    if (!hoverTipAdded_) {
        ::SendMessageW(hoverTip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        hoverTipAdded_ = true;
    }

    POINT sp{x, y};
    ::ClientToScreen(hwnd_, &sp);
    // 标准 TRACK 顺序：先关 → 定位 → 换文本 → 再开（与工具栏气泡同一套写法）。
    // 光标右下方偏移，避开鼠标指针本身。
    ::SendMessageW(hoverTip_, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
    ::SendMessageW(hoverTip_, TTM_TRACKPOSITION, 0,
                   MAKELPARAM(sp.x + 12, sp.y + MulDiv(22, dpi, 96)));
    ::SendMessageW(hoverTip_, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    ::SendMessageW(hoverTip_, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

void Editor::HideHoverTip() {
    if (!hoverTip_) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = kHoverTipId;
    ::SendMessageW(hoverTip_, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

// ---- 批次 77：Chroma 3380 语句名补全 ----------------------------------------
//
// 【它和普通词汇补全、和签名提示的分工】
//                触发条件                       候选内容
//   签名提示     光标在已收录语句的实参里（越过左括号）  该参数的手册候选值
//   语句名补全   光标在**语句起始位置**（行首 / `;` `{` `}` 之后）  词表里的语句名
//   词汇补全     其余任意位置的 3 字符以上词首        语言词表 + 文档词汇
//   三者互斥且有序：实参位置优先于语句位置，语句位置优先于词汇兜底。
//
// 【为什么要按"位置"而不是"词形"判】
//   `FORC` 在语句开头是 FORCE_* 的前缀，在实参里（`FORCE_V_MLDPS(Vcc, FORC|`）
//   就不是——同一个词形在两种位置含义不同。位置判定收在
//   chroma3380::IsStatementStart 里（纯函数、可单测）。
//
// 【列表右侧那一列是只显示、不插入的】
//   Scintilla 的 typesep（默认 `?`）语义：`名字?附加列`，ListBox 在 `?` 处断词，
//   插入的只是前半截。所以 `FORCE_V_MLDPS?4.9.4` 接受后上屏的是
//   `FORCE_V_MLDPS`。用它是为了在不写自绘列表的前提下把"手册章节号"这一列
//   摆出来——章节号是回溯手册原文的钥匙，也是这套数据最值钱的部分。
bool Editor::HandleStatementCompletion(sptr_t wordStart, const std::string& prefix) {
    // 取词前的一小段做位置判定（缩进 + 上一条语句的结尾就够）。取窗口而不是
    // 全文：这一步在每次按键上跑，代价必须与文档大小无关。
    constexpr sptr_t kCtxWindow = 512;
    const sptr_t lo = (wordStart > kCtxWindow) ? wordStart - kCtxWindow : 0;
    std::string ctx;
    GetTextRangeUtf8((long long)lo, (size_t)(wordStart - lo), ctx);
    if (!chroma3380::IsStatementStart(ctx, ctx.size())) return false;

    std::vector<chroma3380::StatementCandidate> cands;
    if (chroma3380::CollectStatementCandidates(lexerName_.c_str(), prefix, cands,
                                               kStmtCompleteMax) <= 0)
        return false;   // 这个前缀没有对应语句 → 交回词汇补全（它可能命中宏/类型名）

    std::string list;
    for (const chroma3380::StatementCandidate& c : cands) {
        if (!list.empty()) list += ' ';
        list += c.name;
        if (!c.section.empty()) { list += '?'; list += c.section; }
    }

    // 下拉框会顶掉气泡（Scintilla 的硬约束，见 HandleSignatureHint 顶部的说明），
    // 所以先把我们的气泡记账清掉，免得随后 CancelSignatureHint() 误判气泡还在。
    CancelSignatureHint();

    Send(SCI_AUTOCSETTYPESEPARATOR, '?');
    Send(SCI_AUTOCSETSEPARATOR, ' ');
    Send(SCI_AUTOCSETIGNORECASE, 1);
    Send(SCI_AUTOCSETAUTOHIDE, 1);
    Send(SCI_AUTOCSETDROPRESTOFWORD, 0);
    // 手册顺序，不是字母序：章节升序天然按硬件族分组（FORCE_I_MLDPS → FORCE_V_MLDPS
    // → DPS 族 → UVI 族 …），字母序会把这个结构打散。
    Send(SCI_AUTOCSETORDER, SC_ORDER_CUSTOM);
    Send(SCI_AUTOCSETMAXHEIGHT, 12);
    Send(SCI_AUTOCSHOW, (uptr_t)prefix.size(), (LPARAM)list.c_str());
    // 批次 78：记下"列表是在哪个位置弹的"。wordStart 就是 Scintilla 之后在
    // SCN_AUTOCCOMPLETED 里报的 position（= ac.posStart - ac.startLen，而
    // 这里 lenEntered == prefix.size() == caret - wordStart），所以两者相等
    // 就是"这次完成确实来自我们这个下拉"的凭据。
    stmtCompleteStart_ = wordStart;
    return true;
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
            Sci_TextRangeFull tr{};                 // *FULL 版：cpMin/cpMax 为 ptrdiff_t
            tr.chrg.cpMin = 0;
            tr.chrg.cpMax = (Sci_Position)docLen;
            tr.lpstrText = buf.data();
            Send(SCI_GETSTYLEDTEXTFULL, 0, (LPARAM)&tr);
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

// --- 批次 87：静态校验诊断标记 ------------------------------------------------
// 用 indicator 而不是 marker：波浪线要**按列**画在被判错的那段文字下面，
// 而 marker 只能整行着色（还会把行号边距一起点亮）。定义见
// DefineDiagIndicators()，这里只负责填/清范围。
// 与 ClearOccurrenceHighlight 一样，用一个 bool 记住"有没有填过"，
// 避免每次编辑都白扫全文两遍。
void Editor::SetDiagMarks(const std::vector<DiagMark>& marks) {
    if (!marks.empty() || diagActive_) ClearDiagMarks();
    if (marks.empty()) return;

    const sptr_t lineCount = Send(SCI_GETLINECOUNT);
    for (const DiagMark& m : marks) {
        // 行越界一律跳过：文本可能在两次校验之间被改短（行号失效时画在
        // 别的行上，比不画更糟）。列越界则**收敛**到行尾，见下。
        if (m.line < 0 || (sptr_t)m.line >= lineCount) continue;
        const sptr_t lineStart = Send(SCI_POSITIONFROMLINE, (uptr_t)m.line);
        const sptr_t lineEnd = Send(SCI_GETLINEENDPOSITION, (uptr_t)m.line);
        sptr_t from = lineStart + (m.start > 0 ? (sptr_t)m.start : 0);
        sptr_t len  = (m.length > 0 ? (sptr_t)m.length : 1);
        if (from >= lineEnd) {
            // 记的位置已经在行尾之后：贴到该行最后一个字节，保证波浪线
            // 仍指在这一行（空行则退化为从行首起的 1 字节）
            from = (lineStart < lineEnd) ? lineEnd - 1 : lineStart;
            len = 1;
        } else if (from + len > lineEnd) {
            len = lineEnd - from;   // 裁剪到行尾，不越到下一行
        }
        Send(SCI_SETINDICATORCURRENT, m.isError ? 10 : 11);
        Send(SCI_INDICATORFILLRANGE, from, len);
        diagActive_ = true;
    }
}

void Editor::ClearDiagMarks() {
    if (!diagActive_) return;   // 无标记时跳过清扫（大文档省两次全文遍历）
    diagActive_ = false;
    const sptr_t docLen = Send(SCI_GETLENGTH);
    Send(SCI_SETINDICATORCURRENT, 10);
    Send(SCI_INDICATORCLEARRANGE, 0, docLen);
    Send(SCI_SETINDICATORCURRENT, 11);
    Send(SCI_INDICATORCLEARRANGE, 0, docLen);
}

void Editor::DefineDiagIndicators() {
    // 诊断波浪线（批次 87）：
    //   10 = IMPORTANT_ERROR 用 INDIC_SQUIGGLE（经典红波浪）
    //   11 = 警告用 INDIC_SQUIGGLELOW（低幅橙波浪，与错误区分且更"弱"）
    // 与 indicator 8/9 同理：STYLECLEARALL 会清掉 indicator 定义，所以每个
    // "重建样式"的位置都要再调一次（值不会丢，丢的是画法）。
    Send(SCI_INDICSETSTYLE, 10, INDIC_SQUIGGLE);
    Send(SCI_INDICSETFORE, 10, RGB(0xE0, 0x30, 0x30));
    Send(SCI_INDICSETSTYLE, 11, INDIC_SQUIGGLELOW);
    Send(SCI_INDICSETFORE, 11, RGB(0xE0, 0x80, 0x10));
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
