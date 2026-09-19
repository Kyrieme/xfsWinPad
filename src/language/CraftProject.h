#pragma once
// xfsWinPad - Chroma 3380 CRAFT 工程模型内核（批次 94 起，方向 D「编译集成」）
//
// 【这一层要解决什么】
//   Chroma 官方工作流是「TextPad 编辑 → Makefile 调 plncmp/patcmp → 看编译结果页，
//   点错误行跳回源码」（操作手册 §2.5.2 / §2.5.3）。要复刻这个闭环，我们必须先
//   知道**这个工程由哪些文件构成、该用哪条命令、编译产物放在哪**。
//   这些知识全部来自工程目录里的 `makefile` 与编译中间目录 —— 本模块就是把它们
//   读成一个结构体，**纯字符串逻辑、零 UI、零进程**。
//
// 【为什么不直接调 make】
//   TextPad 自带 make 与向导（§2.5 原文："After using the wizard to generate the
//   Makefile"）。现场机器不一定装了 make.exe，但**一定装了 CRAFT**（否则没法干活）。
//   所以这里走「解析 makefile 的官方变量名 → 自己拼命令」这条路，只依赖 CRAFT。
//
// 【取证：全部来自厂商范例工程，不是推测】
//   `makefile` 自己就带一份变量说明注释（"User Definition Area"），官方原名如下：
//     PLN_SOURCE   .pln 源文件名            PLN_TARGET   .pin 目标
//     PLN_CFLAGS   plncmp 编译期选项        PAT_SOURCE   .pat 源文件名（可多个）
//     PAT_CFLAGS   patcmp 编译期选项        PAT_LFLAGS   patcmp 链接期选项
//     PAT_TARGET   .ppo 目标                PATH_PAT0    .pat 所在目录
//   调用形态有两种，同一个工程里只出现一种：
//     @plncmp $(PLN_CFLAGS) $(PLN_SOURCE)                    ← 裸命令，靠 PATH
//     @$(CRAFT_HOME)\bin\plncmp $(PLN_CFLAGS) $(PLN_SOURCE)  ← 靠 CRAFT_HOME 环境变量
//   ⇒ **`CRAFT_HOME` 是官方环境变量名**，探测工具链时必须查它。
//
// 【取证：编译中间目录与产物（10/10 个工程全部吻合）】
//   中间目录 = 工程根 + "." + <主 .pln 去扩展名>
//     例：`alpha.pln` → `.alpha/`；`beta.pln` → `.beta/`（10/10 个工程实测吻合）
//   里面与"编译结果页"有关的文件：
//     <stem>.stnline    ★ 语句序号 → .pln 源码行号（空格分隔的 1-based 行号表）
//     compilied.ver     本次实际使用的工具链版本，如 `CRAFT_3380_2.50`
//     decconfg          DEC 编译器配置：`APAS <0|1>` / `MAXSITE <n>`
//     <decStem>.ful     本次实际使用的 .dec（带引号的相对路径）
//     <decStem>.px      本次生效的 PIN_LIST 块名（uint32 个数 + uint32 长度 + 字节）
//   注意 `<decStem>` 取自 **.dec 文件名的去扩展名**，不是 .pln 的名字；
//   .dec 名从 makefile 的规则依赖行取（`$(PLN_TARGET): $(PLN_SOURCE) .\PAT\xxx.dec`）。
//
// 【铁律：读不出来就不猜】
//   本模块对每个产物都**独立**解析：缺文件、格式对不上、字段认不出 ⇒ 该项留空，
//   不报错、不猜值。工程缺 `makefile` 也照样能建出 Project（只是 `vars.ok=false`），
//   因为"用户只是打开了一个 .pln"是完全正常的用法。
//
// 【与其它模块的关系】
//   Chroma3380Diagnostics.h —— 方向 C，回答"这份文本有哪些**确定**的错"（纯静态）。
//                             本模块回答"这个工程该怎么编译"，两者互不依赖。
//   DiagnosticsPanel        —— 方向 C 的展示层。方向 D 的编译输出**不能**混进去
//                             （那份摘要里写着「非 CRAFT 编译结果」，而这里是真编译
//                             结果），所以 UI 侧要用不同的标题与措辞。
//
// 【编译输出的解析：刻意保守】
//   手上有 10 个编译成功的工程，但**一个编译失败的输出样本都没有** —— 也就是说
//   编译器报错文本长什么样，我们**没有任何实证**。所以：
//     · 原文一律原样保留（`plainLines`），UI 必须能看到未经我们加工的输出；
//     · 只识别近乎通用的 `file(line)` / `file:line:col` 形态，认不出就归入原文；
//     · `parsed=false` 表示"一条带行号的都没认出来" ⇒ UI 应退化为纯文本展示，
//       **不要**假装能跳转。
//   宁可不能跳，也不能跳到错的行 —— 后者会让工程师怀疑整个工具。

