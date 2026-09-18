// test_chromasig.cpp — 批次 73：Chroma 3380 签名提示的位置解析单测。
//                     批次 77：追加语句名补全的候选生成（顺序/范围/位置判定）。
//                     批次 78：追加"接受语句名后要不要补 `(`"的书写形态判定。
//                     批次 79：签名抽取取页窗口/标题前瞻修好后，翻面 RELAY_ON 等绊线。
//
// 【为什么这段逻辑值得单独钉测试】
//   它最容易出的是**静默错**：少算一个逗号，下拉框就会把 A 参数的档位表挂到 B
//   参数上——用户拿到的是一份语法完全合法、数值却不对的代码，不报任何异常。
//   所以这里把嵌套括号、多行调用、字符串里的逗号、越界、取巧的 `-`/`@` 前缀
//   全部覆盖，并且直接断言手册里那两个参数的真实候选值。
//
// 【还顺带守住数据源】
//   kParams/kValues/kStatements 是脚本生成的，这里断言池内下标不越界、候选值
//   不含空格（空格是下拉列表的分隔符，含空格会被 Scintilla 拆成两项），以及
//   索引与槽位的结构关系。

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../src/language/ChromaSignature.h"
#include "../src/language/Chroma3380Complete.h"   // 批次 77：语句补全候选生成
#include "../src/language/XfsLexer.h"             // 三支词法器的名字（kLexChromaPlan 等）

using namespace xfs;
using namespace xfs::chroma3380;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- 小工具 -----------------------------------------------------------------

// 在文本末尾（光标在最后）解析。返回是否命中。
static bool ResolveAtEnd(const std::string& text, SignatureHint& h) {
    return ResolveSignatureHint(text, text.size(), h);
}

// 把某参数槽解码成一串候选值，便于直接断言。
static std::string EnumOf(const ParamDef* p) {
    if (!p || !(p->flags & kParamHasEnum) || p->valCount <= 0) return {};
    std::string out;
    for (int i = 0; i < p->valCount; ++i) {
        if (!out.empty()) out += ' ';
        out += kValues[p->valStart + i];
    }
    return out;
}

// ---- 1) 手册里那两个参数的真实候选值（用户点名要的功能）---------------------

static void RunForceVMldps() {
    // 手册 4.3 Example 的写法；光标停在 v_range 这个实参里
    const std::string src = "FORCE_V_MLDPS(VDDUSB+VPP, 0.0V, ";
    SignatureHint h;
    CHECK(ResolveAtEnd(src, h));
    CHECK(h.stmt && std::strcmp(h.stmt->name, "FORCE_V_MLDPS") == 0);
    CHECK(h.paramIndex == 2);
    CHECK(h.positional);
    CHECK(h.hasEnum);
    CHECK(h.param && std::strcmp(h.param->name, "v_range") == 0);
    CHECK(EnumOf(h.param) == "@6V @12V");        // ← 用户要的下拉框内容
    CHECK(h.argStart == (int)src.size());        // 实参为空，替换起点即光标

    // 第 4 个实参 i_range：7 档，且都必须带 @。
    // 这一条同时钉死生成器的**段级去重**：曾经因为逐个值去重，@25mA/@500mA/@1A
    // 早被别的语句放进池子，valStart/valCount 区间被截断，这里会解出
    // "@5uA @25uA @250uA @2.5mA 5uA 25uA 250uA" —— 语法合法、全错。
    SignatureHint h2;
    CHECK(ResolveAtEnd("FORCE_V_MLDPS(Vcc, 6V, @6V, ", h2));
    CHECK(h2.paramIndex == 3);
    CHECK(h2.param && std::strcmp(h2.param->name, "i_range") == 0);
    CHECK(EnumOf(h2.param) ==
          "@5uA @25uA @250uA @2.5mA @25mA @500mA @1A");

    // 第 5 个实参 I_clamp：同样的档位值，但**不带 @**（手册 Example 写 250uA）
    SignatureHint h3;
    CHECK(ResolveAtEnd("FORCE_V_MLDPS(Vcc, 6V, @6V, @250uA, ", h3));
    CHECK(h3.paramIndex == 4);
    CHECK(h3.param && std::strcmp(h3.param->name, "I_clamp") == 0);
    CHECK(EnumOf(h3.param) == "5uA 25uA 250uA 2.5mA 25mA 500mA 1A");

    // 第 3 个实参 f_volt 是自由数值，没有候选值 → 交回词汇补全
    SignatureHint h4;
    CHECK(ResolveAtEnd("FORCE_V_MLDPS(Vcc, ", h4));
    CHECK(h4.paramIndex == 1);
    CHECK(!h4.hasEnum);

    // 语句名大小写不敏感（现场文件大小写混用很常见）
    SignatureHint h5;
    CHECK(ResolveAtEnd("force_v_mldps(Vcc, 0V, ", h5));
    CHECK(h5.stmt != nullptr && h5.paramIndex == 2 && h5.hasEnum);
}

