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

// 可选入口：`test_chromadiag <文件>` —— 把任意工程文件当 .pln/.dec/.pat 扫一遍。
// 存在的理由：**真实文件才是零误报的最终证据**，而 temp/ 下的真实样例是私密文件
// （被 .gitignore 排除，CI 上不存在），不能写进上面的断言里。所以把它做成一个可
// 手动调用的入口：命中诊断返回 1，干净返回 0，方便脚本化回归。
static int ScanFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("cannot open: %s\n", path);
        return 2;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    const ChromaFileKind kind = FileKindFromPath(path);
    const std::vector<Diagnostic> d = ValidateChromaSource(text, kind);
    std::printf("%s  kind=%d  bytes=%zu  diagnostics=%zu\n", path, (int)kind,
                text.size(), d.size());
    for (const Diagnostic& x : d) {
        std::printf("  line %d col %d  %s [p%d]  %s\n", x.line + 1, x.start + 1,
                    x.code ? x.code : "?", x.manualPage, x.message.c_str());
    }
    return d.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1) return ScanFile(argv[1]);
    std::printf("== test_chromadiag ==\n");
    RunFileKind();
    RunSetDecFile(ChromaFileKind::Plan);
    RunSetDecFile(ChromaFileKind::Pattern);
    RunPinList();
    RunPinGroup();
    RunCrossKind();
    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
