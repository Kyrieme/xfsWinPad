# ARCHITECTURE.md — xfsWinPad

## Module Map
```
src/
  app/        MainWindow (frame, menus, accelerators, layout, tab painting),
              FindDialog/GotoDialog (tabbed 查找/替换 pages), ResultsPanel
              (bottom-docked search results, double-click locate),
              DialogBuilder removed (dialogs built via CreateWindowEx), StatusBar
  core/       CommandIds (single command registry), Log (file logger),
              Util (UTF conversions, chunked file IO, path normalize)
  document/   Document = Editor + path + encoding + readonly state
  editor/     Editor: owns one Scintilla control; styling, margins, folding,
              lexer attach, text/EOL/status helpers
  encoding/   BOM sniffing, UTF-8 validity, UTF-16 LE/BE (BOM + NUL-ratio
              heuristic), ANSI fallback; forced decode for reload-as;
              internal text is always UTF-8 (SC_CP_UTF8)
  language/   Extension -> lexer name + keyword sets (Lexilla CreateLexer)
  search/     SearchService: Sci target-based find/replace-current/
              replace-all (single undo step), wrap-around, case/word flags;
              SearchAll: hit collection across current/open documents for
              the results panel (file/line/content rows)
  settings/   Flat-JSON settings store (%APPDATA%\xfsWinPad\settings.json):
              theme, editor font/size/tab/wrap + window geometry;
              BOM-tolerant reader, unknown-key skipping, defaults on corrupt
  theme/      Light/Dark palettes (ThemeDef tokens: editor base, syntax
              roles, tab chrome); Editor applies base + per-lexer style
              tables from vendored SciLexer.h constants
  tabs/       TabBar: subclassed SysTabControl32, owner-draw, close glyph,
              hover states, drag reorder, middle-click close
  plugin/     xfs_plugin_api.h: versioned C ABI (v3). A plugin DLL exports
              xfsPlugin_getInfo; host calls registerPlugin(host) with a struct
              of function pointers: add/removeCommand + editor/document access
              (openFile, save, bounded UTF-8 get/set text, caret insert,
              cursor/goto line, modified/readonly flags) + event hooks
              (XFS_EVT_* lifecycle/text/cursor bits, app raises via
              PluginManager::Raise; doc events come from Workspace slots,
              text/cursor coalesced on a 250 ms frame timer) + getConfigDir.
              All callbacks run on the UI thread; with no workspace open they
              degrade gracefully.
              PluginManager loads %APPDATA%\xfsWinPad\plugins\*.dll, verifies
              the exact ABI version, assigns command ids >=10000 (palette +
              Plugins menu), and bridges editor access by sending raw SCI_*
              messages to the active document's Scintilla child HWND.
  workspace/  Workspace: owns documents, open/save/save-as/reload/
              reload-as/set-target-encoding/close-all, recent files
              (%APPDATA%\xfsWinPad\recent.txt)
tests/        CTest unit tests (encoding round trips, settings persistence)
```

## Ownership & Lifetime
- `MainWindow` owns `Workspace`, `StatusBar`, `FindDialog`.
- `Workspace` owns all `Document`s (`unique_ptr` vector); each `Document`
  owns its own Scintilla child window (hidden/shown on activation).
- All windows are children of `editorHost_`; notifications are forwarded to
  the frame via `WM_NOTIFY`/`WM_COMMAND`.

## Data Flow
- Open: bytes → `ReadFileBytes` (64 MB chunks) → `encoding::DecodeToUtf8`
  → EOL dominance scan → `Editor::SetTextUtf8` → savepoint → tab insert.
- Save: `GetTextUtf8` → `encoding::EncodeFromUtf8(document encoding)` →
  `WriteFileBytes`.
- Dirty tracking: `SCI_SETSAVEPOINT` after load/save; `SCN_SAVEPOINTLEFT/
  REACHED` refresh titles (red dot) and title bar.

## Concurrency
Single UI thread. No background workers yet (large-file engine will add
them behind the same `Document` API).

## Coding Standards
- C++20, RAII, no owning raw pointers, no god classes, UI holds no business logic.
- Commands live only in `core/CommandIds.h`; menus + accelerators share ids.
- Logging through `Logger` (`%LOCALAPPDATA%\xfsWinPad\logs\xfsWinPad.log`).

## Planned Extensions (phases)
Encoding menu/reload-with-encoding → Find-in-files → themes/settings store →
macro recorder → plugin API/host → large-file mmap engine → hex/diff/log →
ATE/STDF → AI panel → terminal/git integration.
