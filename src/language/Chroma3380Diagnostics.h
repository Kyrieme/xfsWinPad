#pragma once
// xfsWinPad - Chroma 3380 静态校验内核（批次 80 起，方向 C）
//
// 【当前进度】批次 80 = 规则 2/5/6（.dec 侧）；批次 86 = 规则 8（.pln 侧参数个数）；
//   批次 87 = 诊断 UI 接线；批次 88 = 规则 3（跨文件 DEC_MODE APAS → IMATCH 失效）；
//   批次 91 = 规则 1（.pat 侧 HEADER pin 数 == 向量宽度，靠闸 B 做到零误报）；
//   批次 92 = 规则 4/10（.pat 侧 SPM_PATTERN 模式组合、RPT 次数区间）；
//   批次 93 = **收窄** DEC-002/003 为按 pin 资源域判定（拿到厂商编译过的真实语料后
//   发现原先的全局判定会误报，详见下方【DEC-002 / DEC-003 为什么必须按资源域判定】）。
//   内核是纯函数，产出的诊断列表由调用方决定怎么展示；跨文件规则需要宿主先把
//   被引用的 .dec 读出来传进来（内核零 IO）。
//
// 【为什么要有这一层】
//   Chroma 官方工作流是「TextPad 编辑 → Makefile 调 plncmp/patcmp → 看编译结果页
//   点击跳转」（操作手册 §2.5），也就是说**错误只有在编译之后才看得到**。把这件
//   事提前到「边写边标」，是与官方工具链相比唯一真正有增量价值的方向。
//   官方构建脚本里的真实命令（2026-09-19 从厂商范例工程取证，变量名即官方原名）：
//     plncmp $(PLN_CFLAGS) <name>.pln          # .pln → .pin，并生成中间 C++ 源码
//     patcmp -c -s $(PAT_CFLAGS) <name>.pat    # .pat → .pdt
//     patcmp $(PAT_LFLAGS) -o<out> -f <lst>    # .pdt → .ppo
//   其中 `.pln` 目标依赖同目录的 `.dec`，即 **plncmp 会读 .dec**。
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
//   Chroma3380Db.h      —— 语句/参数/候选值数据库。规则 8（参数个数）经
//                          `FindStatement` 取手册签名；**但不用 kParams 的参数槽表**
//                          做必填性判断（实测槽表不可信，理由见 .cpp 里规则 8 一节）。
//   Chroma3380Complete.h —— 复用 `IsStatementStart`，与补全共用同一个"语句起始"口径，
//                          避免"补全认它是语句、校验不认"这种两套口径打架。
//   本模块只回答「这段文本有哪些**确定**的错」。
//
// 【manualPage 的取值约定】
//   1..N = 手册（Language Manual）PDF 页码；**0 = 没有单一页码**。
//   规则 8 的依据是"该语句自己的 Format 块"，不是某一页，所以取 0，
//   章节号走文案参数（形如「手册 §4.5 签名最多 9 个参数」）。

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
    Error,    // 手册明文说会报错（`An error will occur …`），或手册从未定义过这种写法
    Warning,  // 依据较弱：手册用希望/建议语气、依据是散文措辞、或需要跨文件才能确认
};

// 诊断文案的 id（批次 134）。
//
// 【为什么是 id 而不是 std::string message】
//   本层是**内核**，不引 I18n.h（分层硬约束，与 CraftRunner::RunNote /
//   BigFileModel::Err 同口径）：这里只说"是哪一条、带哪些参数"，人类可读的短句
//   由 UI 侧按 id 查语言键（src/app/MainWindow.cpp 的 kDiagMsgKeys[]），参数按
//   顺序填进文案里的 {0} {1} …。
//   若直接把中文写死成本层的 std::string，换语言不会跟着变 —— 而 R6/R7
//   （scripts/check-lang-keys.py）正会把这种"绕过 Tr() 的硬编码文案"报红。
//
// 【参数约定】args 是 UTF-8 文本，条数与顺序与语言键里的占位符一一对应。
//   数字也按文本传（内核不负责本地化数字格式）。
enum class DiagMsgId {
    None = 0,                 // 无文案（不该出现；UI 兜底显示规则码）
    SetDecFileNoSemicolon,    // COM-001 无参
    DuplicatePinName,         // DEC-001 {0}=pin 名 {1}=首次定义行（1-based）
    DuplicateAteChannel,      // DEC-002 {0}=ATE 通道号 {1}=首次定义行
    DuplicateDutPin,          // DEC-003 {0}=DUT pin 号 {1}=首次定义行
    DuplicatePinGroup,        // DEC-004 {0}=pin_group 名 {1}=首次定义行
    TooManyArgs,              // PLN-010 {0}=语句名 {1}=§节号 {2}=签名上限 {3}=实给个数
    TooFewArgs,               // PLN-011 {0}=语句名 {1}=§节号 {2}=必填个数 {3}=实给个数
    VectorWidthMismatch,      // PAT-001 {0}=向量宽度 {1}=HEADER 行 {2}=声明的 pin 数
    VectorWidthMismatchMany,  // PAT-001 多行变体：同上 + {3}=本模块不符行数
    RptOutOfRange,            // PAT-003 {0}=RPT 次数（原样文本）
    ImatchApasConflict,       // XFILE-001 无参
};

