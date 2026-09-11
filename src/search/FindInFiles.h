#pragma once
// xfsWinPad - Find in Files / Projects: search decoded text of files on disk.

#include "SearchAll.h"
#include <string>
#include <vector>

namespace xfs {

struct FindInFilesOptions {
    std::wstring directory;     // root to scan
    std::wstring filters;       // "*.txt;*.log" or empty = all files
    FindState st;
    bool recursive = true;
};

std::vector<SearchHit> RunFindInFiles(const FindInFilesOptions& opt);

// Replace in files: reads each matching file, replaces occurrences, writes back.
// Returns the number of files modified.
int RunReplaceInFiles(const FindInFilesOptions& opt, const std::wstring& replaceWith);

} // namespace xfs
