// xfsWinPad - CRAFT 工程模型内核单测（批次 94，方向 D 第一刀）
//
// 【这个测试文件的重点】
//   方向 D 要复刻「Makefile 调 plncmp/patcmp → 看编译结果页 → 点错误行跳源码」
//   这个闭环。闭环的前提是**把工程读对**：工程根在哪、主 .pln 是谁、该跑哪几条
//   命令、中间产物在哪。这些东西读错的后果不是"报个错"，而是**跑错命令**或
//   **跳到错的行** —— 比不做事更糟。
//
//   所以这里把**厂商 makefile 原文**（去掉了工程特有的文件名之外的一切修饰，
//   保留官方变量名与两种调用形态）与**厂商中间产物的字节**都钉成用例。
//
// 【为什么内嵌而不是读 temp/ 下的真实工程】
//   `temp/` 是私密目录、被 .gitignore 排除，CI 上不存在 —— 测试里读它会在 CI 挂。
//   所以这里内嵌**结构等价**的样本：变量名、调用形态、字节布局都与厂商产物一致，
//   只把文件名换成了中性名字。
//
// 【不变式（10/10 个厂商工程实测成立，已钉成断言）】
//   · `stnline` 的行号**严格递增**，且最大值 ≤ 该 .pln 的总行数
//   · 中间目录名 = "." + 主 .pln 去扩展名
//   · `<decStem>.ful` / `<decStem>.px` 的 stem 取自 makefile 的 .dec 依赖，不是 .pln 名

#include "../src/language/CraftProject.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace xfs::craft;

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------------------
// 样本：厂商 makefile 原文（变量名与调用形态照抄；文件名改成中性名）
//   形态 A（裸命令，靠 PATH）—— 7/10 个工程是这样
// ---------------------------------------------------------------------------
static const char* kMakefileBare = R"MK(
# ---------------------User Definition Area Begin-----------------------------
# PLN_SOURCE: Specify plan source file name whose extension name is .pln.
# PLN_CFLAGS: plncmp options on compiling phase e.g. -c, -v
#
# PAT_SOURCE: Specify pattern source file name whose extension name is .pat.
# PAT_CFLAGS: patcmp options on compiling phase e.g. -x, -p, -eXXX
# PAT_LFLAGS: patcmp options on linking phase e.g -oXXX, -r
# NOCOMPRESS_FLAG => remove comment mark "#" below to turn on the patcmp's "-n" flag.
#
PATH_PAT0     = .\PAT
PLN_SOURCE   = demo.pln 
PLN_TARGET   = demo.pin
PLN_CFLAGS   = 
PAT_SOURCE0   =                 $(PATH_PAT0)\func.pat 

PAT_CFLAGS0   =
PAT_LFLAGS0   = -o.\PAT\demo
PAT_TARGET0   =.\PAT\demo.ppo
# ---------------------User Definition Area End------------------------------

PAT_OBJECTS_TMP0    = $(PAT_SOURCE0:.pat=.pdt)
PAT_OBJECTS0        = $(PAT_OBJECTS_TMP0:.PAT=.pdt)
# ---------------------------------------------------------------------------
.precious: $(PAT_OBJECTS0) $(PAT_COBJECTS0)

all: $(PLN_TARGET) $(PAT_TARGET0) 

$(PLN_TARGET): $(PLN_SOURCE) .\PAT\demo_pin.dec
  @plncmp $(PLN_CFLAGS) $(PLN_SOURCE)

$(PAT_TARGET0):: .\PAT\demo_pin.dec
  @patcmp $(PAT_CFLAGS0) $(PAT_LFLAGS0) -f makefile_pat0.lst

$(PAT_TARGET0):: $(PAT_OBJECTS0)
  @patcmp $(PAT_LFLAGS0) -f makefile_pdt0.lst

.pat.pdt :
  @patcmp -c -s $(PAT_CFLAGS0) $<

build:
  @plncmp $(PLN_CFLAGS) $(PLN_SOURCE)
  @patcmp $(PAT_CFLAGS0) $(PAT_LFLAGS0) -f makefile_pat0.lst

clean:
  @touch -c *.pln
)MK";

// 形态 B（靠 CRAFT_HOME 环境变量）—— 3/10 个工程是这样
static const char* kMakefileCraftHome = R"MK(
PATH_PAT0     = .\PAT
PLN_SOURCE   = demo.pln
PLN_TARGET   = demo.pin
PLN_CFLAGS   =
PAT_SOURCE0   =                 $(PATH_PAT0)\func.pat\
              $(PATH_PAT0)\second.pat
PAT_CFLAGS0   =
PAT_LFLAGS0   = -o.\PAT\demo
PAT_TARGET0   =.\PAT\demo.ppo

$(PLN_TARGET): $(PLN_SOURCE) .\PAT\shared_pin.dec
  @$(CRAFT_HOME)\bin\plncmp $(PLN_CFLAGS) $(PLN_SOURCE)

$(PAT_TARGET0):: .\PAT\shared_pin.dec
  @$(CRAFT_HOME)\bin\patcmp $(PAT_CFLAGS0) $(PAT_LFLAGS0) -f makefile_pat0.lst
)MK";

// ---------------------------------------------------------------------------
// 一、makefile 解析
// ---------------------------------------------------------------------------

static void RunParseMakefile() {
    std::printf("-- RunParseMakefile --\n");

    {
        const MakeVars v = ParseMakefile(kMakefileBare);
        CHECK(v.hasMakefile);
        CHECK(v.ok);
        CHECK(v.plnSource == L"demo.pln");          // 行尾空白已 trim
        CHECK(v.plnTarget == L"demo.pin");
        CHECK(v.plnCFlags.empty());
        CHECK(v.patPath == L".\\PAT");
        CHECK(v.patCFlags.empty());
        CHECK(v.patLFlags == L"-o.\\PAT\\demo");
        CHECK(v.patTarget == L".\\PAT\\demo.ppo");
        // $(PATH_PAT0) 必须被展开
        CHECK(v.patSources.size() == 1);
        CHECK(v.patSources.size() == 1 && v.patSources[0] == L".\\PAT\\func.pat");
        // 裸命令 → 前缀为空
        CHECK(v.toolPrefix.empty());
        CHECK(!v.usesCraftHome);
        // .dec 依赖：同一条规则重复出现也只留一个（去重保序）
        CHECK(v.decDeps.size() == 1);
        CHECK(v.decDeps.size() == 1 && v.decDeps[0] == L".\\PAT\\demo_pin.dec");
    }

    {
        const MakeVars v = ParseMakefile(kMakefileCraftHome);
        CHECK(v.ok);
        // 续行：两行合成一条 PAT_SOURCE0，展开后是两个文件
        CHECK(v.patSources.size() == 2);
        if (v.patSources.size() == 2) {
            CHECK(v.patSources[0] == L".\\PAT\\func.pat");
            CHECK(v.patSources[1] == L".\\PAT\\second.pat");
        }
        // CRAFT_HOME 形态：前缀保留**原文**（未定义变量不展开）
        CHECK(v.usesCraftHome);
        CHECK(v.toolPrefix == L"$(CRAFT_HOME)\\bin\\");
        CHECK(v.decDeps.size() == 1);
        CHECK(v.decDeps.size() == 1 && v.decDeps[0] == L".\\PAT\\shared_pin.dec");
    }

    // 空 / 垃圾输入：不能崩，也不能假装成功
    {
        const MakeVars v = ParseMakefile("");
        CHECK(!v.ok);
        CHECK(v.hasMakefile);
        CHECK(v.decDeps.empty());
        CHECK(v.patSources.empty());
    }
    {
        // **真实工程里踩到的坑**：续行反斜杠后面**还跟一个空格**。
        // 厂商 makefile 的原始字节是 `...\func.pat\ `（`cat -A` 下 `\ $`）。
        // 只判"行尾是反斜杠"会把续行整条丢掉 → **少编译一个 .pat**，而且不报错。
        const MakeVars v = ParseMakefile(
            "PATH_PAT0 = .\\PAT\n"
            "PAT_SOURCE0 = $(PATH_PAT0)\\a.pat\\ \n"
            "  $(PATH_PAT0)\\b.pat \n");
        CHECK(v.patSources.size() == 2);
        if (v.patSources.size() == 2) {
            CHECK(v.patSources[0] == L".\\PAT\\a.pat");   // 反斜杠必须被吃掉
            CHECK(v.patSources[1] == L".\\PAT\\b.pat");
        }
    }
    {
        // 续行后面**没有**空格（另一种写法）也要认
        const MakeVars v = ParseMakefile(
            "PATH_PAT0 = .\\PAT\nPAT_SOURCE0 = $(PATH_PAT0)\\a.pat\\\n  x.pat\n");
        CHECK(v.patSources.size() == 2);
    }
    {
        // 只有注释与目标、没有 PLN_SOURCE → 不算 ok（没法编译）
        const MakeVars v = ParseMakefile("# nothing\nall:\n\techo hi\n");
        CHECK(!v.ok);
    }
    {
        // 未知变量必须被忽略而不是错位认领
        const MakeVars v = ParseMakefile(
            "FOO = bar\nPLN_SOURCE = x.pln\nUNKNOWN0 = .\\PAT\\y.pat\n");
        CHECK(v.plnSource == L"x.pln");
        CHECK(v.patSources.empty());
    }
    {
        // `:=` 与 `?=` 也要认（不同 make 方言）
        const MakeVars v = ParseMakefile("PLN_SOURCE := a.pln\nPLN_CFLAGS ?= -v\n");
        CHECK(v.plnSource == L"a.pln");
        CHECK(v.plnCFlags == L"-v");
    }
}

