#pragma once
// xfsWinPad - Chroma 3380 平台语言数据（批次 73）
//
// 【本文件由脚本生成，请勿手改】
//   生成器：temp/_chroma_extract/gen_cpp_db.py
//   数据源：EN_3380_Language_Manual_1901.pdf 逐节抽取 + 人工复核
//           （复核过程与已知手册笔误见 docs/chroma3380-language-pack.md）
//   Chroma 出新版手册时重跑生成器即可，不要手工修补数据。
//
// 【数据布局：三个扁平池】
//   kValues     候选值字符串池（去重）
//   kParams     参数槽：名称 / 手册默认值 / 值区间 / 可选与枚举标记
//   kStatements 语句：名称 / 章节 / 手册签名 / 参数区间
//   用扁平池而不是「每条语句一个 static 数组」，是因为候选值跨族高度复用
//   （ON/OFF、AVE/RMS 各有几十处引用），数组化会产生大量重复符号。

#include <cstddef>

namespace xfs {
namespace chroma3380 {

enum ParamFlag : unsigned {
    kParamOptional = 1u,   // 位于签名方括号内（手册 `[ , ... ]` 约定）
    kParamHasEnum  = 2u,   // 有可信候选值（可出下拉）
    kParamHasDef   = 4u,   // 手册写了 no_entry 默认值
};

// 语句级标志
enum StmtFlag : unsigned {
    // 参数槽与手册签名**逐位对应**（签名可按逗号数出位置）。
    // 未置位 = 该语句的签名是重复组语法 `[ ... ]*` 或被参数注释污染，
    // 槽序来自旧口径、不可信：**不得**按实参下标挂候选值，也不要高亮参数。
    kStmtPositional = 1u,
};

struct ParamDef {
    const char* name;      // 参数名（已剔除手册拼写笔误，见文档 5.2 节）
    const char* def;       // 手册 `no_entry:` 默认值；nullptr = 手册未载明
    int         valStart;  // kValues 下标（kParamHasEnum 时有效）
    int         valCount;  // 候选值个数；0 = 无枚举
    unsigned    flags;     // ParamFlag 位或
};

struct StatementDef {
    const char* name;
    const char* section;   // 手册章节号，便于回溯与「跳到手册」功能
    const char* signature; // 手册原文签名（用于签名提示的展示文案）
    int         paramStart;
    int         paramCount;
    unsigned    flags;     // StmtFlag 位或
};

extern const ParamDef      kParams[];
extern const int           kParamCount;
extern const char* const   kValues[];
extern const int           kValueCount;
extern const StatementDef  kStatements[];
extern const int           kStatementCount;

// ---- 词表（空格分隔，词法器构造函数注入）---------------------------------
// 放这里而不是 LanguageMap 的 keywords[2]：LanguageMap 只有两个槽位，
// 而 .pln 需要「语句 / 宏 / C 库 / C 关键字 / pin type」五张互不相同的表。
extern const char* const kPlanStmtWords;   // .pln 测试语句（12 族）
extern const char* const kPlanMacroWords;  // .pln CRAFT 内建宏变量
extern const char* const kPlanClibWords;   // .pln C 语言库函数
extern const char* const kPlanBlockWords;  // .pln 块语句（流程骨架）
extern const char* const kPlanCtlWords;    // .pln C 控制关键字
extern const char* const kPlanTypeWords;   // .pln 类型名（C 基础 + 手册语义类型）
extern const char* const kPlanPinTypeWords;// .pln pin_type 枚举
extern const char* const kDecBlockWords;   // .dec 顶层块语句
extern const char* const kDecModeWords;    // .dec DEC_MODE 取值
extern const char* const kDecNameWords;    // .dec 内建对象名
extern const char* const kDecPinTypeWords; // .dec PIN_LIST 的 pin_type
extern const char* const kPatModuleWords;  // .pat 模块语句
extern const char* const kPatMicroWords;   // .pat 微指令

// 按名查语句（大小写不敏感）。未收录返回 nullptr。
// 线性扫描是够的：只在「签名提示」触发时调用一次，315 条量级可忽略。
const StatementDef* FindStatement(const char* name, std::size_t len);

} // namespace chroma3380
} // namespace xfs
