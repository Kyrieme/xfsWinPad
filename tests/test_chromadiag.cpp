// xfsWinPad - Chroma 3380 静态校验内核单测（批次 80）
//
// 【这个测试文件的重点不是"能报错"，而是"不误报"】
//   校验功能的信任是一次性的：误报一次，工程师就关掉它。所以这里最值钱的两组用例是
//     1) 手册 **§2.3.3 / §2.4.3 的官方 .dec 示例**（原文照抄）→ 必须 **0 诊断**；
//     2) 真实 AAA `.pln` 的关键行（文件头 /*..*/ 块注释、`#define` 预处理指令、
//        `SET_DEC_FILE "..."` 无分号）→ 必须 **0 诊断**。
//   反例则逐条钉：重复 pin 名 / 重复 ATE 号 / 重复 DUT 号 / 重复组名 / 分号。
//
// 【为什么内嵌而不是读 temp/ 下的真实文件】
//   `temp/` 是私密目录、被 .gitignore 排除，CI 上不存在 —— 测试里读它会在 CI 挂。

#include "../src/language/Chroma3380Diagnostics.h"

#include <cstdio>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace xfs::chroma3380;

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int CountCode(const std::vector<Diagnostic>& v, const char* code) {
    int n = 0;
    for (const Diagnostic& d : v) {
        if (d.code && std::string(d.code) == code) ++n;
    }
    return n;
}

// 手册 §2.3.3 + §2.4.3 的官方 `< Device Definition File >` 示例，**原文照抄**
// （含原文多余的空格与行尾空白，就是要拿它当"宽容解析"的压力测试）。
static const char* kManualDecExample = R"DEC(PIN_LIST  ( LS299_4_Sites )  
{ 
        SEL0   =  0 : 288 : 320 : 352  =  1  =    IN  ;    
        G1       =  2 : 290 : 322 : 354  =  2  =    IN  ;    
        G2       =  4 : 292 : 324 : 356  =  3  =    IN  ;    
        QG        =  6 : 294 : 326 : 358  =  4  =    IO  ;    
        QE        =  8 : 296 : 328 : 360  =  5  =    IO  ;    
        QC        = 10 : 298 : 330 : 362  =  6  =    IO  ;    
        QA        = 12 : 300 : 332 : 364  =  7  =    IO  ;    
        CLR       = 14 : 302 : 334 : 366  =  9  =    IN  ;  
        SR        = 15 : 303 : 335 : 367  = 11  =    IN  ;    
        CLK       = 13 : 301 : 333 : 365  = 12  =    IN  ;    
        QB        = 11 : 299 : 331 : 363  = 13  =    IO  ;    
        QD        =  9 : 297 : 329 : 361  = 14  =    IO  ;    
        QF        =  7 : 295 : 327 : 359  = 15  =    IO  ;    
        QH        =  5 : 293 : 325 : 357  = 16  =    IO  ;    
        SL        =  3 : 291 : 323 : 355  = 18  =    IN  ;   
        SEL1      =  1 : 289 : 321 : 353  = 19  =    IN  ;   
        Vdps    =  576 : 577 : 578 : 579 = 21  =    DPS  ;  
} 
 
PIN_GROUP 
{ 
  CTRL       = CLR+SEL0+SEL1+G1+G2+SL+SR;  
  SEL01      = SEL0+SEL1;  
  QQ         = QA+QB+QC+QD+QE+QF+QG+QH;  
} 
POWER_PIN_GROUP  
{ 
    DPS_OS_PINS = Vdps;  
} 
)DEC";

// 现场 `.pln` 的开头（结构照抄，注释里的中文换成 ASCII，
// 因为源文件是 UTF-8 而真实 .pln 是 GBK，直接内嵌会造出一个假的编码问题）。
// ⚠️ 标识字段（公司名 / chip 名 / 文件名）一律用**中性占位**：这里要验的是
//    结构（块注释 + TEST CHIP 行 + 无分号 SET_DEC_FILE），不是具体值。
static const char* kRealPlanHeader = R"PLN(/*  ========================== Base information =======================
                                  Test Vendor Ltd.
    ===================================================================
            TEST CHIP              : AAA
            VERSION                : 01
     ===================================================================  */

SET_DEC_FILE ".\AAA_CP_S3.dec"

#define _UNICODE
#define UNICODE

int  rst_lvl = 1;

/* 多行块注释里出现的假语句，必须全部忽略：
SET_DEC_FILE "ignored.dec" ;
*/
)PLN";

static void RunFileKind() {
    CHECK(FileKindFromPath("a.pln") == ChromaFileKind::Plan);
    CHECK(FileKindFromPath("A.PLN") == ChromaFileKind::Plan);
    CHECK(FileKindFromPath("D:\\x\\y.Pln") == ChromaFileKind::Plan);
    CHECK(FileKindFromPath("x.dec") == ChromaFileKind::Dec);
    CHECK(FileKindFromPath("x.DEC") == ChromaFileKind::Dec);
    CHECK(FileKindFromPath("x.pat") == ChromaFileKind::Pattern);
    CHECK(FileKindFromPath("x.txt") == ChromaFileKind::Unknown);
    CHECK(FileKindFromPath("noext") == ChromaFileKind::Unknown);
    // Unknown 一律不校验（避免把别的语言当 Chroma 来报错）
    CHECK(ValidateChromaSource("SET_DEC_FILE \"a.dec\" ;", ChromaFileKind::Unknown).empty());
}

static void RunSetDecFile(ChromaFileKind kind) {
    // ---- 正例：手册 §3.3.2 的写法与真实 .pln 的写法 ----
    CHECK(ValidateChromaSource("SET_DEC_FILE \".\\ls299_16sites_pin.dec\"\n", kind).empty());
    CHECK(ValidateChromaSource(kRealPlanHeader, kind).empty());

    // ---- 正例：分号在注释里 / 在字符串里，都不是错 ----
    CHECK(ValidateChromaSource("SET_DEC_FILE \"a.dec\"   // 见 x;\n", kind).empty());
    CHECK(ValidateChromaSource("SET_DEC_FILE \"a.dec\"  /* x; */\n", kind).empty());
    CHECK(ValidateChromaSource("SET_DEC_FILE \"a;b.dec\"\n", kind).empty());
    CHECK(ValidateChromaSource("#define SET_DEC_FILE \"a.dec\" ;\n", kind).empty());
    // 名字更长的不算（读标识符要读满）
    CHECK(ValidateChromaSource("SET_DEC_FILE_EXTRA \"a.dec\" ;\n", kind).empty());

    // ---- 反例：末尾有分号 ----
    {
        const std::string s = "SET_DEC_FILE \".\\a.dec\" ;\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, kind);
        CHECK(d.size() == 1);
        CHECK(CountCode(d, "C3380-COM-001") == 1);
        CHECK(d[0].line == 0);
        CHECK(d[0].start == (int)s.find(';'));
        CHECK(d[0].length == 1);
        CHECK(d[0].severity == DiagSeverity::Error);
        CHECK(d[0].manualPage == 41);
        CHECK(!d[0].message.empty());
    }
    // 小写 / 前面有缩进 / 分号后还有空白 —— 都要能认出来
    CHECK(CountCode(ValidateChromaSource("   set_dec_file \"a.dec\";\n", kind),
                    "C3380-COM-001") == 1);
    CHECK(CountCode(ValidateChromaSource("SET_DEC_FILE \"a.dec\";   \n", kind),
                    "C3380-COM-001") == 1);
    // 多行文本里只报那一行
    {
        const std::vector<Diagnostic> d = ValidateChromaSource(
            "int a = 1;\nSET_DEC_FILE \"a.dec\" ;\nint b = 2;\n", kind);
        CHECK(d.size() == 1 && d[0].line == 1);
    }
}