// ---------------------------------------------------------------------------
// 二、工程模型
// ---------------------------------------------------------------------------

static void RunBuildProject() {
    std::printf("-- RunBuildProject --\n");

    {
        // 源码就在工程根
        const Project p = BuildProject(L"D:\\proj\\demo.pln", kMakefileBare, true);
        CHECK(p.ok);
        CHECK(p.root == L"D:\\proj");
        CHECK(p.plnName == L"demo.pln");
        CHECK(p.stem == L"demo");
        // 中间目录 = "." + stem（10/10 实测）
        CHECK(p.interDir == L"D:\\proj\\.demo");
        CHECK(p.makefilePath == L"D:\\proj\\makefile");
        // decStem 取自 .dec 依赖，不是 .pln 名
        CHECK(p.decStem == L"demo_pin");
        CHECK(p.fulPath == L"D:\\proj\\.demo\\demo_pin.ful");
        CHECK(p.pxPath == L"D:\\proj\\.demo\\demo_pin.px");
        CHECK(p.stnLinePath == L"D:\\proj\\.demo\\demo.stnline");
        CHECK(p.versionPath == L"D:\\proj\\.demo\\compilied.ver");
        CHECK(p.decCfgPath == L"D:\\proj\\.demo\\decconfg");
    }

    {
        // 现场常见：源码在 PAT\ 子目录，makefile 在上一层
        const Project p = BuildProject(L"D:\\proj\\PAT\\demo_pin.dec",
                                       std::string(), /*makefileExists=*/false,
                                       /*parentHasMakefile=*/true, kMakefileBare);
        CHECK(p.ok);
        CHECK(p.root == L"D:\\proj");           // 向上找到了
        CHECK(p.plnName == L"demo.pln");        // 来自 makefile 的 PLN_SOURCE
        CHECK(p.stem == L"demo");
        CHECK(p.interDir == L"D:\\proj\\.demo");
    }

    {
        // 孤立 .pln（没有 makefile）：照样能建，plnName 用自己
        const Project p = BuildProject(L"D:\\solo\\thing.pln", std::string(), false);
        CHECK(p.ok);
        CHECK(p.root == L"D:\\solo");
        CHECK(p.plnName == L"thing.pln");
        CHECK(p.stem == L"thing");
        CHECK(!p.vars.ok);
        CHECK(p.decStem.empty());
        CHECK(p.fulPath.empty());               // 没有 decStem 就不给假路径
        CHECK(p.pxPath.empty());
    }

    {
        // 非 .pln 且没有 makefile → 建不出来（ok=false），但也不能崩
        const Project p = BuildProject(L"D:\\solo\\readme.txt", std::string(), false);
        CHECK(!p.ok);
        CHECK(p.root == L"D:\\solo");
        CHECK(p.plnName.empty());
    }
    {
        // 空路径
        const Project p = BuildProject(L"", std::string(), false);
        CHECK(!p.ok);
    }

    // 路径工具
    CHECK(StemOf(L".\\PAT\\pin.dec") == L"pin");
    CHECK(StemOf(L"D:/x/y/alpha_pin.dec") == L"alpha_pin");
    CHECK(StemOf(L"\"\\.\\PAT\\pin.dec\"") == L"pin");   // .ful 里带引号
    CHECK(StemOf(L"noext") == L"noext");
    CHECK(StemOf(L".hidden") == L".hidden");            // 前导点不算扩展名

    CHECK(ResolveRelative(L"D:\\proj", L".\\PAT\\pin.dec") == L"D:\\proj\\PAT\\pin.dec");
    CHECK(ResolveRelative(L"D:\\proj", L"./PAT/pin.dec") == L"D:\\proj\\PAT\\pin.dec");
    CHECK(ResolveRelative(L"D:\\proj", L"C:\\abs\\x.dec") == L"C:\\abs\\x.dec");
    CHECK(ResolveRelative(L"D:\\proj", L"") == L"");
}

// ---------------------------------------------------------------------------
// 三、工具链探测（三个来源的优先级）
// ---------------------------------------------------------------------------

static void RunDetectToolchain() {
    std::printf("-- RunDetectToolchain --\n");

    // 假实现：只有列在 "existing" 里的候选存在
    const std::vector<std::wstring> existing = {
        L"C:\\CRAFT\\bin\\plncmp.exe", L"C:\\CRAFT\\bin\\patcmp.exe",
        L"plncmp.exe", L"patcmp.exe",
    };
    auto finder = [&existing](const std::wstring& cand, std::wstring& full) {
        for (const std::wstring& e : existing) {
            if (e == cand) { full = cand; return true; }
        }
        return false;
    };

    {
        // 设置目录优先于 CRAFT_HOME 与 PATH
        const Toolchain t = DetectToolchain(L"C:\\CRAFT\\bin", L"C:\\Other", finder);
        CHECK(t.Complete());
        CHECK(t.plncmp == L"C:\\CRAFT\\bin\\plncmp.exe");
        CHECK(t.plncmpOrigin == ToolOrigin::Settings);
        CHECK(t.patcmpOrigin == ToolOrigin::Settings);
    }
    {
        // 设置目录为空 → 落到 CRAFT_HOME\bin
        const Toolchain t = DetectToolchain(L"", L"C:\\CRAFT", finder);
        CHECK(t.Complete());
        CHECK(t.plncmpOrigin == ToolOrigin::CraftHome);
        CHECK(t.craftHome == L"C:\\CRAFT");
    }
    {
        // 两者都空 → 裸名字（交给 PATH）
        const Toolchain t = DetectToolchain(L"", L"", finder);
        CHECK(t.Complete());
        CHECK(t.plncmp == L"plncmp.exe");
        CHECK(t.plncmpOrigin == ToolOrigin::Path);
    }
    {
        // 什么都没装：Complete()==false，且**不抛不崩**（优雅降级的前提）
        auto none = [](const std::wstring&, std::wstring&) { return false; };
        const Toolchain t = DetectToolchain(L"", L"", none);
        CHECK(!t.Complete());
        CHECK(t.plncmp.empty());
        CHECK(t.patcmp.empty());
        CHECK(t.plncmpOrigin == ToolOrigin::None);
        CHECK(std::wstring(ToolOriginName(ToolOrigin::None)).empty());
    }
    {
        // 只有一个装了：不能算 Complete（两个都要有才敢编译）
        auto onlyPln = [](const std::wstring& cand, std::wstring& full) {
            if (cand == L"plncmp.exe") { full = cand; return true; }
            return false;
        };
        const Toolchain t = DetectToolchain(L"", L"", onlyPln);
        CHECK(!t.Complete());
        CHECK(t.plncmpOrigin == ToolOrigin::Path);
        CHECK(t.patcmpOrigin == ToolOrigin::None);
    }
}

// ---------------------------------------------------------------------------
// 四、中间产物解析
// ---------------------------------------------------------------------------