// ---- 2) 候选值里的 `@`/`-`/`.` 不能被当分隔符 -------------------------------

static void RunArgStart() {
    // 用户已经打了 "-" / "@" / 小数点：替换区间必须**包含**它们。
    // 若把 '-' 当分隔符，替换区间会从 "2.5V" 开始，Tab 选中后得到 "-@6V"——非法。
    // 前缀 "FORCE_V_MLDPS(A, B, " 长 20，所以实参起点恒为 20。
    static const char* kFrag[] = {"-", "-2.5V", "@", "@6", "+1e3"};
    for (const char* frag : kFrag) {
        const std::string src = std::string("FORCE_V_MLDPS(A, B, ") + frag;
        SignatureHint h;
        CHECK(ResolveAtEnd(src, h));
        CHECK(h.paramIndex == 2);
        CHECK(h.argStart == 20);
        CHECK(src.substr((size_t)h.argStart) == frag);   // 前缀恰好是实参片段
    }
}

// ---- 3) 多行调用 / 嵌套括号 / 字符串里的逗号 --------------------------------

static void RunTrickyText() {
    // 手册自己的示例就把调用写成两行：换行**不是**语句边界
    SignatureHint h1;
    CHECK(ResolveAtEnd("FORCE_V_MLDPS(VDDUSB+VPP, 0.0V,\n              @6V, ", h1));
    CHECK(h1.paramIndex == 3 && h1.hasEnum);

    // 嵌套括号：命中的应是最内层那个调用
    SignatureHint h2;
    CHECK(ResolveAtEnd("MEAS_FREQ(pin, ", h2));
    CHECK(h2.paramIndex == 1);

    // 字符串里的逗号不算实参分隔（TDO_PRINTF 的首参是带逗号的格式串）
    SignatureHint h3;
    CHECK(ResolveAtEnd("TDO_PRINTF(\"CSV_FIELD A=1, B=2\", ", h3));
    CHECK(h3.paramIndex == 1);
    CHECK(!h3.hasEnum);

    // 单引号同理
    SignatureHint h4;
    CHECK(ResolveAtEnd("FORCE_V_MLDPS('a,b', 0V, ", h4));
    CHECK(h4.paramIndex == 2 && h4.hasEnum);
}

// ---- 4) 该「不出手」的场合一律返回 false -----------------------------------

static void RunNegative() {
    SignatureHint h;
    // 光标在 0 处 / 文本为空
    CHECK(!ResolveSignatureHint("", 0, h));
    CHECK(!ResolveSignatureHint("FORCE_V_MLDPS(", 0, h));

    // 括号已闭合 → 不在实参里
    CHECK(!ResolveAtEnd("FORCE_V_MLDPS(A, B, @6V, @5uA, 5uA, NORM, ON, 1mS); ", h));

    // 未收录的语句（普通 C 函数调用）不参与
    CHECK(!ResolveAtEnd("printf(", h));
    CHECK(!ResolveAtEnd("(a + b, ", h));          // 括号前不是标识符

    // 语句边界 ; { } 之后不再回溯
    CHECK(!ResolveAtEnd("FORCE_V_MLDPS(A; ", h));

    // 实参个数超出手册签名 → 不出手（宁可不出，也不能挂错参数）
    CHECK(!ResolveAtEnd("FORCE_V_MLDPS(A,B,C,D,E,F,G,H, ", h));

    // 槽序不可信的语句：命中但 positional=false（调用方据此拒绝出手）
    SignatureHint h2;
    CHECK(ResolveAtEnd("SET_DVM_MODE(P1, MLDPS, ", h2));
    CHECK(!h2.positional);
    CHECK(!h2.hasEnum);       // 即使该槽挂着枚举也不得使用
}

// ---- 5) SignatureArgRange：签名里的第 N 个实参 ------------------------------