static void RunPinList() {
    // ---- 正例：手册官方示例整体 0 诊断 ----
    const std::vector<Diagnostic> man =
        ValidateChromaSource(kManualDecExample, ChromaFileKind::Dec);
    for (const Diagnostic& d : man) {
        std::printf("    unexpected: line %d  %s  %s\n", d.line + 1,
                    d.code ? d.code : "?", d.message.c_str());
    }
    CHECK(man.empty());

    // ---- 正例：多块 PIN_LIST 各自独立（不同 loadboard），同名 pin 合法 ----
    {
        const char* two = "PIN_LIST (BRD0) {\n  A = 0 = 1 = IN;\n}\n"
                          "PIN_LIST (BRD1) {\n  A = 8 = 1 = IN;\n}\n";
        CHECK(ValidateChromaSource(two, ChromaFileKind::Dec).empty());
    }
    // ---- 正例：块注释里的条目、非四段写法、缺分号 —— 一律不报 ----
    {
        const char* s = "PIN_LIST (B) {\n"
                        "/* A = 0 = 1 = IN; */\n"
                        "  Weird = nothing here\n"
                        "  A = 0 = 1 = IN\n"
                        "}\n";
        CHECK(ValidateChromaSource(s, ChromaFileKind::Dec).empty());
    }

    // ---- 反例 1：pin 名重复（C3380-DEC-001）----
    {
        const char* s = "PIN_LIST (B) {\n  A = 0 = 1 = IN;\n  B = 2 = 2 = IN;\n  A = 4 = 3 = IO;\n}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(d.size() == 1);
        CHECK(CountCode(d, "C3380-DEC-001") == 1);
        CHECK(d[0].line == 3);
        CHECK(d[0].manualPage == 25);
    }
    // ---- 反例 2：ATE 通道号重复（C3380-DEC-002，多工位 `:` 列表里也要查）----
    {
        const char* s = "PIN_LIST (B) {\n  A = 0 : 288 = 1 = IN;\n  B = 288 = 2 = IN;\n}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(d.size() == 1);
        CHECK(CountCode(d, "C3380-DEC-002") == 1);
        CHECK(d[0].line == 2);
    }
    // ---- 反例 3：DUT pin 号重复（C3380-DEC-003）----
    {
        const char* s = "PIN_LIST (B) {\n  A = 0 = 1 = IN;\n  B = 2 = 1 = IN;\n}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(d.size() == 1);
        CHECK(CountCode(d, "C3380-DEC-003") == 1);
        CHECK(d[0].line == 2);
    }
    // ---- 三条规则同时命中：各自报一次，且按行号升序 ----
    {
        const char* s = "PIN_LIST (B) {\n  A = 0 = 1 = IN;\n  A = 0 = 1 = IO;\n}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(d.size() == 3);
        CHECK(d[0].line == 2 && d[1].line == 2 && d[2].line == 2);
        CHECK(CountCode(d, "C3380-DEC-001") == 1);
        CHECK(CountCode(d, "C3380-DEC-002") == 1);
        CHECK(CountCode(d, "C3380-DEC-003") == 1);
    }
    // ---- 块头与 `{` 分行、`{` 后有空白 —— 都要能进块 ----
    {
        const char* s = "PIN_LIST (B)\n{\n  A = 0 = 1 = IN;\n  A = 2 = 2 = IO;\n}\n";
        CHECK(CountCode(ValidateChromaSource(s, ChromaFileKind::Dec),
                        "C3380-DEC-001") == 1);
    }

    // ================= 资源域（pin_type 分域）=================
    // 唯一性只在**同一资源域内**判定。域 = 手册的 *_ALLPINS 默认组：
    //   {IN, OUT, IO} 一个域（§2.4.1 IO_ALLPINS）；其余每个 pin_type 自成一域。
    // 依据（两条都是实证，不是推断）：
    //   ① §2.5.3 的 UR 官方示例里 UR_C0/UR_C1 复用了信号脚的 ATE 0..7，手册判为正确；
    //   ② 厂商的 GANG 范例工程里功率脚与信号脚共用 dut#、用户继电器脚与信号脚
    //      共用 ATE 通道号，而该工程**编译成功**（编译器生成的 pin 初始化源码把
    //      全部 54 个 pin 原样声明，无去重无报错）。完整取证见项目私密文档。

    // 跨域复用**不报**：手册 §2.5.3 的 UR 示例原样（ATE 0/2 被 IN 与 UR 同时使用）
    {
        const char* s = "PIN_LIST (B) {\n"
                        "  SEL0  = 0 = 1 = IN;\n"
                        "  G1    = 2 = 2 = IN;\n"
                        "  UR_C0 = 0 : 1 : 2 : 3 = = UR;\n"
                        "}\n";
        CHECK(ValidateChromaSource(s, ChromaFileKind::Dec).empty());
    }
    // 跨域复用**不报**：TMU 与 IO 共用 ATE（§2.4.1 Notice：TMU 不得与 IO 同组）
    {
        const char* s = "PIN_LIST (B) {\n"
                        "  A = 5 = 1 = IO;\n"
                        "  T = 5 = = TMU;\n"
                        "}\n";
        CHECK(ValidateChromaSource(s, ChromaFileKind::Dec).empty());
    }
    // 跨域复用**不报**：功率脚 MLDPS 与信号脚共用 dut#（厂商 GANG 样本的形状）
    {
        const char* s = "PIN_LIST (B) {\n"
                        "  SEL0 = 40  = 1 = IO;\n"
                        "  Gnd  =     = 1 = GND;\n"
                        "  Vdd1 = 577 = 1 = MLDPS;\n"
                        "}\n";
        CHECK(ValidateChromaSource(s, ChromaFileKind::Dec).empty());
    }
    // 同域内**仍要报**：MLDPS vs MLDPS 共用 dut#
    {
        const char* s = "PIN_LIST (B) {\n"
                        "  Vdd  = 576 = 0 = MLDPS;\n"
                        "  VddX = 577 = 0 = MLDPS;\n"
                        "}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(CountCode(d, "C3380-DEC-003") == 1);
        CHECK(d[0].line == 2);
    }
    // 同域内**仍要报**：UR vs UR 共用 ATE
    {
        const char* s = "PIN_LIST (B) {\n"
                        "  U1 = 8 = = UR;\n"
                        "  U2 = 8 = = UR;\n"
                        "}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(CountCode(d, "C3380-DEC-002") == 1);
    }
    // IN / OUT / IO 属**同一个域**（IO_ALLPINS）：跨这三个类型仍要报
    {
        const char* s = "PIN_LIST (B) {\n"
                        "  A = 7 = 1 = IN;\n"
                        "  B = 7 = 2 = OUT;\n"
                        "  C = 9 = 3 = IO;\n"
                        "  D = 9 = 4 = IO;\n"
                        "}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(CountCode(d, "C3380-DEC-002") == 2);
    }
    // pin_type 段认不出 → 002/003 一律不报（域未知，宁漏不误）；001 照报
    {
        const char* s = "PIN_LIST (B) {\n"
                        "  A = 0 = 1 = ;\n"
                        "  A = 0 = 1 = IN;\n"
                        "}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(CountCode(d, "C3380-DEC-001") == 1);
        CHECK(CountCode(d, "C3380-DEC-002") == 0);
        CHECK(CountCode(d, "C3380-DEC-003") == 0);
    }
}

static void RunPinGroup() {
    // ---- 正例：手册 §2.4.3 的 PIN_GROUP 段 ----
    {
        const char* s = "PIN_GROUP \n{ \n  CTRL       = CLR+SEL0+SEL1+G1+G2+SL+SR;  \n"
                        "  SEL01      = SEL0+SEL1;  \n"
                        "  QQ         = QA+QB+QC+QD+QE+QF+QG+QH;  \n} \n";
        CHECK(ValidateChromaSource(s, ChromaFileKind::Dec).empty());
    }
    // ---- 正例：手册自己的示例里 SEL0/SEL1 同时属于 CTRL 与 SEL01，
    //      「同一 pin 不得属于两个组」因此**故意不实现**（见头文件）。这条用例
    //      就是防回归的绊线：谁把那条例外规则加回去，这里立刻红。
    CHECK(CountCode(ValidateChromaSource(kManualDecExample, ChromaFileKind::Dec),
                    "C3380-DEC-005") == 0);

    // ---- 反例：组名重复（C3380-DEC-004）----
    {
        const char* s = "PIN_GROUP {\n  G1 = A+B;\n  G2 = C;\n  G1 = D;\n}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, ChromaFileKind::Dec);
        CHECK(d.size() == 1);
        CHECK(CountCode(d, "C3380-DEC-004") == 1);
        CHECK(d[0].line == 3);
        CHECK(d[0].manualPage == 27);
    }
    // ---- UR_PIN_GROUP / POWER_PIN_GROUP 不能被当成 PIN_GROUP 来查 ----
    {
        const char* s = "POWER_PIN_GROUP {\n  X = A;\n  X = B;\n}\n";
        CHECK(ValidateChromaSource(s, ChromaFileKind::Dec).empty());
    }
    // ---- 没有 `=` 或没有 `;` 的行（认不出来）不报 ----
    {
        const char* s = "PIN_GROUP {\n  Weird line\n  G1 = A+B\n}\n";
        CHECK(ValidateChromaSource(s, ChromaFileKind::Dec).empty());
    }
}

