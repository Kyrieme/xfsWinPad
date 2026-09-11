#include "FindInFiles.h"
#include "../core/Util.h"
#include "../encoding/Encoding.h"
#include <windows.h>
#include <shlwapi.h>
#include <filesystem>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

namespace xfs {
namespace {

std::vector<std::wstring> SplitFilters(const std::wstring& filters) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : filters) {
        if (c == L';' || c == L',') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    for (auto& f : out) {
        for (auto& ch : f) ch = (wchar_t)towlower(ch);
    }
    return out;
}

bool MatchesAnyFilter(const std::wstring& nameLower,
                      const std::vector<std::wstring>& filters) {
    if (filters.empty()) return true;
    for (const auto& f : filters)
        if (::PathMatchSpecW(nameLower.c_str(), f.c_str())) return TRUE;
    return false;
}

} // namespace

std::vector<SearchHit> RunFindInFiles(const FindInFilesOptions& opt) {
    std::vector<SearchHit> hits;
    if (opt.st.text.empty() || opt.directory.empty()) return hits;

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root(opt.directory);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return hits;

    auto filters = SplitFilters(opt.filters);

    auto scanOne = [&](const fs::path& p) {
        // size guard: skip absurd files (>64 MB) in the synchronous v1 path
        std::error_code sec;
        uintmax_t sz = fs::file_size(p, sec);
        if (sec || sz == 0 || sz > (uintmax_t)64 * 1024 * 1024) return;
        std::string raw;
        if (!ReadFileBytes(p.wstring(), raw)) return;
        DecodedText dec = encoding::DecodeToUtf8(raw);
        CollectHitsInText(dec.utf8, p.filename().wstring(), p.wstring(), opt.st, &hits);
    };

    if (opt.recursive) {
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        int visited = 0;
        while (it != end && !ec) {
            if (++visited > 20000) break;      // safety cap
            try {
                const fs::directory_entry& e = *it;
                if (e.is_regular_file(ec)) {
                    std::wstring name = e.path().filename().wstring();
                    std::transform(name.begin(), name.end(), name.begin(),
                                   [](wchar_t c){ return (wchar_t)towlower(c); });
                    if (MatchesAnyFilter(name, filters)) scanOne(e.path());
                }
            } catch (...) { /* skip entry */ }
            it.increment(ec);
            if (hits.size() > 20000) break;
        }
    } else {
        fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::directory_iterator end;
        while (it != end && !ec) {
            const fs::directory_entry& e = *it;
            std::error_code fec;
            if (e.is_regular_file(fec)) {
                std::wstring name = e.path().filename().wstring();
                std::transform(name.begin(), name.end(), name.begin(),
                               [](wchar_t c){ return (wchar_t)towlower(c); });
                if (MatchesAnyFilter(name, filters)) scanOne(e.path());
            }
            it.increment(ec);
            if (hits.size() > 20000) break;
        }
    }
    return hits;
}

int RunReplaceInFiles(const FindInFilesOptions& opt, const std::wstring& replaceWith) {
    int filesChanged = 0;
    if (opt.st.text.empty()) return 0;

    // the caller's replacement text is authoritative (the dialog passes the
    // 文件中替换 tab's 替换为 box here; opt.st.replace belongs to another tab)
    FindState st = opt.st;
    st.replace = replaceWith;

    // find matching files first
    std::vector<SearchHit> hits = RunFindInFiles(opt);
    if (hits.empty()) return 0;

    // collect unique paths
    std::vector<std::wstring> paths;
    for (const auto& h : hits) {
        if (h.path.empty()) continue;
        bool dup = false;
        for (const auto& p : paths) if (p == h.path) { dup = true; break; }
        if (!dup) paths.push_back(h.path);
    }

    for (const auto& path : paths) {
        std::string raw;
        if (!ReadFileBytes(path, raw)) continue;
        DecodedText dec = encoding::DecodeToUtf8(raw);
        std::string utf8 = dec.utf8;
        int n = ReplaceInText(&utf8, st);
        if (n == 0) continue;
        // re-encode using the original encoding
        std::string out = encoding::EncodeFromUtf8(utf8, dec.encoding);
        HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ,
                                     nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        ::WriteFile(hFile, out.data(), (DWORD)out.size(), &written, nullptr);
        ::CloseHandle(hFile);
        ++filesChanged;
    }
    return filesChanged;
}

} // namespace xfs