static void RunParseArtifacts() {
    std::printf("-- RunParseArtifacts --\n");

    // stnline：厂商产物是"空格分隔 + 每行 10 个 + CRLF 换行"
    {
        const std::vector<int> v = ParseStnLine(
            "18 19 21 24 25 29 31 34 40 42 \r\n"
            "45 51 53 56 61 63 73 84 95 96 \r\n"
            "109 111 112 114 115 116 117 119 120 122 \r\n");
        CHECK(v.size() == 30);
        CHECK(v.size() == 30 && v[0] == 18);
        CHECK(v.size() == 30 && v[29] == 122);
        // 不变式：严格递增（10/10 个厂商工程实测成立）
        bool inc = true;
        for (std::size_t i = 1; i < v.size(); ++i) if (v[i] <= v[i - 1]) inc = false;
        CHECK(inc);
        // 序号 → 源码行号
        CHECK(SourceLineOfStatement(v, 1) == 18);
        CHECK(SourceLineOfStatement(v, 30) == 122);
        CHECK(SourceLineOfStatement(v, 0) == 0);    // 越界 → 0（未知）
        CHECK(SourceLineOfStatement(v, 31) == 0);
        CHECK(SourceLineOfStatement(v, -5) == 0);
    }
    // 认不出的形态一律整表作废（宁可不跳，也不能跳错）
    CHECK(ParseStnLine("").empty());
    CHECK(ParseStnLine("18 abc 21").empty());
    CHECK(ParseStnLine("18 -3 21").empty());
    CHECK(ParseStnLine("0 1 2").empty());          // 行号从 1 起
    CHECK(ParseStnLine("18,19,21").empty());       // 逗号不是分隔符
    CHECK(ParseStnLine("   ").empty());
    CHECK(SourceLineOfStatement({}, 1) == 0);

    // compilied.ver：无换行、无空格
    CHECK(ParseCraftVersion("CRAFT_3380_2.50") == L"CRAFT_3380_2.50");
    CHECK(ParseCraftVersion("CRAFT_3380_2.50\r\n") == L"CRAFT_3380_2.50");
    CHECK(ParseCraftVersion("").empty());

    // decconfg：`KEY VALUE` 逐行
    {
        int apas = -1, maxSite = -1;
        CHECK(ParseDecConfig("APAS 0\r\nMAXSITE 2\r\n", apas, maxSite));
        CHECK(apas == 0);
        CHECK(maxSite == 2);
    }
    {
        int apas = -1, maxSite = -1;
        CHECK(ParseDecConfig("APAS 1\nMAXSITE 0\n", apas, maxSite));
        CHECK(apas == 1);
        CHECK(maxSite == 0);
    }
    {
        // 未知键忽略，但已知键照样认出
        int apas = -1, maxSite = -1;
        CHECK(ParseDecConfig("FOO 9\nAPAS 0\n", apas, maxSite));
        CHECK(apas == 0);
        CHECK(maxSite == -1);      // 没读到就保持 -1（不是 0）
    }
    {
        int apas = -1, maxSite = -1;
        CHECK(!ParseDecConfig("", apas, maxSite));
        CHECK(!ParseDecConfig("justoneword\n", apas, maxSite));
    }

    // .ful：带引号的相对路径 + CRLF
    CHECK(ParseDecUsed("\".\\PAT\\pin.dec\"\r\n") == L".\\PAT\\pin.dec");
    CHECK(ParseDecUsed("  \"x.dec\"  ") == L"x.dec");
    CHECK(ParseDecUsed("") == L"");

    // .px：uint32 个数 + uint32 长度 + 字节（厂商产物 pin.px 的字节照抄）
    {
        const std::string px("\x01\x00\x00\x00\x06\x00\x00\x00Gang32\x01\x00\x00\x00\x00\x00",
                             20);
        CHECK(px.size() == 20);
        CHECK(ParsePinListBlock(px) == L"Gang32");
    }
    // 长度越界 / 长度离谱 / 不可打印 → 不猜
    // 注意：`\x` 会吞掉后面所有十六进制字符，所以十六进制转义后面要断开字符串
    // （`"\x00abc"` 会被解析成 `\x00abc` 这个越界的单一转义）。
    {
        const std::string bad1("\x01\x00\x00\x00\xFF\x00\x00\x00" "abc", 11);
        CHECK(ParsePinListBlock(bad1).empty());
    }
    {
        const std::string bad2("\x00\x00\x00\x00\x00\x00\x00\x00", 8);
        CHECK(ParsePinListBlock(bad2).empty());
    }
    {
        const std::string bad3("\x01\x00\x00\x00\x03\x00\x00\x00" "a" "\x01" "b", 11);
        CHECK(ParsePinListBlock(bad3).empty());
    }
    CHECK(ParsePinListBlock("").empty());
    CHECK(ParsePinListBlock("short").empty());

    // LoadArtifacts：注入假读盘器（内核零 IO 的证明 —— 测试里一个真实文件都没读）
    {
        const Project p = BuildProject(L"D:\\proj\\demo.pln", kMakefileBare, true);
        std::map<std::wstring, std::string> fake;
        fake[p.versionPath] = "CRAFT_3380_2.50";
        fake[p.decCfgPath]   = "APAS 0\r\nMAXSITE 2\r\n";
        fake[p.fulPath]      = "\".\\PAT\\demo_pin.dec\"\r\n";
        fake[p.pxPath]       = std::string(
            "\x01\x00\x00\x00\x06\x00\x00\x00Gang32\x01\x00\x00\x00\x00\x00", 20);
        fake[p.stnLinePath]  = "18 19 21\r\n24 25 29\r\n";
        auto reader = [&fake](const std::wstring& path, std::string& out) {
            auto it = fake.find(path);
            if (it == fake.end()) return false;
            out = it->second;
            return true;
        };
        const Artifacts a = LoadArtifacts(p, reader);
        CHECK(a.ok);
        CHECK(a.craftVersion == L"CRAFT_3380_2.50");
        CHECK(a.apas == 0);
        CHECK(a.maxSite == 2);
        CHECK(a.decUsed == L".\\PAT\\demo_pin.dec");
        CHECK(a.pinListBlock == L"Gang32");
        CHECK(a.stnLines.size() == 6);
        CHECK(a.stnLines.size() == 6 && a.stnLines[5] == 29);
    }
    {
        // 一个产物都没有（没编译过）→ ok=false，且每一项都是"空"，不是垃圾值
        const Project p = BuildProject(L"D:\\proj\\demo.pln", kMakefileBare, true);
        auto reader = [](const std::wstring&, std::string&) { return false; };
        const Artifacts a = LoadArtifacts(p, reader);
        CHECK(!a.ok);
        CHECK(a.craftVersion.empty());
        CHECK(a.apas == -1);
        CHECK(a.maxSite == -1);
        CHECK(a.stnLines.empty());
    }
    {
        // 没有 readFile 回调也不能崩
        const Project p = BuildProject(L"D:\\proj\\demo.pln", kMakefileBare, true);
        const Artifacts a = LoadArtifacts(p, nullptr);
        CHECK(!a.ok);
    }
}

// ---------------------------------------------------------------------------
// 五、构建计划
// ---------------------------------------------------------------------------