#include <functional>
#include <string>
#include <vector>

namespace xfs {
namespace craft {

// ---------------------------------------------------------------------------
// 一、工具链探测
// ---------------------------------------------------------------------------

// 工具是从哪找到的（决定 UI 的提示措辞）
enum class ToolOrigin {
    None,       // 没找到
    Settings,   // 用户在设置里指定的目录
    CraftHome,  // 环境变量 CRAFT_HOME 下的 bin 目录
    Path,       // 系统 PATH
};

const wchar_t* ToolOriginName(ToolOrigin o);

struct Toolchain {
    std::wstring plncmp;            // 完整路径；空 = 未找到
    std::wstring patcmp;
    ToolOrigin   plncmpOrigin = ToolOrigin::None;
    ToolOrigin   patcmpOrigin = ToolOrigin::None;
    std::wstring craftHome;         // 环境变量原值（可能为空）

    bool Complete() const { return !plncmp.empty() && !patcmp.empty(); }
};

// `findOnPath(candidate, full)`：宿主提供，**统一裁决"这个候选存不存在"**。
//   · candidate 可能是完整路径（设置目录 / CRAFT_HOME\bin 下的），
//     也可能是裸名字（交给系统 PATH 查找）—— 由宿主实现决定怎么判。
//   · 宿主实现建议：先 `GetFileAttributesW(candidate) != INVALID`，
//     失败再 `SearchPathW(nullptr, candidate, L".exe", ...)`。
// 测试里传假实现，即可完全离线验证探测顺序。
// 查找顺序：设置目录 → CRAFT_HOME\bin → PATH。**先找到先用**，不混合。
Toolchain DetectToolchain(
    const std::wstring& settingsDir,
    const std::wstring& craftHome,
    const std::function<bool(const std::wstring& candidate, std::wstring& full)>& findOnPath);

// ---------------------------------------------------------------------------
// 二、makefile 解析
// ---------------------------------------------------------------------------

struct MakeVars {
    bool         ok = false;        // 是否认出了至少 PLN_SOURCE（没有它就没法编译）
    bool         hasMakefile = false;

    std::wstring plnSource;         // PLN_SOURCE
    std::wstring plnTarget;         // PLN_TARGET
    std::wstring plnCFlags;         // PLN_CFLAGS
    std::wstring patPath;           // PATH_PAT0
    std::wstring patCFlags;         // PAT_CFLAGS0
    std::wstring patLFlags;         // PAT_LFLAGS0
    std::wstring patTarget;         // PAT_TARGET0

    // PAT_SOURCE0 可以**多行续行**（行尾 `\`）且**列多个文件**，展开 $(VAR) 之后
    // 按空白切开。例：两条 .pat 时写成两行，每行一个。
    std::vector<std::wstring> patSources;

