#pragma once
// xfsWinPad - DiffView: side-by-side compare layout for two documents with
// diff-aligned synchronized scrolling.
//
// The two documents keep their own Scintilla controls; DiffView only positions
// them left/right inside the editor host and translates scroll positions
// through the LCS line mapping (line -> corresponding line, or -1 if unmatched).

#include "../document/Document.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

namespace xfs {

class DiffView {
public:
    // mapX[line] = corresponding line in the other doc, or -1 when unmatched
    void Enter(Document* a, Document* b,
               std::vector<int> mapA, std::vector<int> mapB);
    void Exit();

    bool Active() const { return a_ != nullptr && b_ != nullptr; }
    bool Contains(const Document* d) const { return d && (d == a_ || d == b_); }

    Document* Left()  const { return a_; }
    Document* Right() const { return b_; }

    // Position both editors side by side inside the given rect.
    void Layout(int x, int y, int w, int h);

    // Re-align the other pane when one pane is scrolled (SCN_UPDATEUI / SC_UPDATE_V_SCROLL).
    void SyncScroll(HWND sourceEditor);

private:
    Document* a_ = nullptr;
    Document* b_ = nullptr;
    std::vector<int> mapA_, mapB_;   // line -> line in other pane, -1 = unmatched
    bool syncing_ = false;
};

} // namespace xfs
