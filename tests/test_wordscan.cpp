// test_wordscan.cpp — 批次 31 自动补全词汇扫描单测。
// 风格过滤表（常量取自 SciLexer.h 编译期核对）+ 纯文本/风格两路扫描。
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <SciLexer.h>

#include "../src/editor/Wordscan.h"

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool Has(const std::set<std::string>& s, const char* w) {
    return s.find(w) != s.end();
}

// ---- 过滤表：覆盖 LanguageMap 目录里的全部 lexer 名 -------------------------
static void RunFilterTable() {
    const WordStyleFilter* cpp = WordStyleFilterFor("cpp");
    CHECK(cpp && cpp->Any());
    CHECK(cpp->Test(SCE_C_COMMENTLINE));
    CHECK(cpp->Test(SCE_C_COMMENTDOC));
    CHECK(cpp->Test(SCE_C_STRING));
    CHECK(cpp->Test(SCE_C_CHARACTER));
    CHECK(cpp->Test(SCE_C_STRINGRAW));
    CHECK(cpp->Test(SCE_C_REGEX));
    CHECK(!cpp->Test(SCE_C_WORD));
    CHECK(!cpp->Test(SCE_C_IDENTIFIER));
    CHECK(!cpp->Test(SCE_C_PREPROCESSOR));

    const WordStyleFilter* py = WordStyleFilterFor("python");
    CHECK(py && py->Test(SCE_P_COMMENTLINE) && py->Test(SCE_P_COMMENTBLOCK));
    CHECK(py->Test(SCE_P_STRING) && py->Test(SCE_P_TRIPLEDOUBLE));
    CHECK(py->Test(SCE_P_FSTRING));
    CHECK(!py->Test(SCE_P_WORD) && !py->Test(SCE_P_DEFNAME));
    // Ruby 复用 SCE_P_* 系（Lexilla 实现如此）
    const WordStyleFilter* rb = WordStyleFilterFor("ruby");
    CHECK(rb && rb->Test(SCE_P_STRING) && rb->Test(SCE_P_COMMENTLINE));

    // XML 复用 SCE_H_* 系；hypertext 覆盖内嵌脚本风格
    const WordStyleFilter* ht = WordStyleFilterFor("hypertext");
    CHECK(ht && ht->Test(SCE_H_COMMENT) && ht->Test(SCE_H_DOUBLESTRING));
    CHECK(ht->Test(SCE_HJ_DOUBLESTRING) && ht->Test(SCE_HJ_COMMENTLINE));
    CHECK(ht->Test(SCE_HB_STRING) && ht->Test(SCE_HP_TRIPLEDOUBLE));
    CHECK(ht->Test(SCE_HPA_TRIPLEDOUBLE) && ht->Test(SCE_H_SGML_COMMENT));
    CHECK(!ht->Test(SCE_H_TAG) && !ht->Test(SCE_H_ATTRIBUTE));
    const WordStyleFilter* xml = WordStyleFilterFor("xml");
    CHECK(xml && xml->Test(SCE_H_COMMENT) && xml->Test(SCE_H_DOUBLESTRING));

    const WordStyleFilter* js = WordStyleFilterFor("json");
    CHECK(js && js->Test(SCE_JSON_STRING) && js->Test(SCE_JSON_LINECOMMENT));
    CHECK(!js->Test(SCE_JSON_PROPERTYNAME));

    const WordStyleFilter* sql = WordStyleFilterFor("sql");
    CHECK(sql && sql->Test(SCE_SQL_STRING) && sql->Test(SCE_SQL_COMMENTLINE));

    const WordStyleFilter* sh = WordStyleFilterFor("bash");
    CHECK(sh && sh->Test(SCE_SH_STRING) && sh->Test(SCE_SH_COMMENTLINE));
    // heredoc/反引号内容按代码处理（保守）
    CHECK(!sh->Test(SCE_SH_HERE_Q) && !sh->Test(SCE_SH_BACKTICKS));

    const WordStyleFilter* ps = WordStyleFilterFor("powershell");
    CHECK(ps && ps->Test(SCE_POWERSHELL_STRING) &&
          ps->Test(SCE_POWERSHELL_COMMENTSTREAM) &&
          ps->Test(SCE_POWERSHELL_HERE_STRING));

    // batch/makefile/props/yaml 无字符串风格：只过滤注释
    const WordStyleFilter* bat = WordStyleFilterFor("batch");
    CHECK(bat && bat->Test(SCE_BAT_COMMENT) && !bat->Test(SCE_BAT_IDENTIFIER));
    const WordStyleFilter* mk = WordStyleFilterFor("makefile");
    CHECK(mk && mk->Test(SCE_MAKE_COMMENT));
    const WordStyleFilter* props = WordStyleFilterFor("props");
    CHECK(props && props->Test(SCE_PROPS_COMMENT));
    const WordStyleFilter* yml = WordStyleFilterFor("yaml");
    CHECK(yml && yml->Test(SCE_YAML_COMMENT) && !yml->Test(SCE_YAML_TEXT));

    const WordStyleFilter* rust = WordStyleFilterFor("rust");
    CHECK(rust && rust->Test(SCE_RUST_STRING) &&
          rust->Test(SCE_RUST_COMMENTLINE) && rust->Test(SCE_RUST_CSTRING));
    const WordStyleFilter* lua = WordStyleFilterFor("lua");
    CHECK(lua && lua->Test(SCE_LUA_LITERALSTRING));
    const WordStyleFilter* toml = WordStyleFilterFor("toml");
    CHECK(toml && toml->Test(SCE_TOML_STRING_DQ));
    const WordStyleFilter* vhd = WordStyleFilterFor("vhdl");
    CHECK(vhd && vhd->Test(SCE_VHDL_BLOCK_COMMENT));
    const WordStyleFilter* asmf = WordStyleFilterFor("asm");
    CHECK(asmf && asmf->Test(SCE_ASM_COMMENTBLOCK) &&
          asmf->Test(SCE_ASM_STRINGBACKQUOTE));

    // 不过滤的语言 / 未知 / 空
    CHECK(WordStyleFilterFor(nullptr) == nullptr);
    CHECK(WordStyleFilterFor("") == nullptr);
    CHECK(WordStyleFilterFor("markdown") == nullptr);
    CHECK(WordStyleFilterFor("diff") == nullptr);
    CHECK(WordStyleFilterFor("nosuchlexer") == nullptr);
}