// 批次 86：规则 8（参数个数，只对 .pln）。
//
// 【本组最值钱的三处】
//   1) **手册自己的示例**必须 0 诊断。`POWER_DOWN_FAIL_SITE( )`（§4.16，签名是
//      `POWER_DOWN_FAIL_SITE([ mode ])`）就是批次 86 抓出 C++ 漏守卫的那一行：
//      "整个参数表包在方括号里"时必填数必须是 0，不是一个。
//   2) **边界**：只在「恰好越过上限 / 恰好少于必填」时报，卡在两端之间不报。
//   3) **排除表绊线**：`SET_JUDGE_MODE( NORM , FEOP_ON )` 是手册 §4.15 自己的示例，
//      而它 Format 只写了 1 个参数 —— 谁把 .cpp 里的排除表删了，这里立刻红。
static void RunArgumentCount() {
    const ChromaFileKind P = ChromaFileKind::Plan;

    // ---- 正例：整体可选（手册 §4.16 原文写法）----
    CHECK(ValidateChromaSource("POWER_DOWN_FAIL_SITE( );\n", P).empty());
    CHECK(ValidateChromaSource("POWER_DOWN_FAIL_SITE();\n", P).empty());
    CHECK(ValidateChromaSource("POWER_DOWN_FAIL_SITE(NORM);\n", P).empty());

    // ---- 正例：恰好等于必填数 / 恰好等于上限 ----
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(PREF, 1mS);\n", P).empty());                    // 2 = 必填
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(PREF, 1mS, 10, AVE, 50uS);\n", P).empty());    // 5 = 上限
    CHECK(ValidateChromaSource("FORCE_V_DPS(DPS1, 3.3V, @4V, @100mA, 100mA, NORM, ON, 1mS);\n", P).empty());  // 8 = 必填
    CHECK(ValidateChromaSource("FORCE_V_DPS(DPS1, 3.3V, @4V, @100mA, 100mA, NORM, ON, 1mS, 1mS);\n", P).empty());  // 9 = 上限
    CHECK(ValidateChromaSource("FORCE_I_PMU(PMU, 1mA, @1mA, @6V, 6V, ON, 3mS);\n", P).empty());  // 7 = 必填 = 上限（交替写法 `x[|x]`）
    CHECK(ValidateChromaSource("JUDGE_VARIABLE_MS(v, 0, 100);\n", P).empty());               // 3 = 必填
    CHECK(ValidateChromaSource("JUDGE_VARIABLE_MS(v, 0, 100, \"tag\");\n", P).empty());      // 4 = 上限

    // ---- 正例：空槽（手册与真实工程文件都用它占位省略）→ 一律不报 ----
    CHECK(ValidateChromaSource("SET_LEVELN(l, P, 0V, 0.5V, 0.2V, 1.2V, 150uA, -150uA, 0V,,);\n", P).empty());
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(PREF, , 1mS);\n", P).empty());

    // ---- 正例：认不出的形态一律不报 ----
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(PREF,\n  1mS);\n", P).empty());        // 实参表跨行
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(pick_pin(), 1mS);\n", P).empty());     // 实参里有嵌套调用
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(\"a,b\", 1mS);\n", P).empty());        // 字符串里的逗号
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(PREF, 1mS);   // , , ,\n", P).empty()); // 注释里的逗号
    CHECK(ValidateChromaSource("int r = MEAS_I_MLDPS(PREF);\n", P).empty());        // 不在语句起始位置
    CHECK(ValidateChromaSource("MY_HELPER(1, 2, 3);\n", P).empty());               // 不在库里
    CHECK(ValidateChromaSource("#define MEAS_I_MLDPS(a) a\n", P).empty());          // 预处理指令整行不算
    // 歧义签名（`SET_CONST_CURRENT_LOAD([ … ])` 有重复组）→ 跳过
    CHECK(ValidateChromaSource("SET_CONST_CURRENT_LOAD(1, 2, 3);\n", P).empty());

    // ---- 正例：排除表绊线（手册示例 vs 手册 Format 自相矛盾）----
    CHECK(ValidateChromaSource("SET_JUDGE_MODE( NORM , FEOP_ON );\n", P).empty());  // §4.15 示例
    CHECK(ValidateChromaSource("PIN_MODE_HV (G1, NRZ, EDGE, ENABLE, IO_NRZ );\n", P).empty());  // §5.5 示例
    CHECK(ValidateChromaSource("USE_WD_WAVEFORM(Without_LPF,5mS);\n", P).empty());  // §4.23 示例

    // ---- 规则只对 .pln 开 ----
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(PREF);\n", ChromaFileKind::Dec).empty());
    CHECK(ValidateChromaSource("MEAS_I_MLDPS(PREF);\n", ChromaFileKind::Pattern).empty());

    // ---- 反例 1：少于必填（PLN-011，Warning）----
    {
        const std::string s = "MEAS_I_MLDPS(PREF);\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, P);
        CHECK(d.size() == 1);
        CHECK(CountCode(d, "C3380-PLN-011") == 1);
        CHECK(d[0].line == 0);
        CHECK(d[0].start == 0);                    // 高亮范围 = 语句名
        CHECK(d[0].length == 12);                  // strlen("MEAS_I_MLDPS")
        CHECK(d[0].severity == DiagSeverity::Warning);   // 取证较弱 → Warning
        CHECK(d[0].manualPage == 0);               // 0 = 无单一页码，章节号在 message 里
        CHECK(d[0].message.find("4.3") != std::string::npos);
        CHECK(d[0].message.find("2") != std::string::npos);
    }
    // 少一个也算（6 个必填，给了 5 个）
    CHECK(CountCode(ValidateChromaSource("FORCE_I_PMU(PMU, 1mA, @1mA, @6V, 6V, ON);\n", P),
                    "C3380-PLN-011") == 1);
    // 缩进 + 全小写：照样认出来，且列号对得上
    {
        const std::vector<Diagnostic> d =
            ValidateChromaSource("    meas_i_mldps(PREF);\n", P);
        CHECK(d.size() == 1 && d[0].start == 4 && d[0].length == 12);
    }

    // ---- 反例 2：多于上限（PLN-010，Error）----
    {
        const std::string s = "MEAS_I_MLDPS(PREF, 1mS, 10, AVE, 50uS, 3);\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(s, P);
        CHECK(d.size() == 1);
        CHECK(CountCode(d, "C3380-PLN-010") == 1);
        CHECK(d[0].severity == DiagSeverity::Error);     // 手册没定义过这种形式
        CHECK(d[0].length == 12);
        CHECK(d[0].message.find("5") != std::string::npos);
    }
    CHECK(CountCode(ValidateChromaSource("FORCE_I_PMU(PMU, 1mA, @1mA, @6V, 6V, ON, 3mS, 3mS);\n", P),
                    "C3380-PLN-010") == 1);

    // ---- 两档 severity + 行号升序 ----
    {
        const std::vector<Diagnostic> d = ValidateChromaSource(
            "MEAS_I_MLDPS(PREF, 1mS, 10, AVE, 50uS, 3);\nMEAS_I_MLDPS(PREF);\n", P);
        CHECK(d.size() == 2);
        CHECK(d[0].line == 0 && d[0].severity == DiagSeverity::Error);
        CHECK(d[1].line == 1 && d[1].severity == DiagSeverity::Warning);
    }
    // 同一行上两条语句（分号分隔）→ 各报一次
    {
        const std::vector<Diagnostic> d = ValidateChromaSource(
            "MEAS_I_MLDPS(PREF); MEAS_I_MLDPS(PREF);\n", P);
        CHECK(d.size() == 2);
        CHECK(d[0].line == 0 && d[1].line == 0);
        CHECK(d[0].start < d[1].start);
    }
    // CRLF 与 LF 等价
    CHECK(CountCode(ValidateChromaSource("MEAS_I_MLDPS(PREF);\r\n", P), "C3380-PLN-011") == 1);
}