struct Diagnostic {
    int          line       = 0;      // 0-based 行号（与 SCI_GETCURLINE 同口径）
    int          start      = 0;      // 行内起始列（字节，0-based）
    int          length     = 0;      // 覆盖字节数，供下划线/高亮用
    DiagSeverity severity   = DiagSeverity::Error;
    const char*  code       = nullptr; // 稳定标识，UI 与测试都按它断言
    int          manualPage = 0;       // 手册（Language Manual）1-based PDF 页
    DiagMsgId    msgId      = DiagMsgId::None;  // 文案 id（UI 侧查语言键）
    std::vector<std::string> args;              // 文案参数（UTF-8），见 DiagMsgId 注释
};

// 规则清单（每条都标了取证位置）：
//
//   C3380-PLN-001  SET_DEC_FILE 末尾不能有分号                        p41 §3.3.2
//   C3380-DEC-001  PIN_LIST：同一个 pin 名定义了多次                 p25 §2.3.2
//   C3380-DEC-002  PIN_LIST：同一**资源域**内同一个 ATE 通道号定义了多次  p25 §2.3.2 + §2.4.1/§2.5.1 分域
//   C3380-DEC-003  PIN_LIST：同一**资源域**内同一个 DUT pin 号定义了多次  p25 §2.3.2 + §2.4.1/§2.5.1 分域
//   C3380-DEC-004  PIN_GROUP：同一个 pin_group 名定义了多次          p27 §2.4.2
//   C3380-PLN-010  实参个数多于手册签名的上限                       各语句 Format 块（Error）
//   C3380-PLN-011  实参个数少于手册签名的必填项                     各语句 Format 块 + `No entry: illegal`（Warning）
//   C3380-PAT-001  向量数据宽度 != HEADER 声明的 pin 个数            培训教材 p45 注意事项 2 + LM §3.3/§3.4.1.2（Warning）
//   C3380-PAT-002  SPM_PATTERN 用 NORM/DBL 配 K_SET/Z_SET             LM p45 §3.4.1.4（Error）
//   C3380-PAT-003  RPT 的重复次数不在 2 .. 16777215                   LM p44 §3.4.1.3（Warning）
//   C3380-XFILE-001 引用的 .dec 声明 DEC_MODE APAS 时使用 IMATCH    LM p44 §3.4.1.3 + p62 注意 4 + 培训教材 p43（Warning）
//
// 取证原文（§2.3.2）：
//   "An error will occur if the same DUT or ATE pin numbers are defined more than
//    once." / "An error will occur if the same pin name is given to more than one
//    DUT pin.  Pin names must be unique within the device definition."
//
// 【DEC-002 / DEC-003 为什么必须**按资源域**判定（2026-09-19 收窄，附两条实证）】
//   上面那句原文**不能按字面全局执行** —— 会误报。证据两条，都不是推断：
//
//   ① 手册 §2.5.3（p29）自己的 UR 官方示例就让 UR 脚复用信号脚的 ATE 号：
//        SEL0   =  0 : 288 : 320 : 352  =  1  =  IN  ;
//        G1     =  2 : 290 : 322 : 354  =  2  =  IN  ;
//        …
//        UR_C0  =  0 : 1  :  2  :   3   =     =  UR  ;
//        UR_C1  =  4 : 5  :  6  :   7   =     =  UR  ;
//      ATE 0/1/2/3 同时属于 SEL0/SEL1/G1/SL，手册把这段判为**正确**写法。
//      ⇒ UR 与信号脚不在同一个号段空间里。
//
//   域怎么划（手册自己的默认组就是域的划分，§2.4.1 / §2.5.1 / §5.4.1）：
//     IO_ALLPINS    = { IN, OUT, IO }   ← 这三者**同域**，跨它们仍要报
//     MXTMU_ALLPINS = { TMU }           ← §2.4.1 Notice 明文："TMU pin-type &
//                                          pin_group can not be assigned in the
//                                          same IO_ALLPINS"
//     UR_ALLPINS    = { UR }
//     MLDPS_ALLPINS = 功率脚（MLDPS / DPS / UVI / PREF）
//     其余类型（GND / TRG / EXT / WG / WD）各自成域。
//   厂商编译器生成的 pin 初始化源码里确实只声明这四个 `*_ALLPINS` 默认组，
//   与该划分吻合。
//
//   结论：**同一号码定义多次只在同一域内才是错误**；跨域复用是合法且常见的
//   （多站点 pin 列表、功率脚模块编号、用户继电器复用通道）。这与内核纪律一致
//   —— 宁可少报，不可误报。
//
//   注意 DEC-001（pin 名重复）**不分域**：手册说的是 "Pin names must be unique
//   within the device definition"，且真实语料里跨域同名从未出现，无证据支持分域。
//
//   （上面两条实证的完整取证 —— 含样本工程名、编译记录、生成源码路径 ——
//     记在项目私密文档里，不写进代码：公开仓库的读者无从打开那些文件。）
//
// 【PLN-010 / PLN-011 为什么一档 Error 一档 Warning】
//   手册**没有**"参数个数不对就报错"的明文（全文 `An error will occur` 只出现 2 次，
//   都属 .dec 的 pin 规则）。这两条的取证是较弱的兩处：各语句 Format 块（调用形式的
//   规范定义）＋ 必填参数的 `No entry: illegal` 措辞。因此：
//     · 多于签名 = 手册从未定义过这种形式 → Error；
//     · 少于必填 = 依据是散文措辞，且手册有"看似必填、实可省略"的**明文例外**
//       （JUDGE 族 min/max：*Omitting the parameter is possible*）→ Warning。
//
// 【PLN-010 / PLN-011 的覆盖面】
//   只对 `.pln` 开。判定只用在"签名无歧义 + 推导上限 == paramCount"的语句上
//   （实测 309 条里 229 条可用），另有 10 条因**手册自己的示例与 Format 矛盾**
//   而被排除 —— 排除了就不报，宁可漏报。`.pat` 不覆盖这两条：能落进受检集的
//   只有 `APM_PATTERN` / `SPM_PATTERN` 两条，而它们的参数表本身带可选方括号，
//   边际价值为零；`.pat` 侧另有一条独立规则 C3380-PAT-001（规则 1，向量宽度）。
//
// 【规则 1 为什么能在"没有 .dec"的前提下开口】
//   它不需要符号表：只比较"HEADER 里逗号分隔的项数"与"向量行两个 `*` 之间的
//   非空白字符数"。判定被三条闸夹住（见 .cpp 的 CheckHeaderVectorWidth 注释），
//   其中闸 B（`%` 分组结构与向量空格结构同形）是必须的 —— 少了它，手册
//   §3.4.1.2 自己那两个 3360 示例、以及 scripts/chroma-e2e.ps1 的 P11 fixture
//   都会被判错。
//   ⚠️ 实现里 VectorShape 的"按空白分组"刻意用标准库（count_if + find_first_of）
//   而不是逐字符循环：MSVC 14.51（v145）的 /O2 会把逐字符版本误编译成"只数
//   非空白字符"，Release 下静默漏报。动手改那段之前先读 .cpp 的函数注释。】
//
// 【规则 4 为什么**不**开口（2026-09-20 实测推翻）】
//   批次 92 曾照手册 §3.4.1.4（p45）实现 `C3380-PAT-002`（Error 档）—— 立规理由是
//   该节用**正误对照**明文点名 `compiler error`，是全手册少见的"明说会报错"之处：
//     SPM_PATTERN ( func_pat , NORM , K_SET | Z_SET )     compiler error
//     SPM_PATTERN ( func_pat , DBL , K_SET | Z_SET )      compiler error
//     SPM_PATTERN ( func_pat , DBL_2X , K_SET | Z_SET )   compiler correct
//   **但实测该断言不成立**：取一个厂商范例工程（`.pln` 与全部 `.pat` 都是原始字节），
//   只把某个 `SPM_PATTERN (func_pat) {` 改成手册点名的那一行
//   `SPM_PATTERN (func_pat, NORM, K_SET) {`，再跑工程自己的 makefile 构建 ——
//   4 个步骤全部 exit=0、patcmp 打印 `Errors : 0   Warning : 0`、生成的 `.pdt` 与
//   未改动时**逐字节相同**、整构建 `allOk: yes`。
//   ⇒ 手册这句话在 CRAFT 2.50 的 `patcmp` 上不执行。照它报错就是在**编译器接受的
//   代码**上标红，正是"零误报 > 多报"要禁止的。故**不实现**（也不降级为 Warning：
//   被否掉的是断言本身，不是作用域）。取证全文在 .cpp 同节。
//   ⚠️ 本次只测了 {NORM} × {K_SET} 一种；另外三种点名组合未测。将来要重新开口，
//   必须先拿到**编译器真的报错**的样本，别凭手册散文恢复。
//
// 【规则 10 为什么是 Warning（而不是 Error）】
//   手册 §3.4.1.3（p44）微指令表与 §3.4.3（p53）两处都写
//   `RPT times 2 <= N <= 16777215 (24bit register)`。上界来自 24bit 寄存器，
//   是**硬件容量**而非语法约束；手册没有"越界即 compiler error"的明文，越界更
//   可能是运行时被截断 / 行为未定义。故取 Warning。
//   【只认十进制字面量】十六进制写法（`0x10` / `20H` / `FF`）里的字母会撞上
//   "数字后面还跟着标识符字符"的判定 → 认不出 → 不报。这是刻意的宁漏不误：
//   同一串纯数字按十六进制解释**只会比十进制更大**，所以"十进制已越界"的结论
//   在两种进制下都成立（不误报）；被漏掉的只是十六进制越界，可接受。
//   【只在最后一个 `*` 之后扫】`*` 之间是 pattern_data 数据区，不参与。
//   且要求该行**至少两个 `*`** —— 只有一个说明这行残缺，那时"最后一个 `*` 之后"
//   其实是数据区，扫它等于在向量数据里找 RPT。
//   【RPT 必须整词】`RPTN`（寄存器版重复，§3.4.1.3 里另一条微指令）与 `TS_RPT`
//   这类都不算；`RPT` 之后必须紧跟十进制字面量（`RPT 100x` 认不出 → 不报）。
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

