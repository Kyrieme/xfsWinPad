#pragma once
// xfsWinPad - Git integration v1: pure status logic, no process spawning.
// Parsing of `git status --porcelain=v1 -z` + repo-root discovery.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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

// Parsed from the leading `## branch...upstream [ahead N, behind M]` record
// emitted by `status --porcelain=v1 -b -z`.
struct BranchTracking {
    bool detached = false;   // "HEAD (no branch)"
    bool hasUpstream = false;
    bool gone = false;       // upstream ref no longer exists
    int ahead = 0;           // local commits not on upstream
    int behind = 0;          // upstream commits not pulled
};
BranchTracking ParseTracking(const std::string& out);

// Parses `for-each-ref --format=%(HEAD)%(refname) refs/heads refs/remotes`
// stdout: one line per ref, first char '*' (current) or ' ', full UTF-8 ref
// name after. refs/heads/* become local entries; refs/remotes/<r>/<b> become
// remote entries named "<r>/<b>" (the <r>/HEAD symref line is skipped).
struct BranchEntry {
    std::wstring name;
    bool current = false;
    bool remote = false;
};
std::vector<BranchEntry> ParseBranchList(const std::string& out);

// Cheap client-side sanity check for a new branch name (git check-ref-format
// stays the authority; this only blocks clearly broken input early).
bool BranchNameOk(const std::wstring& name);

// Trims a `rev-parse --abbrev-ref HEAD` stdout line.
std::wstring ParseBranch(const std::string& out);

// Win32 argv quoting (inverse of CommandLineToArgvW); embed in git arg lines.
std::wstring QuoteArg(const std::wstring& arg);

} // namespace xfs::git
