// test_atelexer.cpp - 自研 ATE 词法器单元测试（批次 72）
//
// 【为什么带一个假 IDocument 而不是只测纯函数】
//   三个词法器的价值全在 LexSpan/Lex 的**行分类结果**上（哪些字节是 opcode、
//   哪些是驱动/比较/掩码、哪些行根本不该着色）。把判据拆成可单独调用的纯函数
//   会诱导出「判据对了但输出错位」的假绿——真正会出事的是 Push 的字节数与
//   文档区间错位一格。所以这里实现一个最小可用的 Scintilla::IDocument，
//   让真正的 Lex()/Fold() 跑在它上面，然后逐字节核对样式。
//
// 【覆盖重点】
//   1. 工厂分流与 ILexer5 契约（名字/标识/版本/词表注入/属性）
//   2. .pat 行分类：向量行 vs 计数行、pin 组、标签、跳转目标、指令、注释
//   3. .stil 块注释跨窗口状态（9MB 文档强制分窗）+ 花括号折叠
//   4. .log 的「保守门」：通用应用日志必须整行不着色；FAIL 行整体升级为红
//   5. 分块安全：样式字节总数必须恰好等于文档长度（错位会污染后半篇）

#include "../src/language/XfsLexer.h"
#include "../src/language/XfsLexerStyles.h"

#include <Scintilla.h>
#include <ILexer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xfs;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------- 假文档
// 行结构照 Scintilla 的约定：'\n' 之后开新行；文档以 '\n' 结尾时末尾会有一个
// 空行。LineFromPosition(Length()) 因此返回**最后一行**的下标，这正是
// FoldSpan() 里 `doc->LineFromPosition(doc->Length())` 的用法所依赖的语义。
class MockDoc : public Scintilla::IDocument {
public:
    // 未着色哨兵。初值**不能**用 0：0 正是 XfsStyleSink 在子类少推样式时的
    // 补齐值，若初值也是 0，「这段压根没被写过」和「被补齐为默认样式」就
    // 分不出来了。0x7F 不在任何真实样式号范围内（最小编号 64）。
    static const int kUnstyled = 0x7F;

    explicit MockDoc(std::string text) : text_(std::move(text)) {
        lineStarts_.push_back(0);
        for (std::size_t i = 0; i < text_.size(); ++i)
            if (text_[i] == '\n') lineStarts_.push_back((Sci_Position)i + 1);
        styles_.assign(text_.size(), (char)kUnstyled);
        levels_.assign(lineStarts_.size(), 0);
    }

    std::size_t LineCount() const { return lineStarts_.size(); }

    // 供测试查询：某字节的样式（IDocument::StyleAt 返回 char，按无符号解读）
    int StyleOf(std::size_t pos) const {
        return pos < styles_.size() ? (int)(unsigned char)styles_[pos] : -1;
    }
    bool RangeIs(std::size_t from, std::size_t len, int style) const {
        for (std::size_t i = from; i < from + len; ++i)
            if (StyleOf(i) != style) return false;
        return true;
    }
    std::size_t CountStyle(int style) const {
        std::size_t n = 0;
        for (char c : styles_)
            if ((int)(unsigned char)c == style) ++n;
        return n;
    }
    const std::string& Text() const { return text_; }

    // ---- Scintilla::IDocument ----
    int SCI_METHOD Version() const override { return 0; }
    void SCI_METHOD SetErrorStatus(int) override {}
    Sci_Position SCI_METHOD Length() const override { return (Sci_Position)text_.size(); }

    void SCI_METHOD GetCharRange(char* buffer, Sci_Position position,
                                 Sci_Position lengthRetrieve) const override {
        if (!buffer || position < 0 || lengthRetrieve <= 0) return;
        const std::size_t p = (std::size_t)position;
        if (p >= text_.size()) return;
        const std::size_t n = (std::min)((std::size_t)lengthRetrieve, text_.size() - p);
        std::memcpy(buffer, text_.data() + p, n);
    }

    char SCI_METHOD StyleAt(Sci_Position position) const override {
        if (position < 0 || (std::size_t)position >= styles_.size()) return 0;
        return styles_[(std::size_t)position];
    }

    Sci_Position SCI_METHOD LineFromPosition(Sci_Position position) const override {
        if (position < 0) return 0;
        std::size_t lo = 0, hi = lineStarts_.size();
        while (lo + 1 < hi) {                       // 最后一个 <= position 的行首
            const std::size_t mid = (lo + hi) / 2;
            if ((std::size_t)lineStarts_[mid] <= (std::size_t)position) lo = mid; else hi = mid;
        }
        return (Sci_Position)lo;
    }

    Sci_Position SCI_METHOD LineStart(Sci_Position line) const override {
        if (line < 0) return 0;
        if ((std::size_t)line >= lineStarts_.size()) return (Sci_Position)text_.size();
        return lineStarts_[(std::size_t)line];
    }

    Sci_Position SCI_METHOD LineEnd(Sci_Position line) const override {
        if (line < 0) return 0;
        const std::size_t li = (std::size_t)line;
        const Sci_Position s = LineStart(line);
        Sci_Position e = (li + 1 < lineStarts_.size()) ? lineStarts_[li + 1]
                                                      : (Sci_Position)text_.size();
        while (e > s && (text_[(std::size_t)e - 1] == '\n' ||
                         text_[(std::size_t)e - 1] == '\r')) --e;
        return e;
    }

    int SCI_METHOD GetLevel(Sci_Position line) const override {
        if (line < 0 || (std::size_t)line >= levels_.size()) return SC_FOLDLEVELBASE;
        return levels_[(std::size_t)line];
    }
    int SCI_METHOD SetLevel(Sci_Position line, int level) override {
        if (line < 0 || (std::size_t)line >= levels_.size()) return 0;
        levels_[(std::size_t)line] = level;
        return 0;
    }
    int SCI_METHOD GetLineState(Sci_Position) const override { return 0; }
    int SCI_METHOD SetLineState(Sci_Position, int) override { return 0; }

    void SCI_METHOD StartStyling(Sci_Position position) override { styleAt_ = position; }

    bool SCI_METHOD SetStyleFor(Sci_Position length, char style) override {
        if (!BumpWindow(length)) return false;
        for (Sci_Position i = 0; i < length; ++i)
            styles_[(std::size_t)(styleAt_ + i)] = style;
        styleAt_ += length;
        return true;
    }