// ---------------------------------------------------------------------------
// 批次 88：跨文件规则（规则 3 —— DEC_MODE APAS 下 IMATCH 失效）
//
// 【取证】三处出处互相印证：
//     · LM p44 §3.4.1.3 微指令表：IMATCH 标注 "(only for normal mode pattern)"；
//     · LM p62 IMATCH 用法注意事项第 4 条："Only support normal mode pattern."；
//     · 培训教材 p43：DEC_MODE APAS 时 IMATCH 功能失效。APAS 由 .dec 的
//       `DEC_MODE APAS;` 声明开启（操作手册 §4.4.3.2.2 佐证它是运行时开关，
//       "if there is \"DEC_MODE APAS\" declaration in dec file"）。
//   「normal mode」即 DEC_MODE NORM（默认值，LM §2.2：不声明就是 NORM）。
//
// 【severity 为什么是 Warning】
//   手册没有"写了就报错"的明文（`An error will occur` 全文只 2 处，都属 .dec 的
//   pin 规则）；失效是**运行时行为**（IMATCH 永不命中，测试静默出错）而不是语法
//   错误；且需要跨文件才能确认。
//
// 【为什么这条开口（跨文件，误报面曾经被高估）】
//   本规则只做三件事的**合取**，每一步都可独立证伪：
//     1) 引用的 .dec **真的在磁盘上找到并读出**（宿主负责；找不到 = 静默跳过）；
//     2) 该 .dec 里解析出**无歧义的** `DEC_MODE APAS`（抹平注释/字符串后行首
//        标识符 DEC_MODE + 整词 APAS；同时出现 NORM 声明视为歧义，不报）；
//     3) 本文件的 IMATCH 是**整词独立 token**（注释/字符串已抹平）。
//   三条同时成立才报。真实 .pln 回归 0 命中（其引用的 .dec 不在扫描机上，
//   走第 1 条的静默跳过）。真实工程里 IMATCH 只出现在向量文件，.pln 侧命中
//   属于"写了不该写的东西"，报 Warning 同样成立。
//
// 【职责边界不变】内核仍然零 IO —— 宿主解析 SET_DEC_FILE 的相对路径、读盘，
//   把**读到的 .dec 文本**（可能多个、可能一个都没有）传进来。
struct DecFileRef {
    int line = 0;        // SET_DEC_FILE 所在行（0-based）
    int start = 0;       // 路径首字符的行内列（字节，不含引号）
    int length = 0;      // 路径字节数
    std::string path;    // 引号内的原始字节（不做编码转换，编码归宿主管）
};

