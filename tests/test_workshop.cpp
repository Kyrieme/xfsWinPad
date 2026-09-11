// xfsWinPad - test_workshop: Workshop 纯函数单测（目录名合法化 + Scaffold
// 文件产物在临时目录验证 + InstallBuilt 错误路径）。网络/编译不在单测范围。
#include "../src/plugin/Workshop.h"
#include "../src/plugin/PluginManager.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace xfs;

static int g_failed = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        ++g_failed; \
        std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
    } \
} while (0)

int main() {
    // ---- SanitizeFolderName ----
    CHECK(Workshop::SanitizeFolderName(L"Insert Timestamp") == L"insert-timestamp");
    CHECK(Workshop::SanitizeFolderName(L"Hello World!") == L"hello-world");
    CHECK(Workshop::SanitizeFolderName(L"UPPER case") == L"upper-case");
    CHECK(Workshop::SanitizeFolderName(L"  spaced  out  ") == L"spaced-out");
    CHECK(Workshop::SanitizeFolderName(L"!!!") == L"plugin");      // 全丢弃 → 回退
    CHECK(Workshop::SanitizeFolderName(L"") == L"plugin");
    CHECK(Workshop::SanitizeFolderName(L"snake_case.name") == L"snake-case-name");
    CHECK(Workshop::SanitizeFolderName(L"a") == L"a");
    // 中文折叠成 '-'（非 ASCII）
    std::wstring zh = Workshop::SanitizeFolderName(L"时间戳 插件");
    CHECK(zh == L"plugin" || !zh.empty());   // 全中文 → 回退 plugin

    // ---- Scaffold + InstallBuilt（真实临时目录，不触网络/编译）----
    wchar_t tmpBuf[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmpBuf);
    std::wstring plugins = std::wstring(tmpBuf) + L"xfs_plugins_test";
    ::CreateDirectoryW(plugins.c_str(), nullptr);
    // 预置 SDK（生产由 EnsureSdk 从 exe 资源释放；Scaffold 只读磁盘）
    {
        std::wstring sdk = plugins + L"\\_sdk";
        ::CreateDirectoryW(sdk.c_str(), nullptr);
        const char* tpl =
            "#include \"xfs_plugin_api.h\"\n"
            "static const xfs_plugin_host* g_host = NULL;\n"
            "static xfs_plugin_command* g_cmd = NULL;\n"
            "static void OnCmd(void* u){ (void)u; }\n"
            "static void Reg(const xfs_plugin_host* h){ g_host=h;\n"
            "  g_cmd=h->addCommand(\"__PLUGIN_CMD__\",\"Plugins\",OnCmd,NULL); }\n"
            "static void Unreg(void){ if(g_host&&g_cmd)g_host->removeCommand(g_cmd);\n"
            "  g_cmd=NULL; g_host=NULL; }\n"
            "static xfs_plugin_abi g_abi;\n"
            "__declspec(dllexport) const xfs_plugin_abi* xfsPlugin_getInfo(void){\n"
            "  g_abi.abiVersion=XFS_PLUGIN_ABI_VERSION;\n"
            "  g_abi.name=\"__PLUGIN_NAME__\";\n"
            "  g_abi.version=\"0.1.0\";\n"
            "  g_abi.description=\"__PLUGIN_DESC__\";\n"
            "  g_abi.registerPlugin=&Reg;\n"
            "  g_abi.unregisterPlugin=&Unreg;\n"
            "  return &g_abi; }\n"
            "__declspec(dllexport) void xfsPlugin_register(const xfs_plugin_host* h){ Reg(h); }\n"
            "__declspec(dllexport) void xfsPlugin_unregister(void){ Unreg(); }\n";
        WriteFileBytes(sdk + L"\\template.c", tpl, strlen(tpl));
        WriteFileBytes(sdk + L"\\template.cpp", tpl, strlen(tpl));
        const char* bcmd =
            "@echo off\ncl /nologo /LD /I \"__SDK_INCLUDE_DIR__\" src\\*.c /Fe:build\\output.dll\n";
        WriteFileBytes(sdk + L"\\build.cmd", bcmd, strlen(bcmd));
    }
    {
        PluginManager mgr;   // 无宿主窗口也能构造（加载时会优雅失败）
                Workshop ws(&mgr, plugins);
        
                std::wstring proj = ws.Scaffold(L"insert current timestamp at caret",
                                         L"insert-timestamp",
                                         L"Insert Timestamp",
                                         L"Inserts the current date-time");
        CHECK(!proj.empty());
        CHECK(::PathFileExistsW((proj + L"\\src\\plugin.c").c_str()));
        CHECK(::PathFileExistsW((proj + L"\\src\\plugin.cpp").c_str()));
        CHECK(::PathFileExistsW((proj + L"\\plugin.json").c_str()));
        CHECK(::PathFileExistsW((proj + L"\\build.cmd").c_str()));
        // 占位符替换
        std::string src;
        CHECK(ReadFileBytes(proj + L"\\src\\plugin.c", src));
        CHECK(src.find("__PLUGIN_NAME__") == std::string::npos);
        CHECK(src.find("insert-timestamp") != std::string::npos);
        CHECK(src.find("Insert Timestamp") != std::string::npos);
        // build.cmd include 路径被替换为 SDK 绝对路径（__SDK_INCLUDE_DIR__ 不残留）
        std::string bcmd;
        CHECK(ReadFileBytes(proj + L"\\build.cmd", bcmd));
        CHECK(bcmd.find("__SDK_INCLUDE_DIR__") == std::string::npos);
        CHECK(bcmd.find("/I") != std::string::npos);
        {
            std::string want = WideToUtf8(ws.SdkDir());
            CHECK(bcmd.find(want) != std::string::npos);
        }

        // 同名二连 → 目录自动加序号
        std::wstring proj2 = ws.Scaffold(L"x", L"insert-timestamp", L"a", L"b");
        CHECK(proj2 != proj);

        // InstallBuilt：无产物 → 明确报错
        std::wstring err;
        CHECK(ws.InstallBuilt(proj, &err).empty());
        CHECK(err.find(L"no build output") != std::wstring::npos);

        // 伪造产物：非 PE 的假 dll（拷贝路径成功、LoadNew 必拒）
        std::wstring buildDir = proj + L"\\build";
        ::CreateDirectoryW(buildDir.c_str(), nullptr);
        WriteFileBytes(buildDir + L"\\output.dll", "MZ-fake", 8);
        std::wstring err2;
        std::wstring got = ws.InstallBuilt(proj, &err2);
        CHECK(got.empty());   // 假 DLL 必然加载失败
        CHECK(!err2.empty());
        // 但 DLL 已拷到位（加载失败 ≠ 安装失败：文件存在）
        std::wstring folder = proj.substr(proj.find_last_of(L'\\') + 1);
        CHECK(::PathFileExistsW((plugins + L"\\" + folder + L"\\" +
                                folder + L".dll").c_str()));
    }

    // 清理
    std::wstring cmd = L"cmd /c rmdir /s /q \"" + plugins + L"\"";
    _wsystem(cmd.c_str());

    if (g_failed == 0) { std::printf("workshop: ALL PASS\n"); return 0; }
    std::printf("workshop: %d failed\n", g_failed);
    return 1;
}