    bool SCI_METHOD SetStyles(Sci_Position length, const char* styles) override {
        if (!BumpWindow(length)) return false;
        std::memcpy(styles_.data() + styleAt_, styles, (std::size_t)length);
        styleAt_ += length;
        return true;
    }

    void SCI_METHOD DecorationSetCurrentIndicator(int) override {}
    void SCI_METHOD DecorationFillRange(Sci_Position, int, Sci_Position) override {}
    void SCI_METHOD ChangeLexerState(Sci_Position, Sci_Position) override {}
    int SCI_METHOD CodePage() const override { return 65001; }
    bool SCI_METHOD IsDBCSLeadByte(char) const override { return false; }
    const char* SCI_METHOD BufferPointer() override { return text_.c_str(); }

    int SCI_METHOD GetLineIndentation(Sci_Position line) override {
        const Sci_Position s = LineStart(line), e = LineEnd(line);
        int col = 0;
        for (Sci_Position i = s; i < e; ++i) {
            if (text_[(std::size_t)i] == ' ') ++col;
            else if (text_[(std::size_t)i] == '\t') col = (col / 4 + 1) * 4;
            else break;
        }
        return col;
    }

    Sci_Position SCI_METHOD GetRelativePosition(Sci_Position positionStart,
                                                Sci_Position characterOffset) const override {
        Sci_Position p = positionStart + characterOffset;
        if (p < 0) p = 0;
        if (p > (Sci_Position)text_.size()) p = (Sci_Position)text_.size();
        return p;
    }

    int SCI_METHOD GetCharacterAndWidth(Sci_Position position, Sci_Position* pWidth) const override {
        if (position < 0 || (std::size_t)position >= text_.size()) {
            if (pWidth) *pWidth = 0;
            return 0;
        }
        if (pWidth) *pWidth = 1;
        return (int)(unsigned char)text_[(std::size_t)position];
    }

private:
    // 与真 Scintilla 一致：越界写样式返回 false 而不是崩
    bool BumpWindow(Sci_Position length) const {
        return length >= 0 && styleAt_ >= 0 &&
               (std::size_t)(styleAt_ + length) <= styles_.size();
    }

    std::string text_;
    std::vector<Sci_Position> lineStarts_;
    std::vector<char> styles_;
    std::vector<int> levels_;
    Sci_Position styleAt_ = 0;
};

// ------------------------------------------------------------------ 小工具

static std::size_t FindIn(const std::string& hay, const char* needle,
                          std::size_t from = 0) {
    const std::size_t p = hay.find(needle, from);
    return p == std::string::npos ? (std::size_t)-1 : p;
}

// 跑一次完整 Lex（模拟 Scintilla 对整篇调用），并返回词法器释放前的样式总数断言
static void LexAll(Scintilla::ILexer5* lx, MockDoc& doc) {
    lx->Lex(0, doc.Length(), 0, &doc);
}

// 断言整篇样式字节数守恒：Lex 必须显式覆盖 [0, Length) 的每一个字节。
// 留下哨兵 = 有一段从没被写过 → 样式与字符错位，后半篇整体串色。
static void CheckFullyStyled(MockDoc& doc) {
    ++g_checks;
    const std::size_t left = doc.CountStyle(MockDoc::kUnstyled);
    if (left != 0) {
        ++g_fail;
        printf("FAIL %zu byte(s) left unstyled (of %d)\n", left, (int)doc.Length());
    }
}

// 找子串并断言其样式（避免手算列号，改文档时不容易失效）
static void CheckTokenStyle(MockDoc& doc, const char* token, int style,
                            std::size_t from = 0) {
    const std::size_t p = FindIn(doc.Text(), token, from);
    if (p == (std::size_t)-1) { ++g_fail; printf("FAIL token not found: %s\n", token); return; }
    ++g_checks;
    const int got = doc.StyleOf(p);
    if (got != style) {
        ++g_fail;
        printf("FAIL token '%s' at %zu: style %d, expected %d\n", token, p, got, style);
    }
}

// =============================================================== 1. 工厂/契约

static void TestFactory() {
    CHECK(XfsIsOwnLexer(kLexAtePattern));
    CHECK(XfsIsOwnLexer(kLexStil));
    CHECK(XfsIsOwnLexer(kLexAteLog));
    CHECK(!XfsIsOwnLexer("cpp"));
    CHECK(!XfsIsOwnLexer(nullptr));
    // 非自研名必须返回 nullptr，让 Editor 回退到 Lexilla 的 ::CreateLexer
    CHECK(XfsCreateLexer("cpp") == nullptr);
    CHECK(XfsCreateLexer("") == nullptr);
    CHECK(XfsCreateLexer(nullptr) == nullptr);

    struct { const char* name; int ident; } kCases[] = {
        { kLexAtePattern, 7201 },
        { kLexStil,       7202 },
        { kLexAteLog,     7203 },
    };
    for (const auto& c : kCases) {
        Scintilla::ILexer5* lx = XfsCreateLexer(c.name);
        CHECK(lx != nullptr);
        if (!lx) continue;
        CHECK(lx->Version() == Scintilla::lvRelease5);
        CHECK(std::strcmp(lx->GetName(), c.name) == 0);
        CHECK(lx->GetIdentifier() == c.ident);
        // 自研族不参与 substyle / 行尾类型 / 具名样式，必须给出安全默认值
        CHECK(lx->LineEndTypesSupported() == 0);
        CHECK(lx->NamedStyles() == 0);
        CHECK(lx->AllocateSubStyles(0, 1) == -1);
        CHECK(lx->SubStylesStart(0) == -1);
        CHECK(lx->DistanceToSecondaryStyles() == 0);
        CHECK(lx->PrivateCall(0, nullptr) == nullptr);
        // Scintilla 会把 fold 等通用属性推给当前词法器：已知的必须回报已处理，
        // 否则会被记成未知属性；未知的回报 -1。
        CHECK(lx->PropertySet("fold", "1") == 0);
        CHECK(lx->PropertySet("fold.comment", "1") == 0);
        CHECK(lx->PropertySet("no.such.property", "1") == -1);
        CHECK(lx->PropertySet(nullptr, nullptr) == -1);
        // 词表下标越界不崩、回报失败
        CHECK(lx->WordListSet(99, "X") == -1);
        CHECK(lx->WordListSet(-1, "X") == -1);
        lx->Release();
    }
}