// 提取 SET_DEC_FILE 引用的路径。宽容：行首标识符不是 SET_DEC_FILE、没有成对
// 双引号、路径为空 —— 一律跳过。`#` 行首注释与块/行注释内的 SET_DEC_FILE
// 不会命中（抹平层负责）。
std::vector<DecFileRef> FindDecFileRefs(const std::string& text);

// ---------------------------------------------------------------------------
// 批次 146：跨文件补全第二段 —— `.pat` 的 label → `.pln` 的 JUDGE_PAT 实参
//
// 【.pln 侧到底消费什么（取证：真实工程 9 处 LOAD_PAT + 全部 JUDGE_PAT 调用点）】
//     LOAD_PAT("./PAT/ls299_pat.ppo");            <- 取的是**编译产物路径串**，不是符号
//     JUDGE_PAT(OS_st, OS_sp);                    <- 取的是 .pat 模块里的 label
//     JUDGE_PAT(scan_func_pat, __scan_func_pat);
//     JUDGE_PAT(fun_78_125K:st, fun_78_125K:sp);  <- 另一种写法 module:label
//   所以「.pat → .pln」的符号消费点是 **JUDGE_PAT 的实参**，不是 LOAD_PAT 的参数。
//
// 【本批只覆盖「裸名」形态，`module:label` 只覆盖到 module 那一半 —— 有意为之】
//   真实语料核对（7 份 .rpt，对应 6 个工程）：`JUDGE_PAT(X, __X)` 裸名形态的实参 X
//   基本全部命中 .rpt 的 Label Name 表 —— SCAN 12/12、open_short 15/15、
//   Normal_DBL 10/10、MCP9600 6/6、GANG 2/2；ALPG 18/20（缺的 `ALPG_Walking`/
//   `__ALPG_Walking` 属下面「陈旧 .rpt」那种情况）。`module:label` 形态（仅 AD7760
//   的 `fun_78_125K:st` 一类）里，module 这一半命中（它本身就是 Offset 0 的
//   Label Name），而 `st`/`sp` 这种**模块内 label** 在 .rpt 里**没有**
//   （AD7760.rpt 的 Label Name 表只有 6 条模块级标签）—— 它们只存在于 .pat 源码
//   （写作单冒号 `st:`）与 CRAFT 的 `.label` 文件里。位置感知地补 `module:` 之后的
//   label 是**后续段**，本批不做，也就不会给错候选。
//
// 【.rpt 可能陈旧 —— 词源固有风险，已记为已知代价】
//   ALPG 工程样本里 `.rpt` 的 Label Name 表与 `.pln`/`.label` 不一致（.rpt 有
//   `os_st`/`os_sp`，而当前 `.label` 里是 `ALPG_Walking`/`__ALPG_Walking`），说明
//   CRAFT 报告可能对应**上一次编译**。取 .rpt 为词源就会跟着陈旧：少给当前构建里
//   有的名字（漏）或多给已删的名字（多）。补全场景下这仍优于完全不补，故接受。
//
// 【词源两条路：.rpt 优先、.pat 兜底】
//   · `.rpt`（CRAFT 编译报告）首选：它是 CRAFT 实际编出的 label 表，补出来的名字
//     CRAFT 一定认得（零误报面）；配对规则「<dir>/<stem>.ppo → <dir>/<stem>/<stem>.rpt」
//     在 8 个真实工程样本上 100% 成立。缺 .rpt（工程没编译过）时该路静默无候选。
//   · `.pat` 源文件兜底：`.pat` 与 `.ppo` **不是**一一对应（`ls299_pat.ppo` 实由
//     `ls299_func.pat` + `ls299_func_scan1.pat` 编成，见 .rpt 末尾的 `File :` 行），
//     所以兜底只能收该目录下**全部** .pat 的 label —— 会多给当前 .ppo 里并不存在的
//     候选。这是**有意**取舍：宁可多给，也不让没编译过的工程完全没有补全。
//
// 【为什么只解析 Label Name 就够（不必再解析 Module Name 行）】
//   JUDGE_PAT 里当实参用的模块名，在 .rpt 里**本身就是一条 Label Name**（Offset 0）——
//   真实 AD7760.rpt 第 32 行 `fun_78_125K` 既是模块名也是 label，而 .pln 正写
//   `JUDGE_PAT(fun_78_125K:st, …)`。少一条解析规则就少一处分位面。
struct PatFileRef {
    int line = 0;        // LOAD_PAT 所在行（0-based）
    int start = 0;       // 路径首字符的行内列（字节，不含引号）
    int length = 0;      // 路径字节数
    std::string path;    // 引号内的原始字节（不做编码转换，编码归宿主管）
};

