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
//   章节号写在 message 里（形如「手册 §4.5 签名最多 9 个参数」）。

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
