#include "PluginInstaller.h"
#include "../core/Util.h"
#include "../core/I18n.h"

#include <windows.h>
#include <winhttp.h>

// miniz：C 接口，头文件自带 extern "C" 保护。
#include "miniz.h"

#include <filesystem>
#include <vector>

namespace xfs {

namespace {

namespace fs = std::filesystem;

std::wstring LastErrorString(DWORD err) {
    wchar_t buf[512]{};
    ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, err, 0, buf, 512, nullptr);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
    return s;
}

// 规整 ZIP 内相对路径：统一反斜杠、去掉首部 "./" 与重复分隔符。
// 注意：只精确吞掉前导 "./"（两个字符），绝不吞 ".."，否则 zip-slip 条目
// "../evil.dll" 会被洗成 "evil.dll" 而绕过 IsUnsafeRelativePath。
std::wstring NormalizeZipPath(const std::wstring& entry) {
    std::wstring out;
    bool lastSep = true;   // 跳过开头的 '/'
    for (size_t i = 0; i < entry.size(); ++i) {
        const wchar_t c = entry[i];
        const bool sep = (c == L'/' || c == L'\\');
        if (sep) {
            if (!lastSep) out += L'\\';
            lastSep = true;
            continue;
        }
        if (lastSep && c == L'.' && out.empty() && i + 1 < entry.size()) {
            const wchar_t n = entry[i + 1];
            if (n == L'/' || n == L'\\') {   // 前导 "./"：跳过点与分隔符
                i += 1;
                continue;
            }
        }
        out += c;
        lastSep = false;
    }
    return out;
}

// zip-slip 防护：条目必须为安全相对路径（无 ".." 段、非绝对、无盘符）。
bool IsUnsafeRelativePath(const std::wstring& rel) {
    if (rel.empty()) return true;
    if (rel[0] == L'\\' || rel[0] == L'/') return true;   // 绝对路径
    if (rel.find(L':') != std::wstring::npos) return true; // 盘符/伪协议
    std::wstring seg;
    for (wchar_t c : rel) {
        if (c == L'\\' || c == L'/') {
            if (seg == L"..") return true;
            seg.clear();
        } else {
            seg += c;
        }
    }
    return seg == L"..";
}

// 递归拷贝 src 内容到 dst（dst 不存在则创建；覆盖已存在文件）。
// 目录本身不复制，只复制其内容。
bool CopyFileTree(const std::wstring& src, const std::wstring& dst,
                  std::wstring* errOut) {
    auto fail = [&](const std::wstring& m) { if (errOut) *errOut = m; return false; };
    std::error_code ec;
    if (!fs::is_directory(src, ec))
        return fail(I18n::Instance().Fmt(L"pluginst.badcontent", {src}));
    fs::create_directories(dst, ec);
    if (ec) return fail(I18n::Instance().Fmt(L"pluginst.mkdir",
                        {dst, Utf8ToWide(ec.message())}));
    for (auto it = fs::recursive_directory_iterator(src, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return fail(I18n::Instance().Fmt(L"pluginst.traverse",
                            {Utf8ToWide(ec.message())}));
        const fs::path rel = fs::relative(it->path(), src, ec);
        if (ec) continue;
        const fs::path target = fs::path(dst) / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(target.parent_path(), ec);
            std::error_code cec;
            fs::copy_file(it->path(), target,
                          fs::copy_options::overwrite_existing, cec);
            if (cec) return fail(I18n::Instance().Fmt(L"pluginst.write",
                                 {target.wstring(), Utf8ToWide(cec.message())}));
        }
    }
    return true;
}

// 解析 http/https URL。成功返回 true 并填充 scheme/host/port/path。
bool ParseHttpUrl(const std::wstring& url, std::wstring& scheme,
                  std::wstring& host, int& port, std::wstring& path) {
    const size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) return false;
    scheme = url.substr(0, schemeEnd);
    for (auto& c : scheme) c = (wchar_t)::towlower(c);
    if (scheme != L"http" && scheme != L"https") return false;

    const size_t hostStart = schemeEnd + 3;
    size_t hostEnd = url.find_first_of(L"/?#", hostStart);
    if (hostEnd == std::wstring::npos) hostEnd = url.size();
    const std::wstring hostPart = url.substr(hostStart, hostEnd - hostStart);

