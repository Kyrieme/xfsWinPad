#include "language/CraftRunner.h"

#include <thread>

namespace xfs {
namespace craft {

namespace {

// 把命令行参数切成"可执行文件路径 + 参数"两段：CreateProcessW 的 lpCommandLine
// 必须是可变的完整命令行，且第一个 token 若是带空格的路径必须加引号。
std::wstring QuoteIfNeeded(const std::wstring& s) {
    if (s.empty()) return L"\"\"";
    if (s.find(L' ') == std::wstring::npos && s.find(L'\t') == std::wstring::npos)
        return s;
    std::wstring q = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') q += L'\\';
        q.push_back(c);
    }
    q += L'"';
    return q;
}

unsigned NowMs() {
    return (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Job Object：KILL_ON_JOB_CLOSE。进程被 TerminateProcess 之后，系统会把 Job 里
// 的**所有**后代一起收掉 —— plncmp 自己调的 C++ 编译器就在里面。
HANDLE MakeKillOnCloseJob() {
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li,
                                   sizeof(li))) {
        ::CloseHandle(job);
        return nullptr;
    }
    return job;
}

} // namespace

StepResult RunStep(const std::wstring& exePath, const BuildStep& step, DWORD timeoutMs) {
    StepResult r;
    r.exe = exePath;
    r.args = step.args;
    r.cwd = step.cwd;
    r.label = step.label;

    if (exePath.empty()) {
        r.spawnFailed = true;
        return r;
    }

    // ---- 管道：stdout 与 stderr 合并到同一个写端（与 GitClient::Run 同口径）----
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) {
        r.spawnFailed = true;
        return r;
    }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    // stdin 必须给一个**有效**句柄：xfsWinPad 是 GUI 进程，本身没有控制台，
    // GetStdHandle(STD_INPUT_HANDLE) 会返回 NULL。而 STARTF_USESTDHANDLES 一旦
    // 置位，子进程的 stdin 就是我们给的那个值 —— 给 NULL 的话，万一某个工具
    // 顺手读一下 stdin，拿到的是无效句柄，行为未定义。接 NUL 设备最省事。
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;          // 编译器的控制台窗口不要弹出来
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul ? nul : ::GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd = QuoteIfNeeded(exePath);
    if (!step.args.empty()) {
        cmd += L' ';
        cmd += step.args;
    }
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    HANDLE job = MakeKillOnCloseJob();

    PROCESS_INFORMATION pi{};
    const DWORD t0 = NowMs();
    const BOOL started = ::CreateProcessW(
        nullptr, buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0),
        nullptr, step.cwd.empty() ? nullptr : step.cwd.c_str(), &si, &pi);
    ::CloseHandle(wr);   // 写端交出去之后必须关掉，否则 ReadFile 永远等不到 EOF
    if (!started) {
        ::CloseHandle(rd);
        if (job) ::CloseHandle(job);
        r.spawnFailed = true;
        r.elapsedMs = NowMs() - t0;
        return r;
    }

    // 先入 Job 再放行：CREATE_SUSPENDED 保证子进程在入 Job 之前一行都不跑，
    // 否则它可能在入 Job 前就自己 fork 出孙子进程（那个孙子就跑不掉了）。
    if (job) {
        ::AssignProcessToJobObject(job, pi.hProcess);
        ::ResumeThread(pi.hThread);
    }

    // 读取线程：不排空管道的话，子进程写满缓冲区就会自己卡住，
    // 而我们这边还在等超时 —— 变成"死锁 15 分钟"。
    std::string out;
    std::thread reader([&rd, &out]() {
        char chunk[4096];
        DWORD got = 0;
        for (;;) {
            if (!::ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) || got == 0) break;
            out.append(chunk, got);
        }
    });

    const DWORD wait = ::WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = (int)code;
    } else {
        r.timedOut = true;
        // 关掉 Job 会连带杀掉 plncmp 及其派生的编译器（整棵进程树）。
        // 再补一发 TerminateProcess 兜底：AssignProcessToJobObject 在"父进程已
        // 处于不允许嵌套的 Job 里"这类情况下会失败，那时 Job 是空的，只靠它就
        // 会留下一个永远不死的子进程 —— 而它还握着管道写端，读取线程会一直等。
        if (job) {
            ::CloseHandle(job);
            job = nullptr;
        }
        ::TerminateProcess(pi.hProcess, 1);
    }
    reader.join();   // 进程一死（或 Job 一关），写端关闭 → ReadFile 拿到 EOF
    r.output = std::move(out);
    r.elapsedMs = (unsigned)(NowMs() - t0);

    if (job) ::CloseHandle(job);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    return r;
}

BuildResult RunPlan(const std::vector<BuildStep>& steps,
                    const std::wstring& plncmpPath,
                    const std::wstring& patcmpPath,
                    DWORD timeoutMsPerStep) {
    BuildResult br;
    for (const BuildStep& s : steps) {
        // 按工具名分派；认不出的工具名直接跳过（CraftProject 只产出这两个，
        // 但将来可能加 —— 跳过比猜路径安全）
        std::wstring exePath;
        if (s.exe == L"plncmp") exePath = plncmpPath;
        else if (s.exe == L"patcmp") exePath = patcmpPath;
        else continue;

        StepResult sr = RunStep(exePath, s, timeoutMsPerStep);
        if (!sr.spawnFailed) br.launched = true;
        const bool ok = !sr.spawnFailed && !sr.timedOut && sr.exitCode == 0;
        if (!ok && br.firstFailedStep < 0) br.firstFailedStep = (int)br.steps.size();
        br.steps.push_back(std::move(sr));
        if (!ok) break;   // 失败即停：编译没过就不该继续链接
    }
    br.allOk = br.launched && br.firstFailedStep < 0;
    // launched 的语义是"**真的起过进程**"，不是"尝试过"。工具链路径为空时
    // CreateProcess 根本没跑，这时候报"已启动"会让 UI 显示一个假的成功。
    if (!br.launched) {
        br.note = br.steps.empty()
                      ? L"没有可执行的步骤"
                      : L"未能启动编译工具（plncmp / patcmp 路径未配置？）";
    }
    return br;
}

} // namespace craft
} // namespace xfs