static void RunPlanBuild() {
    std::printf("-- RunPlanBuild --\n");

    {
        const Project p = BuildProject(L"D:\\proj\\demo.pln", kMakefileBare, true);
        const std::vector<BuildStep> s = PlanBuild(p);
        CHECK(s.size() == 3);
        if (s.size() == 3) {
            // 1) .pln → .pin（PLN_CFLAGS 为空 → 不能多出空白）
            CHECK(s[0].exe == L"plncmp");
            CHECK(s[0].args == L"demo.pln");
            CHECK(s[0].cwd == L"D:\\proj");
            CHECK(!s[0].label.empty());
            // 2) .pat → .pdt，`-c -s` 是厂商 makefile 的写法
            CHECK(s[1].exe == L"patcmp");
            CHECK(s[1].args == L"-c -s .\\PAT\\func.pat");
            // 3) 链接
            CHECK(s[2].exe == L"patcmp");
            CHECK(s[2].args == L"-o.\\PAT\\demo -f makefile_pdt0.lst");
        }
    }
    {
        // 两个 .pat → 四步（1 编译 + 2 单文件 + 1 链接）
        const Project p = BuildProject(L"D:\\proj\\demo.pln", kMakefileCraftHome, true);
        const std::vector<BuildStep> s = PlanBuild(p);
        CHECK(s.size() == 4);
        if (s.size() == 4) {
            CHECK(s[1].args == L"-c -s .\\PAT\\func.pat");
            CHECK(s[2].args == L"-c -s .\\PAT\\second.pat");
            CHECK(s[3].args == L"-o.\\PAT\\demo -f makefile_pdt0.lst");
        }
    }
    {
        // 非空 PLN_CFLAGS / PAT_CFLAGS 要按顺序出现在正确位置
        const MakeVars v = ParseMakefile(
            "PLN_SOURCE = a.pln\nPLN_CFLAGS = -c -v\n"
            "PAT_SOURCE0 = .\\PAT\\a.pat\nPAT_CFLAGS0 = -x\nPAT_LFLAGS0 = -r\n");
        Project p;
        p.ok = true; p.root = L"D:\\p"; p.stem = L"a"; p.vars = v;
        const std::vector<BuildStep> s = PlanBuild(p);
        CHECK(s.size() == 3);
        if (s.size() == 3) {
            CHECK(s[0].args == L"-c -v a.pln");
            CHECK(s[1].args == L"-c -s -x .\\PAT\\a.pat");
            CHECK(s[2].args == L"-r -f makefile_pdt0.lst");
        }
    }
    {
        // 没有 .pat 时只出 plncmp 一步（不能凭空造链接步骤）
        const MakeVars v = ParseMakefile("PLN_SOURCE = a.pln\n");
        Project p;
        p.ok = true; p.root = L"D:\\p"; p.stem = L"a"; p.vars = v;
        const std::vector<BuildStep> s = PlanBuild(p);
        CHECK(s.size() == 1);
        CHECK(s.size() == 1 && s[0].exe == L"plncmp");
    }
    {
        // 没建出来的工程 → 空计划（UI 据此禁用"编译"菜单）
        const Project p;
        CHECK(PlanBuild(p).empty());
    }

    // 向导生成的两份文件列表（厂商工程里逐字可见）
    {
        const std::vector<std::wstring> src = { L".\\PAT\\func.pat", L".\\PAT\\second.pat" };
        CHECK(MakePatListFile(src) == ".\\PAT\\func.pat\r\n.\\PAT\\second.pat\r\n");
        CHECK(MakePdtListFile(src) == ".\\PAT\\func.pdt\r\n.\\PAT\\second.pdt\r\n");
        CHECK(MakePatListFile({}).empty());
    }
}

// ---------------------------------------------------------------------------
// 六、编译输出解析（保守）
// ---------------------------------------------------------------------------

// plncmp 编译**失败**的真机输出：在装有 CRAFT 的机器上实跑一个"末尾 `}` 被删掉"
// 的厂商范例工程副本得到的，报告自报 1414 字节，这里逐字节抄录。
//
// 【为什么非 ASCII 写成 \xNN】那些字节是 **GBK**（CRAFT 与 cl.exe 都按系统 ANSI
// 代码页输出）。源码里直接写中文会得到 UTF-8 字节 —— 那就不是"逐字节真实"了，而这份
// 样本的全部价值正在于格式是真的。每行右侧注释是该行的可读文本。
//
// 【刻意保留的两处真实形态】
//   · 第 18 行起是 cl.exe 区段，行尾是 `\r\r\n`；前面 CRAFT 自己打的行是 `\r\n` ——
//     **同一份输出里行尾混用**，这不是抄错，是真机就这样（cl.exe 的输出被 CRAFT
//     中继时又经了一次文本模式展开）。
//   · 第 20 行是个空行：SplitLinesAscii 会跳过它，所以它不进 issues —— 下面按
//     "25 条 = 26 行 - 1 空行"核对，就是在钉这一点。
static const char kRealPlncmpFailure[] =
    "Test Plan file compiler for CRAFT_3380_2.50 Copyright (c) 2010 CHROMA\r\n"
    "default linked library : ws2_32.lib \r\n"
    "Parse Plan open_short.pln : \r\n"
    "Make Declaration File ...Success!!\r\n"
    "Parse Plan open_short.pln : ........................................\r\n"
    "TIP : RESULT_PIN can be replaced by RESULT_PIN_MS for multiple sites.\r\n"
    ".\r\n"
    "TIP : JUDGE_VARIABLE can be replaced by JUDGE_VARIABLE_MS for multiple sites.\r\n"
    "........................The brace is not match in the test item CLK_TO_QA_DELAY_test()\r\n"
    ".Finished\r\n"
    "Start Creating Label Library Routine ...\r\n"
    "Create SPECDEFVARI Library Success!!\r\n"
    "Start Creating Category Library Routine ...\r\n"
    "Create Category Library Success!!\r\n"
    "Start Creating Global Library Routine ...\r\n"
    "Global Library Create Success!!\r\n"
    // 17：CRAFT 自己的"这一步失败了"—— 判 Error，但没有位置
    "Create open_short Body Library ...Compile Failed !! Exit Code(2)\r\n"
    "----[Current Compiler VISUAL STUDIO 2013]---- \r\r\n"
    // 19：cl.exe 版本横幅（GBK）。里面有 `(R)` `(C)` 两对括号，都不是数字 → 不许当位置
    "\xd3\xc3\xd3\xda x86 \xb5\xc4 Microsoft (R) C/C++ \xd3\xc5\xbb\xaf\xb1\xe0\xd2\xeb\xc6\xf7 18.00.40629 \xb0\xe6\xb0\xe6\xc8\xa8\xcb\xf9\xd3\xd0(C) Microsoft Corporation\xa1\xa3  \xb1\xa3\xc1\xf4\xcb\xf9\xd3\xd0\xc8\xa8\xc0\xfb\xa1\xa3\r\r\n"
    "\r\r\n"
    "open_short_body.cpp\r\r\n"
    // 22：MSVC 自带头文件的警告 —— 路径**含空格**，是"前缀必须从行首取"的实证
    "C:\\Program Files (x86)\\Microsoft Visual Studio 12.0\\VC\\INCLUDE\\xlocale(337) : warning C4530: \xca\xb9\xd3\xc3\xc1\xcb C++ \xd2\xec\xb3\xa3\xb4\xa6\xc0\xed\xb3\xcc\xd0\xf2\xa3\xac\xb5\xab\xce\xb4\xc6\xf4\xd3\xc3\xd5\xb9\xbf\xaa\xd3\xef\xd2\xe5\xa1\xa3\xc7\xeb\xd6\xb8\xb6\xa8 /EHsc\r\r\n"
    // 23：真正的错误，位置 659
    "open_short.pln(659) : error C2601: \xa1\xb0LOADPIN\xa1\xb1: \xb1\xbe\xb5\xd8\xba\xaf\xca\xfd\xb6\xa8\xd2\xe5\xca\xc7\xb7\xc7\xb7\xa8\xb5\xc4\r\r\n"
    // 24：不带级别的附注行（有位置、无 error/warning 字样）→ Plain 但可跳
    "        open_short.pln(623):  \xb4\xcb\xd0\xd0\xd3\xd0\xd2\xbb\xb8\xf6\xa1\xb0{\xa1\xb1\xc3\xbb\xd3\xd0\xc6\xa5\xc5\xe4\xcf\xee\r\r\n"
    // 25：**正文里也有一对数字括号** `(位于"…(623)")` —— 这是"括号后必须是冒号"的实证
    "open_short.pln(663) : fatal error C1075: \xd3\xeb\xd7\xf3\xb2\xe0\xb5\xc4 \xb4\xf3\xc0\xa8\xba\xc5\xa1\xb0{\xa1\xb1(\xce\xbb\xd3\xda\xa1\xb0open_short.pln(623)\xa1\xb1)\xc6\xa5\xc5\xe4\xd6\xae\xc7\xb0\xd3\xf6\xb5\xbd\xce\xc4\xbc\xfe\xbd\xe1\xca\xf8\r\r\n"
    "Error : Can't make plan object file\r\n";

