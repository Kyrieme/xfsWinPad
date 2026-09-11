// xfsWinPad - 插件清单解析 + 已安装注册表逻辑单测（不依赖真实控件）。
#include "../src/plugin/PluginCatalog.h"
#include "../src/plugin/PluginRegistry.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <string>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); return 1; } } while (0)

using namespace xfs;
namespace fs = std::filesystem;

static int failures = 0;

int main() {
    // ---- 1) 清单解析 ----
    const std::string sample = R"(
{
    "name": "Test List",
    "version": "3.2.1",
    "arch": "x86",
    "npp-plugins": [
        {
            "folder-name": "Alpha",
            "display-name": "Alpha Plugin",
            "version": "1.0.0",
            "repository": "https://x/alpha.zip",
            "description": "first",
            "author": "someone",
            "homepage": "https://alpha"
        },
        { "folder-name": "Beta", "display-name": "Beta Tool", "version": "2.5.0" }
    ]
}
)";
    PluginCatalog cat;
    const bool ok = ParsePluginList(sample, cat);
    CHECK(ok, "ParsePluginList success");
    CHECK(cat.listName == L"Test List", "listName");
    CHECK(cat.listVersion == L"3.2.1", "listVersion");
    CHECK(cat.plugins.size() == 2, "plugin count");
    if (cat.plugins.size() == 2) {
        CHECK(cat.plugins[0].folderName == L"Alpha", "folder Alpha");
        CHECK(cat.plugins[0].displayName == L"Alpha Plugin", "display Alpha");
        CHECK(cat.plugins[0].version == L"1.0.0", "version Alpha");
        CHECK(cat.plugins[0].description == L"first", "desc Alpha");
        CHECK(cat.plugins[1].folderName == L"Beta", "folder Beta");
        CHECK(cat.plugins[1].version == L"2.5.0", "version Beta");
    }

    // ---- 2) 非法 JSON 拒绝 ----
    PluginCatalog bad;
    CHECK(!ParsePluginList("not json at all", bad), "reject malformed");

    // ---- 3) 默认清单可解析且包含种子 ----
    bool used = false;
    PluginCatalog dft;
    const std::wstring tmpDir = [] {
        wchar_t p[MAX_PATH]; ::GetTempPathW(MAX_PATH, p);
        return std::wstring(p) + L"xfs_test_plugin_catalog";
    }();
    std::error_code ec0;
    fs::remove_all(tmpDir, ec0);   // 清掉上次失败运行残留的 installed.json
    fs::create_directories(tmpDir);
    CHECK(LoadPluginCatalog(tmpDir, dft, used), "load defaults with missing file");
    CHECK(used, "usedDefaults true");
    CHECK(dft.plugins.size() == 5, "default has 5 curated seeds");
    bool hasNpp = false;
    bool hasDoxy = false;
    for (const auto& p : dft.plugins) {
        if (p.folderName == L"NppExec") hasNpp = true;
        if (p.folderName == L"DoxyIt" && !p.repository.empty()) hasDoxy = true;
    }
    CHECK(hasNpp, "default seeds NppExec");
    CHECK(hasDoxy, "default seeds DoxyIt with repository URL");

    // ---- 4) 版本比较 ----
    CHECK(ComparePluginVersions(L"1.9", L"1.10") < 0, "1.9 < 1.10");
    CHECK(ComparePluginVersions(L"2.0.1", L"2.0.1") == 0, "equal");
    CHECK(ComparePluginVersions(L"3.0", L"2.99.999") > 0, "3.0 > 2.99.999");
    CHECK(ComparePluginVersions(L"1.0", L"1") > 0, "1.0 > 1");

    // ---- 5) 注册表：安装/更新/持久化/移除 ----
    PluginRegistry reg(tmpDir);
    reg.Refresh();
    CHECK(!reg.IsInstalled(L"DemoDeploy"), "nothing installed initially");

    reg.MarkInstalled(L"DemoDeploy", L"1.2.0");
    CHECK(reg.IsInstalled(L"DemoDeploy"), "installed after Mark");
    CHECK(reg.VersionOf(L"DemoDeploy") == L"1.2.0", "version recorded");
    CHECK(!reg.IsRealFile(L"DemoDeploy"), "virtual install is not a real file");

    reg.MarkInstalled(L"DemoDeploy", L"2.0.0");   // 更新到更高版本
    CHECK(reg.VersionOf(L"DemoDeploy") == L"2.0.0", "version updated");

    // 持久化：新建同目录实例应仍能看到
    PluginRegistry reg2(tmpDir);
    reg2.Refresh();
    CHECK(reg2.IsInstalled(L"DemoDeploy"), "persisted across registry instance");
    CHECK(reg2.VersionOf(L"DemoDeploy") == L"2.0.0", "persisted version");

    // 虚拟安装可干净移除
    const bool clean = reg2.TryUninstall(L"DemoDeploy");
    CHECK(clean, "virtual uninstall returns true");
    CHECK(!reg2.IsInstalled(L"DemoDeploy"), "removed after uninstall");

    // 清理临时目录
    std::error_code ec;
    fs::remove_all(tmpDir, ec);

    if (failures) std::printf("%d failures\n", failures);
    std::printf("plugin_catalog: all ok\n");
    return 0;
}