static void RunArgRange() {
    const StatementDef* st = FindStatement("FORCE_V_MLDPS", 13);
    CHECK(st != nullptr);
    if (!st) return;
    const std::string sig = st->signature;
    static const char* kWant[] = {"MLDPS_name", "f_volt", "v_range", "i_range",
                                  "I_clamp", "m_mode", "ON/OFF", "wait_time"};
    for (int i = 0; i < 8; ++i) {
        int s = 0, e = 0;
        CHECK(SignatureArgRange(sig, i, s, e));
        CHECK(sig.substr(s, e - s) == kWant[i]);
    }
    int s = 0, e = 0;
    CHECK(!SignatureArgRange(sig, 8, s, e));      // 越界
    CHECK(!SignatureArgRange(sig, -1, s, e));

    // 方括号是「尾部可选参数」的排版记号，必须透明：否则整组会被并成一个实参
    const StatementDef* mf = FindStatement("MEAS_FREQ", 9);
    CHECK(mf != nullptr);
    if (mf) {
        const std::string s2 = mf->signature;
        static const char* kWant2[] = {"pin_name", "start_addr", "stop_addr",
                                       "ignore_addr", "freq_range", "divide_count",
                                       "timeout", "TFTM_plus_count"};
        for (int i = 0; i < 8; ++i) {
            int a = 0, b = 0;
            CHECK(SignatureArgRange(s2, i, a, b));
            CHECK(s2.substr(a, b - a) == kWant2[i]);
        }

        int a8 = 0, b8 = 0;
        // ⚠ 最要紧的一条：**签名第 N 个实参必须就是 kParams 第 N 个槽**。
        //   这条一旦错，表现是「下拉框把 A 参数的档位挂到 B 参数上」——语法合法、
        //   数值不对、不报错。而方括号场景正是最容易错开一格的地方：
        //   手册写成 `freq_range [, divide_count, … ]`（`[` 在分隔逗号**之前**），
        //   若把 `[` 当深度，两侧的实参个数就对不上，槽位整体错位。
        //   本批实测：`SignatureArgRange` 先漏了尾部 `]`、又漏了尾部 `[`，
        //   两次都只在这里被抓出来——所以这个断言不是"顺带"，是主要目的。
        CHECK(!SignatureArgRange(s2, 8, a8, b8));               // 越界（只有 8 个实参）
        if (mf->paramCount == 8)
            for (int i = 0; i < 8; ++i)
                CHECK(std::strcmp(kParams[mf->paramStart + i].name, kWant2[i]) == 0);
        else
            CHECK(false);   // paramCount 变了说明生成器侧改过槽数，需重新核对
    }

    // 没有参数表的签名（CRAFT 宏 / 块语句）→ false
    int a = 0, b = 0;
    CHECK(!SignatureArgRange("TEST_PRO {", 0, a, b));
    CHECK(!SignatureArgRange("", 0, a, b));
}

// ---- 6) 展示名 ---------------------------------------------------------------

static void RunDisplayName() {
    SignatureHint h;
    CHECK(ResolveAtEnd("FORCE_V_MLDPS(A, B, ", h));
    CHECK(ParamDisplayName(h) == "v_range");

    // arg0/arg3 是抽取期合成的占位名 → 按「没有名字」处理
    SignatureHint h2;
    CHECK(ResolveAtEnd("TDO_PRINTF(", h2));
    CHECK(h2.param && std::strcmp(h2.param->name, "arg0") == 0);
    CHECK(ParamDisplayName(h2).empty());

    SignatureHint empty;
    CHECK(ParamDisplayName(empty).empty());
}

// ---- 7) 数据源结构完整性（生成器的护栏，在下游再兜一道）---------------------

// ---- 8) 批次 77：语句名补全的候选生成 ---------------------------------------
//
// 【这一段守的是什么】
//   补全列表的两个属性都是"用户一眼就能看出不对、单测不写就没人守"的：
//     ① **顺序**必须是手册次序（章节升序），不是字母序 —— 字母序会把
//        FORCE_I_MLDPS / FORCE_V_MLDPS 这一对拆开，也会把 4.3 的 DPS 族
//        和 4.9 的 PMU 族打乱；工程师翻手册找的就是这个次序。
//     ② **范围**必须是"这个文件类型认识的语句"，即词法器高亮用的同一份词表 ——
//        多给会让用户补出一个不着色的名字，少给则等于没有。
//   顺带钉住两条安全不变量：名字里不含 `?`（typesep，含了会被 ListBox 截断成
//   两列）、前缀门槛 2 字符。

