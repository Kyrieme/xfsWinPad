#include "DiffView.h"
#include "../editor/Editor.h"
#include "../core/Log.h"
#include "../core/Util.h"

#include <Scintilla.h>
#include <algorithm>

namespace xfs {

void DiffView::Enter(Document* a, Document* b,
                     std::vector<int> mapA, std::vector<int> mapB) {
    if (!a || !b || a == b) return;
    a_ = a;
    b_ = b;
    mapA_ = std::move(mapA);
    mapB_ = std::move(mapB);
    Logger::Info("DiffView entered: '" + WideToUtf8(a_->DisplayName()) + "' | '" +
                 WideToUtf8(b_->DisplayName()) + "'");
}

void DiffView::Exit() {
    if (!Active()) return;
    Logger::Info("DiffView exited");
    a_ = nullptr;
    b_ = nullptr;
    mapA_.clear();
    mapB_.clear();
}

void DiffView::Layout(int x, int y, int w, int h) {
    if (!Active()) return;
    int half = w / 2;
    // 2px center gap lets the host background show through as a divider
    ::MoveWindow(a_->editor.Hwnd(), x, y, (std::max)(0, half - 1), h, TRUE);
    ::MoveWindow(b_->editor.Hwnd(), x + half + 1, y, (std::max)(0, w - half - 1), h, TRUE);
    ::ShowWindow(a_->editor.Hwnd(), SW_SHOW);
    ::ShowWindow(b_->editor.Hwnd(), SW_SHOW);
}

void DiffView::SyncScroll(HWND sourceEditor) {
    if (!Active() || syncing_) return;
    if (sourceEditor != a_->editor.Hwnd() && sourceEditor != b_->editor.Hwnd()) return;

    syncing_ = true;

    const bool srcIsA = (sourceEditor == a_->editor.Hwnd());
    Editor& srcEd = srcIsA ? a_->editor : b_->editor;
    Editor& dstEd = srcIsA ? b_->editor : a_->editor;
    const std::vector<int>& srcMap = srcIsA ? mapA_ : mapB_;

    const int srcLines = (int)srcEd.Send(SCI_GETLINECOUNT);
    int firstSrc = (int)srcEd.Send(SCI_GETFIRSTVISIBLELINE);
    const int visible = (int)srcEd.Send(SCI_LINESONSCREEN);

    // Find an anchor: the first mapped line at/after the top visible line;
    // if none below, fall back to the closest mapped line above.
    int srcAnchor = -1, dstAnchor = -1;
    for (int l = firstSrc; l < srcLines && l < firstSrc + visible; ++l) {
        if (l < (int)srcMap.size() && srcMap[l] >= 0) {
            srcAnchor = l;
            dstAnchor = srcMap[l];
            break;
        }
    }
    if (srcAnchor < 0) {
        for (int l = (std::min)(firstSrc, srcLines - 1); l >= 0; --l) {
            if (l < (int)srcMap.size() && srcMap[l] >= 0) {
                srcAnchor = l;
                dstAnchor = srcMap[l];
                break;
            }
        }
    }

    int dstTop;
    if (srcAnchor >= 0) {
        dstTop = dstAnchor - (srcAnchor - firstSrc);
    } else {
        dstTop = firstSrc;   // no mapping at all: plain line-number sync
    }
    int dstLines = (int)dstEd.Send(SCI_GETLINECOUNT);
    dstTop = (std::max)(0, (std::min)(dstTop, dstLines - 1));
    dstEd.Send(SCI_SETFIRSTVISIBLELINE, dstTop);

    syncing_ = false;
}

} // namespace xfs
