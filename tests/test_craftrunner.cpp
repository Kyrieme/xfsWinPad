// xfsWinPad - CRAFT 编译命令执行器单测（批次 95，方向 D 第二刀）
//
// 【这个测试文件的重点】
//   方向 D 的闭环是「跑 plncmp/patcmp → 收输出 → 点错误行跳源码」。上一批（94）
//   把"该跑什么"算对了；这一批负责"真的跑起来并把输出收回来"。执行器的失败模式
//   全都**不是编译错误**，而是它自己出问题：
//     · 子进程写满管道缓冲区、我们还在傻等 → 双方死锁（表现为"卡住 15 分钟"）
//     · 超时只杀了 plncmp，没杀它自己调起的 C++ 编译器 → 孤儿进程占着 .obj/.dll
//       文件句柄，下一次编译直接失败
//     · 第一步就失败却继续跑第二步 → 一串"文件不存在"把真正的错误埋掉
//     · 可执行文件根本不存在，却报"编译完成"
//   这几条都无法靠读代码确信，必须真的起进程去撞。所以下面全部是**真跑进程**。
//
// 【为什么让测试进程自己当子进程】
//   最省事的做法是调 `cmd.exe /c ...` 或 `ping`。但：① 这台机器上 `cmd.exe` 从
//   构建沙箱里被拦；② 依赖系统工具会让 CI 变得脆。所以测试进程用 `--child <mode>`
//   把自己重新拉起来当被测子进程 —— 零外部依赖，而且行为完全可控（能精确地
//   "睡 3 秒再写文件"）。
//
// 【手工探针】
//   本文件同时是个手工工具：`test_craftrunner <工程文件>` 会真的把该工程编译一遍
//   并打印每一步的命令行、退出码、耗时与输出。**这个模式会真的调 CRAFT 编译器、
//   真的在工程目录里生成中间产物**，所以它不在 CI 里跑（ctest 不带参数）。

#include "../src/language/CraftRunner.h"
#include "../src/language/CraftProject.h"
#include "../src/language/CraftHost.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

using namespace xfs::craft;

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static std::wstring SelfPath() {
    wchar_t buf[4096];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, 4096);
    return std::wstring(buf, n);
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                        nullptr, 0, nullptr, nullptr);
    std::string out((std::size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n,
                          nullptr, nullptr);
    return out;
}