// 用两字符前缀把所有候选扫一遍（任何名字都有两字符前缀），收集去重。
static void CollectAllCandidates(const char* lexer,
                                 std::set<std::string>& names) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
    std::vector<StatementCandidate> out;
    for (const char* a = kAlphabet; *a; ++a) {
        for (const char* b = kAlphabet; *b; ++b) {
            const std::string prefix = std::string(1, *a) + *b;
            CollectStatementCandidates(lexer, prefix, out, 1000);
            for (const StatementCandidate& c : out) names.insert(c.name);
        }
    }
}

static void RunStatementCandidates() {
    std::vector<StatementCandidate> out;

    // ① 非 Chroma 词法器一条都不给 —— 这是"另外 36 种语言零改动"的前提
    CHECK(CollectStatementCandidates("cpp", "FO", out, 400) == 0);
    CHECK(CollectStatementCandidates(nullptr, "FO", out, 400) == 0);
    CHECK(CollectStatementCandidates("", "FO", out, 400) == 0);

    // ② 前缀门槛：1 个字符不给（语句名首字母高度重复，1 字符等于给整表）
    CHECK(CollectStatementCandidates(kLexChromaPlan, "F", out, 400) == 0);
    CHECK(CollectStatementCandidates(kLexChromaPlan, "", out, 400) == 0);
    CHECK(kStmtCompleteMinPrefix == 2);

    // ③ .pln 的 FORCE 族：顺序 = 手册章节序（4.3 的 MLDPS 族在最前）
    CHECK(CollectStatementCandidates(kLexChromaPlan, "FORCE_", out, 400) == 14);
    CHECK(out.size() == 14);
    CHECK(out[0].name == "FORCE_I_MLDPS" && out[0].section == "4.3");
    CHECK(out[1].name == "FORCE_V_MLDPS" && out[1].section == "4.3");
    CHECK(out[2].name == "FORCE_I_DPS"   && out[2].section == "4.5");
    CHECK(out[13].name == "FORCE_I_PPMU" && out[13].section == "4.10");
    // 每一条的章节号都必须非空（这 14 条都在手册语句章节里）
    for (const StatementCandidate& c : out) CHECK(!c.section.empty());

    // ④ 前缀过滤是真的按前缀：FORCE_V_ 应只剩 6 条（VOLT/CURRENT 不匹配）
    CHECK(CollectStatementCandidates(kLexChromaPlan, "FORCE_V_", out, 400) == 6);
    for (const StatementCandidate& c : out) CHECK(c.name.rfind("FORCE_V_", 0) == 0);
    CHECK(out[0].name == "FORCE_V_MLDPS");
    CHECK(out[5].name == "FORCE_V_PPMU");

    // ⑤ 已打全的项不进列表（没有可补的部分）——但**更长的**同名前缀项仍要给：
    //    `SET_LEVELN` 打全后，`SET_LEVELN_HV`(5.5) 还得能补出来
    CHECK(CollectStatementCandidates(kLexChromaPlan, "FORCE_V_PMU", out, 400) == 0);
    CHECK(CollectStatementCandidates(kLexChromaPlan, "SET_LEVELN", out, 400) == 1);
    CHECK(out[0].name == "SET_LEVELN_HV" && out[0].section == "5.5");
    // 大小写不敏感：小写前缀同样命中（现场文件大小写混用）
    CHECK(CollectStatementCandidates(kLexChromaPlan, "force_v_", out, 400) == 6);

    // ⑥ 上限生效
    CHECK(CollectStatementCandidates(kLexChromaPlan, "SE", out, 400) >= 10);
    CHECK(CollectStatementCandidates(kLexChromaPlan, "SE", out, 3) == 3);
    CHECK(CollectStatementCandidates(kLexChromaPlan, "SE", out, 0) == 0);

    // ⑦ .pat：模块语句带章节、微指令没有章节（微指令不在手册的语句章节里）
    CHECK(CollectStatementCandidates(kLexAtePattern, "SPM_", out, 400) == 1);
    CHECK(out[0].name == "SPM_PATTERN" && out[0].section == "3.4");
    CHECK(CollectStatementCandidates(kLexAtePattern, "IM", out, 400) == 1);
    CHECK(out[0].name == "IMATCH" && out[0].section.empty());
    // .pat 里的 SET_DEC_FILE（手册 4.2，是 .pat 的第一条语句）也要能补出来
    CHECK(CollectStatementCandidates(kLexAtePattern, "SET_D", out, 400) == 1);
    CHECK(out[0].name == "SET_DEC_FILE" && out[0].section == "4.2");
    // .pat 不该给出 .pln 的语句（两个文件类型的词表不同）
    CHECK(CollectStatementCandidates(kLexAtePattern, "FORCE_", out, 400) == 0);

    // ⑧ .dec：块语句（词表内、手册未收录 → 章节为空）
    CHECK(CollectStatementCandidates(kLexChromaDec, "PIN_", out, 400) == 2);
    CHECK(out[0].name == "PIN_LIST" && out[0].section.empty());
    CHECK(out[1].name == "PIN_GROUP");

    // ⑨ 安全不变量：三个词法器的**全部**候选名都不含 `?`（typesep）与空格
    //    （空格是列表分隔符，`?` 会被 ListBox 当成"附加列"的起点）。
    //    顺带把每个文件类型的候选**规模**钉死 —— 它就是"范围"这个属性本身：
    //    .pln 282 条（语句 258 + 流程块新词 3 + C 库 24－与语句重叠）
    //    .pat  30 条（模块语句 5 + 微指令 25）
    //    .dec   8 条（块语句 7 + 内建对象名 1）
    //    这些数变了就说明词表被改动过，必须回头确认是不是有意的。
    struct LexScale { const char* lexer; int want; };
    static const LexScale kScales[] = {
        { kLexChromaPlan, 282 },
        { kLexAtePattern,  30 },
        { kLexChromaDec,    8 },
    };
    for (const LexScale& s : kScales) {
        std::set<std::string> all;
        CollectAllCandidates(s.lexer, all);
        CHECK((int)all.size() == s.want);
        for (const std::string& n : all) {
            CHECK(n.find('?') == std::string::npos);
            CHECK(n.find(' ') == std::string::npos);
        }
    }
}

