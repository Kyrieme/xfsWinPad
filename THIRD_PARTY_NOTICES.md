# THIRD_PARTY_NOTICES — xfsWinPad

This product includes third-party software. Each component keeps its original
license file inside its source tree; the trees are vendored unmodified.

## Scintilla 5.6.6
- Source: https://www.scintilla.org/scintilla566.zip (official release)
- Vendored at: `third_party/scintilla`
- License: Historical Permission Notice and Disclaimer (permissive) — see
  `third_party/scintilla/License.txt` (reproduced below in summary; the file
  itself is authoritative).
- Summary: Permission to use, copy, modify, and distribute this software and
  its documentation for any purpose and without fee is granted, provided the
  copyright notices and this permission notice appear in all copies. The
  copyright holders disclaim all warranties.

## Lexilla 5.5.3
- Source: https://www.scintilla.org/lexilla553.zip (official release)
- Vendored at: `third_party/lexilla`
- License: same HPND-style license — see `third_party/lexilla/License.txt`.

## miniz 3.1.2
- Source: https://github.com/richgel999/miniz (public domain / unlicense)
- Vendored at: `third_party/miniz`
- License: Unlicense (public domain) — see `third_party/miniz/LICENSE`.
- Used for: ZIP archive read/extract in the plugin download/install pipeline
  (`PluginInstaller`). Compiled as a static library.

## Build notes
Scintilla and Lexilla are built from their unmodified sources as DLLs by
`third_party/CMakeLists.txt` and shipped next to `xfsWinPad.exe`; miniz is
compiled as a static library and linked into the host and its tests.

No Notepad++ code or assets are used anywhere in this project.
