// test_balloon_wrap.cpp — 批次 118：悬停气泡折行的真实覆盖。
//
// 【为什么值得单独一个目标】
//   批次 103 的折行算法里有一条**防御分支**：整段没有空格的超长"词"按像素硬切。
//   对当前手册语料它走不到（最长 token 64 字符 < 阈值 ~87），所以当时只能写下
//   「留着防手册改版，但没有实测覆盖 —— 别当已验证」。本项目纪律：没有实测覆盖的
//   防御分支等于**未验证的断言**，而"手册改版"正是它存在的理由。批次 118 把算法
//   从 Editor.cpp（抓着 HDC，没法测）抽成纯函数 BalloonWrap，本文件负责把它钉住。
//
// 【两层判据，缺一不可】
//   A. **重构等价性**：这里留了一份批次 103 算法的冻结副本 LegacyWrap，用同一把
//      量尺对撞（含确定性伪随机语料）。回答的是"搬家有没有搬坏"。
//   B. **从需求推出的判据**：手算期望值 + 三条不变量（行长 / 不丢字符 / 无空行）。
//      回答的是"它对不对"。**只有 A 是不够的**——两份实现可以同向出错
//      （本项目纪律：与第二实现一致 ≠ 对）。
//
// 【为什么用函数指针量尺而不是 mock 框架】
//   折行只通过 MeasureFn 与外界耦合，喂一把等宽量尺就完全确定，不需要任何 mock。

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../src/editor/BalloonWrap.h"

using xfs::balloon::MeasureFn;
using xfs::balloon::WrapByWidth;

// ---- 诚实标签骨架（照抄 test_dmm_marshal.cpp，别手打标签）-----------------------
// 教训（批次 117a/117b 各复发一次）：裸 printf("[n] ...: ok\n") 会在段落已经失败时
// 照样打印 ok，把红说成绿。标签必须由本段新增的失败数派生。
static int g_fail = 0;
static int g_sinceLabel = 0;
#define FAIL() do { ++g_fail; ++g_sinceLabel; } while (0)
#define CHECK(cond) do { if (!(cond)) { FAIL(); \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void SectionEnd(const char* label) {
    if (g_sinceLabel == 0) {
        std::printf("%s: ok\n", label);
    } else {
        std::printf("%s: FAILED (%d new failure(s))\n", label, g_sinceLabel);
    }
    g_sinceLabel = 0;
}

// ---- 小工具 -----------------------------------------------------------------

// 把 ASCII 字面量变成 wstring，省得处处写 L"..." 再转换。
static std::wstring W(const char* ascii) {
    std::wstring o;
    for (const char* p = ascii; *p; ++p) o.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    return o;
}

// 把 wstring 打成一行可读形式（\r\n 显式化），失败时能直接看出差在哪。
static std::string N(const std::wstring& s) {
    std::string o;
    for (wchar_t c : s) {
        if (c == L'\r')      o += "\\r";
        else if (c == L'\n') o += "\\n";
        else if (c == L'\t') o += "\\t";
        else if (c < 128)    o += static_cast<char>(c);
        else                 o += '?';
    }
    return o;
}

static void CheckEq(const std::wstring& got, const std::wstring& want, const char* what) {
    if (got == want) return;
    FAIL();
    std::printf("  %s\n    want=[%s]\n    got =[%s]\n", what, N(want).c_str(), N(got).c_str());
}

static void CheckTrue(bool ok, const char* what) {
    if (ok) return;
    FAIL();
    std::printf("  %s\n", what);
}

// ---- 量尺：等宽（每字符 charPx 像素）-----------------------------------------
// 等宽的意义是期望值可以手算：maxPx=100、charPx=10 ⇒ 一行正好 10 个字符。
struct Mono { int charPx; };

static int MonoMeasure(void* ctx, const wchar_t* s, std::size_t n) {
    (void)s;
    return static_cast<int>(n) * static_cast<Mono*>(ctx)->charPx;
}

