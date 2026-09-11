#pragma once
// xfsWinPad - reusable single-line modal text input dialog.

#include <windows.h>
#include <string>

namespace xfs {

// Modal single-line text input.
//   value - in: initial text; out: entered text on OK.
// Returns true if OK pressed, false if cancelled / failed.
bool InputBox(HWND parent, HINSTANCE hInst, const std::wstring& title,
              const std::wstring& label, std::wstring& value);

} // namespace xfs