    const size_t colon = hostPart.find(L':');
    if (colon != std::wstring::npos) {
        host = hostPart.substr(0, colon);
        port = ::_wtoi(hostPart.substr(colon + 1).c_str());
    } else {
        host = hostPart;
        port = (scheme == L"https") ? INTERNET_DEFAULT_HTTPS_PORT
                                    : INTERNET_DEFAULT_HTTP_PORT;
    }
    if (host.empty()) return false;
    if (port <= 0) port = (scheme == L"https") ? INTERNET_DEFAULT_HTTPS_PORT
                                               : INTERNET_DEFAULT_HTTP_PORT;

    path = (hostEnd < url.size()) ? url.substr(hostEnd) : L"/";
    return true;
}

void EmitProgress(const std::function<void(const InstallProgress&)>& cb,
                  InstallProgress::Phase phase, int percent,
                  const std::wstring& message) {
    if (!cb) return;
    InstallProgress p;
    p.phase = phase;
    p.percent = percent;
    p.message = message;
    cb(p);
}

} // namespace

// ================= WinHttpDownloader =================

bool WinHttpDownloader::Download(const std::wstring& url,
                                 const std::wstring& dstPath,
                                 const std::function<void(const InstallProgress&)>& progress) {
    // 本地路径 / file:// 直拷（离线包、单测、内网共享）。
    std::wstring scheme, host, path;
    int port = 0;
    if (!ParseHttpUrl(url, scheme, host, port, path)) {
        std::wstring local = url;
        if (local.rfind(L"file://", 0) == 0) local = local.substr(7);
        EmitProgress(progress, InstallProgress::kDownloading, 50, Tr(L"pluginst.copypkg"));
        std::error_code ec;
        fs::copy_file(local, dstPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            EmitProgress(progress, InstallProgress::kFailed, 100,
                         I18n::Instance().Fmt(L"pluginst.readfail", {local}));
            return false;
        }
        EmitProgress(progress, InstallProgress::kDownloading, 100, Tr(L"pluginst.localready"));
        return true;
    }

    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 跟随系统代理（PAC/手动配置，如
    // 127.0.0.1:10809 这类本地网关）；Win7 老系统无此常量时回退 NO_PROXY。
    // 此前 DEFAULT_PROXY 只取进程启动快照、不解析系统代理，配置了代理的
    // 机器上会「浏览器能开 GitHub、安装器连不上」。
    HINTERNET hSession = ::WinHttpOpen(L"xfsWinPad/0.1",
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
#else
                                       WINHTTP_ACCESS_TYPE_NO_PROXY,
#endif
                                       WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = ::WinHttpConnect(hSession, host.c_str(), (INTERNET_PORT)port, 0);
    HINTERNET hRequest = nullptr;
    if (hConnect) {
        const DWORD flags = (scheme == L"https") ? WINHTTP_FLAG_SECURE : 0;
        hRequest = ::WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }
    bool ok = false;
    if (hRequest) {
        if (::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            ::WinHttpReceiveResponse(hRequest, nullptr)) {
            // Content-Length（可选）
            uint64_t total = 0;
            {
                DWORD len = sizeof(DWORD);
                DWORD cl = 0;
                if (::WinHttpQueryHeaders(hRequest,
                                          WINHTTP_QUERY_CONTENT_LENGTH |
                                              WINHTTP_QUERY_FLAG_NUMBER,
                                          WINHTTP_HEADER_NAME_BY_INDEX,
                                          &cl, &len, WINHTTP_NO_HEADER_INDEX))
                    total = cl;
            }

            HANDLE hFile = ::CreateFileW(dstPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                         nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                ok = true;
                std::vector<BYTE> buf(64 * 1024);
                uint64_t received = 0;
                while (true) {
                    DWORD avail = 0;
                    if (!::WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    const DWORD want = (DWORD)std::min<size_t>(buf.size(), avail);
                    DWORD read = 0;
                    if (!::WinHttpReadData(hRequest, buf.data(), want, &read) || read == 0) {
                        ok = false;
                        break;
                    }
                    DWORD written = 0;
                    if (!::WriteFile(hFile, buf.data(), read, &written, nullptr) ||
                        written != read) {
                        ok = false;
                        break;
                    }
                    received += read;
                    EmitProgress(progress, InstallProgress::kDownloading,
                                 (int)((total ? (received * 100 / total) : 0) & 0xFF),
                                 Tr(L"pluginst.downloading"));
                }
                ::CloseHandle(hFile);
                if (!ok) ::DeleteFileW(dstPath.c_str());
            }
        }
    }
    if (hRequest) ::WinHttpCloseHandle(hRequest);
    if (hConnect) ::WinHttpCloseHandle(hConnect);
    ::WinHttpCloseHandle(hSession);

    if (!ok)
        EmitProgress(progress, InstallProgress::kFailed, 100,
                     I18n::Instance().Fmt(L"pluginst.dlfail",
                         {url, LastErrorString(::GetLastError())}));
    return ok;
}

// ================= MinizZipExtractor =================

bool MinizZipExtractor::Extract(const std::wstring& zipPath,
                                const std::wstring& outDir,
                                std::wstring* errOut) {
    auto fail = [&](const std::wstring& m) { if (errOut) *errOut = m; return false; };

    std::string bytes;
    if (!ReadFileBytes(zipPath, bytes))
        return fail(I18n::Instance().Fmt(L"pluginst.readzip", {zipPath}));
    if (bytes.size() < 4 || bytes[0] != 'P' || bytes[1] != 'K')
        return fail(I18n::Instance().Fmt(L"pluginst.notzip", {zipPath}));

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0))
        return fail(I18n::Instance().Fmt(L"pluginst.zipparse", {zipPath}));

    std::error_code ec;
    fs::create_directories(outDir, ec);

    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (!st.m_filename || !*st.m_filename) continue;
        if (st.m_is_directory || mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        std::wstring rel = NormalizeZipPath(Utf8ToWide(st.m_filename));
        if (IsUnsafeRelativePath(rel)) {
            mz_zip_reader_end(&zip);
            return fail(I18n::Instance().Fmt(L"pluginst.unsafe",
                        {Utf8ToWide(st.m_filename)}));
        }

        const fs::path dst = fs::path(outDir) / fs::path(rel);
        fs::create_directories(dst.parent_path(), ec);

        size_t size = 0;
        void* buf = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!buf) {
            mz_zip_reader_end(&zip);
            return fail(I18n::Instance().Fmt(L"pluginst.extractfail",
                        {Utf8ToWide(st.m_filename)}));
        }
        const bool ok = WriteFileBytes(dst.wstring(), buf, size);
        mz_free(buf);
        if (!ok) {
            mz_zip_reader_end(&zip);
            return fail(I18n::Instance().Fmt(L"pluginst.writedst", {dst.wstring()}));
        }
    }
    mz_zip_reader_end(&zip);
    return true;
}

