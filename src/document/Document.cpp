#include "Document.h"

namespace xfs {

std::wstring Document::DisplayName() const {
    if (!HasPath()) return untitledName_;
    return path.filename().wstring();
}

std::wstring Document::TitleForTab() const {
    std::wstring name = DisplayName();
    if (editor.Modified()) name = L"* " + name;
    return name;   // close X is drawn by the tab strip, not part of the title
}

} // namespace xfs