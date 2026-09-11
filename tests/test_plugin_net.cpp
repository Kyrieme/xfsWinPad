// xfsWinPad - 真实网络安装闭环（网络可达时）：WinHttpDownloader 直连 GitHub
// release 资产（含 302 → objects.githubusercontent.com 重定向）+ 真实
// MinizZipExtractor 解压官方 zip + NormalizeExtractedRoot + 落盘。
// 网络不可达（沙箱/CI）时打印 SKIP 并按成功退出——仿 test_terminal 先例。
#include "../src/plugin/PluginInstaller.h"
#include "../src/plugin/PluginRegistry.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <filesystem>
#include <string>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); return 1; } } while (0)

using namespace xfs;
namespace fs = std::filesystem;

namespace {

// 轻量探测：github.com 3 秒内不可达即视为离线（跳过整测）。
// 用 GET（HEAD 部分网络栈/代理链会拒），状态码 2xx/3xx 均算可达。
bool NetReachable() {
    HINTERNET s = ::WinHttpOpen(L"xfsWinPad-nettest",
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
#else
                                WINHTTP_ACCESS_TYPE_NO_PROXY,
#endif
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    HINTERNET c = ::WinHttpConnect(s, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) { ::WinHttpCloseHandle(s); return false; }
    HINTERNET r = ::WinHttpOpenRequest(c, L"GET", L"/", nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (r) {
        if (::WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            ::WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, len = sizeof(DWORD);
            if (::WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE |
                                       WINHTTP_QUERY_FLAG_NUMBER,
                                       WINHTTP_HEADER_NAME_BY_INDEX,
                                       &code, &len, WINHTTP_NO_HEADER_INDEX))
                ok = (code >= 200 && code < 400);
        }
        ::WinHttpCloseHandle(r);
    }
    ::WinHttpCloseHandle(c);
    ::WinHttpCloseHandle(s);
    return ok;
}

} // namespace

int main() {
    if (!NetReachable()) {
        std::printf("SKIP: github.com unreachable (offline environment)\n");
        return 0;
    }

    wchar_t tp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tp);
    const std::wstring root = std::wstring(tp) + L"xfs_test_realnet";
    const std::wstring pluginsDir = root + L"\\plugins";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(pluginsDir);

    // 真实下载 + 真实解压 + 落盘（不注入任何假实现）。
    PluginInstaller inst(pluginsDir);
    std::wstring err;
    const std::wstring url =
        L"https://github.com/dail8859/DoxyIt/releases/download/v0.4.4/DoxyIt_v0.4.4_x64.zip";
    CHECK(inst.Install(L"DoxyIt", L"0.4.4", url, nullptr, &err), WideToUtf8(err).c_str());

    // 落盘断言：官方 zip 平铺单个 DoxyIt.dll → plugins\DoxyIt\DoxyIt.dll。
    const std::wstring dll = pluginsDir + L"\\DoxyIt\\DoxyIt.dll";
    CHECK(fs::exists(dll, ec), "DoxyIt.dll landed in plugins\\DoxyIt");
    CHECK(fs::file_size(dll, ec) == 326656, "dll size matches official release");

    // 位宽断言：64 位（machine=0x8664）。
    std::string bytes;
    CHECK(ReadFileBytes(dll, bytes), "read installed dll");
    CHECK(bytes.size() > 0x40 && bytes[0] == 'M' && bytes[1] == 'Z', "MZ header");
    const int peOff = *reinterpret_cast<const int*>(&bytes[0x3C]);
    CHECK(peOff > 0 && (int)bytes.size() > peOff + 6, "pe header in range");
    const unsigned short machine = *reinterpret_cast<const unsigned short*>(&bytes[peOff + 4]);
    CHECK(machine == 0x8664, "x64 plugin");

    // 离线缓存 + 临时目录清理。
    CHECK(fs::exists(pluginsDir + L"\\downloads\\DoxyIt.zip", ec), "zip cached in downloads");
    CHECK(!fs::exists(pluginsDir + L"\\downloads\\_tmp_DoxyIt", ec), "temp dir cleaned");

    // 二次安装走本地缓存（离线重装语义）：删掉 DLL 后重装应从缓存恢复。
    fs::remove_all(pluginsDir + L"\\DoxyIt", ec);
    CHECK(inst.Install(L"DoxyIt", L"0.4.4", url, nullptr, &err), "reinstall from cached zip");
    CHECK(fs::exists(dll, ec), "reinstalled dll present");

    // 注册表闭环：MarkInstalled → IsInstalled。
    PluginRegistry reg(pluginsDir);
    reg.MarkInstalled(L"DoxyIt", L"0.4.4");
    CHECK(reg.IsInstalled(L"DoxyIt"), "registry records install");

    fs::remove_all(root, ec);
    std::printf("ALL REALNET PLUGIN TESTS PASSED\n");
    return 0;
}
