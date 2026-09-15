#pragma once
// xfsWinPad - session persistence: open files + cursor positions saved on exit,
// restored on launch (when no CLI files are given).
//
// Format (session.json) carries both views of the split editor so a right/other
// view survives a restart. Older single-view files (no entries1/active1/
// activeView keys) load back as one view; the extra fields default to empty.
// A manual Language-menu pick survives too ("lang" key); files without it
// keep the -1 default and re-derive the lexer from the extension.

#include <string>
#include <vector>

namespace xfs {

struct SessionEntry {
    std::wstring path;
    int line = 1;
    int col = 1;
    // Untitled (never-saved) documents: display name + full text snapshot so
    // unsaved scratch tabs survive a restart. Both empty for path'd documents.
    std::wstring name;          // "new 3" / "new1 2"
    std::wstring text;          // full document text (capped by the saver)
    bool locked = false;        // user tab lock (close protection + read-only)
    // Language menu manual pick: index into LanguageMenuCatalog. -1 = never
    // picked, fall back to extension auto-detect. Absent key (old files) = -1.
    int lang = -1;
};

struct SessionState {
    std::vector<SessionEntry> entries;   // left/primary view
    std::vector<SessionEntry> entries1;  // right/other view (split)
    int activeIndex = 0;                 // active tab in the left view
    int activeIndex1 = 0;                // active tab in the right view
    int activeView = 0;                  // which view had focus: 0 = left, 1 = right
};

std::wstring SessionFilePath();
bool SessionSave(const std::wstring& path, const SessionState& s);
bool SessionLoad(const std::wstring& path, SessionState* out);

} // namespace xfs
