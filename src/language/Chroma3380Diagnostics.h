#pragma once
// xfsWinPad - Chroma 3380 静态校验内核（批次 80，方向 C 第一刀）
//
// 【为什么要有这一层】
//   Chroma 官方工作流是「TextPad 编辑 → Makefile 调 plncmp/patcmp → 看编译结果页
//   点击跳转」（操作手册 §2.5），也就是说**错误只有在编译之后才看得到**。把这件
//   事提前到「边写边标」，是与官方工具链相比唯一真正有增量价值的方向。
//
// 【定位：自建诊断模型，不是官方错误码】
//   手册与操作手册里**没有公开错误码表**（操作手册全文 `error` 只出现 14 次）。
//   所以本模块每条规则都逐条从手册原文取证，每条诊断都带 `manualPage` 供追溯，
//   文案是我们自己的。UI 展示时**必须**标注「非 CRAFT 编译结果」，不要伪装成官方诊断。
//
// 【铁律：零误报 > 多报】
//   误报一次，工程师就会把整个校验关掉，之后修复成本极高。因此本模块：
//     · 解析一律**宽容**——认不出来的行/字段直接跳过，不报；
//     · 只在手册**明文写了会报错**的地方开口（原句 `An error will occur if …`）；
//     · 手册**自相矛盾**的规则不实现（见 DEC-004 下方注释里的实例）。
//
// 【职责边界】
//   纯字符串逻辑：不依赖 Scintilla、不碰 UI、不知道光标在哪。产出的诊断列表由调用方
//   决定怎么展示（squiggle / 面板 / 状态栏）。因此可以直接单测。
//
// 【与其它模块的关系】
//   Chroma3380Db.h    —— 语句/参数/候选值数据库（本模块暂不依赖它，规则 8「参数个数
//                        与必填性」是下一批要接的）
//   ChromaSignature.h —— 签名提示的位置解析（不做校验）
//   本模块只回答「这段文本有哪些**确定**的错」。

#include <string>
#include <vector>

namespace xfs {
namespace chroma3380 {

// Chroma 三种文件类型。校验规则按类型分族——.pln 的 SET_DEC_FILE 不能有分号，
// 而 .dec 的 DEC_MODE 反而必须有分号，混在一张表里必然误报。
enum class ChromaFileKind {
    Unknown,
    Plan,     // .pln 测试计划
    Dec,      // .dec 设备定义
    Pattern,  // .pat 向量
};

// 按扩展名判类型（大小写不敏感）。认不出返回 Unknown，校验直接返回空。
ChromaFileKind FileKindFromPath(const std::string& path);

enum class DiagSeverity {
    Error,    // 手册明文说会报错
    Warning,  // 手册用希望/建议语气，或者需要跨文件才能确认
};

struct Diagnostic {
    int          line       = 0;      // 0-based 行号（与 SCI_GETCURLINE 同口径）
    int          start      = 0;      // 行内起始列（字节，0-based）
    int          length     = 0;      // 覆盖字节数，供下划线/高亮用
    DiagSeverity severity   = DiagSeverity::Error;
    const char*  code       = nullptr; // 稳定标识，UI 与测试都按它断言
    int          manualPage = 0;       // 手册（Language Manual）1-based PDF 页
    std::string  message;              // 中文文案，短句
};

// 规则清单（每条都标了取证位置）：
//
//   C3380-PLN-001  SET_DEC_FILE 末尾不能有分号                        p41 §3.3.2
//   C3380-DEC-001  PIN_LIST：同一个 pin 名定义了多次                 p25 §2.3.2
//   C3380-DEC-002  PIN_LIST：同一个 ATE 通道号定义了多次             p25 §2.3.2
//   C3380-DEC-003  PIN_LIST：同一个 DUT pin 号定义了多次             p25 §2.3.2
//   C3380-DEC-004  PIN_GROUP：同一个 pin_group 名定义了多次          p27 §2.4.2
//
// 取证原文（§2.3.2）：
//   "An error will occur if the same DUT or ATE pin numbers are defined more than
//    once." / "An error will occur if the same pin name is given to more than one
//    DUT pin.  Pin names must be unique within the device definition."
//
// 【为什么不实现「同一 pin 不得属于两个 group」（§2.4.2 原话）
//  "The same pin cannot be assigned to more than one group."】
//   因为**手册自己的示例就违反了它**：§2.4.3 的官方 .dec 示例里
//     CTRL  = CLR+SEL0+SEL1+G1+G2+SL+SR;
//     SEL01 = SEL0+SEL1;
//   SEL0 / SEL1 同时属于 CTRL 与 SEL01。照这句原话实现，手册示例会被判错 ——
//   零误报铁律下必须不实现。真要做，得先找到「分组是否互斥」的真实约束（可能只
//   对 relay / pattern 用途的组成立），那需要真实 .dec 样本与现场确认。
//
// 【为什么 PIN_LIST 的唯一性检查按「块」而不是按「文件」范围
//  一个 .dec 可以声明多个 PIN_LIST 块（每个 loadboard 一块，如手册 2.2.2 的
//  `PIN_LIST (LPC_BOARD_00)` 与 `PIN_LIST (LPC_BOARD_00 _2sites)`），不同板卡的
//  通道映射本来就不同。按文件判重会把这种合法写法判错，所以范围收到块内。
std::vector<Diagnostic> ValidateChromaSource(const std::string& text,
                                            ChromaFileKind kind);

} // namespace chroma3380
} // namespace xfs