// ================= PluginInstaller =================

PluginInstaller::PluginInstaller(std::wstring pluginsDir)
    : pluginsDir_(std::move(pluginsDir)) {}

std::wstring PluginInstaller::DownloadDir() const {
    return pluginsDir_ + L"\\downloads";
}

std::wstring PluginInstaller::NormalizeExtractedRoot(const std::wstring& tempDir,
                                                     bool* hadPluginsRoot) {
    if (hadPluginsRoot) *hadPluginsRoot = false;
    std::error_code ec;
    // 1) N++ 打包惯例：顶层 plugins\ 目录。
    const fs::path pluginsSub = fs::path(tempDir) / L"plugins";
    if (fs::is_directory(pluginsSub, ec)) {
        if (hadPluginsRoot) *hadPluginsRoot = true;
        return pluginsSub.wstring();
    }
    // 2) 根下仅一个子目录且无散文件 → 切进该子目录。
    bool hasFile = false;
    std::wstring single;
    for (auto it = fs::directory_iterator(tempDir, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (single.empty()) single = it->path().wstring();
            else { single.clear(); break; }
        } else {
            hasFile = true;
        }
    }
    if (!hasFile && !single.empty()) return single;
    // 3) 原样。
    return tempDir;
}

bool PluginInstaller::EnsureLocalPackage(
    const std::wstring& folder, const std::wstring& url, std::wstring& zipPath,
    const std::function<void(const InstallProgress&)>& progress,
    std::wstring* errOut) {
    auto fail = [&](const std::wstring& m) { if (errOut) *errOut = m; return false; };

    std::error_code ec;
    fs::create_directories(DownloadDir(), ec);
    const std::wstring localZip = DownloadDir() + L"\\" + folder + L".zip";
    if (fs::exists(localZip, ec)) {
        zipPath = localZip;   // 离线安装：本地包优先
        return true;
    }
    if (url.empty())
        return fail(I18n::Instance().Fmt(L"pluginst.nosrc", {localZip}));

    Downloader* d = downloader_ ? downloader_ : &defaultDownloader_;
    auto fwd = [&](const InstallProgress& p) {
        EmitProgress(progress, p.phase, p.percent, p.message);
    };
    EmitProgress(progress, InstallProgress::kDownloading, 0, Tr(L"pluginst.dlstart"));
    if (!d->Download(url, localZip, fwd)) {
        std::wstring msg = I18n::Instance().Fmt(L"pluginst.dlfail2", {url});
        EmitProgress(progress, InstallProgress::kFailed, 100, msg);
        fs::remove(localZip, ec);
        return fail(msg);
    }
    if (!fs::exists(localZip, ec) || fs::file_size(localZip, ec) == 0) {
        fs::remove(localZip, ec);
        return fail(I18n::Instance().Fmt(L"pluginst.emptyfile", {url}));
    }
    zipPath = localZip;   // 下载成功同样要回填路径（此前漏掉 → Extract 收到空路径）
    return true;
}

