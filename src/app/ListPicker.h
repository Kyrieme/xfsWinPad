#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace xfs {

// Modal list-picker dialog. currentSel is the initially highlighted item
// (-1 for none). Returns true with outSel >= 0 on OK/double-click.
bool ListPicker(HWND parent, HINSTANCE hInst, const std::wstring& title,
                const std::wstring& label,
                const std::vector<std::wstring>& items,
                int currentSel, int& outSel);

} // namespace xfs