// ================================================================== 2. 样式号

static void TestStyleNumbering() {
    // 与 Scintilla 保留区（32..39）和 STYLE_MAX(255) 的关系
    static_assert(kXfsStyleBase == 64, "ate style base changed");
    CHECK(kXfsStyleBase > 39);
    CHECK(kXfsStyleEnd <= 128);
    // 三族区间必须互不重叠，否则切换语言会串色
    CHECK(SCE_ATEP_DEFAULT < SCE_STIL_DEFAULT);
    CHECK(SCE_STIL_DEFAULT < SCE_ATEL_DEFAULT);
    CHECK(SCE_ATEP_DIRECTIVE < SCE_STIL_DEFAULT);
    CHECK(SCE_STIL_STRING < SCE_ATEL_DEFAULT);
    CHECK(SCE_ATEL_RECORD < kXfsStyleEnd);
    // Push() 把样式归一到 7 位（style & 0x7F），所以任何 ≥128 的样式号都会
    // 静默回绕成另一个（合法的）样式 —— 那是「某些记号莫名其妙变成另一种颜色」
    // 这一类极难定位的 bug。这里直接校验最大样式号在归一下不变。
    CHECK((SCE_ATEP_DEFAULT & 0x7F) == SCE_ATEP_DEFAULT);
    CHECK((SCE_ATEL_RECORD & 0x7F) == SCE_ATEL_RECORD);
    CHECK(SCE_ATEL_RECORD < 128);
    // 哨兵值必须落在真实样式号区间之外，否则「未着色」判据会误报
    CHECK(MockDoc::kUnstyled >= kXfsStyleEnd);
}

// FoldLevel 的位布局与 Lexilla 对齐（低位=本层，高位=下一层，都含 Base）
static void TestFoldLevelEncoding() {
    // 低位：Base + 本层；高位：Base + 下一层。与 LexCPP 的
    // FoldLevelForCurrentNext(cur, next) = cur | next << 16 同构。
    {
        const int lev = FoldLevel(0, 1, false, true);
        CHECK((lev & SC_FOLDLEVELNUMBERMASK) == SC_FOLDLEVELBASE + 0);
        CHECK((lev >> 16) == SC_FOLDLEVELBASE + 1);
        CHECK((lev & SC_FOLDLEVELHEADERFLAG) != 0);
        CHECK((lev & SC_FOLDLEVELWHITEFLAG) == 0);
        // Lexilla 的增量续扫写法必须能还原出绝对层级
        CHECK((lev >> 16) - SC_FOLDLEVELBASE == 1);
    }
    {
        const int lev = FoldLevel(2, 2, true, false);
        CHECK((lev & SC_FOLDLEVELNUMBERMASK) == SC_FOLDLEVELBASE + 2);
        CHECK((lev >> 16) == SC_FOLDLEVELBASE + 2);
        CHECK((lev & SC_FOLDLEVELWHITEFLAG) != 0);
        CHECK((lev & SC_FOLDLEVELHEADERFLAG) == 0);
    }
    // 负值钳到 0（语法不完整的片段不能让层级变负）
    {
        const int lev = FoldLevel(-5, -5, false, false);
        CHECK((lev & SC_FOLDLEVELNUMBERMASK) == SC_FOLDLEVELBASE);
        CHECK((lev >> 16) == SC_FOLDLEVELBASE);
    }
    // 极深缩进钳位：越界会溢进 WhiteFlag/HeaderFlag 位，把折叠箭头弄乱
    {
        constexpr int kMaxDepth = SC_FOLDLEVELNUMBERMASK - SC_FOLDLEVELBASE;
        const int lev = FoldLevel(99999, 99999, false, false);
        CHECK((lev & SC_FOLDLEVELNUMBERMASK) == SC_FOLDLEVELNUMBERMASK);
        CHECK((lev >> 16) == SC_FOLDLEVELBASE + kMaxDepth);
        CHECK((lev & SC_FOLDLEVELWHITEFLAG) == 0);   // 未被层级值污染
        CHECK((lev & SC_FOLDLEVELHEADERFLAG) == 0);
    }
}

// ============================================================== 3. KeywordSet

static void TestKeywordSet() {
    KeywordSet ks;
    CHECK(ks.Empty());
    CHECK(!ks.Has("RPT", 3));
    ks.Set("RPT JMP  CALL");
    CHECK(!ks.Empty());
    CHECK(ks.Has("RPT", 3));
    CHECK(ks.Has("call", 4));          // ATE 现场大小写混用，判据不敏感
    CHECK(ks.Has("Call", 4));
    CHECK(!ks.Has("RP", 2));           // 前缀不算命中
    CHECK(!ks.Has("RPTX", 4));         // 后缀不算命中
    CHECK(!ks.Has("", 0));
    CHECK(!ks.Has(nullptr, 3));
    // 累积而不是替换（SetLexerByName 会在切主题时重设）
    ks.Set("GOTO");
    CHECK(ks.Has("RPT", 3) && ks.Has("GOTO", 4));
    // 超长 token 走堆缓冲分支（Has 里 small[64] 的边界）
    {
        const std::string longWord(200, 'q');
        ks.Set(longWord.c_str());
        CHECK(ks.Has(longWord.c_str(), longWord.size()));
    }
}

// ==================================================== 4. ATE Pattern 行分类

