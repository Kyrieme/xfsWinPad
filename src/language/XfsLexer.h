#pragma once
// xfsWinPad - 自研 ILexer5 词法器基础设施（批次 72：ATE 语言模式包）
//
// 【为什么自研，而不是给 vendored Lexilla 加词法器】
//   third_party/lexilla 是原样 vendored 的上游源码，在其中新增 lexer 意味着
//   长期维护一份上游补丁（上游一升级就冲突）。而 Scintilla 5 的 SCI_SETILEXER
//   接受任意 ILexer5*，所以 ATE 系列词法器实现在应用侧，由 XfsCreateLexer()
//   分流，third_party/ 零改动。
//
// 【分块约定（子类不必关心）】
//   基类把 Lex() 的 [startPos, startPos+lengthDoc) 切成 ≤kSpanMax 且对齐行首的
//   窗口，逐窗口取文本到内存后调子类的 LexSpan()，并把窗口结束时的样式状态回填
//   给下一个窗口——跨窗口的多行构造（块注释、长字符串、多行向量块）状态不丢。
//   子类因此只需写一个纯内存的逐字符状态机，不必关心 Scintilla 的缓冲区分段。
//
// 【样式输出】
//   子类不直接碰 IDocument 的样式接口，一律经 XfsStyleSink::Push()。sink 内部
//   按 4KB 批量写回，既有 LexAccessor 的批量化效果，又不依赖 Lexilla.dll 的
//   内部符号（lexlib 的实现编在 DLL 里，不可链接）。

#include <ILexer.h>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace xfs {

// ---- 自研词法器名（LanguageMap / Editor / 测试共用）------------------------
inline constexpr const char* kLexAtePattern = "ate_pattern";
inline constexpr const char* kLexStil       = "stil";
inline constexpr const char* kLexAteLog     = "ate_log";
// 批次 73：Chroma 3380 专属词法器。与 ate_pattern 的区别在于 ate_pattern 面向
// 「各家 ATE 方言的共同记号」，这两个直接实现 Chroma 手册第 2/4 章的语法。
inline constexpr const char* kLexChromaDec  = "chroma_dec";
inline constexpr const char* kLexChromaPlan = "chroma_plan";

// ---- ILexer5 标识号（GetIdentifier，也就是 SCI_GETLEXER 的返回值）------------
//
// 【为什么"必须全局唯一"是硬约束而不是洁癖】
//   这个号是「控件上挂的到底是哪个词法器」**唯一可读的证据**。单元测试能把
//   GetName() 比字符串，但跨进程的端到端脚本只能 SendMessageW，凡是"往调用方
//   缓冲区里写"的查询（SCI_GETLEXERLANGUAGE 之类）都跨不过进程边界——那会让
//   目标进程按自己的地址空间去写，实测直接把被测进程写崩（详见
//   scripts/ate-langs-e2e.ps1 顶部的教训）。于是 E2E 只能靠这个整数来判定
//   「按名字挑的自研词法器真的挂上去了」，**它一旦撞号，断言就失去分辨力**。
//   批次 73 就撞过一次：chroma_dec 与 stil 同为 7202、chroma_plan 与 ate_log
//   同为 7203 —— 即「.stil 挂上了 stil」这条断言在 .dec 上也会通过。
//   所以：名字与号并排放在一处，并用 kOwnLexers 表 + 单测钉死唯一性
//   （tests/test_atelexer.cpp 的 TestFactory 遍历 kOwnLexers 检查两两不等）。
inline constexpr int kLexIdAtePattern = 7201;
inline constexpr int kLexIdStil       = 7202;
inline constexpr int kLexIdAteLog     = 7203;
inline constexpr int kLexIdChromaDec  = 7204;
inline constexpr int kLexIdChromaPlan = 7205;

