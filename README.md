# xfsWinPad

Professional native Windows text editor (C++20 / Win32 / Scintilla 5 + Lexilla 5).

## Build (VS 2026 toolchain required)
```powershell
cmake -S . -B build            # generator: Visual Studio 18 2026 (see CMakePresets.json)
cmake --build build --config Release
```
Output: `build\bin\Release\xfsWinPad.exe` (+ `Scintilla.dll`, `Lexilla.dll` copied alongside).

Or with presets: `cmake --preset windows-x64` then `cmake --build --preset release`.

## Run
```
xfsWinPad.exe [options] [files...]
  files...        one or more files to open
  --line N        jump to line N in the last opened file
  --search TEXT   open Find dialog prefilled with TEXT
  --readonly      open files read-only
  --new           accepted for forward compatibility (single-instance later)
```

## Layout
See `ARCHITECTURE.md`. Docs: `PROJECT.md`, `TODO.md`, `THIRD_PARTY_NOTICES.md`.
