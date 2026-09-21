#pragma once
// xfsWinPad - Chroma 3380 提示文案：签名位置解析 + 悬停气泡（批次 73 / 103）
//
// 【职责边界】
//   本文件只回答三个问题：光标是不是落在某条已收录语句的实参位置上、是第几个
//   实参、以及「鼠标停在某个语句名上时气泡里显示什么」。读代码、数括号、数逗号、
//   查语句库——纯字符串逻辑，不依赖 Scintilla，不碰任何 UI，因此可以直接单测。
//   弹下拉框、Tab 选中、状态栏标注、弹气泡窗口这些都在 Editor 侧。
//
// 【为什么必须单独抽出来做纯函数】
//   这段逻辑最容易出**静默错**：少算一个逗号，就会把 A 参数的候选值挂到 B 参数
//   上，用户拿到的是一份语法完全合法、数值却是错的代码。只有把它做成纯函数，
//   才能把嵌套括号、多行调用、字符串里的逗号、注释、越界这些情形全部钉进测试。
//
// 【与手册的数据关系】
//   stmt/param 指向 Chroma3380Db.h 的只读表；paramIndex 与手册签名的位置一一
//   对应（生成期有 _alignment_audit.txt 保证对齐）。

#include <cstddef>
#include <string>
#include "Chroma3380Db.h"

namespace xfs {
namespace chroma3380 {

struct SignatureHint {
    const StatementDef* stmt  = nullptr;   // 命中的语句（kStatements 表内）
    const ParamDef*     param = nullptr;   // 命中的参数槽（kParams 表内）
    int  paramIndex = -1;    // 0-based 实参序号，与手册签名位置一一对应
    int  argStart   = 0;     // 当前实参的替换起点（相对传入文本的字节偏移）
    bool hasEnum    = false; // 该参数是否有可信候选值（有才弹下拉框）
    // 该语句的槽序是否可信（kStmtPositional）。为假时 param/paramIndex 只是
    // 「按老口径排出来的第 N 个」，既不能拿来挂候选值，也不能拿来高亮签名。
    // 这种语句的签名本身也常被参数注释污染（LOAD_VI_WAVEFORM 的签名里印着
    // `**no_entry: illegal`），所以调用方连签名气泡都别出。
    bool positional = false;
};

// 解析 text[0, caret) 上的签名上下文。命中返回 true。
//
// 「命中」= 光标位于某条已收录语句的实参列表内且已越过左括号。是否弹下拉框由
// 调用方看 hasEnum 决定：没有候选值的参数必须交回普通词汇补全，不能把功能吞掉。
bool ResolveSignatureHint(const std::string& text, std::size_t caret,
                          SignatureHint& out);

// 光标所在实参的显示名（状态栏文案用）。手册该处没有具名参数时返回空串。
std::string ParamDisplayName(const SignatureHint& h);

// 手册签名里第 index 个顶层实参的字节区间 [start, end)。
//
// 用途：把签名气泡里的当前参数高亮出来（SCI_CALLTIPSETHLT 收字节偏移）。
// 越界、签名没有参数表、或 index 超出实参个数时返回 false（调用方不高亮即可）。
//
// 【和 ResolveSignatureHint 同源的必要性】
//   两处的「第 N 个实参」必须用同一套数逗号的规则，否则气泡高亮的会是别的参数
//   ——那种错很隐蔽：看起来在提示，指的却是隔壁。所以本函数复用同样的
//   深度/引号规则，并且**方括号在这里透明**（见下）。
//
// 【方括号为什么透明】
//   手册用 `[ , divide_count, timeout ]` 表示尾部可选参数，方括号是**排版记号**，
//   用户在代码里不会写它。若把 `[` 当深度，整组可选参数会被并成一个实参，
//   高亮就落不到单独一个参数上。而调用文本里（ResolveSignatureHint 那侧）
//   方括号可能是数组字面量，所以那边仍然计深——两边口径的差异是有意的。
//   注意"透明"要**两件事都做**：跳过（不参与计数）**且**从区间两端剔除，
//   而且两端都要认得 `[` 和 `]` 两种字符。手册的写法是
//   `freq_range [, divide_count, … ]` —— `[` 写在**分隔逗号之前**，
//   所以它会落在**上一个**参数的尾部（"freq_range ["），而 `]` 落在最后一个参数的尾部。
//   批次 73 实测先漏了尾部、又漏了尾部的 '['，被 tests/test_chromasig.cpp 的
//   MEAS_FREQ 用例连抓两次——这个用例值钱，别删。
bool SignatureArgRange(const std::string& signature, int index,
                       int& start, int& end);

// ---- 悬停气泡文案（批次 103）------------------------------------------------
// 鼠标停在某个标识符上时，气泡里该显示什么。
//
// 【文案形态】
//   `FORCE_V_PPMU(pin_name, f_volt, …);//This statement sets the Per-pin PMU …`
//   即「手册原文签名」+ `;//` +「手册原文说明」。分号被摘掉是为了让 `;//` 连成
//   一串——手册把分号排成 `wait_time) ;`，照抄会显示成 `) ;//`。
//
// 【「有哪个显示哪个」】
//   实测 309 条语句里 51 条**没有签名**（CRAFT 宏、SET_DEC_FILE、RF_Initialize …），
//   但它们**全都有说明**；另有 2 条有签名而手册没写说明。所以四种组合都要给文案：
//     签名 + 说明 → `签名;//说明`（用户点名的形态）
//     只有签名    → `签名`
//     只有说明    → `说明`
//     两者皆无    → false（全库 0 条，留着是为了手册改版时不出空气泡）
//   写成「没签名就不出气泡」会让那 51 条悬停时什么都不弹 —— 用户分不清是
//   「手册没写语法」还是「编辑器不认识这个词」。
//
// 【为什么不在这里判「鼠标在哪个词上」】
//   取词要用 Scintilla 自己的词边界（`SCI_WORDSTARTPOSITION`），否则
//   `FORCE_V_PPMU` 里的下划线、以及后面紧跟的 `(` 都会成为分歧点。那一侧在
//   Editor 里，本函数只负责「给定标识符 → 文案」这一半，因而可以直接单测。
//
// 返回值：true = out 已填好，调用方显示气泡；false = 未收录或无可提示内容，
// 调用方不出气泡（out 保持不变）。
bool BuildHoverTip(const char* name, std::size_t len, std::string& out);

} // namespace chroma3380
} // namespace xfs