// 语句起始位置判定：补全的触发条件，判错就会在实参中间弹语句名。
// 传入的是**词首之前的文本**（等价于把词首当作末尾）——与 Editor 侧的调用口径
// 一致：那里取的是 "词首往回的 512 字节窗口"。
static void RunStatementStart() {
    auto at = [](const char* before) {
        const std::string s(before);
        return IsStatementStart(s, s.size());
    };
    // 规则 1：行首（含缩进行）
    CHECK(at(""));
    CHECK(at("  "));
    CHECK(at("\t\t"));
    CHECK(at("TEST_PRO {\n"));
    CHECK(at("SET_DEC_FILE xx;\r\n"));
    // 块注释之后另起一行仍是行首（此时往回跳空白会落在 `*/` 的 `/` 上，
    // 规则 2 抓不到 —— 这正是规则 1 必须存在的原因）
    CHECK(at("/* note */\n  "));
    // 规则 2：同一条语句序列上（前一个非空白字符是 `;` `{` `}`）
    CHECK(at("A(); "));
    CHECK(at("TEST_PRO { "));
    CHECK(at("} "));
    // 不该触发的：实参里、括号里、表达式里、三目里、以及紧跟词尾
    CHECK(!at("FORCE_V_MLDPS(Vcc, "));
    CHECK(!at("FORCE_V_MLDPS("));
    CHECK(!at("int a = b + "));
    CHECK(!at("x = c ? 1 : "));
    CHECK(!at("TEST_PRO"));
    CHECK(!at("*1010 X0,"));
    // 越界 pos 按末尾处理（不崩，且与 pos=len 等价）
    CHECK(IsStatementStart("abc", 99) == IsStatementStart("abc", 3));
}

// 普通词汇补全的兜底词表：Chroma 三支在 LanguageMap 里一个关键词都没有，
// 全靠这条把词法器的词表接过去（否则新开的 .pln 里打 FORC 什么都不弹）。
static void RunExtraWords() {
    std::vector<const char*> w;
    auto has = [&w](const char* word) {
        for (const char* s : w) if (std::strcmp(s, word) == 0) return true;
        return false;
    };
    // 非 Chroma：空 —— 既有语言的行为一字不变
    CollectExtraWords("cpp", w);
    CHECK(w.empty());
    CollectExtraWords(nullptr, w);
    CHECK(w.empty());

    CollectExtraWords(kLexChromaPlan, w);
    CHECK(has("FORCE_V_MLDPS"));        // 语句名（语句补全与词汇兜底共用同一份词表）
    CHECK(has("if"));                  // C 控制关键字（小写，靠大小写不敏感匹配）
    CHECK(has("VOLTAGE"));             // 手册语义类型
    CHECK(has("IN"));                  // pin_type
    CHECK(has("TEST_LOT_ID"));         // CRAFT 宏
    CHECK(has("CRAFT_c_set_leveln_w")); // C 语言库函数
    CHECK(has("BEFORE_TEST"));         // 流程块（词表有、手册语句章节未收录）
    CHECK(w.size() == 349);            // 282 语句名 + 67 其它词（去重后）

    CollectExtraWords(kLexAtePattern, w);
    CHECK(has("NOP"));                 // 微指令
    CHECK(has("SPM_PATTERN"));
    CHECK(!has("FORCE_V_MLDPS"));      // .pat 不该拿到 .pln 的语句
    CHECK(w.size() == 30);

    CollectExtraWords(kLexChromaDec, w);
    CHECK(has("PIN_LIST"));
    CHECK(has("DEC_MODE"));
    CHECK(has("APAS"));                // DEC_MODE 的取值
    CHECK(!has("FORCE_V_MLDPS"));
    CHECK(w.size() == 24);             // 8 块语句名 + DEC_MODE 取值 2 + pin_type 14（去重后）

    // 去重：词表之间有重叠（kPlanBlockWords 大部分与 kPlanStmtWords 重复）
    CollectExtraWords(kLexChromaPlan, w);
    std::set<std::string> uniq(w.begin(), w.end());
    CHECK(uniq.size() == w.size());
}

