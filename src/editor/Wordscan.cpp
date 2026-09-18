// xfsWinPad - 自动补全词汇扫描实现（批次 31）
// 风格常量取自 vendored SciLexer.h（编译期核对，杜绝手抄数值）。
// 例外：批次 72/73 的自研 ILexer5 族样式号在 XfsLexerStyles.h（64 起编），
// 与 Lexilla 的 SciLexer.h 编号是两套体系。
#include "Wordscan.h"
#include "../language/XfsLexerStyles.h"

#include <map>
#include <initializer_list>

#include <SciLexer.h>

namespace xfs {

namespace {

WordStyleFilter Make(std::initializer_list<unsigned> comment,
                     std::initializer_list<unsigned> str) {
    WordStyleFilter f;
    for (unsigned s : comment) f.bits[s >> 6] |= 1ull << (s & 63);
    for (unsigned s : str)     f.bits[s >> 6] |= 1ull << (s & 63);
    return f;
}

// 常量均出自 third_party/lexilla/include/SciLexer.h；每种语言一行，
// 对照 LanguageMap 目录里的 lexerName。XML 复用 SCE_H_* 系（SCLEX_XML），
// Ruby 复用 SCE_P_* 系（SCLEX_RUBY）——与 Lexilla 实际实现一致。
const std::map<std::string, WordStyleFilter>& StyleFilterMap() {
    static const std::map<std::string, WordStyleFilter> kMap = {
        // C/C++/C#/Java/JS/TS/Go 共用 cpp 词法分析器
        {"cpp", Make({SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC,
                      SCE_C_COMMENTLINEDOC, SCE_C_COMMENTDOCKEYWORD,
                      SCE_C_COMMENTDOCKEYWORDERROR, SCE_C_PREPROCESSORCOMMENT,
                      SCE_C_PREPROCESSORCOMMENTDOC, SCE_C_TASKMARKER},
                     {SCE_C_STRING, SCE_C_CHARACTER, SCE_C_STRINGEOL,
                      SCE_C_VERBATIM, SCE_C_REGEX, SCE_C_STRINGRAW,
                      SCE_C_TRIPLEVERBATIM, SCE_C_HASHQUOTEDSTRING,
                      SCE_C_ESCAPESEQUENCE})},
        {"python", Make({SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK},
                        {SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE,
                         SCE_P_TRIPLEDOUBLE, SCE_P_STRINGEOL, SCE_P_FSTRING,
                         SCE_P_FCHARACTER, SCE_P_FTRIPLE, SCE_P_FTRIPLEDOUBLE})},
        {"ruby", Make({SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK},
                      {SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE,
                       SCE_P_TRIPLEDOUBLE, SCE_P_STRINGEOL})},
        {"hypertext", Make({SCE_H_COMMENT, SCE_H_XCCOMMENT,
                            SCE_H_SGML_COMMENT, SCE_H_SGML_1ST_PARAM_COMMENT,
                            SCE_HJ_COMMENT, SCE_HJ_COMMENTLINE,
                            SCE_HJ_COMMENTDOC, SCE_HJA_COMMENT,
                            SCE_HJA_COMMENTLINE, SCE_HJA_COMMENTDOC,
                            SCE_HB_COMMENTLINE, SCE_HBA_COMMENTLINE,
                            SCE_HP_COMMENTLINE, SCE_HPA_COMMENTLINE},
                           {SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING,
                            SCE_H_SGML_DOUBLESTRING, SCE_H_SGML_SIMPLESTRING,
                            SCE_HJ_DOUBLESTRING, SCE_HJ_SINGLESTRING,
                            SCE_HJ_STRINGEOL, SCE_HJ_TEMPLATELITERAL,
                            SCE_HJA_DOUBLESTRING, SCE_HJA_SINGLESTRING,
                            SCE_HJA_STRINGEOL, SCE_HJA_TEMPLATELITERAL,
                            SCE_HB_STRING, SCE_HB_STRINGEOL, SCE_HBA_STRING,
                            SCE_HBA_STRINGEOL, SCE_HP_STRING, SCE_HP_CHARACTER,
                            SCE_HP_TRIPLE, SCE_HP_TRIPLEDOUBLE,
                            SCE_HPA_STRING, SCE_HPA_CHARACTER, SCE_HPA_TRIPLE,
                            SCE_HPA_TRIPLEDOUBLE})},
        {"xml", Make({SCE_H_COMMENT, SCE_H_XCCOMMENT,
                      SCE_H_SGML_COMMENT, SCE_H_SGML_1ST_PARAM_COMMENT,
                      SCE_HJ_COMMENT, SCE_HJ_COMMENTLINE, SCE_HJ_COMMENTDOC,
                      SCE_HB_COMMENTLINE, SCE_HP_COMMENTLINE},
                     {SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING,
                      SCE_H_SGML_DOUBLESTRING, SCE_H_SGML_SIMPLESTRING,
                      SCE_HJ_DOUBLESTRING, SCE_HJ_SINGLESTRING,
                      SCE_HJ_STRINGEOL, SCE_HB_STRING, SCE_HB_STRINGEOL,
                      SCE_HP_STRING, SCE_HP_CHARACTER, SCE_HP_TRIPLE,
                      SCE_HP_TRIPLEDOUBLE})},
        {"css", Make({SCE_CSS_COMMENT},
                     {SCE_CSS_DOUBLESTRING, SCE_CSS_SINGLESTRING})},
        {"json", Make({SCE_JSON_LINECOMMENT, SCE_JSON_BLOCKCOMMENT},
                      {SCE_JSON_STRING, SCE_JSON_STRINGEOL})},
        {"yaml", Make({SCE_YAML_COMMENT}, {})},
        {"sql", Make({SCE_SQL_COMMENT, SCE_SQL_COMMENTLINE, SCE_SQL_COMMENTDOC,
                      SCE_SQL_COMMENTLINEDOC, SCE_SQL_COMMENTDOCKEYWORD,
                      SCE_SQL_COMMENTDOCKEYWORDERROR, SCE_SQL_SQLPLUS_COMMENT},
                     {SCE_SQL_STRING, SCE_SQL_CHARACTER})},
        {"bash", Make({SCE_SH_COMMENTLINE},
                      {SCE_SH_STRING, SCE_SH_CHARACTER})},
        {"powershell", Make({SCE_POWERSHELL_COMMENT, SCE_POWERSHELL_COMMENTSTREAM,
                             SCE_POWERSHELL_COMMENTDOCKEYWORD},
                            {SCE_POWERSHELL_STRING, SCE_POWERSHELL_CHARACTER,
                             SCE_POWERSHELL_HERE_STRING,
                             SCE_POWERSHELL_HERE_CHARACTER})},
        {"batch", Make({SCE_BAT_COMMENT}, {})},
        {"makefile", Make({SCE_MAKE_COMMENT}, {})},
        {"cmake", Make({SCE_CMAKE_COMMENT},
                       {SCE_CMAKE_STRINGDQ, SCE_CMAKE_STRINGLQ,
                        SCE_CMAKE_STRINGRQ, SCE_CMAKE_STRINGVAR})},
        {"rust", Make({SCE_RUST_COMMENTBLOCK, SCE_RUST_COMMENTLINE,
                       SCE_RUST_COMMENTBLOCKDOC, SCE_RUST_COMMENTLINEDOC},
                      {SCE_RUST_STRING, SCE_RUST_STRINGR, SCE_RUST_CHARACTER,
                       SCE_RUST_BYTECHARACTER, SCE_RUST_BYTESTRING,
                       SCE_RUST_BYTESTRINGR, SCE_RUST_CSTRING,
                       SCE_RUST_CSTRINGR})},
        {"lua", Make({SCE_LUA_COMMENT, SCE_LUA_COMMENTLINE, SCE_LUA_COMMENTDOC},
                     {SCE_LUA_STRING, SCE_LUA_CHARACTER, SCE_LUA_LITERALSTRING,
                      SCE_LUA_STRINGEOL})},
        {"perl", Make({SCE_PL_COMMENTLINE, SCE_PL_POD, SCE_PL_POD_VERB,
                       SCE_PL_DATASECTION},
                      {SCE_PL_STRING, SCE_PL_CHARACTER, SCE_PL_LONGQUOTE,
                       SCE_PL_BACKTICKS, SCE_PL_HERE_Q, SCE_PL_HERE_QQ,
                       SCE_PL_HERE_QX, SCE_PL_STRING_Q, SCE_PL_STRING_QQ,
                       SCE_PL_STRING_QX, SCE_PL_STRING_QR, SCE_PL_STRING_QW,
                       SCE_PL_STRING_VAR, SCE_PL_BACKTICKS_VAR,
                       SCE_PL_HERE_QQ_VAR, SCE_PL_HERE_QX_VAR})},
        {"pascal", Make({SCE_PAS_COMMENT, SCE_PAS_COMMENT2, SCE_PAS_COMMENTLINE},
                        {SCE_PAS_STRING, SCE_PAS_STRINGEOL, SCE_PAS_CHARACTER,
                         SCE_PAS_MULTILINESTRING})},
        {"fortran", Make({SCE_F_COMMENT},
                         {SCE_F_STRING1, SCE_F_STRING2, SCE_F_STRINGEOL})},
        {"verilog", Make({SCE_V_COMMENT, SCE_V_COMMENTLINE,
                          SCE_V_COMMENTLINEBANG, SCE_V_COMMENT_WORD},
                         {SCE_V_STRING, SCE_V_STRINGEOL})},
        {"vhdl", Make({SCE_VHDL_COMMENT, SCE_VHDL_COMMENTLINEBANG,
                       SCE_VHDL_BLOCK_COMMENT},
                      {SCE_VHDL_STRING, SCE_VHDL_STRINGEOL})},
        {"matlab", Make({SCE_MATLAB_COMMENT},
                        {SCE_MATLAB_STRING, SCE_MATLAB_DOUBLEQUOTESTRING})},
        {"props", Make({SCE_PROPS_COMMENT}, {})},
        {"toml", Make({SCE_TOML_COMMENT},
                      {SCE_TOML_STRING_SQ, SCE_TOML_STRING_DQ,
                       SCE_TOML_TRIPLE_STRING_SQ, SCE_TOML_TRIPLE_STRING_DQ,
                       SCE_TOML_STRINGEOL})},
        {"asm", Make({SCE_ASM_COMMENT, SCE_ASM_COMMENTBLOCK,
                      SCE_ASM_COMMENTDIRECTIVE},
                     {SCE_ASM_STRING, SCE_ASM_CHARACTER, SCE_ASM_STRINGEOL,
                      SCE_ASM_STRINGBACKQUOTE})},
        // 批次 72/73：ATE 族。**不能在这里写样式号** —— 这几个词法是自研的
        // ILexer5，样式号在 XfsLexerStyles.h（64 起编），而本表的常量出自
        // third_party/lexilla/include/SciLexer.h，两套编号毫无关系，混用会
        // 屏蔽掉错误的样式位。用 XfsLexerStyles.h 的枚举常量来填。
        {"ate_pattern", Make({SCE_ATEP_COMMENT}, {SCE_ATEP_STRING})},
        {"stil",        Make({SCE_STIL_COMMENT}, {SCE_STIL_STRING})},
        {"ate_log",     Make({SCE_ATEL_COMMENT}, {})},
        {"chroma_dec",  Make({SCE_DEC_COMMENT}, {SCE_DEC_STRING})},
        {"chroma_plan", Make({SCE_PLN_COMMENT}, {SCE_PLN_STRING})},
    };
    return kMap;
}

} // namespace

const WordStyleFilter* WordStyleFilterFor(const char* lexerName) {
    if (!lexerName || !*lexerName) return nullptr;
    const auto& m = StyleFilterMap();
    auto it = m.find(lexerName);
    return it == m.end() ? nullptr : &it->second;
}

bool IsWordByte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

void ScanWordsPlain(const char* text, size_t len, std::set<std::string>& out) {
    std::string run;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (IsWordByte(c)) { run += (char)c; continue; }
        if (run.size() >= 3) out.insert(run);
        run.clear();
    }
    if (run.size() >= 3) out.insert(run);
}

void ScanWordsStyled(const unsigned char* cells, size_t cellCount,
                     const WordStyleFilter& f, std::set<std::string>& out) {
    std::string run;
    bool runInCode = false;   // 本词至少一次出现在非注释非字符串风格
    auto flush = [&]() {
        if (run.size() >= 3 && runInCode) out.insert(run);
        run.clear();
        runInCode = false;
    };
    for (size_t i = 0; i < cellCount; ++i) {
        unsigned char c = cells[i * 2];
        unsigned char style = cells[i * 2 + 1];
        if (IsWordByte(c)) {
            run += (char)c;
            // 风格 >= 128 不在表内（Lexilla 词法器只用到 117），按代码处理
            if (!f.Test(style)) runInCode = true;
            continue;
        }
        flush();
    }
    flush();
}

} // namespace xfs
