#include "GitStatus.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <vector>

#include "../core/Util.h"

namespace xfs::git {
namespace fs = std::filesystem;

std::wstring FindRepoRoot(const std::wstring& path, const std::wstring& stopDir) {
    if (path.empty()) return std::wstring();
    std::error_code ec;
    fs::path cur(path);
    if (!fs::is_directory(cur, ec)) cur = cur.parent_path();
    while (!cur.empty()) {
        DWORD attr = ::GetFileAttributesW((cur.wstring() + L"\\.git").c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) return cur.wstring();  // dir or file (worktree)
        if (!stopDir.empty() && _wcsicmp(cur.c_str(), stopDir.c_str()) == 0)
            return std::wstring();
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return std::wstring();
}

static std::wstring LowerAbs(const std::wstring& s) {
    std::wstring out = s;
    for (auto& ch : out) ch = towlower(ch);
    return out;
}

std::wstring ToAbsPath(const std::wstring& root, const std::string& relUtf8) {
    std::wstring rel = Utf8ToWide(relUtf8);
    for (auto& ch : rel) if (ch == L'/') ch = L'\\';
    std::wstring abs = root;
    if (!abs.empty() && abs.back() != L'\\') abs += L'\\';
    abs += rel;
    return LowerAbs(abs);
}

StateMap ParseStatusPorcelainZ(const std::string& out, const std::wstring& root) {
    std::vector<std::string> toks;
    size_t start = 0;
    while (start < out.size()) {
        size_t z = out.find('\0', start);
        if (z == std::string::npos) z = out.size();
        if (z > start) toks.push_back(out.substr(start, z - start));
        start = z + 1;
    }

    StateMap map;
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& tok = toks[i];
        if (tok.size() < 4) continue;
        char x = tok[0], y = tok[1];
        std::string rel = tok.substr(3);

        FileState st;
        if (x == '?' && y == '?') {
            st = FileState::Untracked;
            while (!rel.empty() && rel.back() == '/') rel.pop_back();  // collapsed dir
        } else if (x == 'U' || y == 'U' || (x == 'A' && y == 'D') ||
                   (x == 'D' && y == 'A')) {
            st = FileState::Conflict;
        } else if (x == 'R' || y == 'R') {
            st = FileState::Renamed;
            if (i + 1 < toks.size()) ++i;  // skip the paired old-path token
        } else if (x == 'C' || y == 'C') {
            st = FileState::Added;
            if (i + 1 < toks.size()) ++i;  // copy source token
        } else if (x == 'A' || y == 'A') {
            st = FileState::Added;
        } else if (x == 'D' || y == 'D') {
            st = FileState::Deleted;
        } else if (x == 'M' || y == 'M') {
            st = FileState::Modified;
        } else {
            continue;  // 'T' type-change, ' ' uninteresting
        }
        map[ToAbsPath(root, rel)] = st;
    }
    return map;
}

static int ColorRank(FileState s) {
    switch (s) {
        case FileState::Conflict:
        case FileState::Deleted:    return 2;  // red
        case FileState::Modified:
        case FileState::Renamed:    return 1;  // orange
        default:                    return 0;  // green
    }
}

void AggregateDirs(StateMap& m, const std::wstring& root) {
    if (root.empty()) return;
    std::wstring rootLower = LowerAbs(root);
    std::map<std::wstring, FileState> dirBest;
    std::vector<std::pair<std::wstring, FileState>> items(m.begin(), m.end());
    for (auto& [path, st] : items) {
        std::wstring cur = path;
        for (;;) {
            size_t k = cur.find_last_of(L'\\');
            if (k == std::wstring::npos || k <= 2) break;
            cur = cur.substr(0, k);
            if (cur.size() < rootLower.size() ||
                cur.compare(0, rootLower.size(), rootLower) != 0 ||
                (cur.size() > rootLower.size() && cur[rootLower.size()] != L'\\'))
                break;
            auto it = dirBest.find(cur);
            if (it == dirBest.end()) dirBest.emplace(cur, st);
            else if (ColorRank(st) > ColorRank(it->second)) it->second = st;
            if (cur == rootLower) break;
        }
    }
    for (auto& [path, st] : dirBest) {
        auto it = m.find(path);
        if (it == m.end()) m.emplace(path, st);
        else if (ColorRank(st) > ColorRank(it->second)) it->second = st;
    }
}

std::wstring ParseBranch(const std::string& out) {
    std::wstring s = Utf8ToWide(out);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
    return s;
}

} // namespace xfs::git