static bool FileExists(const std::wstring& p) {
    return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 每个用例一个独立的临时文件名，避免并发 ctest 互相踩。
static std::wstring TempMarker(const wchar_t* tag) {
    wchar_t dir[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, dir);
    std::wstring p = dir;
    p += L"xfs_craftrunner_";
    p += tag;
    p += L"_";
    p += std::to_wstring((unsigned long)::GetCurrentProcessId());
    p += L".tmp";
    ::DeleteFileW(p.c_str());
    return p;
}

static BuildStep MkStep(const wchar_t* exe, const std::wstring& args,
                        const std::wstring& cwd = std::wstring()) {
    BuildStep s;
    s.exe = exe;
    s.args = args;
    s.cwd = cwd;
    s.label = exe;
    return s;
}

static bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 子进程模式：把测试进程自己重新拉起来，扮演各种"被测子进程"
//   调用形态一律是  <self> --child <mode> [参数...]
// ---------------------------------------------------------------------------

static int RunChild(int argc, wchar_t** argv) {
    if (argc < 3) return 90;
    const std::wstring mode = argv[2];

    if (mode == L"ok") {
        std::fputs("hello from child\n", stdout);
        return 0;
    }
    if (mode == L"fail") {
        std::fputs("child exploded on purpose\n", stderr);
        return 3;
    }
    if (mode == L"echo") {   // 把剩余参数原样打回来，用来验证命令行传参
        for (int i = 3; i < argc; ++i) {
            if (i > 3) std::fputc(' ', stdout);
            std::fputs(ToUtf8(argv[i]).c_str(), stdout);
        }
        std::fputc('\n', stdout);
        return 0;
    }
    if (mode == L"cwd") {    // 打印自己的工作目录，用来验证 step.cwd 被采纳
        wchar_t buf[4096] = {0};
        ::GetCurrentDirectoryW(4096, buf);
        std::fputs(ToUtf8(buf).c_str(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    if (mode == L"spew") {   // 狂写输出：撞管道缓冲区，验证读取线程真的在排空
        const long kb = argc > 3 ? std::wcstol(argv[3], nullptr, 10) : 64;
        std::string chunk(4096, 'A');
        long left = kb * 1024;
        while (left > 0) {
            const std::size_t n =
                (std::size_t)(left < (long)chunk.size() ? left : (long)chunk.size());
            std::fwrite(chunk.data(), 1, n, stdout);
            left -= (long)n;
        }
        std::fflush(stdout);
        return 0;
    }
    if (mode == L"sleep") {
        const long ms = argc > 3 ? std::wcstol(argv[3], nullptr, 10) : 1000;
        ::Sleep((DWORD)ms);
        return 0;
    }
    if (mode == L"mark") {   // 睡 ms 毫秒后写下标记文件 —— 用来证明"它还活着"
        const std::wstring path = argc > 3 ? argv[3] : L"";
        const long ms = argc > 4 ? std::wcstol(argv[4], nullptr, 10) : 1000;
        ::Sleep((DWORD)ms);
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const char tag[] = "alive";
            DWORD wrote = 0;
            ::WriteFile(h, tag, sizeof(tag) - 1, &wrote, nullptr);
            ::CloseHandle(h);
        }
        return 0;
    }
    if (mode == L"tree") {
        // 拉起一个"孙子进程"（同样是本进程），然后自己长睡不醒。
        // 孙子进程**继承了我们交给子进程的那根管道写端** —— 这正是"超时强杀必须
        // 带走整棵进程树"的场景：只杀子进程的话，管道不会关，读取线程会一直等。
        const std::wstring marker = argc > 3 ? argv[3] : L"";
        const long ms = argc > 4 ? std::wcstol(argv[4], nullptr, 10) : 3000;

        std::wstring cmd = L"\"" + SelfPath() + L"\" --child mark \"" + marker +
                           L"\" " + std::to_wstring(ms);
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
        ::Sleep(INFINITE);
        return 0;
    }
    return 91;
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

static void RunSuccess() {
    std::printf("[1] 正常成功：退出码 0，stdout 收回\n");
    const std::wstring self = SelfPath();
    const StepResult r = RunStep(self, MkStep(L"plncmp", L"--child ok"), 10000);
    CHECK(!r.spawnFailed);
    CHECK(!r.timedOut);
    CHECK(r.exitCode == 0);
    CHECK(Contains(r.output, "hello from child"));
}

static void RunFailure() {
    std::printf("[2] 非零退出码：stderr 必须也被收回来（合并到同一根管道）\n");
    const std::wstring self = SelfPath();
    const StepResult r = RunStep(self, MkStep(L"plncmp", L"--child fail"), 10000);
    CHECK(!r.spawnFailed);
    CHECK(!r.timedOut);
    CHECK(r.exitCode == 3);
    CHECK(Contains(r.output, "child exploded on purpose"));
}

static void RunSpawnFailed() {
    std::printf("[3] 启动失败：空路径 / 不存在的路径都不能假装成功\n");
    const std::wstring self = SelfPath();

    const StepResult a = RunStep(L"", MkStep(L"plncmp", L"--child ok"), 5000);
    CHECK(a.spawnFailed);
    CHECK(a.exitCode == -1);
    CHECK(a.output.empty());

    const StepResult b = RunStep(L"C:\\__no_such_dir__\\__no_such_tool__.exe",
                                 MkStep(L"plncmp", L"--child ok"), 5000);
    CHECK(b.spawnFailed);
    CHECK(b.exitCode == -1);

    (void)self;
}

static void RunTimeout() {
    std::printf("[4] 超时强杀：到点必须回来，不能一直等\n");
    const std::wstring self = SelfPath();
    const unsigned long long t0 = ::GetTickCount64();
    const StepResult r = RunStep(self, MkStep(L"plncmp", L"--child sleep 30000"), 1500);
    const unsigned long long spent = ::GetTickCount64() - t0;

    CHECK(r.timedOut);
    CHECK(!r.spawnFailed);
    CHECK(spent >= 1400);          // 不能提前返回（那就没等）
    CHECK(spent < 8000);           // 也不能等到子进程自己睡醒
    CHECK(r.elapsedMs >= 1400);
    CHECK(r.elapsedMs < 8000);
}

// 这一条是整个文件里最重要的：验证 Job Object 真的把**整棵进程树**带走了。
//
//   子进程（tree）拉起孙子进程（mark），孙子在 3000ms 后写标记文件；我们只给
//   2000ms 超时。
//     · Job Object 生效  → 2000ms 时父子一起被杀 → 标记文件**不存在**，
//                          且管道写端全关 → 读取线程立刻拿到 EOF → 秒回。
//     · Job Object 失效  → 只杀子进程；孙子活到 3000ms 写下标记文件，并且它
//                          一直握着管道写端 → 读取线程要等到孙子退出才 EOF。
//   所以两个断言（标记文件不存在 / 耗时 < 6s）都能区分这两种世界，而且**都不会
//   把测试挂死** —— 最坏情况也就多等 3 秒。
static void RunTreeKill() {
    std::printf("[5] 超时强杀必须带走整棵进程树（否则留下孤儿编译器占文件句柄）\n");
    const std::wstring self = SelfPath();
    const std::wstring marker = TempMarker(L"tree");

    const unsigned long long t0 = ::GetTickCount64();
    const StepResult r = RunStep(
        self, MkStep(L"plncmp", L"--child tree \"" + marker + L"\" 3000"), 2000);
    const unsigned long long spent = ::GetTickCount64() - t0;

    CHECK(r.timedOut);
    CHECK(!FileExists(marker));    // 孙子进程必须在写标记之前就被杀
    CHECK(spent < 6000);           // 管道必须随进程树一起关闭，不能等孙子自然退出
    ::DeleteFileW(marker.c_str());
}

// 管道排空：子进程写 512KB，远超管道缓冲区（默认 4KB 量级）。没有读取线程的话
// 子进程会在写满时阻塞、我们会在超时上白等 15 分钟。
static void RunSpew() {
    std::printf("[6] 大输出：读取线程必须排空管道，否则双方死锁\n");
    const std::wstring self = SelfPath();
    const StepResult r = RunStep(self, MkStep(L"plncmp", L"--child spew 512"), 20000);
    CHECK(!r.spawnFailed);
    CHECK(!r.timedOut);
    CHECK(r.exitCode == 0);
    CHECK(r.output.size() == 512u * 1024u);
}

static void RunCwdAndArgs() {
    std::printf("[7] 工作目录与命令行传参\n");
    const std::wstring self = SelfPath();

    wchar_t tmp[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tmp);
    // 去掉结尾反斜杠，免得命令行里出现 `"C:\...\Temp\"` 这种被转义的形态
    std::wstring dir = tmp;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();

    const StepResult a = RunStep(self, MkStep(L"plncmp", L"--child cwd", dir), 10000);
    CHECK(!a.spawnFailed);
    CHECK(a.exitCode == 0);
    // 注意：子进程的 stdout 是文本模式，`\n` 会被写成 `\r\n` —— 所以取第一行时
    // 必须把 `\r` 也剥掉。这不是测试的小事：**CRAFT 的输出同样是 `\r\n`**，
    // 批次 96 的编译输出面板必须处理行尾 `\r`，否则跳转位置会算错一列。
    std::string got = a.output.substr(0, a.output.find('\n'));
    while (!got.empty() && (got.back() == '\r' || got.back() == '\n')) got.pop_back();
    const std::string want = ToUtf8(dir);
    CHECK(got == want);
    if (got != want) std::printf("      got=[%s] want=[%s]\n", got.c_str(), want.c_str());

    // 带空格的参数必须原样到达子进程的 argv（走的是 Windows 自己的命令行解析）
    const StepResult b =
        RunStep(self, MkStep(L"plncmp", L"--child echo \"a b c\" --flag"), 10000);
    CHECK(b.exitCode == 0);
    CHECK(Contains(b.output, "a b c --flag"));
}

static void RunPlanOk() {
    std::printf("[8] 计划全绿：两步都成功\n");
    const std::wstring self = SelfPath();
    std::vector<BuildStep> plan;
    plan.push_back(MkStep(L"plncmp", L"--child ok"));
    plan.push_back(MkStep(L"patcmp", L"--child ok"));

    const BuildResult br = RunPlan(plan, self, self, 10000);
    CHECK(br.launched);
    CHECK(br.allOk);
    CHECK(br.firstFailedStep == -1);
    CHECK(br.steps.size() == 2);
    CHECK(br.note.empty());
}

static void RunPlanStopsOnFailure() {
    std::printf("[9] 失败即停：第一步挂了就不能再跑第二步\n");
    const std::wstring self = SelfPath();
    std::vector<BuildStep> plan;
    plan.push_back(MkStep(L"plncmp", L"--child fail"));
    plan.push_back(MkStep(L"patcmp", L"--child ok"));

    const BuildResult br = RunPlan(plan, self, self, 10000);
    CHECK(br.launched);
    CHECK(!br.allOk);
    CHECK(br.firstFailedStep == 0);
    CHECK(br.steps.size() == 1);          // 第二步根本没跑
    CHECK(br.steps[0].exitCode == 3);
}

static void RunPlanSkipsUnknownTool() {
    std::printf("[10] 认不出的工具名跳过，而不是猜一个路径去执行\n");
    const std::wstring self = SelfPath();
    std::vector<BuildStep> plan;
    plan.push_back(MkStep(L"plncmp", L"--child ok"));
    plan.push_back(MkStep(L"mysterytool", L"--child ok"));

    const BuildResult br = RunPlan(plan, self, self, 10000);
    CHECK(br.steps.size() == 1);
    CHECK(br.allOk);
}

static void RunPlanMissingToolchain() {
    std::printf("[11] 工具链缺失：不能报成功，也不能假装「已启动」\n");
    std::vector<BuildStep> plan;
    plan.push_back(MkStep(L"plncmp", L"--child ok"));

    const BuildResult a = RunPlan(plan, L"", L"", 5000);
    CHECK(!a.launched);
    CHECK(!a.allOk);
    CHECK(a.steps.size() == 1);
    CHECK(a.steps[0].spawnFailed);
    CHECK(!a.note.empty());

    const BuildResult b = RunPlan(std::vector<BuildStep>(), L"", L"", 5000);
    CHECK(!b.launched);
    CHECK(!b.allOk);
    CHECK(b.steps.empty());
    CHECK(!b.note.empty());
}

// ---------------------------------------------------------------------------
// 手工探针：test_craftrunner <工程文件> —— 真的编译一遍
//   走的是 CraftHost::LoadProject —— **和 UI 完全同一段读盘代码**，不是这里另写
//   一份。否则探针验证过的路径和 UI 跑的路径会悄悄分叉。
// ---------------------------------------------------------------------------

static int ProbeProject(const wchar_t* anyPath) {
    const Project proj = LoadProject(anyPath);
    std::printf("== test_craftrunner probe ==\n");
    std::printf("input      : %s\n", ToUtf8(anyPath).c_str());
    std::printf("project ok : %s\n", proj.ok ? "yes" : "NO");
    if (!proj.ok) return 1;
    std::printf("root       : %s\n", ToUtf8(proj.root).c_str());
    std::printf("pln        : %s\n", ToUtf8(proj.plnName).c_str());
    std::printf("interDir   : %s\n", ToUtf8(proj.interDir).c_str());
    std::printf("makefile   : %s  (exists=%d)\n", ToUtf8(proj.makefilePath).c_str(),
                (int)proj.vars.hasMakefile);

    const Toolchain tc = DetectToolchainFromEnv(L"");
    std::printf("craftHome  : [%s]\n", ToUtf8(tc.craftHome).c_str());
    std::printf("toolchain  : plncmp=[%s] (%s)\n", ToUtf8(tc.plncmp).c_str(),
                ToUtf8(ToolOriginName(tc.plncmpOrigin)).c_str());
    std::printf("             patcmp=[%s] (%s)\n", ToUtf8(tc.patcmp).c_str(),
                ToUtf8(ToolOriginName(tc.patcmpOrigin)).c_str());

    const std::vector<BuildStep> plan = PlanBuild(proj);
    std::printf("steps      : %d\n", (int)plan.size());
    for (const BuildStep& s : plan)
        std::printf("             %s %s\n", ToUtf8(s.exe).c_str(),
                    ToUtf8(s.args).c_str());

    if (!tc.Complete()) {
        std::printf("SKIP: 工具链不完整，不执行 —— 这就是 UI 上要走的那条降级路径\n");
        return 2;
    }

    std::printf("-- 真的开始编译（会往工程目录写中间产物）--\n");
    const BuildResult br = RunPlan(plan, tc.plncmp, tc.patcmp, kDefaultStepTimeoutMs);
    for (std::size_t i = 0; i < br.steps.size(); ++i) {
        const StepResult& s = br.steps[i];
        std::printf("[step %d] %s\n", (int)i, ToUtf8(s.label).c_str());
        std::printf("  cmd    : %s %s\n", ToUtf8(s.exe).c_str(), ToUtf8(s.args).c_str());
        std::printf("  exit   : %d  spawnFailed=%d timedOut=%d  %ums\n", s.exitCode,
                    (int)s.spawnFailed, (int)s.timedOut, s.elapsedMs);
        std::printf("  output : %d bytes\n", (int)s.output.size());
        std::printf("%s\n", s.output.c_str());
    }
    std::printf("allOk      : %s\n", br.allOk ? "yes" : "NO");
    if (!br.note.empty()) std::printf("note       : %s\n", ToUtf8(br.note).c_str());
    return br.allOk ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 2 && std::wstring(argv[1]) == L"--child") return RunChild(argc, argv);
    if (argc > 1) return ProbeProject(argv[1]);

    std::printf("== test_craftrunner ==\n");
    RunSuccess();
    RunFailure();
    RunSpawnFailed();
    RunTimeout();
    RunTreeKill();
    RunSpew();
    RunCwdAndArgs();
    RunPlanOk();
    RunPlanStopsOnFailure();
    RunPlanSkipsUnknownTool();
    RunPlanMissingToolchain();

    if (g_fail) {
        std::printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    std::printf("ALL PASSED\n");
    return 0;
}