static void TestAtePattern() {
    MockDoc doc(
        "RPT 10\n"                         // 0  计数行（首记号是 opcode → 否定向量）
        "0 1 X\n"                          // 1  向量行：驱动/驱动/掩码
        "( P1 P2 ) 1 0 H\n"                // 2  pin 组行 + 驱动 + 比较
        "0101\n"                           // 3  位块 → 向量行
        "LBL_MAIN:\n"                      // 4  标签定义
        "    JMP LBL_MAIN\n"               // 5  opcode + 跳转目标
        ".INCLUDE \"cfg.pat\"\n"           // 6  指令 + 字符串
        "VDD P1\n"                         // 7  pin 目录行
        "0x1F\n"                           // 8  十六进制
        "10\n"                             // 9  非向量行 → 号码而非向量
        "TSET ts1\n"                       // 10 timing 引用
        "RPT 2 # 尾部注释\n");             // 11 尾部注释

    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAtePattern);
    LexAll(lx, doc);

    // 行 0：`RPT 10` —— 首记号是 opcode，10 必须保持**号码**色（不能升格成向量）
    CheckTokenStyle(doc, "RPT 10", SCE_ATEP_OPCODE);
    CheckTokenStyle(doc, "10", SCE_ATEP_NUMBER);
    // 行 1：向量行的 0/1 是驱动色，X 是掩码色 —— 同一拍必须都能分出来
    {
        const std::size_t l1 = FindIn(doc.Text(), "0 1 X");
        CHECK(l1 != (std::size_t)-1);
        CHECK(doc.RangeIs(l1 + 0, 1, SCE_ATEP_VECTOR));   // 0 → 驱动
        CHECK(doc.RangeIs(l1 + 1, 1, SCE_ATEP_DEFAULT));  // 空格
        CHECK(doc.RangeIs(l1 + 2, 1, SCE_ATEP_VECTOR));   // 1 → 驱动
        CHECK(doc.RangeIs(l1 + 4, 1, SCE_ATEP_MASK));     // X → 掩码
    }
    // 行 2：括号 pin 组里的名字是 pin 色，组外的 0/1/H 按向量分色
    {
        const std::size_t l2 = FindIn(doc.Text(), "( P1 P2 ) 1 0 H");
        CHECK(l2 != (std::size_t)-1);
        CHECK(doc.RangeIs(l2, 1, SCE_ATEP_OPERATOR));         // '('
        CHECK(doc.RangeIs(l2 + 2, 2, SCE_ATEP_PIN));          // P1
        CHECK(doc.RangeIs(l2 + 5, 2, SCE_ATEP_PIN));          // P2
        CHECK(doc.RangeIs(l2 + 10, 1, SCE_ATEP_VECTOR));      // 1 → 驱动
        CHECK(doc.RangeIs(l2 + 12, 1, SCE_ATEP_VECTOR));      // 0 → 驱动
        CHECK(doc.RangeIs(l2 + 14, 1, SCE_ATEP_EXPECT));      // H → 比较
    }
    // 行 3：`0101` 四位位块 → 整块驱动色（不是号码）
    CheckTokenStyle(doc, "0101", SCE_ATEP_VECTOR);
    // 行 4：标签定义（NAME:）
    CheckTokenStyle(doc, "LBL_MAIN:", SCE_ATEP_LABEL);
    // 行 5：跳转 opcode 后的标识符是**目标标签**，不是普通名
    CheckTokenStyle(doc, "JMP", SCE_ATEP_OPCODE);
    {
        const std::size_t p = FindIn(doc.Text(), "JMP ");
        CHECK(doc.RangeIs(p + 4, 8, SCE_ATEP_LABEL));
    }
    // 行 6：以 '.' 开头 → 指令；引号串 → 字符串
    CheckTokenStyle(doc, ".INCLUDE", SCE_ATEP_DIRECTIVE);
    CheckTokenStyle(doc, "\"cfg.pat\"", SCE_ATEP_STRING);
    // 行 7：内置 pin 表命中 + pin 目录行（全是标识符）整体按 pin 上色
    CheckTokenStyle(doc, "VDD", SCE_ATEP_PIN);
    {
        const std::size_t p = FindIn(doc.Text(), "VDD ");
        CHECK(doc.RangeIs(p + 4, 2, SCE_ATEP_PIN));          // P1：靠 pinListLine 判据
    }
    // 行 8：十六进制
    CheckTokenStyle(doc, "0x1F", SCE_ATEP_HEX);
    // 行 9：独立 `10`（<4 位纯 01 串且非向量行）→ 号码
    {
        const std::size_t p = FindIn(doc.Text(), "0x1F\n");
        CHECK(doc.RangeIs(p + 5, 2, SCE_ATEP_NUMBER));
    }
    // 行 10：timing 引用（内置第三张表，LanguageMap 的两个 keywords 槽位放不下）
    CheckTokenStyle(doc, "TSET", SCE_ATEP_TIMING);
    // 行 11：`#` 起到底都是注释
    {
        const std::size_t p = FindIn(doc.Text(), "# 尾部注释");
        CHECK(p != (std::size_t)-1);
        CHECK(doc.RangeIs(p, std::strlen("# 尾部注释"), SCE_ATEP_COMMENT));
    }
    // 每个字节都被显式写过（Push 总数守恒，后半篇才不会整体错位）
    CheckFullyStyled(doc);

    lx->Release();
}

// .pat 的词表注入：LanguageMap 的 keywords 槽位之外还能追加内置表
static void TestAtePatternWordList() {
    MockDoc doc("MYOP 5\nXPIN 2\nrpt 3\n");
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAtePattern);
    CHECK(lx->WordListSet(0, "MYOP") == 0);
    CHECK(lx->WordListSet(1, "XPIN") == 0);
    LexAll(lx, doc);
    CheckTokenStyle(doc, "MYOP", SCE_ATEP_OPCODE);
    CheckTokenStyle(doc, "XPIN", SCE_ATEP_PIN);
    // 小写输入照样命中 opcode（现场文件大小写混用）
    CheckTokenStyle(doc, "rpt", SCE_ATEP_OPCODE);
    // 注入是追加，内置表仍在
    CHECK(lx->WordListSet(0, "RPT") == 0);
    lx->Release();
}