// ---------------------------------------------------------------------------
// 批次 88：规则 3 —— DEC_MODE APAS → IMATCH 失效（跨文件）
//
// 【正例的全部内容都来自公开手册】
//   .dec 侧 = LM §2.2.2 的官方 DEC_MODE APAS 示例（原文照抄，含原文的空格）；
//   .pat 侧 = LM p62 IMATCH 工作示例里的向量行（原文照抄）。
//   与批次 80/86 同一纪律：手册原文必须 0 误报 —— 但这条规则的正例恰恰**是**
//   手册的 IMATCH 用法本身，所以断言的是"恰好每处 IMATCH 一条 Warning"，
//   而**不含** IMATCH 的手册 .dec 示例必须 0 诊断。
// ---------------------------------------------------------------------------

// LM §2.2.2（p23）官方示例，原文照抄。
static const char* kManualApasDec = R"DEC(DEC_MODE  APAS;

PIN_LIST   (LPC_BOARD_00 _2sites) {
 /*name = ATE channel = DUT channel = Type */
p0     =0  : 4    =1     =IO;
p1     =1  : 5     =2     =IO;
p2     =2  : 6     =3     =IO;
p3     =3  : 7    =4     =IO;
p4     = 8  : 12   =5     =IO;
p5     = 9  :  13   =6     =IO;
p6     = 10 : 14   =7     =IO;
p7     = 11 : 15    =8     =IO;
}
)DEC";

// LM p62 IMATCH 工作示例的向量行（原文照抄两行，微指令 IMATCH）。
static const char* kManualImatchPat = R"PAT(*1 01 00 1 X1 XXXXXXXX XH*TS2,IMATCH;
*1 01 00 1 X0 XXXXXXXX XL*TS2,IMATCH;//Shift L signal
)PAT";

static void RunCrossFileApas() {
    // ---- FindDecFileRefs：提取与宽容 ----
    {
        const std::vector<DecFileRef> r =
            FindDecFileRefs("SET_DEC_FILE \".\\ls299_pin.dec\"\n");
        CHECK(r.size() == 1);
        if (r.size() == 1) {
            CHECK(r[0].line == 0);
            CHECK(r[0].path == ".\\ls299_pin.dec");
            CHECK(r[0].length == (int)r[0].path.size());
        }
    }
    CHECK(FindDecFileRefs("SET_DEC_FILE \"./PAT/FW_ls299_4sites_pin.dec\"\n").size() == 1);
    CHECK(FindDecFileRefs("SET_DEC_FILE \"a.dec\" ;\n").size() == 1);   // COM-001 照报，引用照提
    CHECK(FindDecFileRefs("SET_DEC_FILE \"a.dec\"\nSET_DEC_FILE \"b.dec\"\n").size() == 2);
    CHECK(FindDecFileRefs("SET_DEC_FILE a.dec\n").empty());             // 没引号
    CHECK(FindDecFileRefs("SET_DEC_FILE \"\"\n").empty());              // 空路径
    CHECK(FindDecFileRefs("set_dec_file \"a.dec\"\n").size() == 1);     // 大小写不敏感
    CHECK(FindDecFileRefs("// SET_DEC_FILE \"a.dec\"\n").empty());      // 行注释
    CHECK(FindDecFileRefs("/* SET_DEC_FILE \"a.dec\" */\n").empty());   // 块注释
    CHECK(FindDecFileRefs("#define SET_DEC_FILE \"a.dec\"\n").empty()); // 预处理
    CHECK(FindDecFileRefs("X_SET_DEC_FILE \"a.dec\"\n").empty());       // 不是它

    // ---- DecDeclaresApas：声明与歧义 ----
    CHECK(DecDeclaresApas("DEC_MODE  APAS;\n"));
    CHECK(DecDeclaresApas("dec_mode apas ;\n"));
    CHECK(DecDeclaresApas(kManualApasDec));
    CHECK(!DecDeclaresApas("DEC_MODE NORM;\n"));
    CHECK(!DecDeclaresApas("/* DEC_MODE APAS; */\n"));                  // 注释里的声明
    CHECK(!DecDeclaresApas("// DEC_MODE APAS;\n"));
    CHECK(!DecDeclaresApas("DEC_MODE;\n"));                             // 认不出 → 不算
    CHECK(!DecDeclaresApas("X_DEC_MODE APAS;\n"));                      // 不是它
    CHECK(!DecDeclaresApas(""));                                        // 读不到内容
    // 歧义：APAS 与 NORM 同时声明 → 宁漏不误
    CHECK(!DecDeclaresApas("DEC_MODE APAS;\nDEC_MODE NORM;\n"));

    // ---- CheckApasImatch：正例（手册 IMATCH 用法，每处一条 Warning）----
    {
        const std::vector<std::string> decs{kManualApasDec};
        const std::vector<Diagnostic> d = CheckApasImatch(kManualImatchPat, decs);
        CHECK(CountCode(d, "C3380-XFILE-001") == 2);
        if (CountCode(d, "C3380-XFILE-001") == 2) {
            CHECK(d[0].severity == DiagSeverity::Warning);
            CHECK(d[0].manualPage == 44);
            CHECK(d[0].line == 0);
            CHECK(d[0].start == 30);          // `*1 01 00 1 X1 XXXXXXXX XH*TS2,` 之长
            CHECK(d[0].length == 6);
            CHECK(d[1].line == 1);
        }
    }
    // 大小写不敏感也能命中（钉住行为，防止将来悄悄变成大小写敏感）
    CHECK(CountCode(CheckApasImatch("x*ts2,imatch;\n",
                                    {std::string("DEC_MODE APAS;\n")}),
                    "C3380-XFILE-001") == 1);

    // ---- CheckApasImatch：反例（宁漏不误的每一道闸）----
    CHECK(CheckApasImatch(kManualImatchPat, {}).empty());                       // 一个 .dec 都没读到
    CHECK(CheckApasImatch(kManualImatchPat, {kManualDecExample}).empty());      // 官方 .dec 无 DEC_MODE
    CHECK(CheckApasImatch(kManualImatchPat, {"DEC_MODE NORM;\n"}).empty());
    CHECK(CheckApasImatch(kManualImatchPat, {"DEC_MODE APAS;\nDEC_MODE NORM;\n"}).empty());
    CHECK(CheckApasImatch("// IMATCH in comment\n", {"DEC_MODE APAS;\n"}).empty());
    CHECK(CheckApasImatch("/* IMATCH */\n", {"DEC_MODE APAS;\n"}).empty());
    CHECK(CheckApasImatch("# IMATCH tail\n", {"DEC_MODE APAS;\n"}).empty());
    CHECK(CheckApasImatch("SET_TITLE(\"IMATCH\");\n", {"DEC_MODE APAS;\n"}).empty()); // 字符串里
    CHECK(CheckApasImatch("*1 01 00 1 X1 X*TS2,IMATCHX;\n", {"DEC_MODE APAS;\n"}).empty());
    CHECK(CheckApasImatch("*1 01 00 1 X1 X*TS2,X_IMATCH;\n", {"DEC_MODE APAS;\n"}).empty());
    CHECK(CheckApasImatch("", {"DEC_MODE APAS;\n"}).empty());

    // ---- 与规则 8 的合流：.pln 里 IMATCH 一样报（真实工程 IMATCH 只在向量里，
    //      .pln 命中 = 写了不该写的东西，Warning 同样成立）----
    {
        const std::vector<std::string> decs{std::string("DEC_MODE APAS;\n")};
        const std::vector<Diagnostic> d =
            CheckApasImatch("SET_DEC_FILE \"./x.dec\"\nIMATCH (R1);\n", decs);
        CHECK(CountCode(d, "C3380-XFILE-001") == 1);
        if (CountCode(d, "C3380-XFILE-001") == 1) CHECK(d[0].line == 1);
    }
    // .dec 里 IMATCH 一词不应触发（IMATCH 是向量微指令，规则只对引用方开 ——
    // 宿主只对 Plan/Pattern 调 CheckApasImatch，内核层面靠调用方约定）
}

