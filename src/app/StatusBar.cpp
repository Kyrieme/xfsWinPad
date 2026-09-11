#include "StatusBar.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <algorithm>
#pragma comment(lib, "comctl32.lib")

namespace xfs {

bool StatusBar::Create(HWND parent, HINSTANCE hInst) {
    hwnd_ = ::CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, parent, (HMENU)(INT_PTR)1001, hInst, nullptr);
    return hwnd_ != nullptr;
}

void StatusBar::Destroy() {
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void StatusBar::Layout(int width, int dpi) {
    // 段语义（SB_SETPARTS 的值 = 各段右缘，最后一段自动延伸到窗口右缘）：
    //   [0] Ln/Col/Sel   [1] Length/Lines/Words   [2] Binary/ReadOnly
    //   [3] EOL          [4] Encoding             [5] Language
    // 旧实现三处硬伤：EOL 段宽度恒为 0；Encoding 段右缘 < 左缘（倒挂不渲染）；
    // int u = dpi/96 在 >100% 缩放下恒为 1，宽度从不随 DPI 放大（"C/C++" 被裁）。
    auto S = [&](int px96) { return ::MulDiv(px96, (std::max)(dpi, 96), 96); };
    const int langW = S(150);   // Language：容纳 "JavaScript"/"PowerShell" 等长名
    const int encW  = S(120);   // Encoding："UTF-8 with BOM" 等
    const int eolW  = S(120);   // EOL："Windows CRLF" 等
    const int binW  = S(110);   // Binary / Read Only
    const int docW  = S(360);   // Length/Lines/Words（最长文本）
    const int posW  = S(230);   // Ln/Col/Sel

    // 右簇从右往左排，保证右缘严格单调递增
    parts_[5] = width;                       // Language（最后段，右缘值被忽略）
    parts_[4] = parts_[5] - langW;           // Encoding
    parts_[3] = parts_[4] - encW;            // EOL
    parts_[2] = parts_[3] - eolW;            // Binary/ReadOnly
    parts_[1] = (std::max)(parts_[2] - binW, posW + docW);  // Length/Lines/Words
    parts_[0] = (std::max)(S(10), parts_[1] - docW);        // Ln/Col/Sel
    ::SendMessageW(hwnd_, SB_SETPARTS, 6, (LPARAM)parts_);
}

void StatusBar::SetPart(int index, const std::wstring& text) {
    ::SendMessageW(hwnd_, SB_SETTEXTW, index, (LPARAM)text.c_str());
}

void StatusBar::SetPosition(int line, int col, int selection) {
    SetPart(0, I18n::Instance().Fmt(L"sb.pos",
        {std::to_wstring(line), std::to_wstring(col), std::to_wstring(selection)}));
}

void StatusBar::SetDocInfo(long long lengthBytes, int lines, int words) {
    wchar_t lenStr[32];
    if (lengthBytes >= 1024 * 1024)
        swprintf_s(lenStr, L"%.1f MB", lengthBytes / 1048576.0);
    else if (lengthBytes >= 1024)
        swprintf_s(lenStr, L"%.1f KB", lengthBytes / 1024.0);
    else
        swprintf_s(lenStr, L"%lld B", lengthBytes);
    if (words >= 0)
        SetPart(1, I18n::Instance().Fmt(L"sb.docinfo.words",
            {lenStr, std::to_wstring(lines), std::to_wstring(words)}));
    else
        SetPart(1, I18n::Instance().Fmt(L"sb.docinfo",
            {lenStr, std::to_wstring(lines)}));
}

void StatusBar::SetEol(const std::wstring& eol) { SetPart(3, eol); }
void StatusBar::SetEncoding(const std::wstring& enc) { SetPart(4, enc); }
void StatusBar::SetLanguage(const std::wstring& lang) { SetPart(5, lang); }

void StatusBar::SetBinary(bool isBinary) {
    SetPart(2, isBinary ? L"Binary" : L"");
}

void StatusBar::SetReadOnly(bool ro) { SetPart(2, ro ? L"Read Only" : L""); }

} // namespace xfs