// ---- 判据 B：三条不变量 -------------------------------------------------------

// 不变量 1 + 3：每行像素宽 <= maxPx（**除非该行只有一个字符**——单个字形本身就
// 宽于 maxPx 时无解），且结果里没有空行。
static bool LinesFit(const std::wstring& out, int maxPx, MeasureFn m, void* ctx) {
    if (out.empty()) return true;                 // 空输入 ⇒ 空输出，没有"行"
    std::size_t start = 0;
    for (;;) {
        std::size_t end = out.find(L"\r\n", start);
        std::wstring line = out.substr(start, (end == std::wstring::npos) ? std::wstring::npos : end - start);
        if (line.empty()) return false;           // 空行 = 行尾多了一个 \r\n
        if (m(ctx, line.data(), line.size()) > maxPx && line.size() > 1) return false;
        if (end == std::wstring::npos) break;
        start = end + 2;
    }
    return true;
}

// 不变量 2：不丢字符、不重排。硬切会在**词内部**断行，所以判据必须是
// "去掉所有空格与换行后逐字符相等"，不能是"按空格拼回"（那样会凭空多出空格）。
static std::wstring Squash(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) if (c != L' ' && c != L'\r' && c != L'\n') o.push_back(c);
    return o;
}

// ---- 判据 A：批次 103 算法的冻结副本（只用于对撞，不参与产品）-------------------
// 与 src/editor/BalloonWrap.cpp 的差别只有"宽度从哪来"：这里也是喂 measure。
// 故意保留旧写法（对子串做 std::wstring 再量宽），以便真正独立于新实现。
static std::wstring LegacyWrap(const std::wstring& text, int maxPx, MeasureFn measure, void* ctx) {
    if (maxPx <= 0) return text;
    auto width = [&](const std::wstring& s) -> int { return measure(ctx, s.data(), s.size()); };

    std::wstring out, line;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i;
        while (j < text.size() && text[j] != L' ') ++j;
        std::wstring word = text.substr(i, j - i);
        i = j;
        while (i < text.size() && text[i] == L' ') ++i;

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
    return out;
}

// ---- 语料：确定性伪随机（不用 <random>，种子固定 ⇒ 失败可复现）------------------
static unsigned g_seed = 0x9E3779B9u;
static unsigned NextRand() {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) & 0x7fffu;
}

// =============================================================================

static void TestWordWrap() {
    Mono m{10};
    // "aaa bbb ccc ddd" 每词 30px，maxPx=100：前两词 70px 放得下，第三个来就超。
    CheckEq(WrapByWidth(W("aaa bbb ccc ddd"), 100, MonoMeasure, &m),
            W("aaa bbb\r\nccc ddd"), "[1] 词边界折行");
    // 每个词都装不下第二词：一行一个。
    CheckEq(WrapByWidth(W("aaa bbb ccc"), 30, MonoMeasure, &m),
            W("aaa\r\nbbb\r\nccc"), "[1] 每行一词");
    // 单行放得下 ⇒ 不加任何 \r\n。
    CheckEq(WrapByWidth(W("aaa bbb"), 1000, MonoMeasure, &m),
            W("aaa bbb"), "[1] 放得下就不折");
    SectionEnd("[1] 词边界折行");
}

static void TestExactBoundary() {
    Mono m{10};
    // "aaa bbb" = 70px。判据是**严格大于**：等于 maxPx 不算超，必须留在同一行。
    // 这条同时钉死 `>` 被误写成 `>=` 的那类改动。
    CheckEq(WrapByWidth(W("aaa bbb"), 70, MonoMeasure, &m),
            W("aaa bbb"), "[2] 恰好等于 maxPx 不折行");
    CheckEq(WrapByWidth(W("aaa bbb"), 69, MonoMeasure, &m),
            W("aaa\r\nbbb"), "[2] 超 1 像素就折行");
    SectionEnd("[2] 精确边界（严格大于）");
}