// ---------------------------------------------------------------------------
// 批次 89：ExtractDecSymbols —— 跨文件补全的词源（正例全部取自公开手册）
// ---------------------------------------------------------------------------

// 手册 §2.7.2.1 的 TIME_NAME_DEF 示例（原文照抄，含行尾注释）。
static const char* kManualTimeNameDef = R"DEC(TIME_NAME_DEF
{
TM1= 1; // Define timing set 1 name is "TM1" for Test plan ( *.pln file )use;
}
)DEC";

static void RunExtractDecSymbols() {
    // 官方 .dec 示例：17 个 pin + 3 个 I/O 组 + 1 个电源组 = 21 个符号，文件序。
    {
        const std::vector<std::string> s = ExtractDecSymbols(kManualDecExample);
        CHECK(s.size() == 21);
        if (s.size() == 21) {
            CHECK(s[0] == "SEL0");            // 文件序（第一条 pin）
            CHECK(s[16] == "Vdps");           // 最后一个 pin
            CHECK(s[17] == "CTRL");           // PIN_GROUP 三条
            CHECK(s[18] == "SEL01");
            CHECK(s[19] == "QQ");
            CHECK(s[20] == "DPS_OS_PINS");    // POWER_PIN_GROUP
        }
        // 去重：同一符号出现两次只进一次
        std::string twice = std::string(kManualDecExample) + "PIN_GROUP \n{ \n X2 = QA; \n} \n";
        const std::vector<std::string> s2 = ExtractDecSymbols(twice);
        int nQA = 0, nX2 = 0;
        for (const std::string& w : s2) {
            if (w == "QA") ++nQA;
            if (w == "X2") ++nX2;
        }
        CHECK(nQA == 1 && nX2 == 1);
    }
    // LM §2.2.2 的 APAS 示例：p0..p7 共 8 个 pin；DEC_MODE 行不是块，不进
    {
        const std::vector<std::string> s = ExtractDecSymbols(kManualApasDec);
        CHECK(s.size() == 8);
        if (s.size() == 8) {
            CHECK(s[0] == "p0" && s[7] == "p7");
        }
    }
    // TIME_NAME_DEF（§2.7 格式 `time_name = no;`）—— 行尾注释里的 "TM1" 不重复计
    {
        const std::vector<std::string> s = ExtractDecSymbols(kManualTimeNameDef);
        CHECK(s.size() == 1);
        if (s.size() == 1) CHECK(s[0] == "TM1");
    }
    // UR_PIN_GROUP 与 PIN_GROUP 同等对待
    CHECK(ExtractDecSymbols("UR_PIN_GROUP\n{\n UREL1 = A+B;\n}\n").size() == 1);
    // 宽容：没分号 / 认不出的块 —— 跳过；畸形行（`B 0 = 1 = IO;`）的行首
    // 标识符 "B" **会**进候选 —— 补全是低风险场景，多一个无害候选只是噪声
    //（口径见头文件：比诊断宽）。
    {
        const std::vector<std::string> s =
            ExtractDecSymbols("PIN_LIST (B) {\n A = 0 = 1 = IO\n B 0 = 1 = IO;\n}\n");
        CHECK(s.size() == 1);
        if (s.size() == 1) CHECK(s[0] == "B");
    }
    CHECK(ExtractDecSymbols("PIN_GROUP {\n = A;\n 123 = A;\n}\n").empty());
    CHECK(ExtractDecSymbols("DEC_MODE APAS;\nDEVICE { X = 1; }\n").empty());  // 非符号块
    // 块注释里的条目不算（kManualDecExample 的注释行无 `;` 结尾，这里显式验证）
    CHECK(ExtractDecSymbols("PIN_GROUP {\n/* GHOST = A; */\nREAL = A;\n}\n").size() == 1);
    // 未闭合块：不崩、不报
    CHECK(ExtractDecSymbols("PIN_GROUP {\n G1 = A;\n").empty());
    CHECK(ExtractDecSymbols("").empty());
    // 长度上限：手册 §2.7 time_name max 64 chars → 65 个字符的标识符不进
    {
        std::string longName(65, 'A');
        const std::string dec = "PIN_GROUP {\n " + longName + " = A;\n}\n";
        CHECK(ExtractDecSymbols(dec).empty());
        longName.resize(64);
        const std::string dec64 = "PIN_GROUP {\n " + longName + " = A;\n}\n";
        CHECK(ExtractDecSymbols(dec64).size() == 1);
    }
}

static void RunCrossKind() {
    // SET_DEC_FILE 的规则不适用 .dec（.dec 的 DEC_MODE 反而必须有分号）
    CHECK(ValidateChromaSource("SET_DEC_FILE \"a.dec\" ;\n", ChromaFileKind::Dec).empty());
    // .dec 的规则不适用 .pln
    CHECK(ValidateChromaSource("PIN_LIST (B) {\n A = 0 = 1 = IN;\n A = 2 = 2 = IO;\n}\n",
                               ChromaFileKind::Plan).empty());
    // 空文本 / 只有换行
    CHECK(ValidateChromaSource("", ChromaFileKind::Plan).empty());
    CHECK(ValidateChromaSource("", ChromaFileKind::Dec).empty());
    CHECK(ValidateChromaSource("\r\n\r\n", ChromaFileKind::Dec).empty());
    // CRLF 与 LF 结果一致
    {
        const std::string lf  = "PIN_LIST (B) {\nA = 0 = 1 = IN;\nA = 2 = 1 = IO;\n}\n";
        std::string crlf = lf;
        for (std::size_t p = crlf.find('\n'); p != std::string::npos;
             p = crlf.find('\n', p + 2)) {
            crlf.insert(p, 1, '\r');
        }
        const std::vector<Diagnostic> a = ValidateChromaSource(lf, ChromaFileKind::Dec);
        const std::vector<Diagnostic> b = ValidateChromaSource(crlf, ChromaFileKind::Dec);
        CHECK(a.size() == b.size());
        CHECK(a.size() == 2);                       // DUT pin 号 1 重复 + pin 名 A 重复
        if (a.size() == b.size()) {
            for (std::size_t i = 0; i < a.size(); ++i) {
                CHECK(a[i].line == b[i].line && a[i].start == b[i].start);
                CHECK(std::string(a[i].code) == std::string(b[i].code));
            }
        }
    }
    // 未闭合的块：不崩、不报
    CHECK(ValidateChromaSource("PIN_LIST (B) {\n A = 0 = 1 = IN;\n", ChromaFileKind::Dec).empty());
    CHECK(ValidateChromaSource("PIN_GROUP {\n G1 = A;\n", ChromaFileKind::Dec).empty());
    // 未闭合的块注释：不崩、不报
    CHECK(ValidateChromaSource("/*\nSET_DEC_FILE \"a.dec\" ;\n", ChromaFileKind::Plan).empty());
}

