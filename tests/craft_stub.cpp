// xfsWinPad - CRAFT 工具链桩（批次 101，给 scripts/craft-e2e.ps1 用；**不入 ctest**）
//
// 【它为什么存在】
//   craft-e2e.ps1 要验证的是「编译输出面板里双击一行 → 编辑器跳到那一行」。开发机上
//   没有 CRAFT（真机在虚拟机里），CI 更没有 —— 所以这条链路在此之前**从来没被自动
//   验证过**：解析器层有 286 条断言、六份真机样本，但"面板把行显示出来、用户双击、
//   编辑器真的跳过去"这一段只有人眼看过。
//
//   本程序就是那个"装出来的 plncmp / patcmp"：把它复制成 plncmp.exe / patcmp.exe
//   放进一个目录，再把该目录写进设置的 craftToolDir，GUI 就会**走它自己那条真实的
//   编译路径**（LoadProject → DetectToolchain → PlanBuild → RunPlan → RowsFromBuild）
//   跑起来。这是刻意的选择：CraftHost.h 的文件头写明，探针若走另一份代码，"两边会
//   悄悄分叉，结果是探针全绿、UI 不工作"。桩只替换**最外层的编译器进程**，不替换
//   我们自己的任何一行逻辑。
//
// 【为什么不用 cmd.exe 或系统工具凑一个】
//   · 本项目的构建沙箱里 cmd.exe 被拦（见 test_craftrunner.cpp 文件头），不能当依赖；
//   · 桩要能精确控制"输出字节"和"退出码"，系统工具做不到。
//
// 【输出与退出码从磁盘读，不编进程序】
//   <自己的目录>\samples\<自己的文件名主干>.txt        —— 原样写到 stdout
//   <自己的目录>\samples\<自己的文件名主干>.exit       —— 退出码（缺省 0）
//   命令行里出现 `-f` 时主干加 `_link` 后缀：厂商 makefile 用
//   `patcmp … -f makefile_pdt0.lst` 区分"编译单个 .pat"与"链接全部 .pdt"两步
//   （见 CraftProject.cpp 的 PlanBuild），桩沿用同一个信号。
//
//   这样样本只存一份（在脚本里），改样本不用重编桩；桩本身对 CRAFT 的格式一无所知。
//   退出码可配是必需的：桩若永远返回 0，RunPlan 的"失败即停"、摘要里的"N/M 步失败"
//   就都测不到。
//
// 【输出必须二进制透传】
//   样本里有 `\r\r\n` 这种行尾（CRAFT 中继子进程输出的那一段）。若按文本模式写
//   stdout，C 运行时会再展开一层变成 `\r\r\r\n` —— 那就不是"逐字节真实"了，而这份
//   样本的全部价值正在于格式是真的。

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

std::wstring SelfPath() {
    wchar_t buf[32768] = {0};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, 32768);
    if (n == 0 || n >= 32768) return std::wstring();
    return std::wstring(buf, n);
}

bool HasArg(int argc, wchar_t** argv, const wchar_t* want) {
    for (int i = 1; i < argc; ++i)
        if (std::wstring(argv[i]) == want) return true;
    return false;
}

// 退出码文件：一行 ASCII 数字。读不出来就当 0（正常成功）。
int ReadExitCode(const fs::path& p) {
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return 0;
    char buf[32] = {0};
    const std::size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (got == 0) return 0;
    return std::atoi(buf);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // 二进制 stdout：见文件头「输出必须二进制透传」
    _setmode(_fileno(stdout), _O_BINARY);

    const fs::path self(SelfPath());
    const std::wstring stem = self.stem().wstring();
    const bool linking = HasArg(argc, argv, L"-f");

    const std::wstring base = stem + (linking ? L"_link" : L"");
    const fs::path dir = self.parent_path();
    const fs::path sample   = dir / L"samples" / (base + L".txt");
    const fs::path exitFile = dir / L"samples" / (base + L".exit");

    std::FILE* f = nullptr;
    if (_wfopen_s(&f, sample.c_str(), L"rb") != 0 || !f) {
        // 走 stderr —— RunStep 把 stdout/stderr 合并到同一根管道，所以这条会出现在
        // 编译输出面板里。桩配错时必须**看得见**，不能静默产出空输出。
        std::fprintf(stderr, "craft_stub: sample not found: %ls\n", sample.c_str());
        return 99;
    }
    char buf[65536];
    std::size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
        std::fwrite(buf, 1, got, stdout);
    std::fclose(f);
    std::fflush(stdout);

    return ReadExitCode(exitFile);
}