// ---- 纯文本扫描 --------------------------------------------------------------
static void RunPlainScan() {
    std::set<std::string> w;
    ScanWordsPlain("int foo; // onlycomment\nfoo bar_1 a bb ccc\n", 43, w);
    CHECK(Has(w, "int") && Has(w, "foo") && Has(w, "bar_1") && Has(w, "ccc"));
    CHECK(Has(w, "onlycomment"));   // 纯文本路径不过滤注释（风格过滤走 Styled）
    CHECK(!Has(w, "a") && !Has(w, "bb"));   // <3 短词不收
    CHECK(!Has(w, "comment"));              // 词边界为准，不拼接
    // 尾部无分隔符的词也要收
    w.clear();
    ScanWordsPlain("x_1 tailword", 12, w);
    CHECK(Has(w, "tailword") && Has(w, "x_1"));
    // 空输入
    w.clear();
    ScanWordsPlain(nullptr, 0, w);
    CHECK(w.empty());
}

// ---- 风格扫描（(char, style) 交错字节对）------------------------------------
namespace {

// 造 cells：text 按给定 style 展开为 (char, style) 对
std::vector<unsigned char> Cells(const char* text, unsigned char style) {
    std::vector<unsigned char> v;
    for (const char* p = text; *p; ++p) {
        v.push_back((unsigned char)*p);
        v.push_back(style);
    }
    return v;
}

} // namespace

static void RunStyledScan() {
    const WordStyleFilter* cpp = WordStyleFilterFor("cpp");
    CHECK(cpp != nullptr);

    // 代码 + 注释 + 字符串拼接：注释/字符串里独有的词不收
    std::vector<unsigned char> cells;
    auto code = Cells("int foo ", 0);
    auto comment = Cells("// cmntonly", SCE_C_COMMENTLINE);
    auto str = Cells("\"stronly\"", SCE_C_STRING);
    auto tail = Cells(" bar\n", 0);
    cells.insert(cells.end(), code.begin(), code.end());
    cells.insert(cells.end(), comment.begin(), comment.end());
    cells.insert(cells.end(), str.begin(), str.end());
    cells.insert(cells.end(), tail.begin(), tail.end());

    std::set<std::string> w;
    ScanWordsStyled(cells.data(), cells.size() / 2, *cpp, w);
    CHECK(Has(w, "int") && Has(w, "foo") && Has(w, "bar"));
    CHECK(!Has(w, "cmntonly"));
    CHECK(!Has(w, "stronly"));

    // 词在注释里出现过、但也在代码里出现 → 收（不牵连）；只出现在注释 → 滤
    cells.clear();
    auto c1 = Cells("foo ", 0);
    auto c1c = Cells("/* foo_in_cmt */ ", SCE_C_COMMENT);
    auto c2 = Cells("baz ", 0);
    auto c2c = Cells("/* wholely */ ", SCE_C_COMMENT);
    auto c3 = Cells("// wholely2", SCE_C_COMMENTLINE);
    cells.insert(cells.end(), c1.begin(), c1.end());
    cells.insert(cells.end(), c1c.begin(), c1c.end());
    cells.insert(cells.end(), c2.begin(), c2.end());
    cells.insert(cells.end(), c2c.begin(), c2c.end());
    cells.insert(cells.end(), c3.begin(), c3.end());
    w.clear();
    ScanWordsStyled(cells.data(), cells.size() / 2, *cpp, w);
    CHECK(Has(w, "foo") && Has(w, "baz"));
    CHECK(!Has(w, "wholely") && !Has(w, "wholely2"));
    CHECK(!Has(w, "foo_in_cmt"));

    // python：三引号字符串里的词过滤
    const WordStyleFilter* py = WordStyleFilterFor("python");
    cells.clear();
    auto p1 = Cells("def name ", 0);
    auto p2 = Cells("\"\"\" docword_only \"\"\"", SCE_P_TRIPLEDOUBLE);
    cells.insert(cells.end(), p1.begin(), p1.end());
    cells.insert(cells.end(), p2.begin(), p2.end());
    w.clear();
    ScanWordsStyled(cells.data(), cells.size() / 2, *py, w);
    CHECK(Has(w, "name"));
    CHECK(!Has(w, "docword_only"));

    // 风格 >= 128（表外）按代码处理
    cells.clear();
    auto hi = Cells("hightone", 128);
    cells.insert(cells.end(), hi.begin(), hi.end());
    w.clear();
    ScanWordsStyled(cells.data(), cells.size() / 2, *cpp, w);
    CHECK(Has(w, "hightone"));
}

int main() {
    RunFilterTable();
    RunPlainScan();
    RunStyledScan();
    if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
    printf("wordscan: all passed\n");
    return 0;
}