static void TestLongWordHardCut() {
    Mono m{10};
    // ★ 这就是批次 103 里"对当前语料是死代码"的那条分支。25 个 'A' = 250px，
    //   maxPx=100 ⇒ 每 10 个字符硬切一刀，最后剩 5 个字符收尾。
    const std::wstring a25 = W("AAAAAAAAAAAAAAAAAAAAAAAAA");
    CheckEq(WrapByWidth(a25, 100, MonoMeasure, &m),
            W("AAAAAAAAAA\r\nAAAAAAAAAA\r\nAAAAA"), "[3] 超长词硬切");

    // 硬切后剩下的残段要能继续和后面的词装同一行（不是"切完就换行"）。
    CheckEq(WrapByWidth(W("AAAAAAAAAAAAAAAAAAAAAAAAA tail"), 100, MonoMeasure, &m),
            W("AAAAAAAAAA\r\nAAAAAAAAAA\r\nAAAAA tail"), "[3] 硬切残段与后续词同装");

    // 硬切前若已有半行，半行必须先冲出去（不能与硬切块混在一行）。
    CheckEq(WrapByWidth(W("ab AAAAAAAAAAAAAAAAAAAAAAAAA"), 100, MonoMeasure, &m),
            W("ab\r\nAAAAAAAAAA\r\nAAAAAAAAAA\r\nAAAAA"), "[3] 硬切前先冲掉半行");

    // 结果里不能有空行、且不丢字符。
    const std::wstring out = WrapByWidth(a25, 100, MonoMeasure, &m);
    CheckTrue(LinesFit(out, 100, MonoMeasure, &m), "[3] 硬切结果无空行且行长合规");
    CheckTrue(Squash(out) == Squash(a25), "[3] 硬切不丢字符");
    SectionEnd("[3] 超长词硬切（原「死代码」分支）");
}

static void TestDegenerate() {
    Mono m{10};
    // 单个字形本身就宽于 maxPx ⇒ 无解，只能单独成行（这是**已知边界**，不是缺陷）。
    CheckEq(WrapByWidth(W("AB"), 5, MonoMeasure, &m), W("A\r\nB"), "[4] 单字符超宽：逐字成行");
    CheckEq(WrapByWidth(W("A"), 5, MonoMeasure, &m), W("A"), "[4] 单字符超宽：就它一个");
    CheckTrue(LinesFit(WrapByWidth(W("AB"), 5, MonoMeasure, &m), 5, MonoMeasure, &m),
              "[4] 逐字成行时，只有「单字符行」才允许超宽");

    // maxPx <= 0 / 量尺为空 ⇒ 原样返回（不折、不丢）。
    CheckEq(WrapByWidth(W("aaa bbb"), 0, MonoMeasure, &m), W("aaa bbb"), "[4] maxPx==0 原样");
    CheckEq(WrapByWidth(W("aaa bbb"), -3, MonoMeasure, &m), W("aaa bbb"), "[4] maxPx<0 原样");
    CheckEq(WrapByWidth(W("aaa bbb"), 100, nullptr, nullptr), W("aaa bbb"), "[4] 无量尺原样");
    SectionEnd("[4] 无解边界与退化输入");
}