// ---- 8) 批次 78：接受语句名之后要不要自动补 `(` -------------------------------
//
//   判据是**手册签名的书写形态**（`NAME(` = 调用形态 / `NAME {` = 块头 /
//   空签名 = 手册未载明），不另设白名单。这里既钉具体名字，也在**整张表**上
//   扫一遍属性——因为一旦生成器把某条的签名改坏（比如把 `{` 形的签名换成调用形），
//   只钉几个名字是抓不到的，而那个错的后果是用户被补上一个多余的左括号、
//   或者该补的地方反而没补。
static void RunWantsParen() {
    // 非语句 / 未收录 / 空输入：一律不动文档
    CHECK(!WantsParenAfterName(nullptr, 0));
    CHECK(!WantsParenAfterName("", 0));
    CHECK(!WantsParenAfterName("no_such_stmt_xyz", 16));
    CHECK(!WantsParenAfterName("FORCE_V_MLDPS", 4));   // 只给前 4 个字符("FORC")

    // 调用形态 → 补
    CHECK(WantsParenAfterName("FORCE_V_MLDPS", 13));
    CHECK(WantsParenAfterName("force_v_mldps", 13));    // FindStatement 大小写不敏感
    CHECK(WantsParenAfterName("WAIT", 4));
    CHECK(WantsParenAfterName("TEST_NO", 7));
    // `START_UP( ) {` 同时含 `(` 和 `{`：真实 .pln 写作 `START_UP();`，
    // 所以它算调用形态。只看"有没有 `(`"这一条就够，不必再判 `{`。
    CHECK(WantsParenAfterName("START_UP", 8));

    // 块头（签名 `NAME {`，后面是缩进块体，没有参数表）→ 不补
    CHECK(!WantsParenAfterName("TEST_PRO", 8));
    CHECK(!WantsParenAfterName("HW_BIN_DEF", 10));
    CHECK(!WantsParenAfterName("CRAFT_STATEMENT", 15));
    // 手册未载明签名 → 不猜。`SET_DEC_FILE` 尤其不能补：它的书写形式
    // 既不带括号也不带分号（`SET_DEC_FILE "ap.dec"`），补上就是语法错的代码。
    CHECK(!WantsParenAfterName("SET_DEC_FILE", 12));
    CHECK(!WantsParenAfterName("RELEASE", 7));

    // ---- 批次 79 取回的三条：手册把它们 4.12/4.8 的 Format 块分页推到了下一页，
    //      老取页窗口 min(下一页-1, pg+14) 收成单页，签名整块丢。修好后必须补 `(`。
    //      实测分量不轻：真实 .pln 里 RELAY_ON 出现 31 次、RELAY_OFF 22 次，
    //      是第 2/4 高频语句。MEAS_CURRENT 的签名最长（5 组可选）。
    CHECK(WantsParenAfterName("RELAY_ON", 8));
    CHECK(WantsParenAfterName("RELAY_OFF", 9));
    CHECK(WantsParenAfterName("MEAS_CURRENT", 12));

    // ---- 同一批修好的**反向**绊线：这四条现在必须**没有**签名 -----------------
    //      它们的"签名"原先是从手册的 Example 段抓来的，带着示例里的具体值
    //      （`"192.168.1.2"`、`RF1`、`100mS`），拿去当签名提示就是教用户写错。
    //      同一批还修了 `format_block` 的结束前瞻：它原来只认 3 级编号
    //      （`\d+\.\d+\.\d+`），而手册的标题常是 4 级（`5.6.8.2 Example`），
    //      拦不住就一路吞进示例段。这个缺陷原先被"单页窗口"掩盖着——
    //      一放窗口就现形，所以这两处必须一起修。
    CHECK(!WantsParenAfterName("RF_Initialize", 13));
    CHECK(!WantsParenAfterName("RF_System_Temperature", 20));
    CHECK(!WantsParenAfterName("RF_Load_Modulation_File", 23));
    CHECK(!WantsParenAfterName("CRAFT_c_meter_GPIB_Meas_volt", 29));

    // ---- SOCKET_INC：真 Format 是块形态 `SOCKET_INC [( FRZ_ON | FRZ_OFF
    //      [ , FRZ_SOCKET_DISABLE ] )] {`（批次 79 给块形态补上「可选方括号组」后
    //      才抓得到）。真实写法 `SOCKET_INC(FRZ_ON) {`，所以要补 `(`；
    //      原先落库的是示例里的 `SOCKET_INC(FRZ_ON) ;`，虽然也补 `(`，
    //      但把 FRZ_ON 说成了唯一写法，还把分号当成了语句结尾。
    CHECK(WantsParenAfterName("SOCKET_INC", 10));

    // ---- 全表属性扫描：判据 == 「签名里有 `(`」，一条不多一条不少 -----------
    int yes = 0, brace = 0, empty = 0;
    for (int i = 0; i < kStatementCount; ++i) {
        const StatementDef& s = kStatements[i];
        const bool hasParen = s.signature && *s.signature &&
                              std::strchr(s.signature, '(') != nullptr;
        CHECK(WantsParenAfterName(s.name, std::strlen(s.name)) == hasParen);
        if (hasParen) ++yes;
        if (s.signature && std::strchr(s.signature, '{')) ++brace;
        if (!s.signature || !*s.signature) ++empty;
    }
    // 三个数字一起钉：309 条恰好被三类分完（调用形态 / 块头 / 空签名），
    // 没有任何一条落在三类之外——这正是"只看签名"能把话说死的前提。
    CHECK(kStatementCount == 309);
    // ⚠️ 批次 79 之后 yes / empty **恰好没变**（进 3 条含 `(` 的、出 3 条含 `(` 的，
    //    SOCKET_INC 新旧签名都含 `(`）——计数抓不到这次改动，真正把关的是上面
    //    那串**点名**断言。别因为"数字没动"就以为这张表没被改过。
    CHECK(yes == 249);
    CHECK(empty == 51);
    CHECK(brace == 12);                 // 批次 79：+SOCKET_INC（块形态 `... {`）
    CHECK(yes + empty == 300);          // 余下 9 条的签名非空且无 `(`（都是块头写法）
}

