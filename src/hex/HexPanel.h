#pragma once
// xfsWinPad - HexPanel: Notepad++-style hex view in a bottom-docked panel.
//
// - Second Scintilla control; the main editor is never touched.
// - Line format (78 chars + CRLF, 16 bytes/line):
//     OOOOOOOO  HH HH ... HH  |AAAAAAAAAAAAAAAA|
//     [0,8)=offset [10,58)=hex [60]='|' [61,77)=ascii [77]='|'
// - Column styling via manual container styling (address gray, hex default,
//   ascii dim, modified bytes highlighted).
// - Byte-level editing: the control stays SCI_SETREADONLY(1); WM_CHAR is
//   intercepted and mapped to (byte offset, nibble). Only validated hex
//   digits / printable ASCII mutate the overlay buffer - a corrupt dump is
//   structurally impossible. Edits land in a sparse overlay applied on save.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Scintilla.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace xfs {

struct ThemeDef;

class HexPanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    bool HasFocus() const;

    // Loads raw bytes of an existing file (cap 1MB, larger = truncated view,
    // saving disabled). Shows the panel.
    bool Load(const std::wstring& filePath);
    void Hide();

    // Applies the overlay back onto the file. Fails (safely) when the view
    // was truncated at load time.
    bool Save();

    bool Modified() const { return !overlay_.empty(); }
    const std::wstring& FilePath() const { return path_; }

    // invoked when the user presses the panel close button
    std::function<void()> onClose;
    // invoked while the user drags the top splitter; arg = desired panel height (px)
    std::function<void(int)> onHeightChange;

    void ApplyTheme(const ThemeDef& t);

    // re-apply localized texts after a language switch
    void Retranslate();

    // internal layout inside the panel rect
    void Layout(int w, int h);

    // byte-level undo/redo for hex edits (Ctrl+Z / Ctrl+Y when panel focused)
    void Undo();
    void Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SciProcThunk(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT SciProc(HWND, UINT, WPARAM, LPARAM);

    // dump geometry / mapping
    struct ByteRef { int line = -1; int index = -1; int nibble = 0; bool ascii = false; bool valid = false; };
    ByteRef PosToByte(sptr_t pos) const;
    sptr_t ByteToPos(int line, int index, bool ascii, int nibble) const;
    static constexpr int kBytesPerLine = 16;
    static constexpr int kOffsetLen = 8;
    static constexpr int kHexStart = 10;          // column of first hex byte
    static constexpr int kAsciiStart = 60;        // 8+2+48+2 = 60 (" |" = 2 chars)
    static constexpr int kLineChars = 77;         // without CRLF
    static constexpr int kLineStride = 79;        // with CRLF

    std::string BuildLine(int line) const;
    void RenderFull();
    void RenderLine(int line);
    void StyleRange(sptr_t start, sptr_t end);
    void StyleAll();
    void UpdateLabel();
    BYTE ByteAt(size_t off) const;
    void SetByte(size_t off, BYTE v);

    HWND hwnd_ = nullptr;      // panel
    HWND sci_ = nullptr;       // hex scintilla
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HFONT font_ = nullptr;
    HINSTANCE inst_ = nullptr;

    // top-edge splitter drag state (screen coordinates: the panel moves
    // under the cursor during resize, so client-relative deltas cancel out)
    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;

    std::wstring path_;
    std::string raw_;                  // file bytes actually shown
    unsigned long long fileSize_ = 0;  // real file size on disk
    bool truncated_ = false;
    std::map<size_t, BYTE> overlay_;   // byteOffset -> new value

    // hex edit undo/redo: (offset, value before, value after)
    struct HexUndo { size_t off; BYTE before; BYTE after; };
    std::vector<HexUndo> undo_;
    std::vector<HexUndo> redo_;

    // style indices (custom, >= STYLE_MAX predef not required; use 10..14)
    enum { ST_ADDR = 10, ST_HEX = 11, ST_HEX_MOD = 12, ST_ASCII = 13, ST_ASCII_MOD = 14 };
    struct {
        COLORREF bg = RGB(0xFF, 0xFF, 0xFF), fg = RGB(0x20, 0x20, 0x20);
        COLORREF addr = RGB(0x90, 0x90, 0x90), dim = RGB(0x78, 0x78, 0x78);
        COLORREF modFg = RGB(0xC0, 0x50, 0x00), modBg = RGB(0xFF, 0xE2, 0xC0);
    } colors_;
};

} // namespace xfs