static void TestSpacePolicy() {
    Mono m{10};
    // 口径：吃掉词间空格（连续多个也吃成一个），前导/尾随空格丢弃。
    CheckEq(WrapByWidth(W("aaa   bbb"), 100, MonoMeasure, &m), W("aaa bbb"), "[5] 连续空格折成一个");
    CheckEq(WrapByWidth(W("   aaa"), 100, MonoMeasure, &m), W("aaa"), "[5] 前导空格丢弃");
    CheckEq(WrapByWidth(W("aaa   "), 100, MonoMeasure, &m), W("aaa"), "[5] 尾随空格丢弃");
    CheckEq(WrapByWidth(W(""), 100, MonoMeasure, &m), W(""), "[5] 空文本");
    CheckEq(WrapByWidth(W("     "), 100, MonoMeasure, &m), W(""), "[5] 全空格");
    // 只有半角空格是分隔符：制表符属于词的一部分，**不会**成为断点。
    // 判据不写"结果等于原文"（窄到放不下时它会按像素硬切，那是另一回事），
    // 而写"制表符被当成了词内字符"的可观测后果：放得下时一行不动；
    // 放不下时硬切的结果里制表符仍在（没被当分隔符吃掉），且不丢字符。
    CheckEq(WrapByWidth(W("aaa\tbbb"), 100, MonoMeasure, &m), W("aaa\tbbb"), "[5] 放得下：制表符不拆词");
    const std::wstring tabCut = WrapByWidth(W("aaa\tbbb"), 69, MonoMeasure, &m);
    CheckTrue(tabCut.find(L"\t") != std::wstring::npos, "[5] 硬切后制表符仍在词里（没被当分隔符）");
    CheckTrue(Squash(tabCut) == Squash(W("aaa\tbbb")), "[5] 硬切含制表符的词不丢字符");
    SectionEnd("[5] 空格口径");
}

static void TestInvariantsOverCorpus() {
    Mono m{10};
    int cases = 0, wrappedCases = 0;
    for (int iter = 0; iter < 600; ++iter) {
        // 拼一段语料：0~6 个词，词长 0~30，词间 1~3 个空格。词长 >10 就会触发硬切。
        std::wstring text;
        const int words = static_cast<int>(NextRand() % 7);
        for (int w = 0; w < words; ++w) {
            if (w) for (unsigned s = 0, sn = 1 + NextRand() % 3; s < sn; ++s) text.push_back(L' ');
            const std::size_t len = NextRand() % 31;
            for (std::size_t c = 0; c < len; ++c) text.push_back(static_cast<wchar_t>(L'a' + NextRand() % 3));
        }
        const int maxPx = 10 + static_cast<int>(NextRand() % 200);   // 10..209

        const std::wstring got = WrapByWidth(text, maxPx, MonoMeasure, &m);
        ++cases;

        if (!LinesFit(got, maxPx, MonoMeasure, &m)) {
            FAIL();
            std::printf("  [6] 不变量破坏 maxPx=%d text=[%s] got=[%s]\n",
                        maxPx, N(text).c_str(), N(got).c_str());
        }
        if (Squash(got) != Squash(text)) {
            FAIL();
            std::printf("  [6] 丢字符 maxPx=%d text=[%s] got=[%s]\n",
                        maxPx, N(text).c_str(), N(got).c_str());
        }
        if (got.find(L"\r\n") != std::wstring::npos) ++wrappedCases;   // 至少"折过行"
    }
    // 语料自检：随机语料必须真的覆盖到折行（否则这 600 例是空断言）。
    CheckTrue(cases == 600, "[6] 语料条数");
    CheckTrue(wrappedCases > 100, "[6] 随机语料确实大量触发折行（否则判据是空的）");
    SectionEnd("[6] 不变量（600 例确定性伪随机）");
}