static void RunDataIntegrity() {
    CHECK(kStatementCount > 300);
    CHECK(kParamCount > 1000);
    CHECK(kValueCount > 100);

    // 语句 → 参数区间必须在池内
    for (int i = 0; i < kStatementCount; ++i) {
        const StatementDef& s = kStatements[i];
        CHECK(s.name && *s.name);
        CHECK(s.paramStart >= 0 && s.paramCount >= 0);
        CHECK(s.paramStart + s.paramCount <= kParamCount);
    }

    // 参数 → 候选值区间必须在池内；候选值不能含空格（空格是下拉框的分隔符，
    // 含空格会被 Scintilla 拆成两项，用户看到的会是半个词）
    int enumSlots = 0;
    for (int i = 0; i < kParamCount; ++i) {
        const ParamDef& p = kParams[i];
        CHECK(p.name && *p.name);
        CHECK(p.valStart >= 0 && p.valCount >= 0);
        CHECK(p.valStart + p.valCount <= kValueCount);
        const bool marked = (p.flags & kParamHasEnum) != 0;
        CHECK(marked == (p.valCount > 0));
        if (!marked) continue;
        ++enumSlots;
        for (int k = 0; k < p.valCount; ++k) {
            const char* v = kValues[p.valStart + k];
            CHECK(v && *v);
            CHECK(std::strchr(v, ' ') == nullptr);
        }
    }
    CHECK(enumSlots > 100);

    // 每条语句名可反查（FindStatement 大小写不敏感）
    for (int i = 0; i < kStatementCount; ++i) {
        const char* n = kStatements[i].name;
        const StatementDef* got = FindStatement(n, std::strlen(n));
        CHECK(got != nullptr);
        CHECK(FindStatement("no_such_stmt_xyz", 16) == nullptr);
    }
}

