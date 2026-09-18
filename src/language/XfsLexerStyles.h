#pragma once
// xfsWinPad - 自研 ATE 词法器的样式号（批次 72 建立，批次 73 按真实 Chroma 语法重排）
//
// 【为什么从 64 起编】
//   Scintilla 保留 32..39（STYLE_DEFAULT=32、STYLE_LINENUMBER=33、
//   STYLE_BRACELIGHT=34、STYLE_BRACEBAD=35、STYLE_CONTROLCHAR=36、
//   STYLE_INDENTGUIDE=37、STYLE_CALLTIP=38、STYLE_FOLDDISPLAYTEXT=39），
//   STYLE_MAX 默认 255。Lexilla 各词法器的 SCE_* 密集占用低段（有的用到 100+），
//   所以自研族统一从 64 起编，与 Lexilla 的编号区间不重叠。
//
// 【批次 73 的重排理由】
//   批次 72 的 .pat 样式是按**自拟语言**设计的（opcode/timing/directive 三分），
//   与真实 Chroma .pat 记号体系对不上：真实文件里没有 `.directive`，`SET_DEC_FILE`
//   是模块语句而不是指令；向量的 `R/S/T/U` 是「驱动+比较」的组合语义，与纯比较
//   的 `H/L/Z` 不是一回事。本批按手册第 3 章重定义，并新增 .dec / .pln 两组。
//
// 【区间预算（必须 ≤ 128）】
//   .pat 64..80 (17) | .dec 81..89 (9) | .pln 90..103 (14)
//   STIL 104..113 (10) | ATE Log 114..124 (11) | 合计 61 个样式

namespace xfs {

enum XfsLexerStyle : int {
    kXfsStyleBase = 64,

    // ---------- Chroma .pat — 向量与微指令文件（手册第 3 章）----------
    // 向量字符分五类而不是三类，是照手册 3.4.1 的**驱动/比较组合**语义：
    //   驱动 only      0 1               → VEC_DRIVE
    //   比较 only      H L Z             → VEC_CMP
    //   驱动 + 比较    R S T U           → VEC_DRV_CMP  ← 批次 72 把 U 当掩码是错的
    //   不驱动不比较   X                 → VEC_MASK
    //   特殊控制       V K 2             → VEC_CTRL
    // pattern 调试最常问「这一拍是驱动还是比较」，五分类正好把「两者都做」这一
    // 最容易误读的情形单独着色，比三分类更能直接回答该问题。
    SCE_ATEP_DEFAULT = kXfsStyleBase,  // pin 名 / 普通标识符
    SCE_ATEP_COMMENT,                  // # 或 // 行注释
    SCE_ATEP_MODULE,                   // SET_DEC_FILE / HEADER / SPM_PATTERN / APM_/RPM_
    SCE_ATEP_MICRO,                    // RPT / JNZ0 / IMATCH / JSR / RET ...
    SCE_ATEP_LABEL,                    // os_st:: （全局）/ os_sp: （局部）
    SCE_ATEP_SEP,                      // 向量界定符 * ... *
    SCE_ATEP_TIMESET,                  // TS1 ~ TS15 时序集引用
    SCE_ATEP_PIN,                      // %SEL0 形式及 pin 上下文中的 pin 名
    SCE_ATEP_VEC_DRIVE,                // 0 1
    SCE_ATEP_VEC_CMP,                  // H L Z
    SCE_ATEP_VEC_DRV_CMP,              // R S T U
    SCE_ATEP_VEC_MASK,                 // X
    SCE_ATEP_VEC_CTRL,                 // V K 2
    SCE_ATEP_HEX,                      // dA0 / c8 十六进制引导
    SCE_ATEP_NUMBER,                   // 十进制计数（RPT 100）
    SCE_ATEP_STRING,                   // "..." / '...'
    SCE_ATEP_OPERATOR,                 // = ; , ( ) [ ] { } : @

    // ---------- Chroma .dec — 设备定义（手册第 2 章）----------
    // 语言形态与 .pat/.pln 都不同：只有 7 个顶层块，块内是
    //   pin_name = ate_pin[:ate_pin] = dut_pin = pin_type ;
    // 这种四段等号结构。所以样式是按**列语义**分的（pin/通道/pin_type），
    // 而不是按语句分 —— 一行里三列各自着色才看得出哪一列写错了。
    SCE_DEC_DEFAULT,
    SCE_DEC_COMMENT,                   // /* */ 与 //
    SCE_DEC_BLOCK,                     // DEC_MODE / PIN_LIST / PIN_GROUP / UR_PIN_GROUP ...
    SCE_DEC_PINTYPE,                   // IN OUT IO MLDPS DPS UVI PREF TMU UR TRG EXT GND WG WD
    SCE_DEC_PIN,                       // pin 名（多数派，但组名/pin 混排时有区分价值）
    SCE_DEC_CHANNEL,                   // ATE 通道号与 DUT pin 号
    SCE_DEC_MODEVAL,                   // APAS / NORM
    SCE_DEC_OPERATOR,                  // = : ; { } + -
    SCE_DEC_STRING,                    // 板卡名等字符串

