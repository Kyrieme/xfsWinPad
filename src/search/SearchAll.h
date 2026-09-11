#pragma once
// xfsWinPad - multi-document search collection for the results panel
// (current document / all open documents)

#include "../search/SearchService.h"
#include "../editor/Editor.h"
#include <string>
#include <vector>

namespace xfs {

struct SearchHit {
    std::wstring file;      // display label (name for open docs, path for disk hits)
    std::wstring path;      // full path when the hit is on disk (not open); else empty
    int docIndex = -1;      // workspace index when from an open document; else -1
    int docView = 0;        // 0 = left/main view, 1 = right/other view (split view)
    int line = 0;           // 1-based
    sptr_t start = 0;       // match span (byte offsets, open docs / decoded text)
    sptr_t end = 0;
    std::wstring lineText;  // trimmed content of the line
};

// Finds every occurrence of st.text in ed. Hits carry docIndex for direct
// activation (pass -1 when the caller will fill it later).
void CollectHitsInEditor(Editor& ed, const std::wstring& fileLabel,
                         const FindState& st, std::vector<SearchHit>* out,
                         int docIndex = -1);

// Pure-text variant over decoded UTF-8 (used by Find in Files / Projects).
void CollectHitsInText(const std::string& utf8, const std::wstring& fileLabel,
                       const std::wstring& fullPath,
                       const FindState& st, std::vector<SearchHit>* out);

// Replaces every occurrence of st.text with st.replace in decoded UTF-8.
// Returns the number of replacements made.
int ReplaceInText(std::string* utf8, const FindState& st);

} // namespace xfs
