#pragma once
// xfsWinPad - Git integration v1: pure status logic, no process spawning.
// Parsing of `git status --porcelain=v1 -z` + repo-root discovery.

#include <cstdint>
#include <map>
#include <string>

namespace xfs::git {

enum class FileState : uint8_t {
    Modified = 1,
    Untracked = 2,
    Added = 3,
    Deleted = 4,
    Conflict = 5,
    Renamed = 6,
};

// Key: absolute path, lower-cased, native separators.
using StateMap = std::map<std::wstring, FileState>;

// Walks up from a file or directory looking for a .git entry.
// stopDir (if non-empty) bounds the search to that subtree.
std::wstring FindRepoRoot(const std::wstring& path,
                          const std::wstring& stopDir = std::wstring());

// root + repo-relative slash path -> lowercase absolute native path.
std::wstring ToAbsPath(const std::wstring& root, const std::string& relUtf8);

// Parses NUL-separated `status --porcelain=v1 -z` stdout.
StateMap ParseStatusPorcelainZ(const std::string& out, const std::wstring& root);

// Propagates each entry's state up to its ancestor directories (up to and
// including the repo root). A directory keeps the highest-priority color:
// red (Conflict/Deleted) > orange (Modified/Renamed) > green (Added/Untracked).
void AggregateDirs(StateMap& m, const std::wstring& root);

// Trims a `rev-parse --abbrev-ref HEAD` stdout line.
std::wstring ParseBranch(const std::string& out);

// Win32 argv quoting (inverse of CommandLineToArgvW); embed in git arg lines.
std::wstring QuoteArg(const std::wstring& arg);

} // namespace xfs::git