    // ---------- Chroma .pln — 测试计划（手册第 4~5 章，全语言的 85%）----------
    // 这里的样式分工回答的是「这一行在测试流程里扮演什么角色」：
    //   块语句  → 流程骨架（TEST_PRO / HW_BIN_DEF）
    //   测试语句→ 真正与硬件打交道的动作（FORCE_* / JUDGE_*）
    //   宏 / C库→ 元数据与宿主函数，写错时表现与语句完全不同，必须分开
    //   流程记号→ ? : #C() #F() => 这套独立子 DSL
    SCE_PLN_DEFAULT,
    SCE_PLN_COMMENT,                   // // 与 /* */
    SCE_PLN_BLOCK,                     // TEST_PRO / HW_BIN_DEF / SW_BIN_DEF / START_UP ...
    SCE_PLN_STMT,                      // FORCE_V_MLDPS / JUDGE_I_DPS / MEAS_* ...
    SCE_PLN_MACRO,                     // TEST_LOT_ID / HWBIN / SWBIN（CRAFT 内建宏）
    SCE_PLN_CLIB,                      // CRAFT_* / DPS2SITE / PREF2SITE（C 库函数）
    SCE_PLN_PINTYPE,                   // pin_type 枚举（出现于 PIN_GROUP 等语句参数里）
    SCE_PLN_FLOW,                      // ? : #C() #F() => !（TEST_PRO 的流程 DSL）
    SCE_PLN_LABEL,                     // 测试项标签 / BEFORE_TEST / AFTER_TEST
    SCE_PLN_CKEYWORD,                  // int if else while return（C 语法骨架）
    SCE_PLN_CTYPE,                     // VOLTAGE / CURRENT / TIME（手册的语义类型名）
    SCE_PLN_STRING,
    SCE_PLN_NUMBER,
    SCE_PLN_OPERATOR,

    // ---------- STIL (.stil, IEEE 1450) ----------
    SCE_STIL_DEFAULT,
    SCE_STIL_COMMENT,                  // // 与 /* */
    SCE_STIL_BLOCK,                    // Signals/Timing/Waveforms/Pattern...
    SCE_STIL_KEYWORD,                  // In/Out/Supply/Period/PatList...
    SCE_STIL_NAME,                     // 'CLK' 单引号名
    SCE_STIL_NUMBER,                   // 20 / 1.8 / 0
    SCE_STIL_UNIT,                     // '20ns' 里的单位与整串带单位量
    SCE_STIL_EVENT,                    // 波形事件 D/U/Z/X/N/T/L/H
    SCE_STIL_OPERATOR,                 // { } ; = + - ( )
    SCE_STIL_STRING,                   // 双引号字符串

    // ---------- ATE Log (.log) ----------
    SCE_ATEL_DEFAULT,
    SCE_ATEL_COMMENT,
    SCE_ATEL_TIMESTAMP,                // 行首日期时间
    SCE_ATEL_SITE,                     // SITE1 / 机台号 / 工位
    SCE_ATEL_TESTNAME,                 // 测试项名
    SCE_ATEL_NUMBER,                   // 测量值
    SCE_ATEL_LIMIT,                    // [1.650, 1.950] 限值区间
    SCE_ATEL_PASS,                     // PASS
    SCE_ATEL_FAIL,                     // FAIL
    SCE_ATEL_WARN,                     // WARN/ERROR/ABORT
    SCE_ATEL_RECORD,                   // BIN/PART/NUM/TOTAL 统计记录词

    kXfsStyleEnd
};

// 编译期护栏：样式号不得越入 Scintilla 保留区或 STYLE_MAX。
static_assert(kXfsStyleBase > 39, "style base must clear Scintilla's reserved 32..39");
static_assert(kXfsStyleEnd <= 128, "xfs lexer styles must stay below 128");
static_assert(SCE_ATEP_DEFAULT < SCE_DEC_DEFAULT, ".pat group must come first");
static_assert(SCE_DEC_DEFAULT < SCE_PLN_DEFAULT, ".dec group must precede .pln");
static_assert(SCE_PLN_OPERATOR < SCE_STIL_DEFAULT, ".pln group must precede STIL");
static_assert(SCE_STIL_STRING < SCE_ATEL_DEFAULT, "STIL group must precede ATE Log");

} // namespace xfs