// 自研词法器清单：新增一个词法器时**必须**在这里补一行，否则唯一性检查覆盖不到。
struct OwnLexerInfo {
    const char* name;
    int         id;
};
inline constexpr OwnLexerInfo kOwnLexers[] = {
    { kLexAtePattern, kLexIdAtePattern },
    { kLexStil,       kLexIdStil       },
    { kLexAteLog,     kLexIdAteLog     },
    { kLexChromaDec,  kLexIdChromaDec  },
    { kLexChromaPlan, kLexIdChromaPlan },
};

// 大小写不敏感关键词集合。ATE 惯例全大写，但现场文件大小写混用很常见，
// 所以统一按不敏感匹配（与 Lexilla 多数词法器的大小写敏感策略是有意偏差）。
class KeywordSet {
public:
    void Set(const char* words);                        // 空白分隔，可重复调用
    bool Has(const char* word, std::size_t len) const;
    bool Empty() const { return words_.empty(); }
private:
    std::set<std::string> words_;
};

// 折叠层级编码：低 12 位 = 当前层级，高 16 位 = 下一层级，
// white = 纯空白行，header = 可折叠块头。自实现，理由同上（不链接 lexlib）。
int FoldLevel(int levelCur, int levelNext, bool white, bool header);

// 直接写一行的折叠层级（层级未变时跳过，避免无谓的文档通知）。
void SetFoldLine(Scintilla::IDocument* doc, Sci_Position line,
                 int levelCur, int levelNext, bool white, bool header);

// 取整行文本（不含行尾 EOL）到 out；返回 out 是否非空。
bool LineText(Scintilla::IDocument* doc, Sci_Position line, std::string* out);

// 样式输出口：把一段字节区间的样式批量写回文档。
// 约束：LexSpan 期间 Push() 的字节总数应正好等于 span 长度；
// 若少推（子类算错）dtor 会用默认样式补齐，避免与后续字符错位。
class XfsStyleSink {
public:
    XfsStyleSink(Scintilla::IDocument* doc, Sci_Position start, Sci_Position len);
    ~XfsStyleSink();
    XfsStyleSink(const XfsStyleSink&) = delete;
    XfsStyleSink& operator=(const XfsStyleSink&) = delete;

    void Push(int style, std::size_t count);
    void PushOne(int style) { Push(style, 1); }
    Sci_Position Pushed() const { return pushed_; }

private:
    void Flush();
    enum { kBufSize = 4096 };
    Scintilla::IDocument* doc_;
    Sci_Position len_;
    Sci_Position pushed_ = 0;
    char buf_[kBufSize];
    int used_ = 0;
};

// ---- 词法器基类 --------------------------------------------------------------
class XfsLexerBase : public Scintilla::ILexer5 {
public:
    // 子类实现：纯内存逐字符状态机。
    //   text/len : 本窗口文本；basePos 是它在文档中的绝对起始位置
    //   initStyle: 进入本窗口时的样式状态（= 上一窗口的返回值）
    //   返回     : 离开本窗口时的样式状态
    virtual int LexSpan(const char* text, std::size_t len,
                        Sci_Position basePos, int initStyle,
                        XfsStyleSink& sink) = 0;

    // 子类实现折叠；默认无折叠。行区间为 [lineFirst, lineLast]（闭区间）。
    virtual void FoldSpan(Scintilla::IDocument* /*doc*/, Sci_Position /*lineFirst*/,
                          Sci_Position /*lineLast*/) {}