// ---- 真机**失败**输出（patcmp / 声明文件编译器）------------------------------
//
// 批次 99 拿到的六份样本。做法：在虚拟机里复制六份厂商范例工程，每份只往一个
// 源文件里注入一处缺陷，再用 test_craftrunner 探针把工具链真跑一遍，把每一步的
// 输出抄回来。**这一步的意义**：批次 97 之前一直**没有** patcmp 失败的样本，
// 所以"出错时输出长什么样"全是猜的；这六份把三套消息语法一次钉死了。
//
// 行尾是厂商原样：CRAFT 自己打的行是 `\r\n`，**它中继子进程输出的那一段是
// `\r\r\n`**（cl.exe 那段如此，声明文件编译器那段也如此 —— 中继这个动作才是
// 原因，跟是哪个子进程无关）。SplitLinesAscii 只剥一个 `\r`、TrimWide 再剥剩下的，
// 所以两种混在一份输出里也不需要额外归一化。
//
// 字节数用 static_assert 钉住（比运行时 CHECK 更早发现问题）：这些数是探针报告里
// `output : N bytes` 自报的，改字面量时对不上就编译不过。
// 六份原始输出非 ASCII 字节数为 0，所以这里全是可读的 ASCII 转义，没有 GBK 乱码。
static const char kPlncmpOk[] =
    "Test Plan file compiler for CRAFT_3380_2.50 Copyright (c) 2010 CHROMA\r\n"
    "default linked library : ws2_32.lib \r\n"
    "Parse Plan open_short.pln : \r\n"
    "Make Declaration File ...Success!!\r\n"
    "Parse Plan open_short.pln : ........................................\r\n"
    "TIP : RESULT_PIN can be replaced by RESULT_PIN_MS for multiple sites.\r\n"
    ".\r\n"
    "TIP : JUDGE_VARIABLE can be replaced by JUDGE_VARIABLE_MS for multiple sites.\r\n"
    ".........................Finished\r\n"
    "Start Creating Label Library Routine ...\r\n"
    "Create SPECDEFVARI Library Success!!\r\n"
    "Start Creating Category Library Routine ...\r\n"
    "Create Category Library Success!!\r\n"
    "Start Creating Global Library Routine ...\r\n"
    "Global Library Create Success!!\r\n"
    "Create open_short Body Library ...Linking ...Success!!\r\n"
    "\r\n"
    "Pln File compile successful .....\r\n"
    "\r\n";
static_assert(sizeof(kPlncmpOk) - 1 == 764u, "样本字节数必须是探针自报的 764");

// patcmp 编译 .pat 失败：**一条文件里四个缺陷，四条都报出来**。
// 这是"patcmp 不在第一个错误上停"的实证 —— 行号 17/17/22/35 一次列全。
static const char kPatcmpMultiErr[] =
    "Compile .\\PAT\\ls299_func.pat   ...\r\n"
    "Make declaration file ... OK\r\n"
    ".\\PAT\\ls299_func.pat(17): error C1000: pin:[DQA] Message:[There is no symbol data for IO pin ! ]\r\n"
    ".\\PAT\\ls299_func.pat(17): error C1000: pin:[DQH] Message:[There is no symbol data for IO pin ! ]\r\n"
    ".\\PAT\\ls299_func.pat(22): error C1000: symbol:[BOGUS_MICRO] Message:[The word cat't be recognized ! ]\r\n"
    ".\\PAT\\ls299_func.pat(35): error C1000: symbol:[TS16] Message:[The word cat't be recognized ! ]\r\n"
    "          Errors :  4                    Warning : 0\r\n";
static_assert(sizeof(kPatcmpMultiErr) - 1 == 515u, "样本字节数必须是探针自报的 515");

// patcmp 编译 .pat 失败：HEADER 里写了个 PIN LIST 里没有的 pin。
// ★ 这份**没有统计行** —— 报完那一行就结束了。所以"统计行在不在"不能当成败信号。
static const char kPatcmpHeaderErr[] =
    "Compile .\\PAT\\ls299_func.pat   ...\r\n"
    "Make declaration file ... OK\r\n"
    ".\\PAT\\ls299_func.pat(3): error C1000: pin:[ZZZ] Message:[The header pin is not declared in PIN LIST or PIN GROUP]\r\n";
static_assert(sizeof(kPatcmpHeaderErr) - 1 == 181u, "样本字节数必须是探针自报的 181");

// patcmp 编译 .pat 失败：少一个大括号。★ 位置报在**文件末尾**（152），不是被删的
// 第 149 行 —— "块没闭合"这类错的定位是**症状位置**，不是病因位置。UI 别过度相信行号。
static const char kPatcmpBraceErr[] =
    "Compile .\\PAT\\ls299_func.pat   ...\r\n"
    "Make declaration file ... OK\r\n"
    ".\\PAT\\ls299_func.pat(152): error C1000: Message:['SPM_PATTERN' unmatch]\r\n"
    "          Errors :  1                    Warning : 0\r\n";
static_assert(sizeof(kPatcmpBraceErr) - 1 == 193u, "样本字节数必须是探针自报的 193");

// patcmp **链接**阶段失败：跳转到一个不存在的 label。
// ★ 关键：编译那一步是**过**的（Errors : 0），错误到链接才冒出来；而且报的位置是
//   **.pdt 中间文件**（绝对路径），不是用户写的 .pat。解析器要把它还原成 .pat。
static const char kPatcmpLinkLabelErr[] =
    "Start time of compilation : Sun Sep 20 05:24:14 2026\r\n"
    "\r\n"
    "Link .\\PAT\\ls299_func.pdt   ...\r\n"
    "Link .\\PAT\\ls299_TMU.pdt   ...\r\n"
    "Z:\\AI_Work\\codex\\xfsPad\\temp\\_vmcheck\\badpat_label\\PAT\\ls299_func.pdt(49): error C1000: label:[no_such_label] Message:[Used label is not defined ! ]\r\n"
    "          Errors :  1                    Warning : 0\r\n"
    "\r\n"
    "End  time  of compilation : Sun Sep 20 05:24:14 2026\r\n"
    "\r\n"
    "Time used :  0  seconds\r\n";
static_assert(sizeof(kPatcmpLinkLabelErr) - 1 == 408u, "样本字节数必须是探针自报的 408");

// patcmp 编译 .pat **成功**（同一份工程里另一支向量）。用来对照：同一批里
// 失败的那支报 1 个错，成功的那支报 0 个 —— 统计行的数字确实是"这一步的"。
static const char kPatcmpOkFunc[] =
    "Compile .\\PAT\\ls299_func.pat   ...\r\n"
    "Make declaration file ... OK\r\n"
    "          Errors :  0                    Warning : 0\r\n";
static_assert(sizeof(kPatcmpOkFunc) - 1 == 120u, "样本字节数必须是探针自报的 120");

static const char kPatcmpOkTmu[] =
    "Compile .\\PAT\\ls299_TMU.pat   ...\r\n"
    "Make declaration file ... OK\r\n"
    "          Errors :  0                    Warning : 0\r\n";
static_assert(sizeof(kPatcmpOkTmu) - 1 == 119u, "样本字节数必须是探针自报的 119");

// plncmp 的**声明文件编译器**挂了：.dec 里 pin 名重复（手册 §2.3.2 明文说会报错）。
// ★ 这是**第三套**消息语法：位置写成 `<< File:[…] Line:[N] Last_Token:[…] >>`，
//   级别在**下一行**的 `Message:[…]` 里。既不是 `file(line)`，也不是 `file:line`。
static const char kPlncmpDecDupName[] =
    "Test Plan file compiler for CRAFT_3380_2.50 Copyright (c) 2010 CHROMA\r\n"
    "default linked library : ws2_32.lib \r\n"
    "Parse Plan open_short.pln : \r\n"
    "Make Declaration File ...Compile Failed !! Exit Code(59)\r\n"
    "Delaration file compiler for CRAFT_3380_2.50 Copyright (c) 2011 CHROMA\r\r\n"
    "\r\r\n"
    "<< File:[.\\PAT\\ls299_pin.dec] Line:[24] Last_Token:[;] >>\r\r\n"
    "    Message:[Error : Duplicate Declare Pin \"QA\"]\r\r\n"
    "\r\r\n";
static_assert(sizeof(kPlncmpDecDupName) - 1 == 387u, "样本字节数必须是探针自报的 387");

// 同上，但缺陷是**通道号重复**（把 QE 的 S0 通道改成另一个 pin 已占的 37）。
// 这一条把工程自己的 C3380-DEC-002 规则拿到了厂商编译器的背书。
static const char kPlncmpDecDupChan[] =
    "Test Plan file compiler for CRAFT_3380_2.50 Copyright (c) 2010 CHROMA\r\n"
    "default linked library : ws2_32.lib \r\n"
    "Parse Plan open_short.pln : \r\n"
    "Make Declaration File ...Compile Failed !! Exit Code(59)\r\n"
    "Delaration file compiler for CRAFT_3380_2.50 Copyright (c) 2011 CHROMA\r\r\n"
    "\r\r\n"
    "<< File:[.\\PAT\\ls299_pin.dec] Line:[9] Last_Token:[;] >>\r\r\n"
    "    Message:[Error(1) : PINLIST=0 PINNAME=QE Channel duplicate define!!]\r\r\n"
    "\r\r\n";
static_assert(sizeof(kPlncmpDecDupChan) - 1 == 410u, "样本字节数必须是探针自报的 410");

