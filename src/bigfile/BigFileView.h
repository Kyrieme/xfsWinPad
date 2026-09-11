#pragma once
// xfsWinPad - BigFileView: bottom-docked read-only viewer for files far beyond
// the 2GB Scintilla limit. Pairs with BigFileModel (paged mmap + sparse line
// index); only the visible window of lines is ever materialized/rendered.
//
// Container = toolbar row (file label, progress, goto/find, close) + a
// self-drawn render child with virtual scrollbars driven by model line count.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include "BigFileModel.h"

namespace xfs {

struct ThemeDef;

class BigFileView {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    bool HasFocus() const;
    void Hide();

    bool LoadFile(const std::wstring& path);
    const std::wstring& Path() const { return model_ ? model_->Path() : empty_; }

    std::function<void()> onClose;               // close button
    std::function<void(int)> onHeightChange;     // top splitter drag (px)

    void ApplyTheme(const ThemeDef& t);
    void Layout(int w, int h);
    void Retranslate();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    // render child
    static LRESULT CALLBACK RenderProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT RenderProc(HWND, UINT, WPARAM, LPARAM);

    void EnsureFont();
    void UpdateMetrics();
    void SyncScrollbars();
    void SyncProgress();
    void SetTopLine(unsigned long long line);
    void Paint(HDC hdc);
    void GotoLine();
    void FindNext();

    HWND hwnd_ = nullptr;        // container
    HWND render_ = nullptr;      // self-drawn render child
    HWND label_ = nullptr;       // file name + progress
    HWND gotoEdit_ = nullptr;
    HWND gotoBtn_ = nullptr;
    HWND findEdit_ = nullptr;
    HWND findBtn_ = nullptr;
    HWND closeBtn_ = nullptr;
    HFONT font_ = nullptr;
    HFONT uiFont_ = nullptr;
    HINSTANCE inst_ = nullptr;

    std::unique_ptr<BigFileModel> model_;
    std::wstring empty_;

    unsigned long long topLine_ = 0;
    int lineH_ = 16;
    int charW_ = 8;
    int gutterW_ = 60;

    // h-scroll in characters
    unsigned long long hChar_ = 0;

    COLORREF bg_ = RGB(0xFF, 0xFF, 0xFF);
    COLORREF fg_ = RGB(0x20, 0x20, 0x20);
    COLORREF numFg_ = RGB(0x88, 0x88, 0x88);
    COLORREF gutterBg_ = RGB(0xF0, 0xF0, 0xF0);

    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;
    int curH_ = 300;

    enum { kTimerId = 1 };
};

} // namespace xfs
