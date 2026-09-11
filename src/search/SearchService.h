#pragma once
// xfsWinPad - search state + plain-text find/replace engine (regex arrives later)

#include "../editor/Editor.h"
#include <string>

namespace xfs {

enum class FindDirection { Forward, Backward };

struct FindState {
    std::wstring text;
    std::wstring replace;
    bool matchCase = false;
    bool wholeWord = false;
    bool regexp = false;
};

// Returns true if a match was found and selected.
bool FindInEditor(Editor& ed, const FindState& st, FindDirection dir);

// Replaces the current selection when it matches the needle; otherwise
// advances to the next match. Returns true when a replacement was made.
bool ReplaceCurrentInEditor(Editor& ed, const FindState& st);

// Replaces every occurrence in the whole document as one undoable step.
// Returns the number of replacements.
int ReplaceAllInEditor(Editor& ed, const FindState& st);

} // namespace xfs