static void RunParseCompilerOutput() {
    std::printf("-- RunParseCompilerOutput --\n");

    {
        // 典型形态：带括号的位置
        const CompileOutput r = ParseCompilerOutput(
            "demo.pln(12) : error: unexpected token ';'\r\n"
            "demo.pln(20,5) : warning: unused label\r\n"
            "Compile finished.\r\n", L"D:\\proj");
        CHECK(r.issues.size() == 3);
        CHECK(r.errorCount == 1);
        CHECK(r.warnCount == 1);
        CHECK(r.locatedCount == 2);
        CHECK(r.parsed);
        CHECK(r.sawAnyError);
        if (r.issues.size() == 3) {
            CHECK(r.issues[0].file == L"demo.pln");
            CHECK(r.issues[0].line == 12);
            CHECK(r.issues[0].column == 0);
            CHECK(r.issues[0].kind == IssueKind::Error);
            CHECK(r.issues[1].file == L"demo.pln");
            CHECK(r.issues[1].line == 20);
            CHECK(r.issues[1].column == 5);
            CHECK(r.issues[1].kind == IssueKind::Warning);
            CHECK(r.issues[2].kind == IssueKind::Plain);
            CHECK(r.issues[2].line == 0);
            // 原文永不丢
            CHECK(!r.issues[2].text.empty());
        }
    }
    {
        // 冒号形态
        const CompileOutput r = ParseCompilerOutput(
            "C:\\proj\\PAT\\x.dec:33: error: bad pin type\n", L"C:\\proj");
        CHECK(r.parsed);
        CHECK(r.issues.size() == 1);
        if (r.issues.size() == 1) {
            CHECK(r.issues[0].file == L"C:\\proj\\PAT\\x.dec");
            CHECK(r.issues[0].line == 33);
            CHECK(r.issues[0].kind == IssueKind::Error);
        }
    }
    {
        // **保守闸**：括号里恰好是数字、但前面不是源码路径 → 不许当成位置
        const CompileOutput r = ParseCompilerOutput(
            "Error: too many sites (2)\n", L"D:\\proj");
        CHECK(!r.parsed);                 // 关键：不能假装能跳
        CHECK(r.locatedCount == 0);
        CHECK(r.issues.size() == 1);
        CHECK(r.issues.size() == 1 && r.issues[0].kind == IssueKind::Error);
        CHECK(r.issues.size() == 1 && r.issues[0].line == 0);
    }
    {
        // 完全不认识的输出 → parsed=false，但每一行都在（UI 退化为纯文本）
        const CompileOutput r = ParseCompilerOutput("???\n###\n", L"D:\\proj");
        CHECK(!r.parsed);
        CHECK(r.locatedCount == 0);
        CHECK(r.errorCount == 0);
        CHECK(r.warnCount == 0);
        CHECK(!r.sawAnyError);
        CHECK(r.issues.size() == 2);
    }
    {
        // 空输出 / 只有空行
        const CompileOutput r = ParseCompilerOutput("", L"D:\\proj");
        CHECK(r.issues.empty());
        CHECK(!r.parsed);
        const CompileOutput r2 = ParseCompilerOutput("\r\n\r\n", L"D:\\proj");
        CHECK(r2.issues.empty());
    }
    {
        // 整词判定：`errorcode` / `warnings2` 里的子串不许当级别
        const CompileOutput r = ParseCompilerOutput(
            "errorcode=5\nmywarnings\n", L"D:\\proj");
        CHECK(r.errorCount == 0);
        CHECK(r.warnCount == 0);
    }
    {
        // 大写也要认
        const CompileOutput r = ParseCompilerOutput(
            "demo.pln(3) : ERROR: boom\n", L"D:\\proj");
        CHECK(r.errorCount == 1);
        CHECK(r.sawAnyError);
        CHECK(r.parsed);
    }

    // ---- 真机输出（在装有 CRAFT 的机器上实跑一个厂商范例工程得到的）--------
    // 逐字节保留原格式：行尾空格、列对齐的大段空格、进度点。工程名与文件名
    // 已换成中性名，其余一个字没动 —— 这份样本的全部价值就在于"格式是真的"。
    // 上面那些手写样本都是**按想象**造的，它们过了不代表真实输出也过。
    {
        // plncmp 编译 .pln 成功
        const CompileOutput r = ParseCompilerOutput(
            "Test Plan file compiler for CRAFT_3380_2.50 Copyright (c) 2010 CHROMA\r\n"
            "default linked library : ws2_32.lib \r\n"
            "Parse Plan demo.pln : \r\n"
            "Make Declaration File ...Success!!\r\n"
            "Parse Plan demo.pln : ........................................\r\n"
            "TIP : RESULT_PIN can be replaced by RESULT_PIN_MS for multiple sites.\r\n"
            ".\r\n"
            "TIP : JUDGE_VARIABLE can be replaced by JUDGE_VARIABLE_MS for multiple sites.\r\n"
            ".........................Finished\r\n"
            "Start Creating Label Library Routine ...\r\n"
            "Create SPECDEFVARI Library Success!!\r\n"
            "Start Creating Category Library Routine ...\r\n"
            "Create Category Library Success!!\r\n"
            "Start Creating Global Library Routine ...\r\n"
            "Global Library Create Success!!\r\n"
            "Create demo Body Library ...Linking ...Success!!\r\n"
            "\r\n"
            "Pln File compile successful .....\r\n", L"D:\\proj");
        CHECK(r.errorCount == 0);
        CHECK(r.warnCount == 0);        // ★ `... : 0` 这类统计不许被当成级别
        CHECK(!r.sawAnyError);
        CHECK(!r.parsed);               // 成功输出里没有可跳转的位置
        CHECK(r.locatedCount == 0);
    }
    {
        // patcmp 编译 .pat 成功 —— **统计行**：
        //   `          Errors :  0                    Warning : 0`
        // "Warning" 在这里是个独立的词（前后都是空格），按词判级会把它当成
        // 一条警告 ⇒ 编译完全成功的工程，面板上会多出一条假警告。
        const CompileOutput r = ParseCompilerOutput(
            "Compile .\\PAT\\alpha.pat   ...\r\n"
            "Make declaration file ... OK\r\n"
            "          Errors :  0                    Warning : 0\r\n", L"D:\\proj");
        CHECK(r.errorCount == 0);
        CHECK(r.warnCount == 0);
        CHECK(!r.sawAnyError);
        CHECK(!r.parsed);
        CHECK(r.issues.size() == 3);    // 原文不丢（统计行也在，只是不带级别）
        if (r.issues.size() == 3) CHECK(r.issues[2].kind == IssueKind::Plain);
    }
    {
        // patcmp 链接向量成功 —— 同样的统计行，另外带时间戳与 `Time used`
        const CompileOutput r = ParseCompilerOutput(
            "Start time of compilation : Sat Sep 19 17:59:56 2026\r\n"
            "\r\n"
            "Link .\\PAT\\alpha.pdt   ...\r\n"
            "Link .\\PAT\\beta.pdt   ...\r\n"
            "          Errors :  0                    Warning : 0\r\n"
            "\r\n"
            "End  time  of compilation : Sat Sep 19 17:59:56 2026\r\n"
            "\r\n"
            "Time used :  0  seconds\r\n", L"D:\\proj");
        CHECK(r.errorCount == 0);
        CHECK(r.warnCount == 0);
        CHECK(!r.parsed);
    }

    // ---- 真机失败输出（plncmp 挂了）------------------------------------------
    // 这份样本是**唯一**能检验"出错时解析成什么样"的实证。抄录的字节数必须等于探针
    // 报告自报的 1414 —— 这是防手抄走样的自检，改字面量时它会立刻红。
    {
        CHECK(sizeof(kRealPlncmpFailure) - 1 == 1414u);

        const CompileOutput r = ParseCompilerOutput(kRealPlncmpFailure, L"D:\\proj");
        CHECK(r.issues.size() == 25);   // 26 行 - 1 个空行
        CHECK(r.parsed);
        CHECK(r.sawAnyError);
        CHECK(r.errorCount == 4);       // 第 17/23/25/26 行
        CHECK(r.warnCount == 1);        // 第 22 行（MSVC 自带头文件）
        CHECK(r.locatedCount == 4);     // 第 22/23/24/25 行

        // 第 17 行：CRAFT 自己的"这一步失败了"。判 Error（它确实是个错误结论），
        // 但**没有位置** —— `Exit Code(2)` 后面没有冒号，不许当成可跳转的行。
        CHECK(r.issues[16].kind == IssueKind::Error);
        CHECK(r.issues[16].line == 0);

        // 第 18 行：分节线（`----[Current Compiler ...]----`）—— 一个 `(` 都没有
        CHECK(r.issues[17].kind == IssueKind::Plain);
        CHECK(r.issues[17].line == 0);

        // 第 19 行：cl.exe 版本横幅。含 `(R)` `(C)` 两对括号，都不是数字 → 不许当位置
        CHECK(r.issues[18].kind == IssueKind::Plain);
        CHECK(r.issues[18].line == 0);

        // 第 22 行：MSVC 自带头文件的警告。★ 路径含空格，**必须取全**。
        // （批次 97 之前按"最后一个空白之后"取，会腰斩成 `12.0\VC\INCLUDE\xlocale`。）
        CHECK(r.issues[20].kind == IssueKind::Warning);
        CHECK(r.issues[20].line == 337);
        CHECK(r.issues[20].file ==
              L"C:\\Program Files (x86)\\Microsoft Visual Studio 12.0\\VC\\INCLUDE\\xlocale");

        // 第 23 行：真正的错误，位置 659
        CHECK(r.issues[21].kind == IssueKind::Error);
        CHECK(r.issues[21].file == L"open_short.pln");
        CHECK(r.issues[21].line == 659);
        CHECK(r.issues[21].column == 0);

        // 第 24 行：**不带级别**的附注行（有位置、没有 error/warning 字样）→ Plain，
        // 但位置要认出来（用户还是想点过去看看那个 `{`）。
        CHECK(r.issues[22].kind == IssueKind::Plain);
        CHECK(r.issues[22].file == L"open_short.pln");
        CHECK(r.issues[22].line == 623);

        // 第 25 行：★ C1075 的**正文里也有一对数字括号** `(位于"…(623)")`。
        // 必须认到行首那个 `(663)`，不能认到正文里去。
        // （批次 97 之前从右往左找最后一对括号，会撞上正文里的 `(623)` 而整行认不出。）
        CHECK(r.issues[23].kind == IssueKind::Error);
        CHECK(r.issues[23].file == L"open_short.pln");
        CHECK(r.issues[23].line == 663);

        // 第 26 行：CRAFT 的收尾错误，没有位置
        CHECK(r.issues[24].kind == IssueKind::Error);
        CHECK(r.issues[24].line == 0);

        // 原文永不丢：每条 text 都是该行去掉首尾空白后的原文
        CHECK(!r.issues[16].text.empty());
        CHECK(r.issues[16].text == L"Create open_short Body Library ...Compile Failed !! Exit Code(2)");
        CHECK(r.issues[24].text == L"Error : Can't make plan object file");
    }
}