    // 规则里 plncmp/patcmp 的调用前缀，**原样保留**：
    //   ""                    → 裸命令（靠 PATH）
    //   "$(CRAFT_HOME)\bin\"  → 靠环境变量
    std::wstring toolPrefix;
    bool         usesCraftHome = false;

    // 规则依赖行里的 .dec 相对路径原文（如 `.\PAT\pin.dec`），去重后按出现顺序。
    std::vector<std::wstring> decDeps;
};

// 解析 makefile 文本。容忍 CRLF / 行尾空白 / `#` 注释 / 未知变量 / 未知目标。
// **不做**递归展开、不做条件分支、不执行任何东西 —— 只认"赋值"与"依赖行"两类。
MakeVars ParseMakefile(const std::string& text);

// ---------------------------------------------------------------------------
// 三、工程模型
// ---------------------------------------------------------------------------

struct Project {
    bool         ok = false;         // 是否能确定工程根与主 .pln
    std::wstring root;               // 工程根目录（makefile 所在目录）
    std::wstring plnName;            // 主 .pln 文件名（不含路径）
    std::wstring stem;               // 主 .pln 去扩展名
    std::wstring interDir;           // root + "\" + "." + stem
    std::wstring makefilePath;       // root + "\makefile"
    std::wstring decStem;            // 由 decDeps[0] 推出的 .dec 去扩展名（可能为空）
    MakeVars     vars;

