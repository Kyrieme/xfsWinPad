// xfsWinPad - 插件安装器单测：下载/解压/落盘编排（注入假实现）+ 真实 miniz ZIP 回环。
// 不依赖网络与真实磁盘 ZIP；真实 miniz 解压路径用本地构造的 ZIP 文件覆盖。
#include "../src/plugin/PluginInstaller.h"
#include "../src/plugin/PluginCatalog.h"
#include "../src/plugin/PluginRegistry.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "miniz.h"   // 构造真实 ZIP 供默认真实解压器使用

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); return 1; } } while (0)

using namespace xfs;
namespace fs = std::filesystem;

// ---------- 假实现注入 ----------

// 成功下载器：把固定假载荷写进 dstPath，并上报 0→100 进度。
class FakeOkDownloader : public Downloader {
public:
    bool called = false;
    bool Download(const std::wstring& url, const std::wstring& dstPath,
                  const std::function<void(const InstallProgress&)>& progress) override {
        called = true;
        InstallProgress p;
        p.phase = InstallProgress::kDownloading; p.percent = 0;
        if (progress) progress(p);
        const std::string fake = "PK-fake-payload:" + WideToUtf8(url);
        if (!WriteFileBytes(dstPath, fake.data(), fake.size())) return false;
        p.percent = 100;
        if (progress) progress(p);
        return true;
    }
};

class FakeFailDownloader : public Downloader {
public:
    bool called = false;
    bool Download(const std::wstring&, const std::wstring&,
                  const std::function<void(const InstallProgress&)>&) override {
        called = true;
        return false;
    }
};

// 成功解压器：在 outDir\plugins\Demo\ 下生成一个假 DLL（模拟 N++ 打包惯例）。
class FakeOkExtractor : public ZipExtractor {
public:
    bool Extract(const std::wstring&, const std::wstring& outDir,
                 std::wstring*) override {
        std::error_code ec;
        fs::create_directories(outDir + L"\\plugins\\Demo", ec);
        const std::string dll = "fake-plugin-dll";
        return WriteFileBytes(outDir + L"\\plugins\\Demo\\NppExec.dll",
                              dll.data(), dll.size());
    }
};

class FakeFailExtractor : public ZipExtractor {
public:
    bool Extract(const std::wstring&, const std::wstring&, std::wstring* errOut) override {
        if (errOut) *errOut = L"fake extract error";
        return false;
    }
};

// 进度记录器：收集回调里出现的 phase/percent 序列。
struct ProgressRecorder {
    std::vector<int> phases;
    std::vector<int> percents;
    std::function<void(const InstallProgress&)> Cb() {
        return [this](const InstallProgress& p) {
            phases.push_back((int)p.phase);
            percents.push_back(p.percent);
        };
    }
    bool Saw(int phase) const {
        for (int ph : phases) if (ph == phase) return true;
        return false;
    }
};

// 用 miniz 文件式写入构造真实 ZIP（条目名 + 内容）。
static bool MakeZip(const std::wstring& zipPath,
                    const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, WideToUtf8(zipPath).c_str(), 0)) return false;
    for (const auto& e : entries) {
        if (!mz_zip_writer_add_mem(&zip, e.first.c_str(),
                                   e.second.data(), e.second.size(), MZ_BEST_SPEED)) {
            mz_zip_writer_end(&zip);
            return false;
        }
    }
    const bool ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}