// ---- 7) 2026-09-18 枚举覆盖审计：漏挂的族档位（用户实测 FORCE_V_PMU 抓出）----
//
//   审计教训：手册的 V_range/@-表格常被归因规则串给相邻参数，再被单位过滤器
//   清空，而 fix_known 滤空后条目仍在 → 语句级 override 永远轮不到。修好管线
//   之后，这里把**每一族**的档位表钉死——MLDPS/DPS/UVI/PREF/PMU/PPMU 各族
//   的 i_range/v_range 取值互不相同，串族即错。

static void RunFamilyRanges() {
    struct Case { const char* stmt; const char* param; const char* want; };
    static const Case kCases[] = {
        // MLDPS 族：电压 2 档、电流 7 档
        {"FORCE_V_MLDPS", "v_range", "@6V @12V"},
        {"FORCE_I_MLDPS", "i_range",
         "@5uA @25uA @250uA @2.5mA @25mA @500mA @1A"},
        // DPS 族：电压 4 档(@4V~@16V)、电流 8 档(@1uA~@2A)
        {"FORCE_V_DPS",   "v_range", "@4V @8V @12V @16V"},
        {"FORCE_I_DPS",   "v_range", "@4V @8V @12V @16V"},
        {"FORCE_I_DPS",   "i_range",
         "@1uA @10uA @100uA @1mA @10mA @100mA @1A @2A"},
        // UVI 族：电压 4 档(@2V~@12V)、电流 7 档(@1uA~@1A)
        {"FORCE_V_UVI",   "v_range", "@2V @4V @6V @12V"},
        {"FORCE_I_UVI",   "v_range", "@2V @4V @6V @12V"},
        {"FORCE_I_UVI",   "i_range",
         "@1uA @10uA @100uA @1mA @10mA @100mA @1A"},
        // PREF 族：电压 4 档(@6V~@48V)、电流 7 档(@1uA~@250mA)
        {"FORCE_V_PREF",  "v_range", "@6V @12V @24V @48V"},
        {"FORCE_I_PREF",  "v_range", "@6V @12V @24V @48V"},
        {"FORCE_I_PREF",  "i_range",
         "@1uA @10uA @100uA @1mA @10mA @100mA @250mA"},
        // PMU 族：电压 4 档(@6V~@48V)、电流 7 档(@1uA~@250mA)——本次审计主案
        {"FORCE_V_PMU",   "V_range", "@6V @12V @24V @48V"},
        {"FORCE_I_PMU",   "V_range", "@6V @12V @24V @48V"},
        {"FORCE_I_PMU",   "I_range",
         "@1uA @10uA @100uA @1mA @10mA @100mA @250mA"},
        // PPMU 族：电流 5 档；v_range 手册明写 "no use (spare)"，必须保持为空
        {"FORCE_I_PPMU",  "I_range", "@2uA @20uA @200uA @2mA @32mA"},
        {"FORCE_V_PPMU",  "v_range", ""},
        {"FORCE_I_PPMU",  "v_range", ""},
        // Focus 校正档位（@RNG0 是 WG 独有；WD 只有 RNG0~3）
        {"SET_WG_WORK_MODE", "v_range",
         "@RNG0 @RNG1 @RNG2 @RNG3 @RNG4 @RNG5 @RNG6 @RNG7 @USER"},
        {"SET_WD_WORK_MODE", "v_range", "@RNG0 @RNG1 @RNG2 @RNG3 @USER"},
    };

    for (const Case& c : kCases) {
        const StatementDef* st = FindStatement(c.stmt, (int)std::strlen(c.stmt));
        CHECK(st != nullptr);
        if (!st) continue;
        bool found = false;
        for (int k = 0; k < st->paramCount && !found; ++k) {
            const ParamDef& p = kParams[st->paramStart + k];
            if (std::strcmp(p.name, c.param) != 0) continue;
            found = true;
            CHECK(EnumOf(&p) == c.want);
        }
        CHECK(found);   // 语句里必须真的有这个参数名（改名时要同步这里）
    }
}

int main() {
    printf("== test_chromasig ==\n");
    RunForceVMldps();
    RunArgStart();
    RunTrickyText();
    RunNegative();
    RunArgRange();
    RunDisplayName();
    RunFamilyRanges();
    RunStatementCandidates();   // 批次 77
    RunStatementStart();        // 批次 77
    RunExtraWords();            // 批次 77
    RunWantsParen();            // 批次 78
    RunDataIntegrity();
    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