bool PluginInstaller::Install(
    const std::wstring& folder, const std::wstring& version,
    const std::wstring& url,
    const std::function<void(const InstallProgress&)>& progress,
    std::wstring* errOut) {
    auto fail = [&](const std::wstring& m) { if (errOut) *errOut = m; return false; };
    std::error_code ec;

    // 1) 取得本地安装包（下载或离线）。
    std::wstring zipPath;
    if (!EnsureLocalPackage(folder, url, zipPath, progress, errOut)) return false;

    // 2) 解压到临时目录。
    const std::wstring tempRoot = DownloadDir() + L"\\_tmp_" + folder;
    fs::remove_all(tempRoot, ec);
    fs::create_directories(tempRoot, ec);
    EmitProgress(progress, InstallProgress::kExtracting, 0, Tr(L"pluginst.extracting"));
    std::wstring zipErr;
    ZipExtractor* ex = extractor_ ? extractor_ : &defaultExtractor_;
    if (!ex->Extract(zipPath, tempRoot, &zipErr)) {
        EmitProgress(progress, InstallProgress::kFailed, 100, zipErr);
        fs::remove_all(tempRoot, ec);
        return fail(zipErr);
    }

    // 3) 规整内容根目录（兼容 N++ 打包/单子目录/平铺）。
    bool hadPluginsRoot = false;
    const std::wstring contentRoot = NormalizeExtractedRoot(tempRoot, &hadPluginsRoot);

    // 4) 拷入目标目录：
    //    - ZIP 自带 plugins\ 根（N++ 打包惯例）→ 内容直接并入宿主 plugins
    //      目录（plugins\Demo\NppExec.dll → plugins\Demo\NppExec.dll）；
    //    - 否则（单子目录/平铺）→ 落进 plugins\<folder>\。
    const std::wstring targetDir = hadPluginsRoot
        ? pluginsDir_
        : pluginsDir_ + L"\\" + folder;
    EmitProgress(progress, InstallProgress::kInstalling, 0, Tr(L"pluginst.installing"));
    std::wstring copyErr;
    if (!CopyFileTree(contentRoot, targetDir, &copyErr)) {
        EmitProgress(progress, InstallProgress::kFailed, 100, copyErr);
        fs::remove_all(tempRoot, ec);
        return fail(copyErr);
    }

    // 5) 清理临时目录。成功与否由调用方登记 PluginRegistry。
    fs::remove_all(tempRoot, ec);
    EmitProgress(progress, InstallProgress::kDone, 100,
                 I18n::Instance().Fmt(L"pluginst.done", {folder, version}));
    return true;
}

} // namespace xfs