// 提取 LOAD_PAT 引用的路径。宽容口径与 FindDecFileRefs 完全一致：行首标识符不是
// LOAD_PAT、没有成对双引号、路径为空 —— 一律跳过。不判扩展名（内核只抽引用，
// 「这个路径能不能推出 .rpt / 该扫哪个目录的 .pat」是宿主的路径知识）。
std::vector<PatFileRef> FindPatFileRefs(const std::string& text);

// .dec 文本里是否声明了**无歧义的** DEC_MODE APAS。同时出现 APAS 与 NORM
// 声明视为歧义 → false（宁漏不误）。
bool DecDeclaresApas(const std::string& decText);

// 规则 3 主体：任一被引用 .dec 声明了 APAS 时，找本文件的 IMATCH 整词使用，
// 每处给一条 Warning（C3380-XFILE-001，highlight 覆盖 IMATCH 六个字节）。
// decTexts 为空（一个 .dec 都没读到）直接返回空 —— 宁漏不误的第一道闸。
std::vector<Diagnostic> CheckApasImatch(const std::string& text,
                                        const std::vector<std::string>& decTexts);

// ---------------------------------------------------------------------------
// 批次 89：跨文件补全的词源 —— 从 .dec 文本抽取符号（标识符）清单
//
// 【抽取范围】手册 §2.1.1 列出的 5 个符号承载块：
//     PIN_LIST          → pin 名（条目首段）
//     PIN_GROUP / UR_PIN_GROUP / POWER_PIN_GROUP → 组名（`=` 左侧）
//     TIME_NAME_DEF     → 时序名（`time_name = no;`，§2.7）
//   手册 §2.7.2 的官方示例正是这条链路的样子：.dec 定义 Vdps / TM1，
//   .pln 里 FORCE_V_DPS(Vdps, …) 消费 —— 补全把"要翻回 .dec 查名"省掉。
//
// 【容差与用途】这是**补全**词源，不是诊断：多进一个无害候选只是列表噪声，
//   漏进一个才是体验损失，所以口径比诊断宽（不要求条目四段齐全，只认
//   「块内、`;` 结尾、`=` 左侧是标识符」）。跨行条目漏抽（宽容方向不变）。
//   名字去重、按文件顺序返回；长度上限 64 字符（手册 §2.7 对 time_name 的上限）。
std::vector<std::string> ExtractDecSymbols(const std::string& decText);

// ---------------------------------------------------------------------------
// 批次 104：同一份抽取，但**带上位置** —— 「转到定义」用
//
// 【为什么不另写一套抽取】名字集合必须与 ExtractDecSymbols **完全一致**：
//   补全能给出的候选，跳转就该跳得到；反过来，跳得到的东西补全也该认得。
//   两套并列实现迟早分叉（改一处忘一处，而且分叉是静默的），所以反过来做 ——
//   `ExtractDecSymbols` 现在是本函数的"只取名字"视图，抽取逻辑只有一份。
//
// 【line/col 的口径】line 是 **1-based** 行号（编辑器行号口径），col 是
//   **0-based** 列偏移，指向名字首字符。列之所以可信：抽取跑在 BlankComments
//   之后的文本上，而它**逐字节保长度**（注释抹成空格、字符串内部抹成空格、
//   引号保留），所以抹平后的列号在原行上同样成立 —— 可以直接拿去选中。
//
// 【去重口径与 ExtractDecSymbols 相同】同名只记**第一次**出现的位置。
//   `.dec` 里同名重复声明本身该由诊断去说，不是这里的事。
//
// 【批次 106：lineText —— 那一行的**原文**】
//   状态栏的「定义」提示要显示用户眼前那一行（`MCLK = 40 = 1 = IO;  //CLK`），
//   而不是我们抹平过的版本。位置与行文本必须**同源**，否则提示里的行号和内容
//   可能指向不同的行 —— 所以这里不是另开一次行扫描，而是拿 lines[nameLine] 从
//   传入的原文里直接切。lineText 已去掉首尾空白（含行尾 \r，SplitLines 已剥），
//   中间原样保留（含注释与对齐空格）。
struct DecSymbolLoc {
    std::string name;
    int line = 0;   // 1-based
    int col  = 0;   // 0-based，名字首字符
    std::string lineText;   // 该行原文（去首尾空白）；name 就在其中的 col 处
};
std::vector<DecSymbolLoc> ExtractDecSymbolLocations(const std::string& decText);

// ---------------------------------------------------------------------------
// 批次 146：跨文件补全第二段的两支抽取（词源两条路的取舍见上方 PatFileRef 一节）
// ---------------------------------------------------------------------------

// 从 CRAFT 编译报告（.rpt）文本里抽 `Label Name :` 行的名字 —— **首选**词源。
//
// 【格式与那个必须绕开的坑】
//     `Label Name :func_pat       Module Name : func_pat       Offset : 0`
//   字段左对齐、空格补齐，且**可能完全没有间隙**：真实样本 ls299_pat.rpt 第 70 行
//     `Label Name :scan_9thFail_patModule Name : scan_9thFail_patOffset : 0`
//   所以**不能**用「切到第一个空白」取名字（那会得到 `scan_9thFail_patModule`），
//   必须切到字面量 `Module Name`。标签只可能是标识符（不含空格），不会误切。
//
// 【容差口径】这是**补全**词源，不是诊断：多一个候选只是列表噪声，所以不校验
//   Offset / 段落位置等其它字段；但名字本身仍按标识符口径校验（1~64 字符、
//   IsIdStart 开头、全 IsIdChar），绝不把明显不是标识符的东西塞进候选。
//   同名只记第一次；按文件顺序返回。
std::vector<std::string> ExtractRptLabels(const std::string& rptText);