int main() {
    wchar_t tp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tp);
    const std::wstring root = std::wstring(tp) + L"xfs_test_installer";
    const std::wstring pluginsDir = root + L"\\plugins";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(pluginsDir);
    const std::wstring dl = pluginsDir + L"\\downloads";
    fs::create_directories(dl);

    // ---- 1) NormalizeExtractedRoot 启发式 ----
    {
        const std::wstring t1 = root + L"\\n1";
        fs::create_directories(t1 + L"\\plugins\\x\\y");
        bool had = false;
        CHECK(PluginInstaller::NormalizeExtractedRoot(t1, &had) == (t1 + L"\\plugins"),
              "normalize: plugins root");
        CHECK(had, "normalize: hadPluginsRoot");

        const std::wstring t2 = root + L"\\n2";
        fs::create_directories(t2 + L"\\Single\\inner");
        bool had2 = false;
        CHECK(PluginInstaller::NormalizeExtractedRoot(t2, &had2) == (t2 + L"\\Single"),
              "normalize: single subdir");
        CHECK(!had2, "normalize: single subdir no plugins root");

        const std::wstring t3 = root + L"\\n3";
        fs::create_directories(t3);
        const std::string f = "x";
        WriteFileBytes(t3 + L"\\a.txt", f.data(), f.size());
        bool had3 = false;
        CHECK(PluginInstaller::NormalizeExtractedRoot(t3, &had3) == t3,
              "normalize: flat stays");
    }

    // ---- 2) 假下载 + 假解压：安装成功 ----
    {
        PluginInstaller inst(pluginsDir);
        FakeOkDownloader d; FakeOkExtractor ex;
        inst.SetDownloader(&d); inst.SetExtractor(&ex);
        ProgressRecorder pr;
        std::wstring err;
        CHECK(inst.Install(L"Demo", L"1.0", L"http://example.com/demo.zip", pr.Cb(), &err),
              "install success");
        CHECK(d.called, "downloader invoked");
        CHECK(fs::exists(pluginsDir + L"\\Demo\\NppExec.dll"), "dll copied to plugins\\Demo");
        CHECK(!fs::exists(dl + L"\\_tmp_Demo"), "temp dir cleaned");
        CHECK(fs::exists(dl + L"\\Demo.zip"), "zip cached in downloads");
        CHECK(pr.Saw(InstallProgress::kDone), "kDone emitted");
    }

    // ---- 3) 离线安装：本地包存在则不再调用下载器 ----
    {
        const std::string z = "local-cached-zip-bytes";
        WriteFileBytes(dl + L"\\Offline.zip", z.data(), z.size());
        PluginInstaller inst(pluginsDir);
        FakeFailDownloader d; FakeOkExtractor ex;
        inst.SetDownloader(&d); inst.SetExtractor(&ex);
        std::wstring err;
        CHECK(inst.Install(L"Offline", L"1.0", L"http://example.com/offline.zip", nullptr, &err),
              "offline install ok");
        CHECK(!d.called, "downloader skipped when local zip exists");
    }

    // ---- 4) 无下载地址且无本地包 → 失败并给出明确原因 ----
    {
        PluginInstaller inst(pluginsDir);
        FakeOkDownloader d; FakeOkExtractor ex;
        inst.SetDownloader(&d); inst.SetExtractor(&ex);
        std::wstring err;
        CHECK(!inst.Install(L"NoUrl", L"1.0", L"", nullptr, &err), "no-url install fails");
        // 本测试不加载 I18n 字典：Tr 回退返回键 id，Fmt 对无占位符的 id 丢弃参数。
        CHECK(err.find(L"pluginst.nosrc") != std::wstring::npos,
              "error mentions pluginst.nosrc");
        CHECK(!d.called, "downloader not called without url");
    }

    // ---- 5) 下载失败 → 安装失败并上报 kFailed ----
    {
        PluginInstaller inst(pluginsDir);
        FakeFailDownloader d; FakeOkExtractor ex;
        inst.SetDownloader(&d); inst.SetExtractor(&ex);
        ProgressRecorder pr;
        std::wstring err;
        CHECK(!inst.Install(L"FailDl", L"1.0", L"http://example.com/f.zip", pr.Cb(), &err),
              "download failure fails install");
        CHECK(d.called, "downloader invoked on http url");
        CHECK(pr.Saw(InstallProgress::kFailed), "kFailed emitted");
    }

    // ---- 6) 解压失败 → 安装失败并传播原因 ----
    {
        PluginInstaller inst(pluginsDir);
        FakeOkDownloader d; FakeFailExtractor ex;
        inst.SetDownloader(&d); inst.SetExtractor(&ex);
        std::wstring err;
        CHECK(!inst.Install(L"FailEx", L"1.0", L"http://example.com/e.zip", nullptr, &err),
              "extract failure fails install");
        CHECK(err.find(L"fake extract error") != std::wstring::npos, "extract error propagated");
    }

    // ---- 7) 真实 miniz ZIP 回环：构造 → 解压 → 规整 plugins 根 → 落盘 ----
    {
        const std::wstring zipPath = dl + L"\\Real.zip";
        const std::string dll = "REAL-PLUGIN-DLL-BYTES";
        const std::string readme = "hello from real zip";
        CHECK(MakeZip(zipPath, {
            { "plugins/Real/NppExec.dll", dll },
            { "plugins/Real/readme.txt",  readme },
        }), "make real zip");
        PluginInstaller inst(pluginsDir);   // 默认（真实）解压器
        FakeFailDownloader d;
        inst.SetDownloader(&d);
        std::wstring err;
        CHECK(inst.Install(L"Real", L"2.0", L"http://example.com/real.zip", nullptr, &err),
              "real zip install ok");
        CHECK(!d.called, "real zip: downloader skipped (offline)");
        std::string dll2, rm;
        CHECK(ReadFileBytes(pluginsDir + L"\\Real\\NppExec.dll", dll2), "read real dll");
        CHECK(dll2 == dll, "real dll content matches");
        CHECK(ReadFileBytes(pluginsDir + L"\\Real\\readme.txt", rm), "read readme");
        CHECK(rm == readme, "readme content matches");
    }

    // ---- 8) zip-slip 防护：../ 条目被拒绝且不越界写盘 ----
    {
        const std::wstring zipPath = dl + L"\\Slip.zip";
        CHECK(MakeZip(zipPath, { { "../evil.dll", "evil" } }), "make slip zip");
        MinizZipExtractor ex;
        const std::wstring outDir = root + L"\\slip_out";
        fs::remove_all(outDir, ec);
        std::wstring err;
        CHECK(!ex.Extract(zipPath, outDir, &err), "zip-slip rejected");
        CHECK(!err.empty(), "slip error reported");
        CHECK(!fs::exists(root + L"\\evil.dll"), "no file escaped temp root");
    }

    // ---- 9) WinHttpDownloader 非 http 分支：本地路径/file:// 直拷 ----
    {
        const std::wstring src = root + L"\\src.bin";
        const std::string data = "local-copy-test-data";
        WriteFileBytes(src, data.data(), data.size());
        const std::wstring dst = root + L"\\dst.bin";
        WinHttpDownloader dw;
        CHECK(dw.Download(src, dst, nullptr), "local copy ok");
        std::string out;
        CHECK(ReadFileBytes(dst, out), "read copied file");
        CHECK(out == data, "copied content matches");
    }

    // ---- 10) UI 闭环复刻：清单→可用→安装→更新→移除→回到可用 ------------------
    // 与 PluginAdminDialog 的数据链路一一对应：
    //   可用页   = LoadPluginCatalog 且 !registry.IsInstalled(folder)
    //   安装完成 = installer.Install() + registry.MarkInstalled()（WM_INSTALL_DONE）
    //   更新页   = ComparePluginVersions(catalog, installed) > 0
    //   移除     = registry.TryUninstall()（真实 DLL 需手动删盘）
    {
        auto writeCatalog = [&](const char* version) {
            const std::string json = std::string(R"({
  "name": "test-catalog", "version": "1", "arch": "x64",
  "npp-plugins": [ {
    "folder-name": "Demo",
    "display-name": "Demo Plugin",
    "version": ")") + version + R"(",
    "repository": "http://example.com/demo.zip",
    "description": "closed-loop demo"
  } ]
})";
            WriteFileBytes(PluginListFilePath(pluginsDir), json.data(), json.size());
        };
        writeCatalog("1.0");

        // 前序段落的 FakeOkExtractor 会留下 plugins\Demo，先清干净保证起点为「未安装」
        fs::remove_all(pluginsDir + L"\\Demo", ec);
        DeleteFileW((dl + L"\\Demo.zip").c_str());

        // 离线包：downloads\Demo.zip（真实 miniz ZIP，N++ 打包惯例 plugins/Demo/…）
        CHECK(MakeZip(dl + L"\\Demo.zip", {
            { "plugins/Demo/NppExec.dll", "loop-dll-v" },
        }), "closed loop: make offline zip");

        PluginCatalog cat; bool usedDefaults = true;
        CHECK(LoadPluginCatalog(pluginsDir, cat, usedDefaults), "catalog from disk");
        CHECK(!usedDefaults, "disk catalog preferred over defaults");
        CHECK(cat.plugins.size() == 1 && cat.plugins[0].folderName == L"Demo", "catalog entry");

        PluginRegistry reg(pluginsDir);
        PluginInstaller inst(pluginsDir);
        std::wstring err;

        // 可用页：未安装 → Demo 应出现
        reg.Refresh();
        CHECK(!reg.IsInstalled(L"Demo"), "available: not installed yet");

        // 安装：离线包直用（url 存在但本地包优先）→ 记录 1.0，DLL 真实落盘
        CHECK(inst.Install(L"Demo", L"1.0", L"http://example.com/demo.zip", nullptr, &err),
              "closed loop: install");
        reg.MarkInstalled(L"Demo", L"1.0");
        reg.Refresh();
        CHECK(reg.IsInstalled(L"Demo") && reg.VersionOf(L"Demo") == L"1.0", "installed 1.0");
        CHECK(reg.IsRealFile(L"Demo"), "dll really on disk after install");
        CHECK(fs::exists(pluginsDir + L"\\Demo\\NppExec.dll"), "installed dll path");

        // 更新页：清单升到 2.0 → 版本比较命中 → 重装覆盖 → 记录 2.0
        writeCatalog("2.0");
        CHECK(LoadPluginCatalog(pluginsDir, cat, usedDefaults), "reload catalog");
        CHECK(ComparePluginVersions(cat.plugins[0].version, reg.VersionOf(L"Demo")) > 0,
              "update row detected");
        CHECK(inst.Install(L"Demo", L"2.0", L"http://example.com/demo.zip", nullptr, &err),
              "closed loop: update install");
        reg.MarkInstalled(L"Demo", L"2.0");
        reg.Refresh();
        CHECK(reg.VersionOf(L"Demo") == L"2.0", "updated to 2.0");

        // 移除：真实 DLL 在盘 → TryUninstall 清记录并返回 false（提示手动删）
        CHECK(!reg.TryUninstall(L"Demo"), "remove: real dll needs manual delete");
        reg.Refresh();
        CHECK(reg.IsInstalled(L"Demo"), "disk dll still counts as installed");
        // 手动删盘后（模拟用户确认）→ 回到可用页
        fs::remove_all(pluginsDir + L"\\Demo", ec);
        reg.Refresh();
        CHECK(!reg.IsInstalled(L"Demo"), "back to available after manual delete");
        DeleteFileW(PluginListFilePath(pluginsDir).c_str());
        DeleteFileW((dl + L"\\Demo.zip").c_str());
    }

    fs::remove_all(root, ec);
    std::printf("plugin_installer: all tests passed\n");
    return 0;
}
