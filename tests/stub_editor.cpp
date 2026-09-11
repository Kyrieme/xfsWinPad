// stub_editor.cpp - linker stub for unit tests that exercise the pure-text
// search helpers (CollectHitsInText / ReplaceInText) without a Scintilla
// control. Only the symbols actually referenced get stubbed.
#include "../src/editor/Editor.h"

namespace xfs {

sptr_t Editor::Send(unsigned int, uptr_t, LPARAM) const { return 0; }

} // namespace xfs