    // ---- ILexer5 --------------------------------------------------------------
    int SCI_METHOD Version() const override { return Scintilla::lvRelease5; }
    void SCI_METHOD Release() override { delete this; }
    const char* SCI_METHOD PropertyNames() override { return ""; }
    int SCI_METHOD PropertyType(const char*) override { return 0; }
    const char* SCI_METHOD DescribeProperty(const char*) override { return nullptr; }
    Sci_Position SCI_METHOD PropertySet(const char* key, const char* val) override;
    const char* SCI_METHOD DescribeWordListSets() override { return ""; }
    Sci_Position SCI_METHOD WordListSet(int n, const char* wl) override;
    void SCI_METHOD Lex(Sci_PositionU startPos, Sci_Position lengthDoc,
                        int initStyle, Scintilla::IDocument* pAccess) override;
    void SCI_METHOD Fold(Sci_PositionU startPos, Sci_Position lengthDoc,
                         int initStyle, Scintilla::IDocument* pAccess) override;
    void* SCI_METHOD PrivateCall(int, void*) override { return nullptr; }
    int SCI_METHOD LineEndTypesSupported() override { return 0; }
    int SCI_METHOD AllocateSubStyles(int, int) override { return -1; }
    int SCI_METHOD SubStylesStart(int) override { return -1; }
    int SCI_METHOD SubStylesLength(int) override { return 0; }
    int SCI_METHOD StyleFromSubStyle(int subStyle) override { return subStyle; }
    int SCI_METHOD PrimaryStyleFromStyle(int style) override { return style; }
    void SCI_METHOD FreeSubStyles() override {}
    void SCI_METHOD SetIdentifiers(int, const char*) override {}
    int SCI_METHOD DistanceToSecondaryStyles() override { return 0; }
    const char* SCI_METHOD GetSubStyleBases() override { return ""; }
    int SCI_METHOD NamedStyles() override { return 0; }
    const char* SCI_METHOD NameOfStyle(int) override { return nullptr; }
    const char* SCI_METHOD TagsOfStyle(int) override { return nullptr; }
    const char* SCI_METHOD DescriptionOfStyle(int) override { return nullptr; }
    const char* SCI_METHOD GetName() override { return name_; }
    int SCI_METHOD GetIdentifier() override { return identifier_; }
    const char* SCI_METHOD PropertyGet(const char*) override { return nullptr; }

    // 与 SCI_SETKEYWORDS 的下标 0..kWordLists-1 对应。
    // 批次 73 从 4 提到 8：.pln 需要「测试语句 / CRAFT 宏 / C 库函数 / C 关键字 +
    // 流程记号 / pin_type」五张互不相同的表，4 个槽位装不下。
    enum { kWordLists = 8 };
    const KeywordSet& Words(int i) const {
        return words_[(i < 0 || i >= kWordLists) ? 0 : i];
    }

protected:
    XfsLexerBase(const char* name, int identifier);
    // 子类构造函数里用：写入内置关键词表。
    // 内置表放词法器里而不是 LanguageMap 里，是为了让 pin/timing 这类
    // 需要三张表以上的语言不受 LanguageInfo.keywords[2] 两个槽位的限制。
    // SCI_SETKEYWORDS 注入的词表会**追加**在内置表之上（set 去重）。
    KeywordSet& MutableWords(int i) {
        return words_[(i < 0 || i >= kWordLists) ? 0 : i];
    }

private:
    const char* name_;
    int identifier_;
    KeywordSet words_[kWordLists];
};

// 在 [text, text+len) 内按行回调：fn(行首指针, 行长不含EOL, EOL 字节数)。
// 行尾按 CRLF/LF/CR 识别；结尾正好落在行尾时不再补一次空行回调。
template <class F>
inline void ForEachLine(const char* text, std::size_t len, F&& fn) {
    std::size_t i = 0;
    while (i < len) {
        const std::size_t start = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') ++i;
        const std::size_t lineLen = i - start;
        std::size_t eol = 0;
        if (i < len) {
            eol = (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') ? 2 : 1;
            i += eol;
        }
        fn(text + start, lineLen, eol);
    }
}

// name 命中自研词法器则返回新实例（所有权交给调用方，通常随即交给 Scintilla），
// 否则返回 nullptr —— 调用方应回退到 Lexilla 的 ::CreateLexer。
Scintilla::ILexer5* XfsCreateLexer(const char* name);

// 该名字是否为自研词法器（不做实例化，供 Editor 判断是否需要注入关键字表）。
bool XfsIsOwnLexer(const char* name);

} // namespace xfs