// ---------------------------------------------------------------------------
// 六之二、真机**失败**输出 —— 批次 99 的六份样本
//
// 批次 97 留了三个问题没答案，因为它们全都要"patcmp 真的失败一次"才能回答：
//   ① patcmp 是停在第一个错误，还是接着数完？
//   ② 逐条错误行长什么样、带不带可跳转的位置？
//   ③ `Errors : N`（N > 0）到底存不存在，能不能拿它当失败信号？
// 六份样本一次把三个都回答了（结论写在 CraftProject.cpp 里 IsCraftSummaryLine
// 上方的注释里，这里只钉行为）。
// ---------------------------------------------------------------------------
static void RunRealFailureSamples() {
    std::printf("-- RunRealFailureSamples --\n");
    const std::wstring kRoot = L"Z:\\AI_Work\\codex\\xfsPad\\temp\\_vmcheck\\badpat_label";

    {
        // ① patcmp **不在第一个错上停**：一份文件四个缺陷，四条全列出来。
        // ③ 统计行 `Errors : 4` 与逐条数出来的 4 完全相等。
        const CompileOutput r = ParseCompilerOutput(kPatcmpMultiErr, kRoot);
        CHECK(r.issues.size() == 7);      // 7 行，没有一个空行
        CHECK(r.errorCount == 4);
        CHECK(r.warnCount == 0);
        CHECK(r.locatedCount == 4);
        CHECK(r.sawAnyError);
        CHECK(r.parsed);

        // 四条诊断的位置：17 / 17 / 22 / 35（同一条 pin 错误在一行上报了两遍）
        CHECK(r.issues[2].file == L".\\PAT\\ls299_func.pat");
        CHECK(r.issues[2].kind == IssueKind::Error);
        CHECK(r.issues[2].line == 17);
        CHECK(r.issues[3].line == 17);
        CHECK(r.issues[4].line == 22);
        CHECK(r.issues[5].line == 35);

        // 统计行原样保留，但**不带级别**（带了就会多出一条假警告）
        CHECK(r.issues[6].kind == IssueKind::Plain);
        CHECK(r.issues[6].line == 0);
    }
    {
        // ③ 的反例，也是"不读统计行那两个数字"的真正依据：
        // 致命错误（HEADER 里有个 PIN LIST 没声明的 pin）**整份输出里没有统计行**。
        // 所以"有没有统计行"不能当失败信号，反过来"统计行写 0"也不能当成功信号。
        const CompileOutput r = ParseCompilerOutput(kPatcmpHeaderErr, kRoot);
        CHECK(r.issues.size() == 3);      // 只有 3 行 —— 第 3 行之后就没有了
        CHECK(r.errorCount == 1);
        CHECK(r.locatedCount == 1);
        CHECK(r.parsed);
        CHECK(r.issues[2].file == L".\\PAT\\ls299_func.pat");
        CHECK(r.issues[2].line == 3);
        CHECK(r.issues[2].kind == IssueKind::Error);
        // 最后一行是诊断本身，不是统计行
        CHECK(r.issues[2].text.find(L"Errors") == std::wstring::npos);
    }
    {
        // ② 一条诊断可以**没有** `pin:[…]` / `symbol:[…]` 字段，只有 Message。
        // 所以判级只看 error 整词，不去要求那个字段存在。
        // 另外：少一个大括号，位置报在**文件末尾 152**，不是被删的第 149 行 ——
        // "块没闭合"这类错给的是**症状位置**，UI 别过度相信这个行号。
        const CompileOutput r = ParseCompilerOutput(kPatcmpBraceErr, kRoot);
        CHECK(r.issues.size() == 4);
        CHECK(r.errorCount == 1);
        CHECK(r.locatedCount == 1);
        CHECK(r.issues[2].file == L".\\PAT\\ls299_func.pat");
        CHECK(r.issues[2].line == 152);
        CHECK(r.issues[2].kind == IssueKind::Error);
        CHECK(r.issues[2].text ==
              L".\\PAT\\ls299_func.pat(152): error C1000: Message:['SPM_PATTERN' unmatch]");
        CHECK(r.issues[3].kind == IssueKind::Plain);   // 统计行
    }
    {
        // ② + ★ .pdt → .pat：链接阶段失败。编译那一步是**过**的（Errors : 0），
        // 错误到链接才冒出来，而且位置指向的是 **.pdt 中间文件**（绝对路径）。
        // 不还原成 .pat 的话，跳转要么落到二进制上、要么整个跳不成。
        const CompileOutput r = ParseCompilerOutput(kPatcmpLinkLabelErr, kRoot);
        CHECK(r.issues.size() == 7);      // 10 行里 3 个空行
        CHECK(r.errorCount == 1);
        CHECK(r.locatedCount == 1);
        CHECK(r.parsed);
        CHECK(r.issues[3].kind == IssueKind::Error);
        CHECK(r.issues[3].file ==
              L"Z:\\AI_Work\\codex\\xfsPad\\temp\\_vmcheck\\badpat_label\\PAT\\ls299_func.pat");
        CHECK(r.issues[3].line == 49);
        CHECK(r.issues[3].text.find(L".pdt(49)") != std::wstring::npos);   // 原文不丢

        // 时间戳那两行里也有 `:` 和数字（`05:24:14`、`Time used :  0`），
        // 但前缀既没有路径分隔符、也不以源码扩展名结尾 → 不许当成位置
        CHECK(r.issues[0].line == 0);
        CHECK(r.issues[0].kind == IssueKind::Plain);
        CHECK(r.issues[6].line == 0);
        CHECK(r.issues[6].kind == IssueKind::Plain);
    }
    {
        // 同一批里的成功输出。顺带钉住 `Copyright (c) 2010 CHROMA` 里那对
        // 字母括号 —— 括号里不是数字，不许当位置。
        const CompileOutput r = ParseCompilerOutput(kPlncmpOk, kRoot);
        CHECK(r.issues.size() == 17);
        CHECK(r.errorCount == 0);
        CHECK(r.warnCount == 0);
        CHECK(!r.sawAnyError);
        CHECK(!r.parsed);
        CHECK(r.locatedCount == 0);
    }
    {
        // 对照：同一批里另一支向量编译**成功**，统计行写 0，一条错都不报
        const CompileOutput rf = ParseCompilerOutput(kPatcmpOkFunc, kRoot);
        CHECK(rf.issues.size() == 3);
        CHECK(rf.errorCount == 0);
        CHECK(rf.warnCount == 0);
        CHECK(!rf.parsed);
        const CompileOutput rt = ParseCompilerOutput(kPatcmpOkTmu, kRoot);
        CHECK(rt.issues.size() == 3);
        CHECK(rt.errorCount == 0);
        CHECK(!rt.parsed);
    }
    {
        // ★ 第三套消息语法：位置在记录头 `<< File:[…] Line:[N] … >>`，
        // 级别在**下一行**的 `Message:[…]`。
        // 位置必须挂到带 Error 的那一行（用户会去点它），不是记录头那一行。
        const CompileOutput r = ParseCompilerOutput(kPlncmpDecDupName, kRoot);
        CHECK(r.issues.size() == 7);      // 9 行里 2 个空行
        CHECK(r.errorCount == 2);         // `Compile Failed !!` 与 `Message:[Error …]`
        CHECK(r.locatedCount == 1);
        CHECK(r.parsed);

        // 记录头那一行：认得出是记录头，但自己**不带**位置
        CHECK(r.issues[5].kind == IssueKind::Plain);
        CHECK(r.issues[5].line == 0);
        CHECK(r.issues[5].text ==
              L"<< File:[.\\PAT\\ls299_pin.dec] Line:[24] Last_Token:[;] >>");

        // 紧接的下一行接住位置
        CHECK(r.issues[6].kind == IssueKind::Error);
        CHECK(r.issues[6].file == L".\\PAT\\ls299_pin.dec");
        CHECK(r.issues[6].line == 24);
    }
    {
        // 同上，缺陷换成"通道号重复"。工程自己的 C3380-DEC-002 规则由厂商
        // 编译器亲自背书：`Channel duplicate define!!`。
        // 注意 `Error(1)` —— 整词判定要认它（`(` 不是标识符字符），
        // 但 `Error(1) :` 那对括号里是数字、后面又跟着冒号，仍然不许当位置：
        // 括号前的 `Message:[Error` 既没有分隔符、也不以源码扩展名结尾。
        const CompileOutput r = ParseCompilerOutput(kPlncmpDecDupChan, kRoot);
        CHECK(r.issues.size() == 7);
        CHECK(r.errorCount == 2);
        CHECK(r.locatedCount == 1);
        CHECK(r.parsed);
        CHECK(r.issues[5].kind == IssueKind::Plain);
        CHECK(r.issues[5].line == 0);
        CHECK(r.issues[6].kind == IssueKind::Error);
        CHECK(r.issues[6].file == L".\\PAT\\ls299_pin.dec");
        CHECK(r.issues[6].line == 9);
    }
}

