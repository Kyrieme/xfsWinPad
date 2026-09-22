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

std::wstring SessionDir();
std::wstring SessionFilePath();
// Multi-window sessions: the first (primary) instance keeps writing the
// legacy session.json; every extra window writes its own per-pid slot file
// (session-<pid>.json) so concurrent processes never clobber each other.
std::wstring SessionSlotPath(bool primary);
// Lists slot files in `dir` (session-<digits>.json), excluding the one for
// `excludePid`. Slots untouched for more than `maxAgeDays` are deleted
// (crash orphans). Pure over `dir` so tests can point it at a temp folder.
std::vector<std::wstring> SessionSlots(const std::wstring& dir,
                                       unsigned long excludePid,
                                       unsigned int maxAgeDays = 30);

// Closing one window is NOT the same event as quitting the whole application.
// Every window is its own process and there is no global owner, so "am I the
// last window?" can only be answered by counting the live main windows at close
// time (MainWindow::CountOtherMainWindows).
//   * last window closing  == the app exits -> leave the state behind, the next
//     launch restores it (this is the long-standing behaviour);
//   * other windows still alive == the user deliberately dropped this one ->
//     leave nothing behind.
// Getting this wrong is what produced the "extra window on every launch" bug:
// a window that was closed while others stayed open still wrote its slot, and
// because a restored child claims the slot and writes a fresh one on its own
// exit, the ghost window was self-perpetuating (only the 30-day GC bounded it).
enum class CloseDisposition {
    PersistSession,   // the app is exiting: write session.json / this slot
    RetireSlot,       // only this window goes away: write nothing
};
CloseDisposition SessionCloseDisposition(int otherLiveWindows);

// Applies the policy above and, when it says "persist", writes `slotPath`.
// `isPrimary` windows always persist: session.json is the canonical session and
// the primary is what a later launch restores as "the main window". When the
// disposition is RetireSlot the path is cleared instead, so a same-pid leftover
// (pid recycling) cannot resurrect the window either.
// Returns true when the state was written.
bool SessionPersistOnClose(const std::wstring& slotPath, bool isPrimary,
                           const SessionState& s, int otherLiveWindows);

bool SessionSave(const std::wstring& path, const SessionState& s);
bool SessionLoad(const std::wstring& path, SessionState* out);

} // namespace xfs
