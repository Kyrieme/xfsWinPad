#pragma once
// xfsWinPad - 自研 ATE 词法器的样式号（批次 72）
//
// 【为什么从 64 起编】
//   Scintilla 保留 32..39（STYLE_DEFAULT=32、STYLE_LINENUMBER=33、
//   STYLE_BRACELIGHT=34、STYLE_BRACEBAD=35、STYLE_CONTROLCHAR=36、
//   STYLE_INDENTGUIDE=37、STYLE_CALLTIP=38、STYLE_FOLDDISPLAYTEXT=39），
//   STYLE_MAX 默认 255。Lexilla 各词法器的 SCE_* 密集占用低段（有的用到 100+），
//   所以自研族统一从 64 起编并在 128 前收尾，与 Lexilla 的编号区间不重叠，
//   切换语言时不会串色。

namespace xfs {

enum XfsLexerStyle : int {
    kXfsStyleBase = 64,

    // ---------- ATE Pattern (.pat) ----------
    // 设计意图：向量行的三类字符必须一眼可分——drive(0/1) / expect(H/L/T) /
    // mask(X/N/Z/U)。pattern 调试时最常问的就是「这一拍是驱动还是比较」，
    // 所以拆成三个样式而不是笼统的 number。
    SCE_ATEP_DEFAULT = kXfsStyleBase,  // pin 名/标识符，保持默认前景色
    SCE_ATEP_COMMENT,                  // # 或 // 行注释
    SCE_ATEP_OPCODE,                   // RPT/JMP/CALL/RET/LBL/END...
    SCE_ATEP_LABEL,                    // 标签定义与跳转目标
    SCE_ATEP_PIN,                      // 关键字表声明的 pin/信号名
    SCE_ATEP_VECTOR,                   // 向量：0 1（驱动值）
    SCE_ATEP_EXPECT,                   // 向量：H L T（比较/期望）
    SCE_ATEP_MASK,                     // 向量：X N Z U（掩码/不关心）
    SCE_ATEP_HEX,                      // 0x.. / 十六进制向量块
    SCE_ATEP_NUMBER,                   // 十进制计数（RPT 10）
    SCE_ATEP_STRING,                   // "..." / '...'
    SCE_ATEP_OPERATOR,                 // = ; , ( ) [ ] { } : @ $
    SCE_ATEP_TIMING,                   // TSET/TIM/EDGE/PERIOD 引用
    SCE_ATEP_DIRECTIVE,                // 首字符指令（.INCLUDE / #define 类）

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

} // namespace xfs
