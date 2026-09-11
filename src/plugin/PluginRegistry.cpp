#include "PluginRegistry.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#include <filesystem>
#include <cstdio>
#include <vector>

namespace xfs {
namespace fs = std::filesystem;

std::wstring ReadDllFileVersion(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<unsigned char> buf(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) || !ffi) return {};
    wchar_t v[64];
    swprintf_s(v, L"%u.%u.%u.%u",
               HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
               HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return v;
}

namespace {

std::wstring RecordFile(const std::wstring& pluginsDir) {
    return pluginsDir + L"\\installed.json";
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\t': out += L"\\t";  break;
            default:    out += c; break;
        }
    }
    return out;
}

} // namespace

PluginRegistry::PluginRegistry(std::wstring pluginsDir) : pluginsDir_(std::move(pluginsDir)) {
}

void PluginRegistry::LoadRecords() {
    records_.clear();
    std::string raw;
    if (!ReadFileBytes(RecordFile(pluginsDir_), raw)) return;
    const std::wstring json = Utf8ToWide(raw);
    // 期望结构： {"plugins":{ "folder":"version", ... }}
    // 定位 "plugins" 键后的对象起始括号，再读内部 "key":"value" 成员。
    std::wstring::size_type p = json.find(L"\"plugins\"");
    if (p == std::wstring::npos) return;
    p = json.find(L'{', p);
    if (p == std::wstring::npos) return;
    ++p;   // 进入成员对象
    while (p < json.size()) {
        std::wstring::size_type q = json.find(L'"', p);
        if (q == std::wstring::npos) break;
        std::wstring::size_type q2 = json.find(L'"', q + 1);
        if (q2 == std::wstring::npos) break;
        const std::wstring key = json.substr(q + 1, q2 - q - 1);
        // 键后找 ':' 分隔符，再定位值的开/闭引号
        std::wstring::size_type colon = json.find(L':', q2 + 1);
        if (colon == std::wstring::npos) break;
        std::wstring::size_type v1 = json.find(L'"', colon + 1);
        if (v1 == std::wstring::npos) break;
        std::wstring::size_type v2 = json.find(L'"', v1 + 1);
        if (v2 == std::wstring::npos) break;
        const std::wstring val = json.substr(v1 + 1, v2 - v1 - 1);
        if (!key.empty()) records_[key] = val;
        p = v2 + 1;
    }
}

void PluginRegistry::SaveRecords() {
    std::wstring out = L"{\n    \"plugins\": {\n";
    int n = 0;
    for (const auto& kv : records_) {
        out += L"        \"" + JsonEscape(kv.first) + L"\": \"" +
               JsonEscape(kv.second) + L"\"";
        out += (++n < (int)records_.size()) ? L",\n" : L"\n";
    }
    out += L"    }\n}\n";
    const std::string utf8 = WideToUtf8(out);
    if (!utf8.empty())
        WriteFileBytes(RecordFile(pluginsDir_), utf8.data(), utf8.size());
}

void PluginRegistry::Refresh() {
    // ---- 1) 磁盘真实条目：folder -> {version,path} ----
    std::map<std::wstring, InstalledPlugin> real;
    if (fs::is_directory(pluginsDir_)) {
        for (const auto& e : fs::directory_iterator(pluginsDir_)) {
            if (!e.is_regular_file()) continue;
            const fs::path p = e.path();
            if (_wcsicmp(p.extension().c_str(), L".dll") != 0) continue;
            const std::wstring folder = p.stem().wstring();
            if (real.count(folder)) continue;
            InstalledPlugin ip;
            ip.folder = folder; ip.realFile = true; ip.path = p.wstring();
            ip.version = ReadDllFileVersion(ip.path);
            real.emplace(folder, std::move(ip));
        }
        // 一级子目录内存在 DLL → 以子目录名记（N++ 惯例 plugins\<folder>）
        for (const auto& e : fs::directory_iterator(pluginsDir_)) {
            if (!e.is_directory()) continue;
            const std::wstring folder = e.path().filename().wstring();
            if (real.count(folder)) continue;
            fs::path dllPath;
            for (const auto& f : fs::directory_iterator(e.path())) {
                if (f.is_regular_file() &&
                    _wcsicmp(f.path().extension().c_str(), L".dll") == 0) {
                    dllPath = f.path();
                    break;
                }
            }
            if (dllPath.empty()) continue;
            InstalledPlugin ip;
            ip.folder = folder; ip.realFile = true; ip.path = dllPath.wstring();
            ip.version = ReadDllFileVersion(ip.path);
            real.emplace(folder, std::move(ip));
        }
    }

    // ---- 2) 本地记录 folder -> version ----
    LoadRecords();

    // ---- 3) 合并：真实 ∪ 记录；版本以记录为准（用户"更新到"的目标版本优先） ----
    std::map<std::wstring, InstalledPlugin> merged = std::move(real);
    for (const auto& kv : records_) {
        InstalledPlugin& ip = merged[kv.first];
        ip.folder = kv.first;
        ip.version = kv.second;
    }
    installed_.clear();
    for (auto& kv : merged) installed_.push_back(kv.second);
}

bool PluginRegistry::IsInstalled(const std::wstring& folder) const {
    for (const auto& ip : installed_)
        if (ip.folder == folder) return true;
    return false;
}

std::wstring PluginRegistry::VersionOf(const std::wstring& folder) const {
    for (const auto& ip : installed_)
        if (ip.folder == folder) return ip.version;
    return {};
}

bool PluginRegistry::IsRealFile(const std::wstring& folder) const {
    for (const auto& ip : installed_)
        if (ip.folder == folder) return ip.realFile;
    return false;
}

void PluginRegistry::MarkInstalled(const std::wstring& folder,
                                   const std::wstring& version) {
    records_[folder] = version;
    SaveRecords();
    Refresh();
}

bool PluginRegistry::TryUninstall(const std::wstring& folder) {
    const bool wasReal = IsRealFile(folder);
    records_.erase(folder);
    SaveRecords();
    Refresh();
    return !wasReal;   // 仅记录安装可"干净移除"；真实 DLL 仍在磁盘上
}

} // namespace xfs