    // 中间产物目录里的两个"DEC 侧"文件名（decStem 为空时也给出，调用方判空）
    std::wstring fulPath;            // interDir + "\" + decStem + ".ful"
    std::wstring pxPath;             // interDir + "\" + decStem + ".px"
    std::wstring stnLinePath;        // interDir + "\" + stem + ".stnline"
    std::wstring versionPath;        // interDir + "\compilied.ver"
    std::wstring decCfgPath;         // interDir + "\decconfg"
};

// anyPath：用户当前打开的 .pln / .dec / .pat 的完整路径（宿主给）。
// makefileText / makefileExists：宿主负责读盘（内核零 IO）。
// 规则：
//   · 工程根 = anyPath 所在目录；若该目录没有 makefile，则再向上找一层
//     （现场常见结构：工程根放 makefile，源码放在 PAT\ 子目录里）。
//   · 主 .pln：makefile 的 PLN_SOURCE 优先；没有 makefile 时用 anyPath 自身
//     （若它就是 .pln）。
Project BuildProject(const std::wstring& anyPath,
                     const std::string& makefileText,
                     bool makefileExists,
                     bool parentHasMakefile = false,
                     const std::string& parentMakefileText = std::string());

// 给一个绝对/相对路径取"去扩展名的文件名"（`.\PAT\pin.dec` → `pin`）
std::wstring StemOf(const std::wstring& path);

// 把 makefile 里的相对路径（`.\PAT\pin.dec`）拼到工程根上。
// 处理 `.\` 与 `/` 混用；已是绝对路径则原样返回。
std::wstring ResolveRelative(const std::wstring& root, const std::wstring& rel);

// ---------------------------------------------------------------------------
// 四、编译中间产物
// ---------------------------------------------------------------------------

// `<stem>.stnline`：空格/换行分隔的 1-based 行号表，**严格递增**（10/10 实测）。
// 认不出（含非数字 token）⇒ 返回空表，不猜。
std::vector<int> ParseStnLine(const std::string& text);

// `compilied.ver`：整文件去掉首尾空白，如 `CRAFT_3380_2.50`。
std::wstring ParseCraftVersion(const std::string& text);

// `decconfg`：逐行 `KEY VALUE`。认出的键写进 apas / maxSite，认不出的忽略。
// 返回是否认出了至少一行。
bool ParseDecConfig(const std::string& text, int& apas, int& maxSite);

// `<decStem>.ful`：带引号的路径，如 `".\PAT\pin.dec"`（CRLF 结尾）。取出去引号的值。
std::wstring ParseDecUsed(const std::string& text);

// `<decStem>.px`：uint32 个数 + (uint32 长度 + 字节) × 个数。
// 只取**第一个**字符串（实测即生效的 PIN_LIST 块名，如 `Gang32`）。
// 越界 / 长度不合理 ⇒ 返回空，不猜。
std::wstring ParsePinListBlock(const std::string& text);

struct Artifacts {
    bool         ok = false;         // 是否至少读到了一个产物
    std::wstring craftVersion;       // compilied.ver
    int          apas = -1;          // decconfg，-1 = 未读到
    int          maxSite = -1;
    std::wstring decUsed;            // <decStem>.ful，相对路径原文
    std::wstring pinListBlock;       // <decStem>.px
    std::vector<int> stnLines;       // <stem>.stnline
};

// 读盘由宿主注入（内核零 IO）。每个文件缺失都只是"该项留空"。
Artifacts LoadArtifacts(
    const Project& p,
    const std::function<bool(const std::wstring& path, std::string& out)>& readFile);

// 语句序号（1-based，与 stnline 表同口径）→ .pln 源码行号（1-based）。
// 越界或表为空 ⇒ 返回 0（调用方把 0 当"未知"）。
int SourceLineOfStatement(const std::vector<int>& stnLines, int stmtIndex1);

// ---------------------------------------------------------------------------
// 五、构建计划（命令拼装）
// ---------------------------------------------------------------------------

struct BuildStep {
    std::wstring exe;     // "plncmp" / "patcmp"（不含路径；路径由 Toolchain 补）
    std::wstring args;    // 不含 exe
    std::wstring cwd;     // 工作目录 = 工程根
    std::wstring label;   // 给人看的一行说明（写进编译输出面板的标题）
};

// 按厂商 makefile 的**原样顺序**产出步骤：
//   1) plncmp <PLN_CFLAGS> <PLN_SOURCE>                       （.pln → .pin）
//   2) 每个 .pat：patcmp -c -s <PAT_CFLAGS> <pat>              （.pat → .pdt）
//   3) patcmp <PAT_LFLAGS> -f makefile_pdt0.lst               （.pdt → .ppo）
// 变量为空时**不插入空串**（否则命令里会多出空白，CRAFT 的解析未必容忍）。
std::vector<BuildStep> PlanBuild(const Project& p);

// TextPad 向导生成的两份文件列表（厂商工程里逐字可见）。
// 内容 = 每行一个 `.\PAT\xxx.pat` / `.\PAT\xxx.pdt`。
std::string MakePatListFile(const std::vector<std::wstring>& patSources);
std::string MakePdtListFile(const std::vector<std::wstring>& patSources);

// ---------------------------------------------------------------------------
// 六、编译输出解析（刻意保守，见文件头说明）
// ---------------------------------------------------------------------------

enum class IssueKind {
    Error,
    Warning,
    Plain,    // 认不出级别：原样保留，UI 按普通文本显示
};

struct CompileIssue {
    std::wstring file;       // 空 = 无法归属到文件
    int          line = 0;   // 0 = 未知
    int          column = 0; // 0 = 未知
    IssueKind    kind = IssueKind::Plain;
    std::wstring text;       // 该行原文（去掉首尾空白）
};

struct CompileOutput {
    std::vector<CompileIssue> issues;      // 逐行一条，**顺序与原文一致**
    int  errorCount = 0;
    int  warnCount = 0;
    int  locatedCount = 0;                 // 带 (file, line) 的条数
    bool parsed = false;                   // locatedCount > 0
    bool sawAnyError = false;              // 原文里出现过 error/ERROR 等字样
};

// 逐行处理，**永不丢行**：认得出形态就填 file/line，认不出就把整行放进 text。
CompileOutput ParseCompilerOutput(const std::string& out, const std::wstring& projectRoot);

} // namespace craft
} // namespace xfs
