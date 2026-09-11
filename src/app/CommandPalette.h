#pragma once
// xfsWinPad - Command Palette (Ctrl+Shift+P): filter-as-you-type launcher
// over the application command registry.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace xfs {

class CommandPalette {
public:
    void Show(HWND parent, HINSTANCE hInst);
    void Close();
    bool IsVisible() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }

    // executed command id lands here (main window dispatches)
    std::function<void(unsigned int)> onExecute;

    struct Item { const wchar_t* label; unsigned int cmd; };

    // Extra dynamic items (plugin commands) appended to the built-in list. The
    // labels stay valid for the lifetime of the palette. Any prior injected
    // pool is released; call before Show().
    void SetExtraItems(const std::vector<Item>& items);

private:
    friend LRESULT CALLBACK PalWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK PalEditProc(HWND, UINT, WPARAM, LPARAM,
                                        UINT_PTR, DWORD_PTR);

    void Refill();
    void ExecuteSelected();

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HWND edit_ = nullptr;
    HWND list_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<Item> visible_;
    std::vector<std::wstring> extraLabels_;   // backing storage for extra items
    std::vector<Item> extraItems_;
};

} // namespace xfs
