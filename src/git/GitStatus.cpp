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

static int IntAfter(const std::string& s, const std::string& key) {
    size_t k = s.find(key);
    if (k == std::string::npos) return 0;
    k += key.size();
    while (k < s.size() && (s[k] == ' ')) ++k;
    int v = 0; bool any = false;
    while (k < s.size() && s[k] >= '0' && s[k] <= '9') {
        v = v * 10 + (s[k] - '0'); ++k; any = true;
    }
    return any ? v : 0;
}

BranchTracking ParseTracking(const std::string& out) {
    BranchTracking t;
    size_t start = 0;
    while (start < out.size()) {
        size_t z = out.find('\0', start);
        if (z == std::string::npos) z = out.size();
        std::string tok = out.substr(start, z - start);
        start = z + 1;
        if (tok.size() < 3 || tok[0] != '#' || tok[1] != '#' || tok[2] != ' ')
            continue;
        std::string body = tok.substr(3);            // after "## "
        size_t lb = body.find('[');
        std::string ref = lb == std::string::npos ? body : body.substr(0, lb);
        while (!ref.empty() && ref.back() == L' ') ref.pop_back();
        t.detached = ref.find("no branch") != std::string::npos;
        t.hasUpstream = ref.find("...") != std::string::npos;
        if (lb == std::string::npos) return t;
        std::string br = body.substr(lb + 1);
        size_t rb = br.find(']');
        if (rb != std::string::npos) br = br.substr(0, rb);
        t.gone = br.find("gone") != std::string::npos;
        t.ahead = IntAfter(br, "ahead");
        t.behind = IntAfter(br, "behind");
        return t;
    }
    return t;
}

std::vector<BranchEntry> ParseBranchList(const std::string& out) {
    std::vector<BranchEntry> list;
    static const char kHeads[] = "refs/heads/";
    static const char kRemotes[] = "refs/remotes/";
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) nl = out.size();
        std::string line = out.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2) continue;
        bool current = line[0] == '*';
        std::string ref = line.substr(1);
        BranchEntry e;
        e.current = current;
        if (ref.rfind(kHeads, 0) == 0) {
            e.name = Utf8ToWide(ref.substr(sizeof(kHeads) - 1));
        } else if (ref.rfind(kRemotes, 0) == 0) {
            std::string rem = ref.substr(sizeof(kRemotes) - 1);  // origin/main
            if (rem == "HEAD" || (rem.size() > 5 && rem.compare(rem.size() - 5, 5, "/HEAD") == 0))
                continue;  // remote HEAD symref, not a branch
            e.name = Utf8ToWide(rem);
            e.remote = true;
        } else {
            continue;
        }
        if (!e.name.empty()) list.push_back(std::move(e));
    }
    return list;
}

bool BranchNameOk(const std::wstring& name) {
    if (name.empty()) return false;
    if (name.front() == L'/' || name.back() == L'/' || name.back() == L'.')
        return false;
    if (name.front() == L'-') return false;
    if (name.find(L"..") != std::wstring::npos) return false;
    if (name.find(L"//") != std::wstring::npos) return false;
    if (name.find(L"@{") != std::wstring::npos) return false;
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, L".lock") == 0)
        return false;
    for (wchar_t c : name) {
        if (c <= L' ' || c == 0x7F) return false;
        if (c == L'~' || c == L'^' || c == L':' || c == L'?' || c == L'*' ||
            c == L'[' || c == L'\\')
            return false;
    }
    return true;
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

std::wstring QuoteArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos && arg.back() != L'\\')
        return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') { out.append(backslashes * 2 + 1, L'\\'); out += L'"'; backslashes = 0; continue; }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

} // namespace xfs::git