static void TestLegacyEquivalence() {
    Mono m{10};
    // 判据 A：与批次 103 算法的冻结副本逐字节相同（重构等价性）。
    // 先在手工语料上对撞……
    const std::vector<std::pair<std::wstring, int>> hand = {
        {W(""), 100}, {W("   "), 100}, {W("aaa"), 100}, {W("aaa bbb"), 70},
        {W("aaa bbb"), 69}, {W("aaa   bbb"), 100}, {W("   aaa   "), 100},
        {W("aaaaaaaaaaaaaaaaaaaaaaaaa"), 100}, {W("ab aaaaaaaaaaaaaaaaaaaaaaaaa"), 100},
        {W("aaaaaaaaaaaaaaaaaaaaaaaaa tail"), 100}, {W("AB"), 5}, {W("A"), 5},
        {W("aaa\tbbb"), 30}, {W("aaa bbb ccc ddd"), 100},
    };
    for (const auto& c : hand) {
        CheckEq(WrapByWidth(c.first, c.second, MonoMeasure, &m),
                LegacyWrap(c.first, c.second, MonoMeasure, &m),
                "[7] 手工语料：新旧一致");
    }

    // ……再在确定性伪随机语料上对撞（词更长，专门压硬切路径）。
    int mismatch = 0;
    for (int iter = 0; iter < 800; ++iter) {
        std::wstring text;
        const int words = static_cast<int>(NextRand() % 6);
        for (int w = 0; w < words; ++w) {
            if (w) for (unsigned s = 0, sn = 1 + NextRand() % 3; s < sn; ++s) text.push_back(L' ');
            const std::size_t len = NextRand() % 45;      // 45 > 10 ⇒ 大量硬切
            for (std::size_t c = 0; c < len; ++c) text.push_back(static_cast<wchar_t>(L'A' + NextRand() % 26));
        }
        const int maxPx = 10 + static_cast<int>(NextRand() % 120);
        if (WrapByWidth(text, maxPx, MonoMeasure, &m) != LegacyWrap(text, maxPx, MonoMeasure, &m)) {
            if (mismatch == 0) {
                FAIL();
                std::printf("  [7] 新旧不一致 maxPx=%d text=[%s]\n", maxPx, N(text).c_str());
                std::printf("      new=[%s]\n", N(WrapByWidth(text, maxPx, MonoMeasure, &m)).c_str());
                std::printf("      old=[%s]\n", N(LegacyWrap(text, maxPx, MonoMeasure, &m)).c_str());
            }
            ++mismatch;
        }
    }
    CheckTrue(mismatch == 0, "[7] 800 例伪随机：新旧逐字节一致");
    SectionEnd("[7] 与批次 103 冻结副本对撞（重构等价性）");
}

static void TestRealisticThreshold() {
    // 把 backlog 的那句话变成**可执行事实**：按真实口径（560px 气泡宽、等宽近似 6px/字符
    // ⇒ 每行约 93 字符），当前手册最长的 token（64 字符）**走不到**硬切分支；
    // 而 200 字符的 token 一定会走到。
    Mono m{6};
    const int maxPx = 560;

    std::wstring tok64(64, L'a');
    CheckEq(WrapByWidth(tok64, maxPx, MonoMeasure, &m), tok64,
            "[8] 64 字符 token（当前手册最长）不触发硬切");
    CheckTrue(WrapByWidth(tok64, maxPx, MonoMeasure, &m).find(L"\r\n") == std::wstring::npos,
              "[8] 64 字符 token 结果里没有换行");

    std::wstring tok200(200, L'a');
    // 每行 93 字符（93*6=558 <= 560；94*6=564 > 560），200 = 93 + 93 + 14。
    CheckEq(WrapByWidth(tok200, maxPx, MonoMeasure, &m),
            std::wstring(93, L'a') + L"\r\n" + std::wstring(93, L'a') + L"\r\n" + std::wstring(14, L'a'),
            "[8] 200 字符 token 触发硬切，分块 93/93/14");
    CheckTrue(LinesFit(WrapByWidth(tok200, maxPx, MonoMeasure, &m), maxPx, MonoMeasure, &m),
              "[8] 硬切结果行长合规");
    SectionEnd("[8] 真实口径阈值（把 backlog 的话变成断言）");
}

int main() {
    std::printf("== test_balloon_wrap ==\n");
    TestWordWrap();
    TestExactBoundary();
    TestLongWordHardCut();
    TestDegenerate();
    TestSpacePolicy();
    TestInvariantsOverCorpus();
    TestLegacyEquivalence();
    TestRealisticThreshold();

    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