// 从 `.pat` 源文本里抽**行首** label 定义 —— **兜底**词源（.rpt 缺席时用）。
//
// 【规则】跳过空白后的**行首**标识符 + 可选空白 + `::`。真实样本里既有紧贴的
//   `OS_st::*X XX…*`，也有冒号前带空格的 `iil_st ::*1 11…*`，两种都要认。
//   限定「行首」是因为 .pat 里行首 `IDENT::` 只有 label 一种含义（该语言没有
//   别的作用域语法；向量行以 `*` 起头、HEADER 行不含 `::`）。
//
// 【已知不足：真实 .pat 里单冒号 `name:` 更常见，本批**不认**】
//   真实语料里 ls299 系用双冒号 `OS_st::`，而 AD7760 / ALPG 系用**单冒号**
//   `st:`、`wadd_10:`、`BF_ST:`（且模块内 label 不进 .rpt）。本批只认 `::`，
//   故对这些工程的 .pat 兜底几乎抽不到东西。之所以不顺手把单冒号也认下：单冒号
//   的误报面还没查清（比如别处的 `Ident:` 结构），而本批主路（.rpt）已能覆盖
//   裸名形态，兜底抽不到只会"少给"，符合"宁可少报不可错报"。单冒号 + 位置感知
//   补 `module:` 之后的 label 一并留给后续段。
//
// 【为什么还要跑在抹平层上】真实样本 SCAN_tutorial/PAT/ls299_func_scan1.pat 里
//   `*0 00 00 0 00 LLLLLLLL *; //sfr_st::` 这类**注释里的 `label::`** 出现 8 次。
//   它们都在行尾（行首是 `*`），光靠"行首"规则就已经被挡掉了。真正需要抹平层
//   的是**块注释 / `#` 行注释**：`/* …\n   sfr_st::\n */` 里那一行在原文上与真
//   定义**同形**，只认原文就会凭空多一个候选（真实语料 ALPG 的 `func.pat` 里
//   就有块注释内的 `History:`，但那是单冒号；双冒号的这种写法当前语料没出现，
//   但这正是"宁可少报"该防的）。抹平层逐字节保长度地把这类注释整段抹成空格，
//   从根上排除；代价是每行一次小分配，兜底路径上可接受。
std::vector<std::string> ExtractPatLabels(const std::string& patText);

// ---------------------------------------------------------------------------
// 批次 147：跨文件补全第三段 —— `module:label` 的**位置感知**补全
//
// 【补的是哪一半】批次 146 把 `.pat` 侧的 label 灌进 `.pln` 的词汇补全，但只覆盖
//   「裸名」形态。真实语料里还存在 `JUDGE_PAT(fun_78_125K:st, fun_78_125K:sp)`
//   这种 `module:label` 写法（module = SPM_PATTERN 名，label = 该 pattern 块内定义的
//   label）。这两半是完全不同的名空间：`st`/`sp` 这类**模块内** label **不进**
//   `.rpt` 的 Label Name 表（真实 AD7760.rpt 只有 6 条模块级标签），只存在于 `.pat`
//   源码里，且写作**单冒号** `st:`（ls299 系则写双冒号 `OS_st::`）。
//
// 【为什么要位置感知】把全部模块内 label 不加区分地灌进词汇表，会在任意位置弹出
//   别的模块的 label —— 对 `.pln` 读者是纯噪声。`module:` 之后该给的只有该模块的
//   label，所以候选集必须**按作用域收窄**，这要求补全侧知道光标前的 `module:` 上下文。
//
// 【本层只做两件纯文本事】① 从 .pat 文本解出 `module → 块内 label`；② 从「光标前
//   的文本」解出作用域名。读盘、路径解析、弹窗都在宿主 / Editor 侧（与批次 146 同分层）。
// ---------------------------------------------------------------------------
struct PatModule {
    std::string              name;    // SPM/APM/RPM_PATTERN 的首个实参
    std::vector<std::string> labels;  // 该块内行首 label（去重、保序）
};

