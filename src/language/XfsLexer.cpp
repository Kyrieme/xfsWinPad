// xfsWinPad - 自研 ILexer5 基础设施实现（批次 72）
// 设计说明见 XfsLexer.h 头部注释。

#include "XfsLexer.h"
#include "XfsLexerStyles.h"
#include "AtePatternLexer.h"
#include "StilLexer.h"
#include "AteLogLexer.h"

#include <Scintilla.h>   // SC_FOLDLEVEL* 常量

#include <algorithm>
#include <cstring>

namespace xfs {

// ------------------------------------------------------------------ KeywordSet

void KeywordSet::Set(const char* words) {
    // 语义与 Lexilla 的 WordList::Set 一致：可重复调用，累积而不是替换。
    // 但重复注入同一份表（SetLexerByName 在切换主题时会重设）不应该让集合
    // 无限膨胀——用 set 天然去重，所以累积是安全的。
    if (!words) return;
    const char* p = words;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
        if (p > start) {
            std::string w(start, (std::size_t)(p - start));
            for (auto& c : w)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            words_.insert(std::move(w));
        }
    }
}

bool KeywordSet::Has(const char* word, std::size_t len) const {
    if (words_.empty() || !word || len == 0) return false;
    char small[64];
    std::string big;
    char* buf = small;
    if (len >= sizeof(small)) {
        big.resize(len);
        buf = big.data();
    }
    for (std::size_t i = 0; i < len; ++i) {
        char c = word[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    return words_.find(std::string(buf, len)) != words_.end();
}

// -------------------------------------------------------------------- folding

int FoldLevel(int levelCur, int levelNext, bool white, bool header) {
    int flags = 0;
    if (white) flags |= SC_FOLDLEVELWHITEFLAG;
    if (header) flags |= SC_FOLDLEVELHEADERFLAG;
    // 低位 12 位要同时装下 Base 与层级，越界会溢进 WhiteFlag/HeaderFlag 位，
    // 表现为「折叠箭头莫名其妙出现或消失」。生成式 pattern 文件可能有极深缩进，
    // 所以这里钳位而不是信任调用方。
    constexpr int kMaxDepth = SC_FOLDLEVELNUMBERMASK - SC_FOLDLEVELBASE;
    if (levelCur < 0) levelCur = 0;
    if (levelNext < 0) levelNext = 0;
    if (levelCur > kMaxDepth) levelCur = kMaxDepth;
    if (levelNext > kMaxDepth) levelNext = kMaxDepth;

    // 高位字存的是**绝对**层级（同样含 SC_FOLDLEVELBASE），与 Lexilla 对齐：
    //   FoldLevelForCurrentNext(cur, next) = cur | next << 16   (cur/next 以 Base 起算)
    //   FoldLevelStart(prev)               = prev >> 16         (增量续扫时还原绝对层级)
    // Scintilla 自身的折叠判定只读本行低 12 位并与**下一行**的低位比较，不碰高位；
    // 高位只出现在折叠边栏的 LevelNumbers 调试位与 Lexilla 的续扫上。存相对层级
    // 也能跑，但会让自研族与内建族的编码不一致 —— 那种差异只在排查折叠问题时
    // 才暴露，是最难定位的一类。所以这里对齐上游。
    return (SC_FOLDLEVELBASE + levelCur) | ((SC_FOLDLEVELBASE + levelNext) << 16) | flags;
}

void SetFoldLine(Scintilla::IDocument* doc, Sci_Position line,
                 int levelCur, int levelNext, bool white, bool header) {
    if (!doc || line < 0) return;
    const int level = FoldLevel(levelCur, levelNext, white, header);
    if (level != doc->GetLevel(line)) doc->SetLevel(line, level);
}

bool LineText(Scintilla::IDocument* doc, Sci_Position line, std::string* out) {
    out->clear();
    if (!doc || line < 0) return false;
    const Sci_Position s = doc->LineStart(line);
    const Sci_Position e = doc->LineEnd(line);
    if (e <= s) return false;
    out->resize((std::size_t)(e - s));
    doc->GetCharRange(out->data(), s, e - s);
    return !out->empty();
}

// ----------------------------------------------------------------- XfsStyleSink

XfsStyleSink::XfsStyleSink(Scintilla::IDocument* doc, Sci_Position start, Sci_Position len)
    : doc_(doc), len_(len) {
    if (doc_) doc_->StartStyling(start);
}

XfsStyleSink::~XfsStyleSink() {
    // 补齐：子类若少推了样式（算错长度），用 0 填掉剩余区间。
    // 0 经 STYLECLEARALL 后与默认前景色一致，观感等同「未着色」，
    // 但绝不能留给后续字符——那会让所有样式整体错位。
    if (pushed_ < len_) Push(0, (std::size_t)(len_ - pushed_));
    Flush();
}

void XfsStyleSink::Push(int style, std::size_t count) {
    if (!doc_ || count == 0) return;
    // 归一到 0..127（本族样式上界 128），保证能塞进 char 传给 SetStyles。
    char attr = (char)((unsigned)style & 0x7Fu);
    while (count > 0 && pushed_ < len_) {
        if (used_ == kBufSize) Flush();
        Sci_Position room = (Sci_Position)(kBufSize - used_);
        Sci_Position left = len_ - pushed_;
        std::size_t n = (std::size_t)((std::min)((Sci_Position)count, (std::min)(room, left)));
        std::memset(buf_ + used_, attr, n);
        used_ += (int)n;
        pushed_ += (Sci_Position)n;
        count -= n;
    }
}

void XfsStyleSink::Flush() {
    if (doc_ && used_ > 0) {
        doc_->SetStyles((Sci_Position)used_, buf_);
        used_ = 0;
    }
}

// ------------------------------------------------------------------ XfsLexerBase

XfsLexerBase::XfsLexerBase(const char* name, int identifier)
    : name_(name), identifier_(identifier) {}

Sci_Position SCI_METHOD XfsLexerBase::PropertySet(const char* key, const char* val) {
    (void)val;
    if (!key) return -1;
    // Scintilla 会把通用属性推给当前词法器（Editor::SetLexerByName 就设了
    // fold / fold.compact）。自研族不走 Lexilla 的 props 机制，这些属性对本
    // 词法器没有语义；但必须回报「已处理」，否则 Scintilla 会记成未知属性。
    static const char* const kAccepted[] = {
        "fold", "fold.compact", "fold.comment", "fold.preprocessor",
        "fold.html", "fold.xml.at.tag.open", "styling.within.preprocessor",
        "tab.timmy.whinge.level", "lexer.cpp.track.preprocessor",
    };
    for (const char* k : kAccepted)
        if (std::strcmp(key, k) == 0) return 0;
    return -1;
}

Sci_Position SCI_METHOD XfsLexerBase::WordListSet(int n, const char* wl) {
    if (n < 0 || n >= kWordLists) return -1;
    words_[n].Set(wl);
    return 0;
}

namespace {

// 窗口上限：8MB。超过的部分靠状态机跨窗口续（见头文件「分块约定」）。
constexpr Sci_PositionU kSpanMax = 8u * 1024u * 1024u;
// 对齐行首时允许向后探测的上限倍数：单行超长（无 '\n'）时就此放弃对齐。
constexpr Sci_PositionU kLineProbeFactor = 4;

// 从 from 起向后找第一个 '\n'，返回其后一位；找不到则返回 from（不对齐）。
Sci_PositionU GrowToLineEnd(Scintilla::IDocument* doc, Sci_PositionU from,
                            Sci_PositionU end) {
    const Sci_PositionU cap =
        (std::min)(end, from + kSpanMax * kLineProbeFactor);
    std::vector<char> probe(65536);
    Sci_PositionU p = from;
    while (p < cap) {
        const Sci_PositionU n = (std::min)((Sci_PositionU)probe.size(), cap - p);
        doc->GetCharRange(probe.data(), (Sci_Position)p, (Sci_Position)n);
        for (Sci_PositionU i = 0; i < n; ++i)
            if (probe[i] == '\n') return p + i + 1;
        p += n;
    }
    return from;
}

} // namespace

void SCI_METHOD XfsLexerBase::Lex(Sci_PositionU startPos, Sci_Position lengthDoc,
                                  int initStyle, Scintilla::IDocument* pAccess) {
    if (!pAccess || lengthDoc <= 0) return;
    const Sci_Position docLen = pAccess->Length();
    if (docLen <= 0 || startPos > (Sci_PositionU)docLen) return;

    Sci_PositionU end = startPos + (Sci_PositionU)lengthDoc;
    if (end > (Sci_PositionU)docLen) end = (Sci_PositionU)docLen;
    if (end <= startPos) return;

    std::vector<char> buf;
    int style = initStyle;
    Sci_PositionU pos = startPos;
    while (pos < end) {
        Sci_PositionU chunk = (std::min)(kSpanMax, end - pos);
        // 只在不是最后一个窗口时才对行首 —— 末窗口若已到文档尾，多探无益。
        if (pos + chunk < end) chunk = GrowToLineEnd(pAccess, pos + chunk, end) - pos;
        if (chunk == 0) break;   // 防御：探测异常时不要死循环

        buf.resize((std::size_t)chunk);
        pAccess->GetCharRange(buf.data(), (Sci_Position)pos, (Sci_Position)chunk);

        XfsStyleSink sink(pAccess, (Sci_Position)pos, (Sci_Position)chunk);
        style = LexSpan(buf.data(), (std::size_t)chunk, (Sci_Position)pos, style, sink);

        pos += chunk;
    }
}

void SCI_METHOD XfsLexerBase::Fold(Sci_PositionU startPos, Sci_Position lengthDoc,
                                   int initStyle, Scintilla::IDocument* pAccess) {
    (void)initStyle;
    if (!pAccess || lengthDoc <= 0) return;
    const Sci_Position docLen = pAccess->Length();
    if (docLen <= 0 || startPos > (Sci_PositionU)docLen) return;

    Sci_PositionU end = startPos + (Sci_PositionU)lengthDoc;
    if (end > (Sci_PositionU)docLen) end = (Sci_PositionU)docLen;

    const Sci_Position first = pAccess->LineFromPosition((Sci_Position)startPos);
    const Sci_Position last =
        pAccess->LineFromPosition((Sci_Position)(end > 0 ? end - 1 : 0));
    if (last < first) return;
    FoldSpan(pAccess, first, last);
}

// -------------------------------------------------------------------- 工厂分流

bool XfsIsOwnLexer(const char* name) {
    if (!name) return false;
    return std::strcmp(name, kLexAtePattern) == 0 ||
           std::strcmp(name, kLexStil) == 0 ||
           std::strcmp(name, kLexAteLog) == 0;
}

Scintilla::ILexer5* XfsCreateLexer(const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, kLexAtePattern) == 0) return new AtePatternLexer();
    if (std::strcmp(name, kLexStil) == 0)       return new StilLexer();
    if (std::strcmp(name, kLexAteLog) == 0)     return new AteLogLexer();
    return nullptr;
}

} // namespace xfs
