#pragma once
// xfsWinPad - Chroma 3380 语句级补全的候选生成（批次 77）
//
// 【职责边界】
//   纯函数：从「词法器名 + 光标前已输入的词前缀 + 词前的一段文本」算出
//   「该不该弹语句名补全、弹哪些、按什么顺序」。不依赖 Scintilla，不弹窗，
//   弹窗与按键记账都在 Editor 侧（Editor::HandleStatementCompletion）。
//
// 【候选集为什么不是 kStatements 全集】
//   kStatements 是手册第 3~5 章的**全部**语句（含 5.3 的 C 库函数原型），共 300+ 条。
//   但「哪些名字会出现在哪类文件里」这件事，批次 73 已经用手写词表
//   （kPlanStmtWords / kPatModuleWords / kDecBlockWords / kPatMicroWords …）
//   定过一次了 —— **词法器高亮用的就是它**。补全若改走 kStatements 全集，
//   就会出现「补全能给出的名字，词法器不认识、不着色」这种两个口径打架的情况。
//   所以候选集 = 词法器自己的词表，手册章节号只是附加的展示列（查得到才有）。
//
// 【顺序为什么必须按手册，不能按字母】
//   字母序会把 FORCE_V_* 排在 FORCE_I_* 前面、把 4.3 的 DPS 族与 4.9 的 PMU 族
//   打散。手册顺序（章节升序）本身就按硬件族分组，工程师翻手册找的就是这个次序；
//   下拉里第一项也因此是确定的（E2E 就靠这一点断言，见 scripts/chroma-e2e.ps1）。

#include <cstddef>
#include <string>
#include <vector>

namespace xfs {
namespace chroma3380 {

// 语句名补全的最小前缀长度。比普通词汇补全（3 字符）小一档：语句名都很长而
// 首字母高度重复（FORCE_* 14 条、SET_* 数十条），打满 3 个字符能筛掉的有限。
// 敢用 2 是因为触发有条件 —— 必须位于语句起始位置（见 IsStatementStart），
// 不是任意位置乱弹。
inline constexpr std::size_t kStmtCompleteMinPrefix = 2;

// 用 std::string 而不是 const char*：候选集里有一部分来自**空格分隔**的关键词
// 表串（`"FORCE_I_DPS FORCE_V_DPS …"`），那里面的词并不是 NUL 结尾的独立串，
// 存指针给出去，"名字"会一路吃到下一个词。这几百条在候选集构建时拷一次、
// 之后常驻缓存，代价可以忽略。
struct StatementCandidate {
    std::string name;     // 语句名
    std::string section;  // 手册章节号；词表里有、手册未收录时为空串
};

// 该名字是否属于 Chroma 3380 语言包（.dec / .pln / .pat 三支）。
bool IsChroma3380LexerName(const char* lexerName);

// 收集以 prefix 开头（大小写不敏感）的语句候选，**按手册顺序**写入 out。
// 顺序 = kStatements 的表序（章节升序）+ 词表里手册未收录的按词表序补在末尾。
// 返回写入条数。下列情形返回 0（调用方据此把这次按键交回普通词汇补全）：
//   * 词法器不属于 Chroma 3380
//   * prefix 短于 kStmtCompleteMinPrefix
//   * 没有任何候选
// 已打全的项（与 prefix 等长且全等）不计入 —— 补全列表里不该出现"没什么可补"
// 的项（与 ShowAutocomplete 的口径一致）。
int CollectStatementCandidates(const char* lexerName, const std::string& prefix,
                               std::vector<StatementCandidate>& out, int maxCount);

// 该词法器**除语句名之外**的词：C 关键字 / 类型名 / pin_type / CRAFT 宏 /
// C 库函数 / 微指令 / .dec 块名 …，供普通词汇补全兜底。
//
// 【为什么需要它：Chroma 三支在普通词汇补全里一个关键词都没有】
//   LanguageMap 里 Chroma 三支的 keywords[2] 都是 nullptr —— 词表在
//   Chroma3380Db 里、由词法器构造函数直接注入（kWordLists 8 个槽位），
//   走不到 Editor::KeywordSetFor 那条按 LanguageMenuItem 取词的路。
//   于是这些文件里"补全只剩文档里出现过的词"，语言关键词一条都没有：
//   新开的 .pln 里打 `FORC` 什么都不弹。批次 77 把词表接到这条路上。
void CollectExtraWords(const char* lexerName, std::vector<const char*>& out);

// text[0, pos) 上，pos 是否位于「语句起始位置」。text 是 pos 之前的文本，
// 等价于把 pos 当作文本末尾。
//
// 判定（任一成立即可）：
//   1. 行首 —— pos 所在行、pos 之前全是空白（缩进）。.pat 的微指令一行一条、
//      .pln 的语句一行一条，都靠这条命中；块注释后紧跟语句也靠它
//      （此时往回跳空白会落在 `*/` 的 `/` 上，规则 2 抓不到）。
//   2. 同一条语句序列上 —— 往回跳过空白后，前一个非空白字符是 `;` `{` `}`。
//      覆盖 `A(); FORC|`、`TEST_PRO { FORC|` 这类同一行写多条的情形。
//
// 刻意**不**把 `:` 计入（标签 `Label:` 之后接语句）：`:` 在三目运算符
// `a ? b : c` 里也会出现，认它会表达式中间也弹语句名，得不偿失。
bool IsStatementStart(const std::string& text, std::size_t pos);

// 接受这条语句名之后，**要不要自动补一个 `(`**？（批次 78）
//
// 【判定只看手册签名的书写形态，不看别的】
//   手册把每条语句都印成三选一：
//     `NAME(a, b [, c]) ;`  → 调用形态，用户下一步就是左括号
//     `NAME {`              → 块头（TEST_PRO / HW_BIN_DEF / CRAFT_STATEMENT…），
//                             后面是缩进块体，**没有**参数表
//     （签名缺失，如 SET_DEC_FILE / RELEASE / CARRAY_DIM）→ 手册没载明，不猜
//   实测 309 条语句恰好被这三类分完（249 / 11 / 51，无一类落在三者之外），
//   所以「签名里有 `(`」就是「调用形态」的精确判据 —— 不需要再列白名单。
//
// 【为什么这里宁可不补也不乱补】
//   多打一个左括号用户看得见、撤销一次即可；少补一个只是少省一次击键。反过来，
//   若给块头或 `SET_DEC_FILE` 补上括号，就会写出一句**语法错的代码**
//   （`SET_DEC_FILE(` 在 Chroma 里是错的，该语句的书写形式不带括号、也不带分号），
//   这触碰的是本项目对 Chroma 族的硬要求：零误报。
//
// 【已知数据缺口（批次 79 修）】
//   RELAY_ON / RELAY_OFF / MEAS_CURRENT 等 5 条的签名在抽取时丢了（手册把
//   Format 块分页推到下一页，见 TODO 批次 79），因此这里返回 false —— 它们是
//   调用形态，本该返回 true。tests/test_chromasig.cpp 里钉了这条当绊线，
//   批次 79 修好数据后那几行断言必须翻面。
bool WantsParenAfterName(const char* name, std::size_t len);

} // namespace chroma3380
} // namespace xfs
