#pragma once
// xfsWinPad - StatusBar: document state display (Ln/Col/Sel, EOL, encoding, language)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace xfs {

class StatusBar {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    void Layout(int width, int dpi);

    void SetPosition(int line, int col, int selection);
    void SetDocInfo(long long lengthBytes, int lines, int words = -1);
    void SetBinary(bool isBinary);
    void SetEol(const std::wstring& eol);
    void SetEncoding(const std::wstring& enc);
    void SetLanguage(const std::wstring& lang);
    void SetReadOnly(bool ro);
    void SetPart(int index, const std::wstring& text);

private:

    HWND hwnd_ = nullptr;
    int parts_[6] = {0};
};

} // namespace xfs
