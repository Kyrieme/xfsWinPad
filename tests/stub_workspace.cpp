// stub_workspace.cpp - linker stub for test_plugin: the plugin host bridge
// (PluginManager.cpp) references a few Workspace members, but the unit-test
// process never attaches a real workspace — the v2 callbacks are exercised
// only through the null-workspace degradation path. Only symbols actually
// referenced get stubbed.
#include "../src/workspace/Workspace.h"

namespace xfs {

Document* Workspace::Active() { return nullptr; }
Document* Workspace::At(int) { return nullptr; }
bool Workspace::Save() { return false; }
void Workspace::OpenPath(const std::wstring&, int, bool, bool) {}
// 4b: the NPP message shim routes SWITCHTOFILE/SAVEALLFILES/RELOAD* through
// Workspace; with no real workspace attached these degrade to false/no-op.
void Workspace::Activate(int) {}
bool Workspace::SaveAll() { return false; }
bool Workspace::ReloadDocument(int, bool) { return false; }
// WorkspaceSink::SaveAllDocs counts dirty docs via the editor control; the
// test process has no Scintilla child, so nothing is ever modified.
bool Editor::Modified() const { return false; }

} // namespace xfs
