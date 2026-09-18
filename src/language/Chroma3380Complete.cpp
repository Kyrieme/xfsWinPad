// Chroma3380Complete.cpp — 批次 77：Chroma 3380 语句级补全的候选生成。
//
// 设计与取舍见头文件顶部的长注释；这里只补两件实现层面的说明：
//
// 1) 候选集**构建一次、常驻缓存**（函数内 static map）。kStatements 是 300+ 条，
//    每次按键都线性扫一遍虽然也只够毫秒级，但按键路径上的分配与扫描能省则省。
//    缓存值是 map 的节点，指针/引用在后续插入时不会失效（node-based）。
// 2) 两个数据来源的职责是分开的：
//      kStatements —— 提供**手册顺序与章节号**（表本身就是章节升序）；
//      关键词表    —— 提供**这个文件类型里允许出现的名字**（词法器高亮用的同一份）。
//    所以在手册顺序上取交集，再把"词表里有、手册没收录"的补在末尾。

#include "Chroma3380Complete.h"
#include "Chroma3380Db.h"
#include "XfsLexer.h"   // 三支词法器的名字（kLexChromaPlan / kLexChromaDec / kLexAtePattern）

#include <cstring>
#include <map>
#include <set>

namespace xfs {
namespace chroma3380 {

namespace {

// 一组词表来源。拆分「语句名」与「其它词」是因为两者进的是两条补全通道：
// 语句名走位置敏感的语句补全（带章节列），其它词只做普通词汇补全的兜底。
struct LexerTables {
    std::vector<const char*> stmtTables;
    std::vector<const char*> otherTables;
};

bool TablesFor(const char* lexerName, LexerTables& t) {
    if (!lexerName) return false;
    if (std::strcmp(lexerName, kLexChromaPlan) == 0) {
        // .pln：TPG 测试语句（12 族）+ 流程块 + C 语言库函数。
        // C 库函数（5.3）虽然不在"测试语句"族里，但它们在语句位置被调用（带括号），
        // 所以同样进语句补全，靠章节列区分。
        t.stmtTables  = { kPlanStmtWords, kPlanBlockWords, kPlanClibWords };
        t.otherTables = { kPlanMacroWords, kPlanCtlWords, kPlanTypeWords,
                          kPlanPinTypeWords };
        return true;
    }
    if (std::strcmp(lexerName, kLexAtePattern) == 0) {
        // .pat：模块语句 + 微指令。微指令（NOP/RPT/SETM/IMATCH…）一行一条，
        // 正是 IsStatementStart 的"行首"规则要服务的对象。
        t.stmtTables  = { kPatModuleWords, kPatMicroWords };
        return true;
    }
    if (std::strcmp(lexerName, kLexChromaDec) == 0) {
        t.stmtTables  = { kDecBlockWords, kDecNameWords };
        t.otherTables = { kDecModeWords, kDecPinTypeWords };
        return true;
    }
    return false;
}

// 拆空格分隔的词表串。表里可能有多余空格，逐词跳。
std::vector<std::string> SplitWords(const char* s) {
    std::vector<std::string> out;
    if (!s) return out;
    for (const char* p = s; *p;) {
        while (*p == ' ' || *p == '\t') ++p;
        const char* q = p;
        while (*q && *q != ' ' && *q != '\t') ++q;
        if (q > p) out.emplace_back(p, (std::size_t)(q - p));
        p = q;
    }
    return out;
}

struct CandidateList {
    std::vector<StatementCandidate> items;  // 手册顺序在前，词表余项在后
    std::vector<std::string>        extra;  // 普通词汇补全的兜底词（全部词表并集）
};

// 按词法器名取（构建一次的）候选集；不是 Chroma 族返回 nullptr。
const CandidateList* CandidatesFor(const char* lexerName) {
    if (!lexerName) return nullptr;
    static std::map<std::string, CandidateList> cache;
    auto it = cache.find(lexerName);
    if (it != cache.end()) return &it->second;

    LexerTables t;
    if (!TablesFor(lexerName, t)) return nullptr;

    CandidateList cl;

    std::set<std::string> stmtWords;   // 词表里的语句名（这个文件类型认识的）
    for (const char* tbl : t.stmtTables)
        for (const std::string& w : SplitWords(tbl)) stmtWords.insert(w);

    // 1) 手册顺序（kStatements 表序 = 章节升序，天然按硬件族分组）
    std::set<std::string> added;
    for (int i = 0; i < kStatementCount; ++i) {
        const StatementDef& s = kStatements[i];
        if (!stmtWords.count(s.name)) continue;
        if (!added.insert(s.name).second) continue;
        cl.items.push_back({ s.name, s.section ? s.section : "" });
    }
    // 2) 词表里有、手册未收录的（TEST_ITEM / HEADER / RPM_PATTERN 这类，
    //    或微指令、.dec 块名——它们本来就不在手册的语句章节里）→ 章节列留空
    for (const char* tbl : t.stmtTables)
        for (const std::string& w : SplitWords(tbl))
            if (added.insert(w).second) cl.items.push_back({ w, "" });

    // 3) 兜底词表：**全部**词表的并集，去重保序（语句名在最前）。
    //    这里刻意不复用 added —— 语句名同样要进兜底词表：语句补全只管"语句起始
    //    位置"，用户在别的位置（实参、表达式里）打 `FORC` 时，兜底那条路也该
    //    认识 FORCE_*，否则同一份词表在两个通道里给的答案不一致。
    std::set<std::string> extraSeen;
    auto addExtra = [&cl, &extraSeen](const char* tbl) {
        for (const std::string& w : SplitWords(tbl))
            if (extraSeen.insert(w).second) cl.extra.push_back(w);
    };
    for (const char* tbl : t.stmtTables) addExtra(tbl);
    for (const char* tbl : t.otherTables) addExtra(tbl);

    return &cache.emplace(lexerName, std::move(cl)).first->second;
}

bool IsSpaceByte(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

}  // namespace

bool IsChroma3380LexerName(const char* lexerName) {
    if (!lexerName) return false;
    return std::strcmp(lexerName, kLexChromaPlan) == 0 ||
           std::strcmp(lexerName, kLexChromaDec) == 0 ||
           std::strcmp(lexerName, kLexAtePattern) == 0;
}

int CollectStatementCandidates(const char* lexerName, const std::string& prefix,
                               std::vector<StatementCandidate>& out, int maxCount) {
    out.clear();
    if (prefix.size() < kStmtCompleteMinPrefix || maxCount <= 0) return 0;
    const CandidateList* cl = CandidatesFor(lexerName);
    if (!cl) return 0;
    for (const StatementCandidate& c : cl->items) {
        if (c.name.size() <= prefix.size()) continue;   // 已打全：没有可补的部分
        if (_strnicmp(c.name.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
        out.push_back(c);
        if ((int)out.size() >= maxCount) break;
    }
    return (int)out.size();
}

void CollectExtraWords(const char* lexerName, std::vector<const char*>& out) {
    out.clear();
    const CandidateList* cl = CandidatesFor(lexerName);
    if (!cl) return;
    out.reserve(cl->extra.size());
    for (const std::string& w : cl->extra) out.push_back(w.c_str());
}

bool IsStatementStart(const std::string& text, std::size_t pos) {
    if (pos > text.size()) pos = text.size();

    // 规则 1：行首（pos 与本行行首之间只有空白）
    std::size_t ls = pos;
    while (ls > 0 && text[ls - 1] != '\n' && text[ls - 1] != '\r') --ls;
    bool onlyWs = true;
    for (std::size_t i = ls; i < pos; ++i) {
        if (!IsSpaceByte(text[i])) { onlyWs = false; break; }
    }
    if (onlyWs) return true;

    // 规则 2：同一条语句序列上（往回跳空白，前面是语句结束符 / 块括号）
    std::size_t i = pos;
    while (i > 0 && IsSpaceByte(text[i - 1])) --i;
    if (i == 0) return true;
    const char c = text[i - 1];
    return c == ';' || c == '{' || c == '}';
}

bool WantsParenAfterName(const char* name, std::size_t len) {
    if (!name || len == 0) return false;
    const StatementDef* st = FindStatement(name, len);
    if (!st || !st->signature) return false;      // 库里没有 / 手册未载明签名
    if (!*st->signature) return false;
    // 只看签名里**有没有** `(`，不要求位置 —— `START_UP( ) {` 这种"带括号的块头"
    // （真实 .pln 里写作 `START_UP();`）应当算调用形态。
    return std::strchr(st->signature, '(') != nullptr;
}

} // namespace chroma3380
} // namespace xfs