// ===========================================================================
// 规则 1（.pat）：HEADER pin 数 == 向量数据宽度   —— C3380-PAT-001
//
// 【本组用例的重点：把手册自己的 .pat 示例全部回放，一条都不许判红】
//   规则 1 的明文依据只在培训教材 p45，而语言手册 §3.4.1.2 里有两个 3360 时代的
//   示例按字面不满足一一对应。所以"闸 B（`%` 分组结构同形）"不是保守起见，
//   是**必需**的：下面 kManualPatternSymbolExample 两条就是钉它的绊线。
// ===========================================================================
static void RunHeaderVectorWidth() {
    // ---- 手册 §3.3.2 示例原文（16 pin ↔ 16 字符；HEADER 跨两行）→ 必须 0 诊断
    static const char* kManualHeaderExample = R"PAT(SET_DEC_FILE "./ls299_16sites_pin.dec" 
HEADER   CLR,%SEL0,SEL1,%G1,G2,%CLK,%SL,SR,%QA,QB,QC, 
          QD,QE,QF,QG,QH;  
SPM_PATTERN  (os_pat) {  
os_st::    *0 00 00 0 00 00000000 *TS15; 
               *0 00 00 0 00 00000000 *; 
               *0 00 00 0 00 00000000 * RPT 100; 
               *Z 00 00 0 00 00000000 *; 
} 
)PAT";
    CHECK(CountCode(ValidateChromaSource(kManualHeaderExample, ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 0);

    // ---- ⚠️ 绊线：手册 §3.4.1.2 的两个 3360 示例 —— 4 项 vs 7 组 / 5 组
    //      按"一一对应"字面读会判红，闸 B 必须把它们拦住（组数不等）。
    //      谁把闸 B 删了，这里立刻红。
    static const char* kManualPatternSymbolExample = R"PAT(SET_DEC_FILE "./3360_ls299_pin.dec" 
HEADER  CTRL1, %CLK, %QQ, %OAH;  
SPM_PATTERN(func_pat) { 
 sfr_st:   *1  01  00  1  X1  HLLLLLLL  HL*TS1; 
         *1  01  00  1  X1  HHLLLLLL  HL*; 
} 
)PAT";
    CHECK(CountCode(ValidateChromaSource(kManualPatternSymbolExample,
                                        ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 0);
    static const char* kManualHexExample = R"PAT(HEADER  CTRL1, %CLK, %QQ, %OAH;  
SPM_PATTERN(func_pat) { 
 sfr_st:   *dA0  1  X1  c80  c8*TS1; 
         *dA0  1  X1  cC0  c8*; 
} 
)PAT";
    CHECK(CountCode(ValidateChromaSource(kManualHexExample, ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 0);

    // ---- 无 `%` 的 HEADER + 无空格的向量（真实 .pat 的形态）→ 0 诊断
    {
        std::string t = "SET_DEC_FILE \"./x.dec\"\nHEADER ";
        for (int i = 0; i < 34; ++i) t += (i ? "," : "") + std::string("P") + std::to_string(i);
        t += ";\nSPM_PATTERN(m)\n{\n  *";
        t += std::string(34, '0');
        t += "*;\n  *";
        t += std::string(34, 'X');
        t += "*;\n}\n";
        CHECK(ValidateChromaSource(t, ChromaFileKind::Pattern).empty());
    }

    // ---- 正例：宽度不符 → 恰好 1 条，锚在**第一条**不符的向量行，范围=数据段
    {
        static const char* kBad = R"PAT(SET_DEC_FILE "./x.dec"
HEADER A,B,C,D;
SPM_PATTERN(m)
{
  *0101*;
  *010*;
  *011*;
}
)PAT";
        const std::vector<Diagnostic> d =
            ValidateChromaSource(kBad, ChromaFileKind::Pattern);
        CHECK(d.size() == 1);
        if (d.size() == 1) {
            CHECK(std::string(d[0].code) == "C3380-PAT-001");
            CHECK(d[0].line == 5);                 // 0-based：第一条不符的向量行
            CHECK(d[0].start == 3);                // `  *010*;` 里数据段首字符
            CHECK(d[0].length == 3);
            CHECK(d[0].severity == DiagSeverity::Warning);
            CHECK(d[0].manualPage == 41);
            CHECK(d[0].message.find("HEADER") != std::string::npos);
        }
    }

    // ---- 每个模块只报一条：5000 条不符的向量行 → 仍只有 1 条诊断
    {
        std::string t = "HEADER A,B,C,D;\nSPM_PATTERN(m)\n{\n";
        for (int i = 0; i < 5000; ++i) t += "  *010*;\n";
        t += "}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(t, ChromaFileKind::Pattern);
        CHECK(d.size() == 1);
        if (d.size() == 1) CHECK(d[0].line == 3);
    }

    // ---- 两个模块各自不符 → 2 条（按模块分别提示）
    {
        static const char* kTwo = R"PAT(HEADER A,B,C;
SPM_PATTERN(m1) {
  *01*;
}
SPM_PATTERN(m2) {
  *0101*;
}
)PAT";
        const std::vector<Diagnostic> d = ValidateChromaSource(kTwo, ChromaFileKind::Pattern);
        CHECK(d.size() == 2);
        if (d.size() == 2) {
            CHECK(d[0].line == 2);
            CHECK(d[1].line == 5);
        }
    }

    // ---- 闸 A：歧义 / 未闭合 / 非标识符项 → 一律不报
    CHECK(ValidateChromaSource("HEADER A,B;\nHEADER C,D;\nSPM_PATTERN(m) {\n *0*;\n}\n",
                               ChromaFileKind::Pattern).empty());          // 两条 HEADER
    CHECK(ValidateChromaSource("HEADER A,B\nSPM_PATTERN(m) {\n *0*;\n}\n",
                               ChromaFileKind::Pattern).empty());          // 缺分号
    CHECK(ValidateChromaSource("HEADER [%]pin_name, x;\nSPM_PATTERN(m) {\n *0*;\n}\n",
                               ChromaFileKind::Pattern).empty());          // Format 行
    CHECK(ValidateChromaSource("HEADER A,B C;\nSPM_PATTERN(m) {\n *0*;\n}\n",
                               ChromaFileKind::Pattern).empty());          // 条目含空格
    CHECK(ValidateChromaSource("HEADER A,,B;\nSPM_PATTERN(m) {\n *0*;\n}\n",
                               ChromaFileKind::Pattern).empty());          // 空条目
    CHECK(ValidateChromaSource("HEADER A,B;\n", ChromaFileKind::Pattern).empty()); // 无块
    CHECK(ValidateChromaSource("HEADER A,B;\nSPM_PATTERN(m) {\n *0*;\n",
                               ChromaFileKind::Pattern).empty());          // 块未闭合

    // ---- 闸 B：向量有空格但 HEADER 无 `%` → 结构不同形，不比较（宁可漏报）
    CHECK(ValidateChromaSource("HEADER A,B,C;\nSPM_PATTERN(m) {\n *0 00*;\n}\n",
                               ChromaFileKind::Pattern).empty());
    // 反向：HEADER 有 `%` 而向量无空格 → 同样拦下
    CHECK(ValidateChromaSource("HEADER A,%B,C;\nSPM_PATTERN(m) {\n *000*;\n}\n",
                               ChromaFileKind::Pattern).empty());
    // 组数相同但分布不同、总宽不符 → 报（这是真实的映射错位）
    // ⚠️ 这条同时是**编译器哨兵**：`*00 0 0*` 的"3 组"是唯一能区分"正确按空白分组"
    //    与"只数了非空白字符总数"的输入 —— 后者会得到 1 组，被闸 B 拦掉 → 0 诊断。
    //    MSVC 14.51（v145）在 /O2 下曾把 VectorShape 的前身误编译成后者，正是这条
    //    抓住的。若这条**只在 Release 下**变红，先怀疑编译器，别改期望值。
    CHECK(CountCode(ValidateChromaSource(
                        "HEADER A,%B,%C;\nSPM_PATTERN(m) {\n *00 0 0*;\n}\n",
                        ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 1);

    // ---- 分组结构同形 + 宽度一致 → 不报（§3.3.1 的 `%` 语义正例）
    CHECK(ValidateChromaSource(
              "HEADER A,%B,C,%D,E;\nSPM_PATTERN(m) {\n *0 00 0 00*TS1;\n}\n",
              ChromaFileKind::Pattern).empty());

    // ---- `#`（ape_field）行不参与比较
    CHECK(ValidateChromaSource("HEADER A,B,C,D;\nSPM_PATTERN(m) {\n *01 # P1 *;\n}\n",
                               ChromaFileKind::Pattern).empty());

    // ---- 标签前缀 `Label::` / `Label:` 都能认出来
    CHECK(CountCode(ValidateChromaSource(
                        "HEADER A,B,C;\nSPM_PATTERN(m) {\n  g1:: *0101*;\n}\n",
                        ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 1);
    CHECK(CountCode(ValidateChromaSource(
                        "HEADER A,B,C;\nSPM_PATTERN(m) {\n  l1: *0101*;\n}\n",
                        ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 1);

    // ---- APM_PATTERN / RPM_PATTERN 同样覆盖（培训教材 p45 三者并列）
    CHECK(CountCode(ValidateChromaSource(
                        "HEADER A,B;\nAPM_PATTERN(m) {\n *010*;\n}\n",
                        ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 1);
    CHECK(CountCode(ValidateChromaSource(
                        "HEADER A,B;\nRPM_PATTERN(m) {\n *010*;\n}\n",
                        ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 1);
    // 带可选参数（NORM/DBL）的模块头也要认
    CHECK(CountCode(ValidateChromaSource(
                        "HEADER A,B;\nSPM_PATTERN(m, DBL, K_SET) {\n *010*;\n}\n",
                        ChromaFileKind::Pattern),
                    "C3380-PAT-001") == 1);

    // ---- 只对 .pat 开：同样的文本按 .pln / .dec 走不得出这条规则
    CHECK(CountCode(ValidateChromaSource("HEADER A,B;\nSPM_PATTERN(m) {\n *010*;\n}\n",
                                         ChromaFileKind::Plan),
                    "C3380-PAT-001") == 0);
    CHECK(CountCode(ValidateChromaSource("HEADER A,B;\nSPM_PATTERN(m) {\n *010*;\n}\n",
                                         ChromaFileKind::Dec),
                    "C3380-PAT-001") == 0);

    // ---- 注释与字符串里的 `*` / HEADER 不参与（抹平层）
    CHECK(ValidateChromaSource("HEADER A,B,C,D;\nSPM_PATTERN(m) {\n // *010*;\n"
                               " /* *010*; */\n  *0101*;\n}\n",
                               ChromaFileKind::Pattern).empty());
    CHECK(ValidateChromaSource("# HEADER A,B;\nSPM_PATTERN(m) {\n *0*;\n}\n",
                               ChromaFileKind::Pattern).empty());

    // ---- 畸形文件保护：模块数超上限时输出被截断（不刷爆面板）
    //      注意必须只有**一条** HEADER，否则闸 A 会整体放弃（见上面的歧义用例）。
    {
        std::string t = "HEADER A,B;\n";
        for (int i = 0; i < 70; ++i) t += "SPM_PATTERN(m) {\n *010*;\n}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(t, ChromaFileKind::Pattern);
        CHECK(d.size() == 64);
    }

    // ---- CRLF 与 LF 结果一致（真实 .pat 是 CRLF）
    {
        const std::string lf = "HEADER A,B,C,D;\nSPM_PATTERN(m)\n{\n  *010*;\n}\n";
        std::string crlf = lf;
        for (std::size_t p = crlf.find('\n'); p != std::string::npos;
             p = crlf.find('\n', p + 2)) {
            crlf.insert(p, 1, '\r');
        }
        const std::vector<Diagnostic> a = ValidateChromaSource(lf, ChromaFileKind::Pattern);
        const std::vector<Diagnostic> b = ValidateChromaSource(crlf, ChromaFileKind::Pattern);
        CHECK(a.size() == 1);
        CHECK(a.size() == b.size());
        if (a.size() == b.size() && a.size() == 1) {
            CHECK(a[0].line == b[0].line);
            CHECK(a[0].start == b[0].start);
            CHECK(a[0].length == b[0].length);
        }
    }
}

// ---------------------------------------------------------------------------
// 规则 4（SPM_PATTERN 的 NORM / DBL 配 K_SET / Z_SET）—— **绊线：必须恒为 0**
// ---------------------------------------------------------------------------
// 批次 92 曾按手册 §3.4.1.4（p45）的"正误对照"实现为 `C3380-PAT-002`（Error 档）。
// 2026-09-20 在装有厂商工具链的机器上实测**推翻了那条断言**：把厂商范例工程里某个
// `SPM_PATTERN (func_pat) {` 改成手册点名的那一行
// `SPM_PATTERN (func_pat, NORM, K_SET) {` 之后，工程自己的 makefile 构建 4 步全
// exit=0，patcmp 打印 `Errors : 0   Warning : 0`，生成的 `.pdt` 与**未改动时逐字节
// 相同**，整构建成功。⇒ 编译器不执行这条检查；照它报错就是在编译器接受的代码上
// 标红。故规则**不实现**（也不降级为 Warning —— 被否掉的是断言本身，不是作用域）。
//
// 本函数是**绊线**（同「同一 pin 不得属于两个 group」的 DEC-005 先例）：把手册点名
// 的那几行钉死在"0 诊断"上。谁把规则加回来，这里会红 ——
// **红的时候先回去读 .cpp 里该节的取证记录，别直接改断言。**
//
// ⚠️ 绊线必须有区分力：最后一段**同时**塞了一条仍然生效的规则（`RPT` 次数越界 →
// `C3380-PAT-003`），用来证明"校验器真的跑了"。否则 `== 0` 在"规则被删掉"与
// "规则正确地沉默"两种情况下输出一模一样，那种 0 没有信息量。
static void RunPatternModeTripwire() {
    const ChromaFileKind P = ChromaFileKind::Pattern;

    // ---- 手册 §3.4.1.4 点名 error 的两行 + 点名 correct 的一行：全部 0 诊断
    //      （第一行是**实测**被编译器接受的；另两行同属那张已被推翻的表）
    for (const char* s : {
             "SPM_PATTERN ( func_pat , NORM , K_SET )",            // 手册：error（实测：接受）
             "SPM_PATTERN ( func_pat , DBL , Z_SET )",             // 手册：error
             "SPM_PATTERN ( func_pat , DBL_2X , K_SET | Z_SET )",  // 手册：correct
             "SPM_PATTERN ( func_pat , NORM , NORM_SET )",         // 手册默认值
             "SPM_PATTERN ( func_pat , NORM )",                    // 只给 mode
             "SPM_PATTERN ( func_pat )",                           // 只给模块名
             "SPM_PATTERN (os_pat)",                               // §3.4.1.5 示例原文
         }) {
        CHECK(CountCode(ValidateChromaSource(s, P), "C3380-PAT-002") == 0);
    }

    // ---- 真实模块形态（手册点名的那一行 + 块体）：规则 4 沉默，
    //      但同一段里的 RPT 越界必须仍然报出来 —— 证明校验器确实跑了
    {
        const std::string t =
            "SPM_PATTERN ( func_pat , NORM , K_SET ) {\n"
            "  *00000000*RPT 1;\n"
            "  *00000000*;\n"
            "}\n";
        const std::vector<Diagnostic> d = ValidateChromaSource(t, P);
        CHECK(CountCode(d, "C3380-PAT-002") == 0);   // 规则 4 沉默（编译器也接受）
        CHECK(CountCode(d, "C3380-PAT-003") == 1);   // 正控：规则 10 仍然开火
    }
}

// ---------------------------------------------------------------------------
// 批次 92 规则 10：RPT 的重复次数必须在 2 .. 16777215
// ---------------------------------------------------------------------------
static void RunRptCount() {
    const ChromaFileKind P = ChromaFileKind::Pattern;

    // 不带 HEADER → 规则 1 静默；`SPM_PATTERN(m)` 只有 1 个实参 → 规则 4 也静默。
    // 于是 d.size() 就是规则 10 的条数，可以整表断言。
    auto mod = [](const std::vector<std::string>& vecs) {
        std::string t = "SPM_PATTERN(m) {\n";
        for (const std::string& v : vecs) t += "  " + v + "\n";
        return t + "}\n";
    };
    auto one = [&](const std::string& vec) {
        return ValidateChromaSource(mod({vec}), P);
    };

    // ---- 手册 §3.4.3（p53）官方示例原文（含 RPT 7 / RPT 6 / RPT 7）→ 0 诊断。
    //      这是最值钱的一条：手册自己的示例必须干净。
    {
        const std::vector<Diagnostic> d = ValidateChromaSource(
            R"PAT(SPM_PATTERN (rpt_pat) {//RPT from 2 to 16777215
  rpt_st::         *0 X0 00 X XX LLLLLLLL LL*TS1;
                  *1 01 00 1 X1 XXXXXXXX HL*RPT 7;
                  *1 01 00 1 X1 HHHHHHHH HH* ;
                 *1 01 00 1 X0 LHHHHHHH LH*;
                  *1 01 00 1 X1 XXXXXXXX HH*RPT 6;
                  *1 01 00 1 X1 HHHHHHHL HL*;
                  *1 01 00 1 X1 HHHHHHHH HH*;
                  *1 01 00 1 X0 XXXXXXXX LH*RPT 7;
                  *1 01 00 1 X0 LLLLLLLL LL*;
}
)PAT",
            P);
        CHECK(d.empty());
    }

    // ---- 边界：2 与 16777215 合法（闭区间）；0 / 1 / 16777216 起越界
    for (const char* v : {"2", "3", "16777214", "16777215"}) {
        CHECK(one(std::string("*00000000* RPT ") + v + ";").empty());
    }
    for (const char* v : {"0", "1", "16777216", "16777217", "99999999"}) {
        const std::vector<Diagnostic> d = one(std::string("*00000000* RPT ") + v + ";");
        CHECK(d.size() == 1);
        if (d.size() == 1) {
            CHECK(std::string(d[0].code) == "C3380-PAT-003");
            CHECK(d[0].severity == DiagSeverity::Warning);   // 硬件容量，非语法错误
            CHECK(d[0].manualPage == 44);
            CHECK(d[0].length == (int)std::string(v).size()); // 高亮覆盖整个数字
            CHECK(d[0].message.find(v) != std::string::npos);
        }
    }

    // ---- 位置：锚在数字本身（模块头是第 0 行，向量是第 1 行）
    {
        const std::string vec = "*00000000* RPT 1;";
        const std::vector<Diagnostic> d = one(vec);
        CHECK(d.size() == 1);
        if (d.size() == 1) {
            CHECK(d[0].line == 1);
            CHECK(d[0].length == 1);
            // 直接断言"被高亮的字节就是那个数字"，不硬编列号
            const std::string line = "  " + vec;   // mod() 缩进两格
            CHECK(line.substr((std::size_t)d[0].start, (std::size_t)d[0].length) == "1");
        }
    }

    // ---- 位数 > 8 直接按越界处理（不做整数转换，防溢出）
    CHECK(CountCode(one("*00000000* RPT 100000000;"), "C3380-PAT-003") == 1);
    CHECK(CountCode(one("*00000000* RPT 999999999999999999999;"), "C3380-PAT-003") == 1);

    // ---- 认不出就不报：整词 / 进制 / 非字面量
    for (const char* tail : {
             "*00000000* RPTN 1;",       // 寄存器版重复，是**另一条**微指令
             "*00000000* TS_RPT 1;",     // 不是独立 token
             "*00000000* RPT 0x1;",      // 十六进制 → 认不出（宁漏不误）
             "*00000000* RPT 1H;",
             "*00000000* RPT 1FF;",
             "*00000000* RPT X0;",       // 寄存器索引，不是字面量
             "*00000000* RPT 100x;",     // 数字后面还接标识符字符
             "*00000000* RPT;",          // 根本没给次数
         }) {
        CHECK(one(tail).empty());
    }

    // ---- 绊线：只有一个 `*` 的残缺行 —— "最后一个 `*` 之后"其实是**数据区**，
    //      扫它等于在向量数据里找 RPT。必须有 ≥2 个 `*` 才扫。
    CHECK(one("*00000000 RPT 1;").empty());

    // ---- 注释已抹平：注释里的 RPT 不参与
    CHECK(one("*00000000*; // RPT 1").empty());
    CHECK(one("*00000000*; /* RPT 1 */").empty());

    // ---- 模块外（没有 SPM/APM/RPM_PATTERN 块）→ 不看
    CHECK(CountCode(ValidateChromaSource("*00000000* RPT 1;\n", P), "C3380-PAT-003") == 0);

    // ---- 一行只报第一条 RPT（宁少不多）：同一行两个越界 RPT → 1 条
    CHECK(CountCode(one("*00000000* RPT 1; RPT 0;"), "C3380-PAT-003") == 1);

    // ---- 畸形文件保护：100 条越界 → 只报 64 条
    {
        std::vector<std::string> v;
        for (int i = 0; i < 100; ++i) v.push_back("*00000000* RPT 1;");
        CHECK(ValidateChromaSource(mod(v), P).size() == 64);
    }

    // ---- CRLF 与 LF 必须同判
    {
        const std::string lf = mod({"*00000000* RPT 1;"});
        std::string crlf = lf;
        for (std::size_t i = 0; i < crlf.size(); ++i) {
            if (crlf[i] == '\n') { crlf.insert(i, "\r"); ++i; }
        }
        const std::vector<Diagnostic> a = ValidateChromaSource(lf, P);
        const std::vector<Diagnostic> b = ValidateChromaSource(crlf, P);
        CHECK(a.size() == 1 && b.size() == 1);
        if (a.size() == 1 && b.size() == 1) {
            CHECK(a[0].line == b[0].line);
            CHECK(a[0].start == b[0].start);
            CHECK(a[0].length == b[0].length);
        }
    }
}

// 可选入口：`test_chromadiag <文件> [--dec <dec文件>]…` —— 把任意工程文件当
// .pln/.dec/.pat 扫一遍；`--dec` 可多次给出"已被宿主读出的被引用 .dec"，让规则 3
// 也能对真实文件对回归。
// 存在的理由：**真实文件才是零误报的最终证据**，而 temp/ 下的真实样例是私密文件
// （被 .gitignore 排除，CI 上不存在），不能写进上面的断言里。所以把它做成一个可
// 手动调用的入口：命中诊断返回 1，干净返回 0，方便脚本化回归。
static int ScanFile(int argc, char** argv) {
    const char* path = argv[1];
    std::vector<std::string> decTexts;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) != "--dec") continue;
        std::ifstream in(argv[i + 1], std::ios::binary);
        if (!in) {
            std::printf("cannot open --dec: %s\n", argv[i + 1]);
            return 2;
        }
        decTexts.emplace_back((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("cannot open: %s\n", path);
        return 2;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    const ChromaFileKind kind = FileKindFromPath(path);
    std::vector<Diagnostic> d = ValidateChromaSource(text, kind);
    if (!decTexts.empty()) {
        const std::vector<Diagnostic> cross = CheckApasImatch(text, decTexts);
        d.insert(d.end(), cross.begin(), cross.end());
        std::stable_sort(d.begin(), d.end(), [](const Diagnostic& a, const Diagnostic& b) {
            if (a.line != b.line) return a.line < b.line;
            return a.start < b.start;
        });
    }
    std::printf("%s  kind=%d  bytes=%zu  decs=%zu  diagnostics=%zu\n", path, (int)kind,
                text.size(), decTexts.size(), d.size());
    for (const Diagnostic& x : d) {
        std::printf("  line %d col %d  %s [p%d]  %s\n", x.line + 1, x.start + 1,
                    x.code ? x.code : "?", x.manualPage, x.message.c_str());
    }
    return d.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1) return ScanFile(argc, argv);
    std::printf("== test_chromadiag ==\n");
    RunFileKind();
    RunSetDecFile(ChromaFileKind::Plan);
    RunSetDecFile(ChromaFileKind::Pattern);
    RunPinList();
    RunPinGroup();
    RunArgumentCount();
    RunHeaderVectorWidth();
    RunPatternModeTripwire();
    RunRptCount();
    RunCrossFileApas();
    RunExtractDecSymbols();
    RunCrossKind();
    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