// ---------------------------------------------------------------------------
// 七、真实工程探针（回归入口）
//
// `test_craftproj <任意工程文件>` —— 把真实工程读成模型并打印出来。
// 这是**唯一**需要读盘的地方，单测部分一个文件都不读（见文件头说明）。
//
// 用途：拿厂商范例工程跑一遍，人工核对"工程根 / 主 .pln / 中间目录 / 命令计划 /
// 中间产物"是不是都对。**不做断言**（厂商目录在 CI 上不存在），只打印。
// ---------------------------------------------------------------------------

#include <fstream>

static bool ReadAllBytes(const std::wstring& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// 极简 UTF-8 编码（BMP + 代理对）。**刻意不引 windows.h** —— 这个测试目标要能
// 在没有任何 Windows 依赖的情况下编译。工程路径里常带中文（机台资料目录），
// 用 '?' 顶替会让下游脚本没法把打印出来的路径拿去磁盘核对。
static std::string ToUtf8(const std::wstring& w) {
    std::string o;
    for (std::size_t i = 0; i < w.size(); ++i) {
        unsigned int cp = (unsigned int)w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            const unsigned int lo = (unsigned int)w[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                ++i;
            }
        }
        if (cp < 0x80u) {
            o.push_back((char)cp);
        } else if (cp < 0x800u) {
            o.push_back((char)(0xC0u | (cp >> 6)));
            o.push_back((char)(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            o.push_back((char)(0xE0u | (cp >> 12)));
            o.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
            o.push_back((char)(0x80u | (cp & 0x3Fu)));
        } else {
            o.push_back((char)(0xF0u | (cp >> 18)));
            o.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
            o.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
            o.push_back((char)(0x80u | (cp & 0x3Fu)));
        }
    }
    return o;
}

static std::string NarrowAscii(const std::wstring& w) { return ToUtf8(w); }

static int ProbeProject(const wchar_t* path) {
    std::string mf, pmf;
    const std::wstring p(path);
    // 本目录的 makefile
    std::wstring dir = p;
    const std::size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash);
    const bool hasMf = !dir.empty() && ReadAllBytes(dir + L"\\makefile", mf);
    // 上一层的 makefile
    std::wstring up;
    const std::size_t slash2 = dir.find_last_of(L"\\/");
    if (slash2 != std::wstring::npos) up = dir.substr(0, slash2);
    const bool hasParentMf = !up.empty() && ReadAllBytes(up + L"\\makefile", pmf);

    const Project proj = BuildProject(p, mf, hasMf, hasParentMf, pmf);
    std::printf("input      : %s\n", NarrowAscii(p).c_str());
    std::printf("ok         : %s\n", proj.ok ? "yes" : "NO");
    std::printf("root       : %s\n", NarrowAscii(proj.root).c_str());
    std::printf("plnName    : %s\n", NarrowAscii(proj.plnName).c_str());
    std::printf("stem       : %s\n", NarrowAscii(proj.stem).c_str());
    std::printf("interDir   : %s\n", NarrowAscii(proj.interDir).c_str());
    std::printf("makefile   : %s (%s)\n", proj.vars.hasMakefile ? "yes" : "no",
                NarrowAscii(proj.makefilePath).c_str());
    std::printf("decStem    : %s\n", NarrowAscii(proj.decStem).c_str());
    std::printf("toolPrefix : [%s]  craftHome=%s\n",
                NarrowAscii(proj.vars.toolPrefix).c_str(),
                proj.vars.usesCraftHome ? "yes" : "no");
    std::printf("decDeps    : %d\n", (int)proj.vars.decDeps.size());
    for (const std::wstring& d : proj.vars.decDeps)
        std::printf("             %s\n", NarrowAscii(d).c_str());
    std::printf("patSources : %d\n", (int)proj.vars.patSources.size());
    for (const std::wstring& d : proj.vars.patSources)
        std::printf("             %s\n", NarrowAscii(d).c_str());

    const std::vector<BuildStep> steps = PlanBuild(proj);
    std::printf("steps      : %d\n", (int)steps.size());
    for (const BuildStep& s : steps) {
        std::printf("             %s %s\n", NarrowAscii(s.exe).c_str(),
                    NarrowAscii(s.args).c_str());
    }

    const Artifacts a = LoadArtifacts(proj, ReadAllBytes);
    std::printf("artifacts  : %s\n", a.ok ? "yes" : "NO");
    std::printf("  craftVer : %s\n", NarrowAscii(a.craftVersion).c_str());
    std::printf("  apas     : %d   maxSite: %d\n", a.apas, a.maxSite);
    std::printf("  decUsed  : %s\n", NarrowAscii(a.decUsed).c_str());
    std::printf("  pinBlock : %s\n", NarrowAscii(a.pinListBlock).c_str());
    std::printf("  stnLines : %d", (int)a.stnLines.size());
    if (!a.stnLines.empty()) {
        bool inc = true;
        for (std::size_t i = 1; i < a.stnLines.size(); ++i)
            if (a.stnLines[i] <= a.stnLines[i - 1]) inc = false;
        std::printf("  first=%d last=%d increasing=%s", a.stnLines.front(),
                    a.stnLines.back(), inc ? "yes" : "NO");
    }
    std::printf("\n");
    return proj.ok ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) return ProbeProject(argv[1]);
    std::printf("== test_craftproj ==\n");
    RunParseMakefile();
    RunBuildProject();
    RunDetectToolchain();
    RunParseArtifacts();
    RunPlanBuild();
    RunParseCompilerOutput();
    RunRealFailureSamples();
    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