// 从 `.pat` 源文本解出每个 pattern 块的名字与其**块内** label。
//
// 【块头】行首标识符为 SPM_PATTERN / APM_PATTERN / RPM_PATTERN（手册 §3.4 明文；
//   RPM_PATTERN 是旧名，MCP9600 的 .pat 注释写明已被 SPM_PATTERN 取代，但仍认）。
//   模块名取紧跟其后的 `(` 内第一个标识符（`SPM_PATTERN(func_run_DBL, DBL)` 的
//   第二实参是模式，不是名字）。仅在**大括号深度为 0** 时认块头 —— 于是没有 `{ }`
//   的畸形写法也不会把后续内容误挂到上一个模块上。
//
// 【块内 label】大括号深度 ≥ 1 的行上取**行首** 标识符 + 可选空白 + **单冒号**
//   （`st:`、`sp :`）。跑在 BlankComments 抹平层上，于是块注释里的 `History:`（真实
//   MCP9600/func.pat 存在）不会混进来。「行首 + 至少一层大括号」两道闸把向量行
//   （以 `*` 起头）、HEADER 行、以及 `[XA:0,…]` 里的冒号（不在行首）全部排除。
//   真实语料逐条核对：AD7760 的 `st:`/`sp :`/`AA:`、ALPG 的 `wadd_10:`/`BF_ST:`/
//   `WK_ST:` 全部命中，无一误抽。
//
// 【批次 148 修正：双冒号 `IDENT::` **不算**模块内 label】这是本批最重要的一处口径
//   修正。`.pat` 的两种定义语法对应**两个不同名空间**，真实语料两侧都验证过：
//     · `IDENT:`  （单冒号）= **模块内** label → `.pln` 必须写 `module:IDENT`
//       实证：AD7760 的 `st:`/`sp:` 只出现在 `JUDGE_PAT(fun_78_125K:st, …)`；且
//       `AD7760.rpt` 的 Label Name 表**只有** 6 条模块级标签，`st`/`sp` 一个都没有。
//     · `IDENT::` （双冒号）= **计划级/全局** label → `.pln` 直接写裸名 `IDENT`
//       实证：ls299 的 `OS_st::`/`clr_st::` 在 `.pln` 里写成 `JUDGE_PAT(OS_st, OS_sp)`、
//       `JUDGE_PAT(clr_st, load_sp)`（**全语料 0 处** `::` 出现在 `.pln`）；且这些名字
//       **都**进了 `.rpt` 的 Label Name 表。
//   于是把 `::` 也当模块内 label 会给出 `func_pat:OS_st` 这种**永远编译不过**的候选
//   （`OS_st` 是全局名，不能也不该被模块限定）—— 属"错报"，必须排除。全局 label 由
//   `ExtractPatLabels`（只认 `::`）与 `.label` 文件走**平铺**词表，不归这里管。
//
// 【去重与顺序】模块内 label 去重保序；模块按出现顺序排列（同名模块由宿主合并）。
std::vector<PatModule> ExtractPatModules(const std::string& patText);

// text[0, pos) 上，若 pos 紧跟在 `IDENT:` 之后，返回 IDENT；否则空串。
//
// 【只认单冒号、且冒号必须紧贴 pos】用于补全时分词：`fun_78_125K:` 之后打 `s`，
//   就能从「`s` 之前是 `:`、`:` 之前是标识符」解出作用域 `fun_78_125K`。
//   标识符口径同 IsSymbolName（1~64、IsIdStart 开头、全 IsIdChar）。
//   刻意不认 `::`（全语料 `.pln` 里 0 处出现）与冒号前带空白的 `IDENT :`（真实 .pln
//   无此写法）—— 三目运算符 `a ? b : c` 的 `:` 会命中，但那只是把 `b` 当成作用域名，
//   宿主查不到同名模块就退回普通补全，属"少报"而非"错报"。
std::string ScopedNameBefore(const std::string& text, std::size_t pos);

// ---------------------------------------------------------------------------
// 批次 148：`.label` 文件 —— 计划级（plncmp 输出）的**裸名 label 权威清单**
//
// 【它是什么】`plncmp <plan>.pln` 会在 `<plan 同目录>/.<plan 名>/<plan 名>.label`
//   产出这个文件（与生成的 `<plan>_body.cpp` 同一目录）。它是**计划自己**的 label
//   清单，即"这份 `.pln` 里可以裸名引用的全部 label"。7 个真实工程逐一核对，它**逐字
//   等于** `.pln` 里 `JUDGE_PAT` 的实参集合：
//     · SCAN 12/12、MCP9600 6/6、ALPG 20/20、ls299_tutorial 16/16、open_short 15/15
//   因此它比 `.rpt` 的 Label Name 表**更贴合本工程**（`.rpt` 是 pattern 级、且可能陈旧）。
//
// 【为什么比 .rpt 更值得信】ALPG 实测：`.rpt` 缺 `ALPG_Walking`/`__ALPG_Walking`
//   （那份 `.rpt` 是旧构建留下的，甚至不含 `ALPG_Walking_pat.pat`），而 `.label` 有它们，
//   `.pln` 也正是用它们 —— 批次 146 靠 `.rpt` 时 ALPG 只命中 18/20，`.label` 补齐到 20/20。
//
// 【格式：定长记录，不是空白分隔的纯文本】4 份真实 `.label` 的字节核对（2026-10-05）：
//     `count`(1B) + `00`(1B) + `count` × 64B 定长记录；记录内是名字 + `\0` 右填充。
//   实证：open_short `0F 00 4F 53 5F 73 74 00…`（count=15）；ALPG `14 00 "contact"…`
//   （count=20）；AD7760 `06 00 "fun_78_125K_C_…"`（count=6）；SCAN `0C 00 "OS_st"…`
//   （count=12）。字节数逐一吻合 `2 + count×64`（962=2+15×64 等）。
//   ⇒ 分隔符**是 `\0`**（不是空格）；名字之间靠定长记录 + NUL 填充隔开。
//   解析口径：跳过 2 字节头（当 `text[1] == '\0'` 时；否则当纯文本从头扫，容错），
//   再按"标识符字符的最长游程"切名 —— NUL / 空白 / 任何非标识符字节都是分隔符。
//   非 IsSymbolName 的碎片丢掉（`count` 字节值 < 0x20 时天然被跳过）。
//
// 【⚠️ 模块内 label 在这里是 CRAFT 拼接名，宿主必须剔除】`.label` 是**计划级 C 标识符**
//   清单：模块内 label（`.pat` 单冒号 `st:`）会被拼成 `<module>_C_<label>`
//   （AD7760 实测 `fun_78_125K_C_st`）。这种名字在 `.pln` 里**不是合法 token**，
//   剔除判据由宿主用「已知模块 × 已知 label」精确匹配，本层只负责切名。
// ---------------------------------------------------------------------------
std::vector<std::string> ExtractLabelFileNames(const std::string& labelText);