// .pat 折叠：缩进表达 pattern 内的层级
static void TestAtePatternFold() {
    MockDoc doc(
        "LBL_A:\n"          // 0  indent 0
        "    RPT 2\n"       // 1  indent 4  → 使 0 成为块头
        "    RPT 3\n"       // 2  indent 4  同层，不得把 1 弹成父层
        "RPT 4\n");         // 3  indent 0

    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAtePattern);
    lx->Fold(0, doc.Length(), 0, &doc);

    // 行 0 必须是可折叠块头，且下一层是 1
    CHECK((doc.GetLevel(0) & SC_FOLDLEVELHEADERFLAG) != 0);
    CHECK(((doc.GetLevel(0) >> 16) - SC_FOLDLEVELBASE) == 1);
    CHECK(((doc.GetLevel(0) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 0);
    // 行 1 同为块内容，不是块头
    CHECK((doc.GetLevel(1) & SC_FOLDLEVELHEADERFLAG) == 0);
    // 连续同缩进的两行必须同层（这是 >= 写成 > 的那个 bug 的回归护栏）
    CHECK((doc.GetLevel(1) & SC_FOLDLEVELNUMBERMASK) ==
          (doc.GetLevel(2) & SC_FOLDLEVELNUMBERMASK));
    CHECK(((doc.GetLevel(2) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 1);
    // 行 3 缩进回到 0 → 层 0
    CHECK(((doc.GetLevel(3) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 0);
    lx->Release();
}

// .pat 判定优先级：上下文必须压过词表。
// RESET/TRIG/STROBE 同时躺在 opcode、timing、pin 三张表里，纯按表顺序判定会
// 给出反直觉结果 —— 这几个 case 全部来自 sample.pat 的真实 dump。
static void TestAtePatternPrecedence() {
    MockDoc doc(
        "( VDD RESET TRIG ) 0 1\n"   // pin 组：同一括号里三者必须同色
        "TSET ts_main\n"             // 首记号是 timing 引用 -> 不是 pin 目录行
        "MAIN:\n");                  // 标签定义压过 opcode 表

    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAtePattern);
    LexAll(lx, doc);

    // 同一个 pin 组里的三个词必须都是 pin 色（原来 RESET/TRIG 会被抢走）
    {
        const std::size_t p = FindIn(doc.Text(), "( VDD RESET TRIG )");
        CHECK(p != (std::size_t)-1);
        CHECK(doc.RangeIs(p + 2, 3, SCE_ATEP_PIN));   // VDD
        CHECK(doc.RangeIs(p + 6, 5, SCE_ATEP_PIN));   // RESET（也是 opcode）
        CHECK(doc.RangeIs(p + 12, 4, SCE_ATEP_PIN));  // TRIG（也是 opcode/timing）
    }
    // timing set 名不得被当成 pin（原来整行被误判为 pin 目录行）
    {
        const std::size_t p = FindIn(doc.Text(), "ts_main");
        CHECK(p != (std::size_t)-1);
        CHECK(doc.RangeIs(p, 7, SCE_ATEP_DEFAULT));
    }
    // `MAIN:` 是定义，不是指令
    CheckTokenStyle(doc, "MAIN", SCE_ATEP_LABEL);
    CheckFullyStyled(doc);
    lx->Release();
}

// ======================================================= 5. STIL 行分类/折叠

static void TestStil() {
    MockDoc doc(
        "Signals {\n"                          // 0 块头
        "'CLK' In;\n"                          // 1 单引号名 + 方向关键字
        "Period '20ns';\n"                     // 2 带单位量
        "WFT1 { 0 D; 0 U; }\n"                 // 3 波形事件
        "Value 1.8e-9;\n"                      // 4 指数计数
        "\"quoted text\";\n"                   // 5 双引号串
        "// 行注释\n"                           // 6 行注释
        "/* 单行块注释 */ x\n"                   // 7 同行内闭合的块注释
        "Signals } // done\n");                // 8

    Scintilla::ILexer5* lx = XfsCreateLexer(kLexStil);
    LexAll(lx, doc);

    CheckTokenStyle(doc, "Signals", SCE_STIL_BLOCK);
    CheckTokenStyle(doc, "'CLK'", SCE_STIL_NAME);
    CheckTokenStyle(doc, "In", SCE_STIL_KEYWORD);
    CheckTokenStyle(doc, "'20ns'", SCE_STIL_UNIT);
    // 波形事件是单字符（D/U/Z/X/N/T/L/H）
    {
        const std::size_t p = FindIn(doc.Text(), "0 D;");
        CHECK(p != (std::size_t)-1);
        CHECK(doc.RangeIs(p, 1, SCE_STIL_NUMBER));
        CHECK(doc.RangeIs(p + 2, 1, SCE_STIL_EVENT));
    }
    CheckTokenStyle(doc, "1.8e-9", SCE_STIL_NUMBER);
    CheckTokenStyle(doc, "\"quoted text\"", SCE_STIL_STRING);
    {
        const std::size_t p = FindIn(doc.Text(), "// 行注释");
        CHECK(doc.RangeIs(p, std::strlen("// 行注释"), SCE_STIL_COMMENT));
    }
    {
        const std::size_t p = FindIn(doc.Text(), "/* 单行块注释 */");
        CHECK(doc.RangeIs(p, std::strlen("/* 单行块注释 */"), SCE_STIL_COMMENT));
    }
    CheckFullyStyled(doc);
    lx->Release();
}

// 跨行块注释：状态必须跨 LexSpan 窗口续上
static void TestStilBlockCommentAcrossLines() {
    MockDoc doc(
        "Signals {\n"
        "/* 开始\n"          // 未闭合
        "仍在注释里 */ 'CLK' In;\n"
        "}\n");
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexStil);
    LexAll(lx, doc);

    {
        const std::size_t p = FindIn(doc.Text(), "/* 开始");
        CHECK(p != (std::size_t)-1);
        // 从 `/*` 到 `*/` 之间（含换行）全是注释色
        const std::size_t close = FindIn(doc.Text(), "*/");
        CHECK(close != (std::size_t)-1 && close > p);
        CHECK(doc.RangeIs(p, close + 2 - p, SCE_STIL_COMMENT));
    }
    // 闭合之后同一行恢复常规词法
    {
        const std::size_t p = FindIn(doc.Text(), "'CLK'");
        CHECK(doc.RangeIs(p, 5, SCE_STIL_NAME));
    }
    CheckFullyStyled(doc);
    lx->Release();
}

// 强制分窗：>8MB 的文档会把 Lex() 切成两个窗口。第 0 行开一个永不闭合的
// `/*`，若窗口间状态丢失，第二个窗口就会按普通文本着色（默认色）——
// 于是「整篇都是注释色」这个断言正好能抓到状态丢失。
static void TestStilStateAcrossWindows() {
    const std::size_t kTarget = (9u << 20);            // 9MB > kSpanMax(8MB)
    std::string filler(79, 'x');
    filler += '\n';                                    // 80 字节/行
    std::string big = "/* unterminated\n";
    while (big.size() < kTarget) big += filler;
    // 补一行让文档不以半个行结束，顺带验证末窗口
    big += "tail\n";

    MockDoc doc(std::move(big));
    CHECK(doc.Length() > (Sci_Position)kTarget);

    Scintilla::ILexer5* lx = XfsCreateLexer(kLexStil);
    LexAll(lx, doc);

    CHECK(doc.CountStyle(SCE_STIL_COMMENT) == (std::size_t)doc.Length());
    CHECK(doc.CountStyle(SCE_STIL_DEFAULT) == 0);
    CheckFullyStyled(doc);
    lx->Release();
}

// STIL 折叠按花括号深度
static void TestStilFold() {
    MockDoc doc(
        "Signals {\n"          // 0  +1 → 块头
        "'CLK' In;\n"          // 1   0
        "A B \"{\"\n"          // 2   0（引号里的花括号不得计数）
        "}\n");                // 3  -1
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexStil);
    lx->Fold(0, doc.Length(), 0, &doc);

    CHECK((doc.GetLevel(0) & SC_FOLDLEVELHEADERFLAG) != 0);
    CHECK(((doc.GetLevel(0) >> 16) - SC_FOLDLEVELBASE) == 1);
    CHECK((doc.GetLevel(1) & SC_FOLDLEVELHEADERFLAG) == 0);
    CHECK(((doc.GetLevel(1) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 1);
    // 引号里的 '{' 被跳过：行 2 不得成为块头
    CHECK((doc.GetLevel(2) & SC_FOLDLEVELHEADERFLAG) == 0);
    CHECK(((doc.GetLevel(2) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 1);
    CHECK(((doc.GetLevel(3) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 0);
    lx->Release();

    // 嵌套块：闭合行必须归**外层**，不能比所在块头更深（否则折叠树凭空多一层）
    MockDoc nested(
        "A {\n"        // 0  lvl 0, 块头
        "  B {\n"      // 1  lvl 1, 块头
        "  }\n"        // 2  lvl 1（不是 2）
        "}\n");        // 3  lvl 0
    Scintilla::ILexer5* lx2 = XfsCreateLexer(kLexStil);
    lx2->Fold(0, nested.Length(), 0, &nested);
    CHECK(((nested.GetLevel(0) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 0);
    CHECK((nested.GetLevel(0) & SC_FOLDLEVELHEADERFLAG) != 0);
    CHECK(((nested.GetLevel(1) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 1);
    CHECK((nested.GetLevel(1) & SC_FOLDLEVELHEADERFLAG) != 0);
    CHECK(((nested.GetLevel(2) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 1);
    CHECK((nested.GetLevel(2) & SC_FOLDLEVELHEADERFLAG) == 0);
    CHECK(((nested.GetLevel(3) & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE) == 0);
    lx2->Release();
}

// ============================================================== 6. ATE Log

// 保守门：通用应用程序日志必须**整行不着色**。这是这个词法器存在的
// 前提条件 —— 把通用日志染花是净损失。
static void TestAteLogConservatism() {
    const char* kGeneric[] = {
        "2026-09-17 08:12:33 INFO Service started successfully",
        "2026-09-17 08:12:33 DEBUG Loading configuration from disk",
        "TOTAL 250 items processed",                    // 弱记录词不得触发
        "[INFO] retry [1]",                             // 括号里没数字+分隔符
        "08:12:33 Listening on port 8080",              // 时间戳本身不是 ATE 证据
        "0x0000FFFF memory mapped",
    };
    for (const char* line : kGeneric) {
        MockDoc doc(std::string(line) + "\n");
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
        LexAll(lx, doc);
        ++g_checks;
        if (doc.CountStyle(SCE_ATEL_DEFAULT) != (std::size_t)doc.Length()) {
            ++g_fail;
            printf("FAIL generic log line got colored: %s\n", line);
        }
        lx->Release();
    }

    // 非 ATE 行里的注释仍然淡化（注释在任何日志里都该淡）
    MockDoc doc("# startup notes\n");
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
    LexAll(lx, doc);
    {
        const std::size_t p = FindIn(doc.Text(), "# startup notes");
        CHECK(doc.RangeIs(p, std::strlen("# startup notes"), SCE_ATEL_COMMENT));
    }
    lx->Release();
}

// ATE 行的四类记号：时间戳 / SITE / 测量值 / 限值 / 判定词
static void TestAteLogPassLine() {
    const char* kLine =
        "[2026-09-17 08:12:33] SITE1 VDD_1P8 1.812 PASS [1.650, 1.950]";
    MockDoc doc(std::string(kLine) + "\n");
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
    LexAll(lx, doc);
    const std::string& t = doc.Text();

    CHECK(doc.RangeIs(0, std::strlen("[2026-09-17 08:12:33]"), SCE_ATEL_TIMESTAMP));
    CheckTokenStyle(doc, "SITE1", SCE_ATEL_SITE);
    CheckTokenStyle(doc, "VDD_1P8", SCE_ATEL_TESTNAME);
    {
        const std::size_t p = FindIn(t, "1.812");
        CHECK(doc.RangeIs(p, 5, SCE_ATEL_NUMBER));      // PASS 行数值保持中性
    }
    CheckTokenStyle(doc, "PASS", SCE_ATEL_PASS);
    {
        const std::size_t p = FindIn(t, "[1.650, 1.950]");
        CHECK(doc.RangeIs(p, std::strlen("[1.650, 1.950]"), SCE_ATEL_LIMIT));
    }
    CheckFullyStyled(doc);
    lx->Release();
}

// FAIL 行：测量值与限值整体升级为失败色，让坏记录成块出现
static void TestAteLogFailLine() {
    const char* kLine =
        "[2026-09-17 08:12:33] SITE1 VDD_1P8 1.612 FAIL [1.650, 1.950]";
    MockDoc doc(std::string(kLine) + "\n");
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
    LexAll(lx, doc);
    const std::string& t = doc.Text();

    CheckTokenStyle(doc, "FAIL", SCE_ATEL_FAIL);
    {
        const std::size_t p = FindIn(t, "1.612");
        CHECK(doc.RangeIs(p, 5, SCE_ATEL_FAIL));        // 数值升级为红
    }
    {
        const std::size_t p = FindIn(t, "[1.650, 1.950]");
        CHECK(doc.RangeIs(p, std::strlen("[1.650, 1.950]"), SCE_ATEL_FAIL));
    }
    // SITE 仍然是 SITE 色（定位信息不能被失败色吃掉）
    CheckTokenStyle(doc, "SITE1", SCE_ATEL_SITE);
    // 描述性名字也不升级，只有「测量值 + 限值」成块变红
    CheckTokenStyle(doc, "VDD_1P8", SCE_ATEL_TESTNAME);
    CheckFullyStyled(doc);
    lx->Release();
}

// 统计记录行：BIN/LOT/WAFER 是强信号（触发着色），COUNT 等弱词跟随
static void TestAteLogRecordLine() {
    MockDoc doc("LOT 12345 WAFER 3 BIN 1 COUNT 250\n");
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
    LexAll(lx, doc);

    CheckTokenStyle(doc, "LOT", SCE_ATEL_RECORD);
    CheckTokenStyle(doc, "WAFER", SCE_ATEL_RECORD);
    CheckTokenStyle(doc, "BIN", SCE_ATEL_RECORD);
    // 弱词只在行已被判定为 ATE 之后着色 —— 本行有 LOT/WAFER/BIN，成立
    CheckTokenStyle(doc, "COUNT", SCE_ATEL_RECORD);
    CheckTokenStyle(doc, "12345", SCE_ATEL_NUMBER);
    CheckFullyStyled(doc);
    lx->Release();
}

// WARN/ERROR/RETEST 归为告警色；FAIL 在同行的优先级最高
static void TestAteLogVerdicts() {
    {
        MockDoc doc("SITE1 TEST1 ERROR timeout\n");
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
        LexAll(lx, doc);
        CheckTokenStyle(doc, "ERROR", SCE_ATEL_WARN);
        lx->Release();
    }
    {
        // 汇总行里同时出现 PASS/FAIL → 按 FAIL 处理更安全
        MockDoc doc("SITE1 SUMMARY PASS 10 FAIL 2\n");
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
        LexAll(lx, doc);
        CheckTokenStyle(doc, "PASS", SCE_ATEL_PASS);
        CheckTokenStyle(doc, "FAIL", SCE_ATEL_FAIL);
        // FAIL 优先：整行按失败升级，数值变红
        {
            const std::size_t p = FindIn(doc.Text(), "10 FAIL");
            CHECK(doc.RangeIs(p, 2, SCE_ATEL_FAIL));
        }
        lx->Release();
    }
}

// 行尾注释在 ATE 行里也要淡化，且不得影响前面的风格
static void TestAteLogTrailingComment() {
    MockDoc doc("SITE1 VDD 1.8 PASS // 判决来源\n");
    Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
    LexAll(lx, doc);

    CheckTokenStyle(doc, "SITE1", SCE_ATEL_SITE);
    CheckTokenStyle(doc, "PASS", SCE_ATEL_PASS);
    {
        const std::size_t p = FindIn(doc.Text(), "// 判决来源");
        CHECK(doc.RangeIs(p, std::strlen("// 判决来源"), SCE_ATEL_COMMENT));
    }
    CheckFullyStyled(doc);
    lx->Release();
}

// 保守门的加固：弱判定词与裸 SITE 不得**单独**触发着色。
// 这几行全部来自 sample.log 的真实 dump —— 其中 `08:12:33 heartbeat ok`
// 一度被整行着色，正是「把通用日志染花」的典型案例。
static void TestAteLogGateHardening() {
    const char* kStillGeneric[] = {
        "08:12:33 heartbeat ok",                          // OK 是弱判定词
        "2026-09-17 09:00:00 ERROR db connection lost",    // ERROR 是弱判定词
        "2026-09-17 09:00:01 WARN cache miss",             // WARN 是弱判定词
        "2026-09-17 09:00:02 TIMEOUT waiting for lock",    // TIMEOUT 是弱判定词
        "2026-09-17 09:00:03 request bad",                 // BAD 是弱判定词
        "SITE : TESTNAME : VALUE",                         // 裸 SITE 无编号
        "2026-09-17 site config reloaded",                 // 小写 site 更不算
    };
    for (const char* line : kStillGeneric) {
        MockDoc doc(std::string(line) + "\n");
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
        LexAll(lx, doc);
        ++g_checks;
        if (doc.CountStyle(SCE_ATEL_DEFAULT) != (std::size_t)doc.Length()) {
            ++g_fail;
            printf("FAIL weak-evidence line got colored: %s\n", line);
        }
        lx->Release();
    }

    // 强判定词自己就足够：裸 FAIL 行必须成块变红
    {
        MockDoc doc("2026-09-17 09:00:03 FAIL\n");
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
        LexAll(lx, doc);
        CheckTokenStyle(doc, "FAIL", SCE_ATEL_FAIL);
        // 时间戳只在行被认定为 ATE 之后才着色
        CHECK(doc.RangeIs(0, 19, SCE_ATEL_TIMESTAMP));
        lx->Release();
    }
    // SITE[1] 是合法记号（编号写在括号里，'[' 不是标识符字符）
    {
        MockDoc doc("SITE[1] VDD 1.8 PASS\n");
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
        LexAll(lx, doc);
        CheckTokenStyle(doc, "SITE", SCE_ATEL_SITE);
        CheckTokenStyle(doc, "VDD", SCE_ATEL_TESTNAME);
        CheckTokenStyle(doc, "PASS", SCE_ATEL_PASS);
        CheckFullyStyled(doc);
        lx->Release();
    }
    // 弱判定词在「本行已是 ATE」时照常着色（锦上添花，不是一律无色）
    {
        MockDoc doc("SITE1 VDD 1.8 ERROR\n");
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAteLog);
        LexAll(lx, doc);
        CheckTokenStyle(doc, "ERROR", SCE_ATEL_WARN);
        lx->Release();
    }
}

// 空文档 / 退化输入不得崩，也不得写越界
static void TestDegenerate() {
    const char* kNames[] = { kLexAtePattern, kLexStil, kLexAteLog };
    for (const char* name : kNames) {
        Scintilla::ILexer5* lx = XfsCreateLexer(name);
        MockDoc empty("");
        lx->Lex(0, 0, 0, &empty);              // lengthDoc == 0
        lx->Lex(0, 10, 0, &empty);             // 超过文档长度
        lx->Fold(0, 0, 0, &empty);
        lx->Lex(0, 5, 0, nullptr);             // 空文档指针
        lx->Fold(0, 5, 0, nullptr);
        lx->Release();
    }
    // 无行尾的末行（CRLF 与裸 CR 也要覆盖）
    {
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAtePattern);
        MockDoc crlf("RPT 2\r\n0 1 X\r\n");
        LexAll(lx, crlf);
        CheckTokenStyle(crlf, "RPT", SCE_ATEP_OPCODE);
        CheckTokenStyle(crlf, "0 1 X", SCE_ATEP_VECTOR);
        CheckFullyStyled(crlf);
        lx->Release();
    }
    {
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAtePattern);
        MockDoc cr("RPT 2\r0 1 X");            // 裸 CR、无末行尾
        LexAll(lx, cr);
        CheckTokenStyle(cr, "RPT", SCE_ATEP_OPCODE);
        CheckTokenStyle(cr, "0 1 X", SCE_ATEP_VECTOR);
        CheckFullyStyled(cr);
        lx->Release();
    }
    // 超长单行（无换行）→ GrowToLineEnd 找不到 '\n' 时的防御路径
    {
        Scintilla::ILexer5* lx = XfsCreateLexer(kLexAtePattern);
        MockDoc longLine(std::string(60000, 'A'));
        LexAll(lx, longLine);
        CheckFullyStyled(longLine);
        lx->Release();
    }
}

// ============================================== 诊断模式：按文件 dump 样式
// 用法：test_atelexer --dump <lexer> <file>
// 不入 ctest。排查「这个文件为什么被染成这样」时手动跑，比反复加 printf 快。
// 末尾的直方图直接回答保守门是否生效：默认色占比应当压倒性地高（通用日志）。

static const char* StyleName(int st) {
    switch (st) {
    // ---- .pat（SCE_ATEP_*）----
    case SCE_ATEP_DEFAULT:   return "default";
    case SCE_ATEP_COMMENT:   return "comment";
    case SCE_ATEP_OPCODE:    return "opcode";
    case SCE_ATEP_LABEL:     return "label";
    case SCE_ATEP_PIN:       return "pin";
    case SCE_ATEP_VECTOR:    return "DRIVE";    // 向量 0/1
    case SCE_ATEP_EXPECT:    return "EXPECT";   // 向量 H/L/T
    case SCE_ATEP_MASK:      return "MASK";     // 向量 X/N/Z/U/D
    case SCE_ATEP_HEX:       return "hex";
    case SCE_ATEP_NUMBER:    return "number";
    case SCE_ATEP_STRING:    return "string";
    case SCE_ATEP_TIMING:    return "timing";
    case SCE_ATEP_DIRECTIVE: return "directive";
    case SCE_ATEP_OPERATOR:  return "oper";
    // ---- .stil（SCE_STIL_*）----
    case SCE_STIL_DEFAULT:   return "default";
    case SCE_STIL_COMMENT:   return "comment";
    case SCE_STIL_BLOCK:     return "BLOCK";
    case SCE_STIL_KEYWORD:   return "keyword";
    case SCE_STIL_NAME:      return "name";
    case SCE_STIL_NUMBER:    return "number";
    case SCE_STIL_UNIT:      return "unit";
    case SCE_STIL_EVENT:     return "EVENT";
    case SCE_STIL_OPERATOR:  return "oper";
    case SCE_STIL_STRING:    return "string";
    // ---- .log（SCE_ATEL_*）----
    case SCE_ATEL_DEFAULT:   return "default";
    case SCE_ATEL_COMMENT:   return "comment";
    case SCE_ATEL_TIMESTAMP: return "timestamp";
    case SCE_ATEL_SITE:      return "SITE";
    case SCE_ATEL_TESTNAME:  return "testname";
    case SCE_ATEL_NUMBER:    return "number";
    case SCE_ATEL_LIMIT:     return "limit";
    case SCE_ATEL_PASS:      return "PASS";
    case SCE_ATEL_FAIL:      return "FAIL";
    case SCE_ATEL_WARN:      return "WARN";
    case SCE_ATEL_RECORD:    return "record";
    default:                 return "?";
    }
}

static int DumpFile(const char* lexerName, const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        printf("cannot open: %s\n", path);
        return 2;
    }
    std::string text;
    char buf[65536];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    Scintilla::ILexer5* lx = XfsCreateLexer(lexerName);
    if (!lx) {
        printf("unknown lexer: %s\n", lexerName);
        return 2;
    }

    MockDoc doc(std::move(text));
    lx->Lex(0, doc.Length(), 0, &doc);

    printf("=== %s  [%s]  %lld bytes, %zu lines ===\n",
           path, lexerName, (long long)doc.Length(), doc.LineCount());

    for (Sci_Position line = 0; line < (Sci_Position)doc.LineCount(); ++line) {
        const Sci_Position s = doc.LineStart(line);
        const Sci_Position e = doc.LineEnd(line);
        std::string runs;
        for (Sci_Position i = s; i < e;) {
            const int st = doc.StyleOf((std::size_t)i);
            Sci_Position j = i;
            while (j < e && doc.StyleOf((std::size_t)j) == st) ++j;
            if (!runs.empty()) runs += ' ';
            runs += StyleName(st);
            runs += "(" + std::to_string((long long)(j - i)) + ")";
            i = j;
        }
        if (runs.size() > 170) runs = runs.substr(0, 167) + "...";
        printf("%5d | %s\n", (int)line + 1, runs.c_str());
    }

    // 直方图：默认色占比高 = 保守门生效（.log 尤其要看这一行）
    struct Count { int style; std::size_t n; };
    std::vector<Count> counts;
    for (Sci_Position i = 0; i < doc.Length(); ++i) {
        const int st = doc.StyleOf((std::size_t)i);
        bool found = false;
        for (auto& c : counts)
            if (c.style == st) { ++c.n; found = true; break; }
        if (!found) counts.push_back({ st, 1 });
    }
    std::sort(counts.begin(), counts.end(),
              [](const Count& a, const Count& b) { return a.n > b.n; });
    printf("--- style histogram (bytes) ---\n");
    for (const auto& c : counts) {
        printf("  %-12s %8zu  %5.1f%%\n", StyleName(c.style), c.n,
               doc.Length() > 0 ? 100.0 * (double)c.n / (double)doc.Length() : 0.0);
    }

    lx->Release();
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--dump") == 0)
        return DumpFile(argv[2], argv[3]);

    TestFactory();
    TestStyleNumbering();
    TestFoldLevelEncoding();
    TestKeywordSet();
    TestAtePattern();
    TestAtePatternWordList();
    TestAtePatternFold();
    TestAtePatternPrecedence();
    TestStil();
    TestStilBlockCommentAcrossLines();
    TestStilStateAcrossWindows();
    TestStilFold();
    TestAteLogConservatism();
    TestAteLogPassLine();
    TestAteLogFailLine();
    TestAteLogRecordLine();
    TestAteLogVerdicts();
    TestAteLogTrailingComment();
    TestAteLogGateHardening();
    TestDegenerate();

    printf("test_atelexer: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