// ---------------------------------------------------------------------------
// 批次 106：状态栏「定义」提示要显示的那行文本（**按码点边界截断**）
//
// 【为什么这件事必须在核心里做】状态栏那一段是固定宽度的，而 .dec 的条目行
//   常常很长（对齐空格 + 行尾注释）。截断本身是 `substr(0, n)` 一行就够 ——
//   但那是**按字节**截的：一个 UTF-8 汉字占 3 字节，切在中间就产生非法序列，
//   经 Utf8ToWide 之后显示成乱码或直接丢字。而 .dec 的行尾注释恰恰经常是中文
//   （`;  //时钟`），所以这不是理论上的坑。截断必须退到**码点边界**，
//   而这是纯文本逻辑 —— 放核心里可以直接单测。
//
// 【fallback】lineText 为空时退回 fallbackName。理论上不会发生（addName 总是
//   在原文里切得到非空行），但"宁可提示里只有名字"也好过状态栏出现一个空的
//   「定义」段 —— 空的提示段会让用户以为功能坏了。
//
// 【maxBytes 的口径】按 UTF-8 字节算。超长时保留前 maxBytes 字节内的**完整**
//   码点，末尾补一个省略号 U+2026（3 字节）。所以返回值可能比 maxBytes 长 3 字节。
//   maxBytes 为 0 时返回空串（没地方放，就别放）。
std::string DefinitionHintText(const std::string& lineText,
                               const std::string& fallbackName,
                               std::size_t maxBytes);

// ---------------------------------------------------------------------------
// 批次 107：查找所有引用 —— 一段 Chroma 文本里某个符号出现的**所有位置**
//
// 【它补的是导航的哪一半】批次 104 的 F12 是"从引用跳到声明"，批次 106 的提示是
//   "脚下这个词声明在哪"。这一条是**反方向**：这个符号在哪些地方被用到。
//   三件事加起来才是完整的导航（跳过去 / 跳回来 / 看全貌），缺第三条时用户只能
//   靠"查找"手工敲一遍名字，而手工敲会漏（注释里的同名、拼写相近的名字）。
//
// 【为什么在抹平层上找，而不是在原文上找】
//   注释里出现同一个名字**不是引用**（`// 用 MCLK 当主时钟`），字符串里出现也不是
//   （`"MCLK"` 是数据不是符号）。所以跑在 BlankComments 之后 —— 它把注释与字符串
//   内容抹成空格且**逐字节保长度**，所以抹平后的列号在原行上同样成立，可以直接
//   拿去定位。这与 DecSymbolLoc 的 col 是同一个理由、同一套保证。
//
// 【整词】前后邻字符都不得是标识符字符（`[A-Za-z0-9_]`）—— 否则 `MCLK` 会命中
//   `MCLK2`，`SEL0` 会命中 `SEL01`。宁可漏一个可疑的，也不要给用户一堆假引用。
//
// 【大小写敏感】与 F12、状态栏提示、跨文件补全完全一致（它们都用 `==` 比名字）。
//   Chroma 的名字是大小写敏感的，这里若放宽成不敏感，三处口径就会分叉。
//
// 【在什么文本上跑】调用方喂**解码后的 UTF-8**（编辑器里那份），与批次 106 的
//   位置口径一致 —— 真实 .dec/.pln 是 ANSI/GBK，拿原始字节算偏移会偏。
struct SymbolRef {
    int line = 0;   // 1-based
    int col  = 0;   // 0-based 行内字节偏移
    int len  = 0;   // 名字长度（字节）
};
// name 为空、或含非标识符字符时返回空（宁可什么都不报）。
std::vector<SymbolRef> FindSymbolReferences(const std::string& text,
                                            const std::string& name);

// 把 (行, 列, 长) 翻成**文档字节区间** —— 结果面板存的就是这个区间，双击时要拿它
// 去选中那个名字。行首 = 上一个 '\n' 之后，与 Scintilla 的 SCI_POSITIONFROMLINE
// 同口径（也因此与 CollectHitsInText 的磁盘命中同一套偏移）。
//
// 【为什么是内核里的一个纯函数，而不是宿主里的三行算术】
//   这段换算错了是**静默**的：行号与行文本都对，只有双击选中的东西偏几个字节，
//   而"能跳过去"看起来就是功能正常。真机探针又验不到它 —— 本沙箱里窗口抢不到
//   前台（前台是别的进程的全屏窗口），键盘与鼠标都送不进去，NM_DBLCLK 也编组
//   不过去（跨进程 WM_NOTIFY 目标直接忽略）。所以只能把它变成可单测的纯函数。
//
// 【为什么"越界就返回 false"而不是夹一下】
//   名字跨到行尾之外说明 text 与 SymbolRef 不是同一份文本（典型：一个在解码后、
//   一个在原始字节上）。夹一下会给出一段选中别的东西的区间 —— 宁可不报。
struct SymbolRefSpan {
    std::size_t start = 0;    // 名字首字节（文档字节位置）
    std::size_t end = 0;      // 名字末字节之后
    std::size_t lineBeg = 0;  // 所在行内容起点（行文本用）
    std::size_t lineEnd = 0;  // 所在行内容终点，不含 \r\n
};
bool LocateSymbolRef(const std::string& text, const SymbolRef& r,
                   SymbolRefSpan& out);

} // namespace chroma3380
} // namespace xfs
