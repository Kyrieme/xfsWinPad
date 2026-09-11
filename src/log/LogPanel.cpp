#include "LogPanel.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../theme/Theme.h"

#include <commctrl.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <regex>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace xfs {

namespace {
constexpr wchar_t kPanelClass[] = L"xfsWinPadLogPanel";
constexpr int ID_LABEL = 1500;
constexpr int ID_CLOSE = 1501;
constexpr int ID_OPEN = 1502;
constexpr int ID_FOLLOW = 1503;
constexpr int ID_HL = 1504;
constexpr int ID_FILTER = 1505;
constexpr int ID_EXCLUDE = 1506;
constexpr int ID_REGEX = 1509;
constexpr int ID_APPLY = 1507;
constexpr int ID_CLEAR = 1508;
constexpr int ID_GOTO_EDIT = 1510;
constexpr int ID_GOTO = 1511;
constexpr int ID_PRESET_COMBO = 1512;
constexpr int ID_PRESET_SAVE = 1513;
constexpr int ID_PRESET_DEL = 1514;
constexpr int ID_PRESET_LBL = 1515;
constexpr int ID_TAB_ADD = 1516;
constexpr unsigned long long kMaxInitial = 8ull * 1024ull * 1024ull;   // tail cap
constexpr DWORD kMaxChunkPerTick = 8u * 1024u * 1024u;

bool ContainsNoCase(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    size_t n = needle.size();
    if (text.size() < n) return false;
    for (size_t i = 0; i + n <= text.size(); ++i) {
        size_t j = 0;
        while (j < n && tolower((unsigned char)text[i + j]) ==
                        tolower((unsigned char)needle[j]))
            ++j;
        if (j == n) return true;
    }
    return false;
}

// number of bytes of an incomplete trailing UTF-8 sequence
size_t IncompleteUtf8Tail(const std::string& s) {
    size_t i = s.size();
    int check = 0;
    while (check < 3 && i > 0) {
        unsigned char c = (unsigned char)s[i - 1];
        if ((c & 0xC0) == 0x80) { --i; ++check; continue; }
        if ((c & 0xE0) == 0xC0) { --i; ++check; }
        else if ((c & 0xF0) == 0xE0) { --i; ++check; }
        else if ((c & 0xF8) == 0xF0) { --i; ++check; }
        break;
    }
    if (check == 0) return 0;
    unsigned char lead = (unsigned char)s[i];
    int need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3
             : (lead & 0xF8) == 0xF0 ? 4 : 0;
    if (need == 0) return 0;
    int have = (int)s.size() - (int)i;
    return have < need ? (size_t)have : 0;
}

// --- filter presets (persisted, container-level) -------------------------------
constexpr wchar_t kPresetSep = 0x1F;
struct FilterPreset {
    std::wstring name, filter;
    bool exclude = false, regex = false, highlight = true;
};

std::wstring PresetFilePath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData;
        CoTaskMemFree(appData);
    }
    ::CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    return base + L"\\xfsWinPad\\log_presets.txt";
}

std::vector<FilterPreset> LoadPresetFile() {
    std::vector<FilterPreset> out;
    std::string content;
    if (!ReadFileBytes(PresetFilePath(), content)) return out;
    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find('\n', start);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        start = end + 1;
        if (line.empty()) continue;
        std::wstring w = Utf8ToWide(line);
        FilterPreset p;
        size_t s = 0, e = w.find(kPresetSep, s);
        if (e == std::wstring::npos) continue;
        p.name = w.substr(s, e - s); s = e + 1;
        e = w.find(kPresetSep, s);
        p.filter = (e == std::wstring::npos) ? w.substr(s) : w.substr(s, e - s);
        if (e == std::wstring::npos) continue; s = e + 1;
        e = w.find(kPresetSep, s);
        if (e == std::wstring::npos) continue;
        p.exclude = w.substr(s, e - s) == L"1"; s = e + 1;
        e = w.find(kPresetSep, s);
        if (e == std::wstring::npos) continue;
        p.regex = w.substr(s, e - s) == L"1"; s = e + 1;
        p.highlight = w.substr(s) != L"0";
        out.push_back(p);
    }
    return out;
}

void SavePresetFile(const std::vector<FilterPreset>& presets) {
    std::string out;
    for (auto& p : presets) {
        std::wstring rec = p.name + kPresetSep + p.filter + kPresetSep +
                           (p.exclude ? L"1" : L"0") + kPresetSep +
                           (p.regex ? L"1" : L"0") + kPresetSep +
                           (p.highlight ? L"1" : L"0");
        out += WideToUtf8(rec) + "\r\n";
    }
    WriteFileBytes(PresetFilePath(), out.data(), out.size());
}

std::wstring FileBase(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
} // namespace

// =============================================================================
//  LogSession — one log file, own Scintilla + follow/filter state
// =============================================================================

bool LogSession::Create(HWND parent, HINSTANCE hInst, HFONT font) {
    if (sci_) return true;
    parent_ = parent;
    inst_ = hInst;
    sci_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"Scintilla", nullptr,
                             WS_CHILD | WS_VISIBLE, 0, 0, 100, 100,
                             parent, nullptr, hInst, nullptr);
    if (!sci_) return false;
    ::SetWindowSubclass(sci_, SciProcThunk, 0x4C4F4731 /*'LOG1'*/, (DWORD_PTR)this);
    ::SendMessageW(sci_, WM_SETFONT, (WPARAM)font, TRUE);

    Send(sci_, SCI_SETREADONLY, 1);
    Send(sci_, SCI_SETCODEPAGE, SC_CP_UTF8);
    Send(sci_, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Consolas");
    Send(sci_, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    Send(sci_, SCI_STYLECLEARALL);
    Send(sci_, SCI_SETWRAPMODE, SC_WRAP_NONE);
    Send(sci_, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    Send(sci_, SCI_SETMARGINMASKN, 0, 0);
    Send(sci_, SCI_SETMARGINSENSITIVEN, 0, 0);
    Send(sci_, SCI_SETMARGINWIDTHN, 0, 46);
    Send(sci_, SCI_SETMARGINWIDTHN, 1, 0);
    Send(sci_, SCI_SETMARGINWIDTHN, 2, 0);
    Send(sci_, SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(0xE8, 0xE8, 0xE8));
    Send(sci_, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x88, 0x88, 0x88));
    Send(sci_, SCI_SETUNDOCOLLECTION, 0);
    Send(sci_, SCI_SETCARETPERIOD, 0);
    Send(sci_, SCI_SETREADONLY, 1);

    Send(sci_, SCI_MARKERDEFINE, MARK_LOG_HL, SC_MARK_BACKGROUND);
    Send(sci_, SCI_MARKERSETBACK, MARK_LOG_HL, RGB(0xFF, 0xF1, 0x9C));
    Send(sci_, SCI_MARKERSETALPHA, MARK_LOG_HL, 60);
    return true;
}

void LogSession::Destroy() {
    if (sci_) {
        ::RemoveWindowSubclass(sci_, SciProcThunk, 0x4C4F4731);
        ::DestroyWindow(sci_);
        sci_ = nullptr;
    }
}

void LogSession::SetTheme(COLORREF bg, COLORREF fg, COLORREF hl) {
    if (!sci_) return;
    Send(sci_, SCI_STYLESETFORE, STYLE_DEFAULT, fg);
    Send(sci_, SCI_STYLESETBACK, STYLE_DEFAULT, bg);
    Send(sci_, SCI_STYLECLEARALL);
    Send(sci_, SCI_MARKERSETBACK, MARK_LOG_HL, hl);
}

bool LogSession::LoadFile(const std::wstring& path, bool preserveState) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Logger::Error("LogPanel: cannot open " + WideToUtf8(path));
        return false;
    }
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h, &li)) { ::CloseHandle(h); return false; }
    unsigned long long size = (unsigned long long)li.QuadPart;

    BY_HANDLE_FILE_INFORMATION bhfi{};
    if (::GetFileInformationByHandle(h, &bhfi)) {
        fileSerial_ = bhfi.dwVolumeSerialNumber;
        fileIndexHi_ = bhfi.nFileIndexHigh;
        fileIndexLo_ = bhfi.nFileIndexLow;
    } else {
        fileSerial_ = fileIndexHi_ = fileIndexLo_ = 0;
    }

    unsigned long long start = 0;
    truncated_ = size > kMaxInitial;
    if (truncated_) start = size - kMaxInitial;

    LARGE_INTEGER li2{};
    li2.QuadPart = (LONGLONG)start;
    ::SetFilePointerEx(h, li2, nullptr, FILE_BEGIN);
    DWORD toRead = (DWORD)(size - start);
    std::string raw(toRead, '\0');
    DWORD got = 0;
    BOOL rd = toRead == 0 || ::ReadFile(h, raw.data(), toRead, &got, nullptr);
    ::CloseHandle(h);
    if (!rd || got != toRead) {
        Logger::Error("LogPanel: read failed " + WideToUtf8(path));
        return false;
    }

    if (truncated_) {
        size_t nl = raw.find('\n');
        if (nl != std::string::npos) raw.erase(0, nl + 1);
    }

    path_ = path;
    fileSize_ = size;
    lastSize_ = size;
    utf8Mode_ = raw.empty() ||
        (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
         (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) ||
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              raw.data(), (int)raw.size(), nullptr, 0) > 0;
    pending_.clear();

    std::string utf8 = utf8Mode_ ? raw : WideToUtf8(Utf8ToWide(raw, CP_ACP));
    Send(sci_, SCI_SETREADONLY, 0);
    Send(sci_, SCI_SETTEXT, 0, (LPARAM)"");
    Send(sci_, SCI_APPENDTEXT, utf8.size(), (LPARAM)utf8.c_str());
    Send(sci_, SCI_SETREADONLY, 1);
    totalLines_ = (int)Send(sci_, SCI_GETLINECOUNT);
    UpdateLineNumberMargin();

    if (!preserveState) {
        filterOn_ = false;
        filter_.clear();
    } else if (filterOn_) {
        ApplyFilter();
    }

    Send(sci_, SCI_GOTOPOS, Send(sci_, SCI_GETLENGTH));
    Logger::Info("LogPanel load: " + WideToUtf8(path) + " size=" +
                 std::to_string(size) +
                 (truncated_ ? " (tail 8MB)" : "") +
                 " utf8=" + std::to_string(utf8Mode_ ? 1 : 0));
    return true;
}

std::string LogSession::DecodeChunk(const std::string& raw) const {
    if (!utf8Mode_) return WideToUtf8(Utf8ToWide(raw, CP_ACP));
    size_t hold = IncompleteUtf8Tail(raw);
    return raw.substr(0, raw.size() - hold);
}

void LogSession::AppendText(const std::string& utf8) {
    Send(sci_, SCI_SETREADONLY, 0);
    Send(sci_, SCI_APPENDTEXT, utf8.size(), (LPARAM)utf8.c_str());
    Send(sci_, SCI_SETREADONLY, 1);
    int newTotal = (int)Send(sci_, SCI_GETLINECOUNT);
    int firstNew = totalLines_ - 1;
    if (firstNew < 0) firstNew = 0;
    if (filterOn_) ApplyFilter();
    else if (highlight_) {
        std::string needle = WideToUtf8(filter_);
        for (int l = std::max(1, firstNew); l < newTotal; ++l) {
            char buf[4096];
            sptr_t len = Send(sci_, SCI_GETLINE, l, (LPARAM)buf);
            buf[len > 0 && len < 4095 ? len : 0] = '\0';
            if (ContainsNoCase(buf, needle))
                Send(sci_, SCI_MARKERADD, l, MARK_LOG_HL);
        }
    }
    totalLines_ = newTotal;
    UpdateLineNumberMargin();
    if (follow_) Send(sci_, SCI_SCROLLTOEND);
}

void LogSession::PollFile() {
    if (path_.empty()) return;
    HANDLE h = ::CreateFileW(path_.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h, &li)) { ::CloseHandle(h); return; }
    unsigned long long size = (unsigned long long)li.QuadPart;

    BY_HANDLE_FILE_INFORMATION bi{};
    bool sameId = false;
    if (::GetFileInformationByHandle(h, &bi)) {
        sameId = (bi.dwVolumeSerialNumber == fileSerial_) &&
                 (bi.nFileIndexHigh == fileIndexHi_) &&
                 (bi.nFileIndexLow == fileIndexLo_);
    }
    if (!sameId) {
        ::CloseHandle(h);
        Logger::Info("LogPanel: file rotated (new identity), reloading " +
                     WideToUtf8(path_));
        LoadFile(path_, true);
        return;
    }

    if (size < lastSize_) {
        ::CloseHandle(h);
        Logger::Info("LogPanel: file shrank, reloading " + WideToUtf8(path_));
        LoadFile(path_, true);
        return;
    }
    if (size == lastSize_) { ::CloseHandle(h); return; }

    unsigned long long avail = size - lastSize_;
    DWORD toRead = (DWORD)std::min<unsigned long long>(avail, kMaxChunkPerTick);
    LARGE_INTEGER li2{};
    li2.QuadPart = (LONGLONG)lastSize_;
    ::SetFilePointerEx(h, li2, nullptr, FILE_BEGIN);
    std::string raw(toRead, '\0');
    DWORD got = 0;
    BOOL rd = ::ReadFile(h, raw.data(), toRead, &got, nullptr);
    ::CloseHandle(h);
    if (!rd || got == 0) return;
    lastSize_ += got;
    fileSize_ = size;

    raw = DecodeChunk(raw);
    if (!pending_.empty()) { raw.insert(0, pending_); pending_.clear(); }
    if (utf8Mode_) {
        size_t hold = IncompleteUtf8Tail(raw);
        if (hold > 0) {
            pending_ = raw.substr(raw.size() - hold);
            raw.erase(raw.size() - hold);
        }
    }
    if (!raw.empty()) AppendText(raw);
}

void LogSession::ApplyFilter() {
    Logger::Debug(std::string("LogPanel ApplyFilter: filter_='") +
                  WideToUtf8(filter_) + std::string("'"));

    std::regex re;
    bool useRegex = false;
    if (regexOn_ && !filter_.empty()) {
        try {
            re.assign(WideToUtf8(filter_), std::regex_constants::icase);
            useRegex = true;
        } catch (const std::regex_error&) {
            Logger::Warn("LogPanel: invalid regex, falling back to substring: " +
                         WideToUtf8(filter_));
        }
    }
    std::string needle = WideToUtf8(filter_);
    auto lineMatches = [&](const char* s) -> bool {
        if (filter_.empty()) return true;
        if (useRegex) {
            try { return std::regex_search(s, re); }
            catch (const std::regex_error&) { return false; }
        }
        return ContainsNoCase(s, needle);
    };

    int lines = (int)Send(sci_, SCI_GETLINECOUNT);
    Send(sci_, SCI_SETREADONLY, 0);
    Send(sci_, SCI_MARKERDELETEALL, MARK_LOG_HL);
    Send(sci_, SCI_SHOWLINES, 0, std::max(0, lines - 1));
    Send(sci_, SCI_SETREADONLY, 1);

    int runStart = -1;
    for (int l = 0; l < lines; ++l) {
        char lineBuf[4096];
        sptr_t len = Send(sci_, SCI_GETLINE, l, (LPARAM)lineBuf);
        lineBuf[len > 0 && len < 4095 ? len : 0] = '\0';
        bool match = lineMatches(lineBuf);
        bool keep = filterOn_ ? (exclude_ ? !match : match) : true;
        if (!keep && runStart < 0) runStart = l;
        if ((keep || l == lines - 1) && runStart >= 0) {
            int runEnd = keep ? l - 1 : l;
            Send(sci_, SCI_HIDELINES, runStart, runEnd);
            runStart = -1;
        }
        if (highlight_ && match && !filter_.empty())
            Send(sci_, SCI_MARKERADD, l, MARK_LOG_HL);
    }
    if (follow_) Send(sci_, SCI_SCROLLTOEND);
}

void LogSession::ClearFilter() {
    filterOn_ = false;
    filter_.clear();
    int lines = (int)Send(sci_, SCI_GETLINECOUNT);
    Send(sci_, SCI_SETREADONLY, 0);
    Send(sci_, SCI_MARKERDELETEALL, MARK_LOG_HL);
    Send(sci_, SCI_SETREADONLY, 1);
    Send(sci_, SCI_SHOWLINES, 0, std::max(0, lines - 1));
}

// jump to the first line whose leading timestamp is >= the requested one
void LogSession::GoToTimestamp(const std::wstring& targetW) {
    if (targetW.empty()) return;
    std::string target = WideToUtf8(targetW);
    int lines = (int)Send(sci_, SCI_GETLINECOUNT);
    for (int l = 0; l < lines; ++l) {
        char buf[4096];
        sptr_t len = Send(sci_, SCI_GETLINE, l, (LPARAM)buf);
        buf[len > 0 && len < 4095 ? len : 0] = '\0';
        if (buf[0] == '\0' || buf[0] == '\r' || buf[0] == '\n' ||
            buf[0] == ' ' || buf[0] == '\t')
            continue;
        std::string head = buf;
        if (head.size() < target.size()) {
            if (head >= target) { Send(sci_, SCI_GOTOLINE, l); return; }
            continue;
        }
        if (head.substr(0, target.size()) >= target) {
            Send(sci_, SCI_GOTOLINE, l);
            return;
        }
    }
    if (lines > 0) Send(sci_, SCI_GOTOLINE, lines - 1);
}

void LogSession::UpdateLineNumberMargin() {
    if (!sci_) return;
    int lines = (int)Send(sci_, SCI_GETLINECOUNT);
    int digits = 1;
    while (lines >= 10) { lines /= 10; ++digits; }
    char sample[16]{};
    sprintf_s(sample, "%d", (1 << digits) - 1);
    sptr_t w = Send(sci_, SCI_TEXTWIDTH, STYLE_LINENUMBER, (LPARAM)sample);
    Send(sci_, SCI_SETMARGINWIDTHN, 0, w + 10);
}

LRESULT LogSession::SciProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CHAR: return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                ::SetFocus(::GetAncestor(parent_, GA_ROOT));
                return 0;
            }
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK LogSession::SciProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                          UINT_PTR, DWORD_PTR ref) {
    auto* self = (LogSession*)ref;
    return self->SciProc(h, m, wp, lp);
}

// =============================================================================
//  LogPanel — container with tab strip
// =============================================================================

bool LogPanel::Create(HWND parent, HINSTANCE hInst) {
    if (hwnd_) return true;
    inst_ = hInst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPanelClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    int dpi = ::GetDpiForWindow(parent);
    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    hwnd_ = ::CreateWindowExW(0, kPanelClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 600, 320, parent, (HMENU)(INT_PTR)1105,
                              hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                  int id) -> HWND {
        HWND c = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 60, 22, hwnd_, (HMENU)(INT_PTR)id,
                                   hInst, nullptr);
        if (c) ::SendMessageW(c, WM_SETFONT, (WPARAM)font_, TRUE);
        return c;
    };

    tabs_ = ::CreateWindowExW(0, L"SysTabControl32", nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH |
                              TCS_OWNERDRAWFIXED | TCS_FOCUSNEVER,
                              0, 0, 400, 22, hwnd_, nullptr, hInst, nullptr);
    if (tabs_) {
        ::SendMessageW(tabs_, WM_SETFONT, (WPARAM)font_, TRUE);
        ::SetWindowSubclass(tabs_, TabProcThunk, 0x4C4F4754 /*'LOGT'*/, (DWORD_PTR)this);
    }
    addBtn_  = mk(L"BUTTON", L"＋", BS_PUSHBUTTON, ID_TAB_ADD);

    label_    = mk(L"STATIC",  Tr(L"panel.log.notopen"), 0, ID_LABEL);
    openBtn_  = mk(L"BUTTON",  Tr(L"panel.log.open"), BS_PUSHBUTTON, ID_OPEN);
    followChk_= mk(L"BUTTON",  Tr(L"log.follow"), BS_AUTOCHECKBOX, ID_FOLLOW);
    hlChk_    = mk(L"BUTTON",  Tr(L"log.hl"), BS_AUTOCHECKBOX, ID_HL);
    filterEdit_=mk(L"EDIT",    L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ID_FILTER);
    excludeChk_=mk(L"BUTTON",  Tr(L"log.exclude"), BS_AUTOCHECKBOX, ID_EXCLUDE);
    regexChk_ = mk(L"BUTTON",  Tr(L"log.regex"), BS_AUTOCHECKBOX, ID_REGEX);
    applyBtn_ = mk(L"BUTTON",  Tr(L"log.apply"), BS_PUSHBUTTON, ID_APPLY);
    clearBtn_ = mk(L"BUTTON",  Tr(L"log.clear"), BS_PUSHBUTTON, ID_CLEAR);
    gotoEdit_  = mk(L"EDIT",   L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ID_GOTO_EDIT);
    gotoBtn_  = mk(L"BUTTON",  Tr(L"log.goto"), BS_PUSHBUTTON, ID_GOTO);
    presetLbl_  = mk(L"STATIC", Tr(L"log.preset"), SS_LEFT, ID_PRESET_LBL);
    presetCombo_ = mk(L"COMBOBOX", L"", WS_BORDER | CBS_DROPDOWN | CBS_HASSTRINGS |
                      WS_VSCROLL | WS_TABSTOP, ID_PRESET_COMBO);
    presetSave_ = mk(L"BUTTON", Tr(L"log.savepreset"), BS_PUSHBUTTON, ID_PRESET_SAVE);
    presetDel_  = mk(L"BUTTON", Tr(L"log.delpreset"), BS_PUSHBUTTON, ID_PRESET_DEL);
    closeBtn_ = mk(L"BUTTON",  Tr(L"panel.log.close"), BS_PUSHBUTTON, ID_CLOSE);
    ::CheckDlgButton(hwnd_, ID_FOLLOW, BST_CHECKED);
    ::CheckDlgButton(hwnd_, ID_HL, BST_CHECKED);

    LoadPresets();
    return true;
}

void LogPanel::Destroy() {
    for (auto& s : sessions_) if (s) s->Destroy();
    sessions_.clear();
    if (hwnd_) { ::KillTimer(hwnd_, kTimerId); ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
}

bool LogPanel::HasFocus() const {
    LogSession* a = Active();
    return a && a->HasFocus();
}

void LogPanel::Hide() {
    if (hwnd_) {
        for (auto& s : sessions_) if (s) s->follow_ = false;
        ::KillTimer(hwnd_, kTimerId);
        ::ShowWindow(hwnd_, SW_HIDE);
    }
}

LogSession* LogPanel::Active() const {
    if (active_ < 0 || active_ >= (int)sessions_.size()) return nullptr;
    return sessions_[active_].get();
}

void LogPanel::Layout(int w, int h) {
    if (!hwnd_) return;
    int dpi = ::GetDpiForWindow(hwnd_);
    int u = MulDiv(1, dpi, 96);
    int pad = 6 * u;
    int splitH = 6 * u;
    int rowH = 24 * u;
    int gap = 4 * u;
    int tabH = 24 * u;
    int tabY = splitH + 2 * u;
    int rowAY = tabY + tabH + 3 * u;
    int rowBY = rowAY + rowH + gap;
    int labelY = rowBY + rowH + 3 * u;
    int labelH = 16 * u;
    int sciY = labelY + labelH + 4 * u;

    int addW = 28 * u;
    // tab strip fills width minus the ＋ button
    ::MoveWindow(tabs_, pad, tabY, std::max(0, w - 2 * pad - addW - gap), tabH, TRUE);
    ::MoveWindow(addBtn_, w - pad - addW, tabY, addW, tabH, TRUE);

    int btnW = 84 * u, chkW = 56 * u, hlW = 80 * u, exW = 52 * u, rxW = 52 * u, applyW = 72 * u, clrW = 70 * u;
    int closeW = 76 * u;
    int fixedRight = closeW + gap + clrW + gap + applyW + gap + rxW + gap + exW + gap;
    int fixedLeft = btnW + gap + chkW + gap + hlW + gap;
    int filterW = std::max(80 * u, w - 2 * pad - fixedLeft - fixedRight);

    int x = pad;
    ::MoveWindow(openBtn_,   x, rowAY, btnW, rowH, TRUE);   x += btnW + gap;
    ::MoveWindow(followChk_, x, rowAY, chkW, rowH, TRUE);   x += chkW + gap;
    ::MoveWindow(hlChk_,     x, rowAY, hlW,  rowH, TRUE);   x += hlW + gap;
    ::MoveWindow(filterEdit_,x, rowAY, filterW, rowH, TRUE); x += filterW + gap;
    ::MoveWindow(excludeChk_,x, rowAY, exW,  rowH, TRUE);   x += exW + gap;
    ::MoveWindow(regexChk_,  x, rowAY, rxW,  rowH, TRUE);   x += rxW + gap;
    ::MoveWindow(applyBtn_,  x, rowAY, applyW, rowH, TRUE); x += applyW + gap;
    ::MoveWindow(clearBtn_,  x, rowAY, clrW,  rowH, TRUE);  x += clrW + gap;
    ::MoveWindow(closeBtn_,  x, rowAY, closeW, rowH, TRUE);

    int plW = 40 * u, pcW = 150 * u, psW = 34 * u, pdW = 34 * u;
    int gtW = 130 * u, gbW = 62 * u;
    x = pad;
    ::MoveWindow(presetLbl_,  x, rowBY, plW, rowH, TRUE);   x += plW + gap;
    ::MoveWindow(presetCombo_,x, rowBY, pcW, rowH, TRUE);   x += pcW + gap;
    ::MoveWindow(presetSave_, x, rowBY, psW, rowH, TRUE);   x += psW + gap;
    ::MoveWindow(presetDel_,  x, rowBY, pdW, rowH, TRUE);   x += pdW + gap * 2;
    ::MoveWindow(gotoEdit_,   x, rowBY, gtW, rowH, TRUE);   x += gtW + gap;
    ::MoveWindow(gotoBtn_,    x, rowBY, gbW, rowH, TRUE);

    ::MoveWindow(label_, pad, labelY, std::max(0, w - 2 * pad), labelH, TRUE);

    int sciW = std::max(0, w - 2 * pad);
    int sciH = std::max(0, h - sciY);
    for (int i = 0; i < (int)sessions_.size(); ++i) {
        HWND s = sessions_[i]->Sci();
        // keep EVERY scintilla synced to the same rect so tab-switch/resize
        // never leaves a hidden one at a stale position
        ::MoveWindow(s, pad, sciY, sciW, sciH, TRUE);
        ::ShowWindow(s, i == active_ ? SW_SHOW : SW_HIDE);
    }
}

int LogPanel::AddTab(const std::wstring& title, bool select) {
    auto s = std::make_unique<LogSession>();
    if (!s->Create(hwnd_, inst_, font_)) return -1;
    s->SetTheme(colors_.bg, colors_.fg, colors_.hl);
    TCITEMW tci{};
    tci.mask = TCIF_TEXT;
    std::wstring t = title.empty() ? Tr(L"panel.log") : title;
    tci.pszText = (LPWSTR)t.c_str();
    int idx = (int)::SendMessageW(tabs_, TCM_INSERTITEM, (WPARAM)sessions_.size(), (LPARAM)&tci);
    sessions_.push_back(std::move(s));
    if (select) ActivateTab((int)sessions_.size() - 1);
    return idx;
}

void LogPanel::ActivateTab(int index) {
    if (index < 0 || index >= (int)sessions_.size()) return;
    active_ = index;
    ::SendMessageW(tabs_, TCM_SETCURSEL, (WPARAM)index, 0);
    LogSession* s = sessions_[index].get();
    if (s) {
        ::ShowWindow(s->Sci(), SW_SHOW);
        ::SetFocus(s->Sci());
    }
    // hide the other scis
    for (int i = 0; i < (int)sessions_.size(); ++i)
        if (i != index && sessions_[i]->Sci())
            ::ShowWindow(sessions_[i]->Sci(), SW_HIDE);
    ::InvalidateRect(tabs_, nullptr, FALSE);
    SyncToolbar();
    UpdateLabel();
}

void LogPanel::CloseTab(int index) {
    if (index < 0 || index >= (int)sessions_.size()) return;
    bool closedActive = (index == active_);
    sessions_[index]->Destroy();
    sessions_.erase(sessions_.begin() + index);
    ::SendMessageW(tabs_, TCM_DELETEITEM, (WPARAM)index, 0);
    if (hoverCloseIndex_ >= index) hoverCloseIndex_ = -1;
    int n = (int)sessions_.size();
    if (n == 0) {
        active_ = -1;
    } else {
        if (active_ >= n) active_ = n - 1;
        if (closedActive) active_ = std::min(active_, n - 1);
        ::SendMessageW(tabs_, TCM_SETCURSEL, (WPARAM)active_, 0);
    }
    for (int i = 0; i < n; ++i)
        ::ShowWindow(sessions_[i]->Sci(), i == active_ ? SW_SHOW : SW_HIDE);
    ::InvalidateRect(tabs_, nullptr, FALSE);
    SyncToolbar();
    UpdateLabel();
}

// copy the active session's filter/follow state into the shared toolbar
void LogPanel::SyncToolbar() {
    LogSession* s = Active();
    if (!s) return;
    ::SetWindowTextW(filterEdit_, s->filter_.c_str());
    ::CheckDlgButton(hwnd_, ID_FOLLOW, s->follow_ ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(hwnd_, ID_HL, s->highlight_ ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(hwnd_, ID_EXCLUDE, s->exclude_ ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(hwnd_, ID_REGEX, s->regexOn_ ? BST_CHECKED : BST_UNCHECKED);
}

// read the shared toolbar flags into the active session (before applying)
static void SyncSelFlags(LogSession* s, LogPanel* p) {
    if (!s) return;
    s->exclude_ = ::IsDlgButtonChecked(p->Hwnd(), ID_EXCLUDE) == BST_CHECKED;
    s->regexOn_ = ::IsDlgButtonChecked(p->Hwnd(), ID_REGEX) == BST_CHECKED;
    s->highlight_ = ::IsDlgButtonChecked(p->Hwnd(), ID_HL) == BST_CHECKED;
}

void LogPanel::Retranslate() {
    if (openBtn_)    ::SetWindowTextW(openBtn_, Tr(L"panel.log.open"));
    if (followChk_)  ::SetWindowTextW(followChk_, Tr(L"log.follow"));
    if (hlChk_)      ::SetWindowTextW(hlChk_, Tr(L"log.hl"));
    if (excludeChk_) ::SetWindowTextW(excludeChk_, Tr(L"log.exclude"));
    if (regexChk_)   ::SetWindowTextW(regexChk_, Tr(L"log.regex"));
    if (applyBtn_)   ::SetWindowTextW(applyBtn_, Tr(L"log.apply"));
    if (clearBtn_)   ::SetWindowTextW(clearBtn_, Tr(L"log.clear"));
    if (gotoBtn_)    ::SetWindowTextW(gotoBtn_, Tr(L"log.goto"));
    if (presetLbl_)  ::SetWindowTextW(presetLbl_, Tr(L"log.preset"));
    if (presetSave_) ::SetWindowTextW(presetSave_, Tr(L"log.savepreset"));
    if (presetDel_)  ::SetWindowTextW(presetDel_, Tr(L"log.delpreset"));
    if (closeBtn_)   ::SetWindowTextW(closeBtn_, Tr(L"panel.log.close"));
    UpdateLabel();
    // 已存在标签页标题不含可翻译文本（沿用文件名），无需重建
}

void LogPanel::UpdateLabel() {
    if (!label_) return;
    LogSession* s = Active();
    if (!s) {
        ::SetWindowTextW(label_, Tr(L"panel.log.notopen"));
        return;
    }
    std::wstring fshow = s->filterOn_ ? s->filter_ : L"";
    int visible = 0;
    if (s->Sci()) {
        int lines = (int)SendMessageW(s->Sci(), SCI_GETLINECOUNT, 0, 0);
        for (int l = 0; l < lines; ++l)
            if (SendMessageW(s->Sci(), SCI_GETLINEVISIBLE, l, 0)) ++visible;
    }
    // 沿用既有格式；过滤/跟随片段为条件拼接（片段本身经字典翻译）
    wchar_t buf[768];
    swprintf_s(buf, Tr(L"log.status.fmt"),
               s->path_.empty() ? Tr(L"panel.log.nofile") : s->path_.c_str(),
               s->fileSize_,
               s->truncated_ ? Tr(L"log.status.trunc") : L"",
               visible,
               s->filterOn_ && !fshow.empty() ? Tr(L"log.status.filter") : L"",
               s->filterOn_ && !fshow.empty() ? fshow.c_str() : L"",
               s->follow_ ? Tr(L"log.status.follow") : L"");
    ::SetWindowTextW(label_, buf);
}

bool LogPanel::LoadFile(const std::wstring& path, bool preserveState) {
    // if already open, just switch to that tab (its live tail keeps flowing)
    for (int i = 0; i < (int)sessions_.size(); ++i) {
        if (sessions_[i]->path_ == path) {
            ActivateTab(i);
            ::ShowWindow(hwnd_, SW_SHOW);
            return true;
        }
    }
    int added = AddTab(FileBase(path), true);
    if (added < 0) return false;
    LogSession* s = Active();
    if (!s || !s->LoadFile(path, preserveState)) {
        CloseTab((int)sessions_.size() - 1);
        return false;
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    // a scintilla may have been created after the last layout (e.g. --log
    // opens the panel before the session exists) — redo the geometry now
    RECT cr; ::GetClientRect(hwnd_, &cr);
    Layout(cr.right, cr.bottom);
    // ensure a timer if anything is being followed (default follow=on)
    ::SetTimer(hwnd_, kTimerId, 500, nullptr);
    UpdateLabel();
    return true;
}

void LogPanel::PollTick() {
    bool anyFollow = false;
    for (auto& s : sessions_) {
        if (s && s->follow_) {
            s->PollFile();
            anyFollow = true;
        }
    }
    UpdateLabel();
    if (!anyFollow) ::KillTimer(hwnd_, kTimerId);
}

void LogPanel::OpenFileDlg() {
    wchar_t fileBuf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    // OPENFILENAME 过滤器以 NUL 分段、双 NUL 结尾；用显式 NUL 字符拼接
    const std::wstring NUL(1, L'\0');
    std::wstring ft = Tr(L"log.filter.filetypes") + NUL +
                      L"*.log;*.txt;*.csv" + NUL +
                      Tr(L"log.filter.all") + NUL +
                      L"*.*" + NUL + NUL;
    ofn.lpstrFilter = ft.c_str();
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) LoadFile(fileBuf);
}

// app-config flags from the toolbar into the active session (before applying)
static void SyncSelFlags(LogPanel* /*p*/, LogSession* s, LogPanel* p) {
    if (!s) return;
    s->exclude_ = ::IsDlgButtonChecked(p->Hwnd(), ID_EXCLUDE) == BST_CHECKED;
    s->regexOn_ = ::IsDlgButtonChecked(p->Hwnd(), ID_REGEX) == BST_CHECKED;
    s->highlight_ = ::IsDlgButtonChecked(p->Hwnd(), ID_HL) == BST_CHECKED;
}

// ---- presets (container-level) ----------------------------------------------

void LogPanel::LoadPresets() {
    if (!presetCombo_) return;
    ::SendMessageW(presetCombo_, CB_RESETCONTENT, 0, 0);
    for (auto& p : LoadPresetFile())
        ::SendMessageW(presetCombo_, CB_ADDSTRING, 0, (LPARAM)p.name.c_str());
}

void LogPanel::ApplyPreset(int index) {
    if (index < 0) return;
    auto presets = LoadPresetFile();
    if (index >= (int)presets.size()) return;
    FilterPreset& p = presets[index];
    LogSession* s = Active();
    if (!s) return;
    s->filter_ = p.filter;
    s->filterOn_ = !p.filter.empty();
    s->exclude_ = p.exclude;
    s->regexOn_ = p.regex;
    s->highlight_ = p.highlight;
    ::SetWindowTextW(filterEdit_, p.filter.c_str());
    ::CheckDlgButton(hwnd_, ID_EXCLUDE, p.exclude ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(hwnd_, ID_REGEX, p.regex ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(hwnd_, ID_HL, p.highlight ? BST_CHECKED : BST_UNCHECKED);
    s->ApplyFilter();
}

void LogPanel::SavePreset() {
    wchar_t nb[256]{};
    SendMessageW(presetCombo_, WM_GETTEXT, 256, (LPARAM)nb);
    std::wstring name = nb;
    LogSession* s = Active();
    if (name.empty()) name = s ? s->filter_ : L"";
    if (name.empty() || !s) return;

    FilterPreset p;
    p.name = name;
    p.filter = s->filter_;
    p.exclude = s->exclude_;
    p.regex = s->regexOn_;
    p.highlight = ::IsDlgButtonChecked(hwnd_, ID_HL) == BST_CHECKED;

    auto presets = LoadPresetFile();
    int found = -1;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].name == name) { found = (int)i; break; }
    if (found >= 0) presets[found] = p;
    else presets.push_back(p);
    SavePresetFile(presets);
    LoadPresets();
    ::SendMessageW(presetCombo_, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)name.c_str());
}

void LogPanel::DeletePreset() {
    int sel = (int)::SendMessageW(presetCombo_, CB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    wchar_t nb[256]{};
    ::SendMessageW(presetCombo_, CB_GETLBTEXT, sel, (LPARAM)nb);
    std::wstring name = nb;
    auto presets = LoadPresetFile();
    for (size_t i = 0; i < presets.size(); ++i) {
        if (presets[i].name == name) { presets.erase(presets.begin() + i); break; }
    }
    SavePresetFile(presets);
    LoadPresets();
}

// ---- tab strip subclass (owner-drawn close buttons, middle-click close) -------

RECT LogPanel::TabCloseRect(const RECT& item) const {
    int dpi = ::GetDpiForWindow(tabs_);
    int size = MulDiv(16, dpi, 96);
    int h = item.bottom - item.top;
    if (size > h - MulDiv(4, dpi, 96)) size = h - MulDiv(4, dpi, 96);
    if (size < 8) size = 8;
    int padV = (h - size) / 2;
    RECT c = item;
    c.right -= MulDiv(4, dpi, 96);
    c.left = c.right - size;
    c.top += padV;
    c.bottom = c.top + size;
    return c;
}

int LogPanel::TabHitTest(POINT pt, bool* inClose) {
    *inClose = false;
    TCHITTESTINFO ht{};
    ht.pt = pt;
    ht.flags = TCHT_ONITEM | TCHT_ONITEMICON | TCHT_ONITEMLABEL;
    int idx = (int)::SendMessageW(tabs_, TCM_HITTEST, 0, (LPARAM)&ht);
    if (idx < 0 || !(ht.flags & (TCHT_ONITEMICON | TCHT_ONITEMLABEL))) return idx;
    RECT rc{};
    if (!::SendMessageW(tabs_, TCM_GETITEMRECT, idx, (LPARAM)&rc)) return idx;
    RECT cr = TabCloseRect(rc);
    *inClose = PtInRect(&cr, pt) != FALSE;
    return idx;
}

void LogPanel::DrawTabItem(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    int idx = (int)dis->itemID;
    RECT r = dis->rcItem;
    HDC dc = dis->hDC;

    bool selected = (idx == active_);
    // background
    HBRUSH bg = ::CreateSolidBrush(selected
        ? RGB(0xFF, 0xFF, 0xFF) : ::GetSysColor(COLOR_BTNFACE));
    ::FillRect(dc, &r, bg);
    ::DeleteObject(bg);
    // thin edge
    HPEN pen = ::CreatePen(PS_SOLID, 1,
        selected ? RGB(0xCC, 0xCC, 0xCC) : ::GetSysColor(COLOR_BTNSHADOW));
    HPEN oldPen = (HPEN)::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, r.left, r.top, r.right, r.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    // label
    RECT tr = r;
    RECT cr = TabCloseRect(r);
    int dpi = ::GetDpiForWindow(tabs_);
    tr.right = cr.left - MulDiv(0, dpi, 96);

    wchar_t title[512];
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = title;
    ti.cchTextMax = 512;
    ::FillMemory(title, sizeof(title), 0);
    ::SendMessageW(tabs_, TCM_GETITEMW, idx, (LPARAM)&ti);

    ::SelectObject(dc, font_);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, selected ? RGB(0x20, 0x20, 0x20)
                                : ::GetSysColor(COLOR_BTNTEXT));
    ::DrawTextW(dc, title, -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // close glyph
    bool hover = (hoverCloseIndex_ == idx);
    int x0 = cr.left, y0 = cr.top, sz = cr.right - cr.left;
    if (hover) {
        HBRUSH hb = ::CreateSolidBrush(RGB(0xE8, 0xE8, 0xE8));
        ::FillRect(dc, &cr, hb);
        ::DeleteObject(hb);
        ::SetBkColor(dc, RGB(0xE8, 0xE8, 0xE8));
    }
    HPEN xp = ::CreatePen(PS_SOLID, MulDiv(2, dpi, 96),
                          hover ? RGB(0x00, 0x00, 0x00) : RGB(0x70, 0x70, 0x70));
    HGDIOBJ xpOld = ::SelectObject(dc, xp);
    int padc = MulDiv(4, dpi, 96);
    ::MoveToEx(dc, x0 + padc, y0 + padc, nullptr);
    ::LineTo(dc, x0 + sz - padc, y0 + sz - padc);
    ::MoveToEx(dc, x0 + sz - padc, y0 + padc, nullptr);
    ::LineTo(dc, x0 + padc, y0 + sz - padc);
    ::SelectObject(dc, xpOld);
    ::DeleteObject(xp);
}

void LogPanel::RedrawTab(int index) {
    RECT rc{};
    if (::SendMessageW(tabs_, TCM_GETITEMRECT, index, (LPARAM)&rc))
        ::InvalidateRect(tabs_, &rc, FALSE);
}

LRESULT LogPanel::TabProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool inClose = false;
            int idx = TabHitTest(pt, &inClose);
            int nh = (inClose && idx >= 0) ? idx : -1;
            if (nh != hoverCloseIndex_) {
                if (hoverCloseIndex_ >= 0) RedrawTab(hoverCloseIndex_);
                hoverCloseIndex_ = nh;
                if (hoverCloseIndex_ >= 0) RedrawTab(hoverCloseIndex_);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoverCloseIndex_ >= 0) {
                RedrawTab(hoverCloseIndex_);
                hoverCloseIndex_ = -1;
            }
            return 0;
        case WM_MBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool inClose = false;
            int idx = TabHitTest(pt, &inClose);
            if (idx >= 0) { CloseTab(idx); return 0; }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool inClose = false;
            int idx = TabHitTest(pt, &inClose);
            if (inClose && idx >= 0) { CloseTab(idx); return 0; }
            break;   // otherwise let the control handle selection
        }
    }
    return ::DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK LogPanel::TabProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR ref) {
    return ((LogPanel*)ref)->TabProc(h, m, wp, lp);
}

// ------------------------------------------------------------------ procs

LRESULT LogPanel::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerId) { PollTick(); return 0; }
            break;
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (nm && nm->hwndFrom == tabs_ && nm->code == TCN_SELCHANGE) {
                int sel = (int)::SendMessageW(tabs_, TCM_GETCURSEL, 0, 0);
                ActivateTab(sel);
                return 0;
            }
            break;
        }
        case WM_MBUTTONDOWN: {
            // middle-click on a session tab closes it (routes via subclass too)
            if (tabs_) {
                POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                TCHITTESTINFO hti{ pt };
                int idx = (int)::SendMessageW(tabs_, TCM_HITTEST, 0, (LPARAM)&hti);
                if (idx >= 0) { CloseTab(idx); return 0; }
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
            if (dis && dis->hwndItem == tabs_) { DrawTabItem(dis); return TRUE; }
            break;
        }
        case WM_COMMAND: {
            WORD id = LOWORD(wp), note = HIWORD(wp);
            LogSession* s = Active();
            if (id == ID_CLOSE && onClose) { onClose(); return 0; }
            if (id == ID_TAB_ADD && note == BN_CLICKED) { OpenFileDlg(); return 0; }
            if (id == ID_OPEN && note == BN_CLICKED) { OpenFileDlg(); return 0; }
            if (id == ID_APPLY && note == BN_CLICKED) { if (s) { SyncSelFlags(s, this); s->ApplyFilter(); } return 0; }
            if (id == ID_CLEAR && note == BN_CLICKED) {
                if (s) s->ClearFilter();
                ::SetWindowTextW(filterEdit_, L"");
                UpdateLabel();
                return 0;
            }
            if (id == ID_GOTO_EDIT && note == EN_CHANGE) { return 0; }
            if (id == ID_GOTO && note == BN_CLICKED) {
                if (s) {
                    wchar_t tbuf[256]{};
                    SendMessageW(gotoEdit_, WM_GETTEXT, 256, (LPARAM)tbuf);
                    s->GoToTimestamp(tbuf);
                }
                return 0;
            }
            if (id == ID_PRESET_COMBO && note == CBN_SELCHANGE) {
                ApplyPreset((int)::SendMessageW(presetCombo_, CB_GETCURSEL, 0, 0));
                return 0;
            }
            if (id == ID_PRESET_SAVE && note == BN_CLICKED) { SavePreset(); return 0; }
            if (id == ID_PRESET_DEL && note == BN_CLICKED) { DeletePreset(); return 0; }
            if (id == ID_FOLLOW && note == BN_CLICKED) {
                if (s) {
                    s->follow_ = ::IsDlgButtonChecked(h, ID_FOLLOW) == BST_CHECKED;
                    if (s->follow_) ::SetTimer(hwnd_, kTimerId, 500, nullptr);
                    else if (!std::any_of(sessions_.begin(), sessions_.end(),
                             [](auto& x){ return x && x->follow_; }))
                        ::KillTimer(hwnd_, kTimerId);
                    UpdateLabel();
                }
                return 0;
            }
            if (id == ID_HL && note == BN_CLICKED) {
                if (s) {
                    s->highlight_ = ::IsDlgButtonChecked(h, ID_HL) == BST_CHECKED;
                    if (s->highlight_ && !s->filter_.empty()) s->ApplyFilter();
                    else {
                        ::SendMessageW(s->Sci(), SCI_SETREADONLY, 0, 0);
                        ::SendMessageW(s->Sci(), SCI_MARKERDELETEALL, LogSession::MARK_LOG_HL, 0);
                        ::SendMessageW(s->Sci(), SCI_SETREADONLY, 1, 0);
                    }
                }
                return 0;
            }
            if (id == ID_EXCLUDE && note == BN_CLICKED) {
                if (s) { s->exclude_ = ::IsDlgButtonChecked(h, ID_EXCLUDE) == BST_CHECKED; if (s->filterOn_) s->ApplyFilter(); }
                return 0;
            }
            if (id == ID_REGEX && note == BN_CLICKED) {
                if (s) { s->regexOn_ = ::IsDlgButtonChecked(h, ID_REGEX) == BST_CHECKED; if (s->filterOn_) s->ApplyFilter(); }
                return 0;
            }
            if (id == ID_FILTER && note == EN_CHANGE) {
                if (s) {
                    wchar_t fbuf[256]{};
                    SendMessageW(filterEdit_, WM_GETTEXT, 256, (LPARAM)fbuf);
                    s->filter_ = fbuf;
                    s->filterOn_ = !s->filter_.empty();
                    static DWORD last = 0;
                    DWORD now = ::GetTickCount();
                    if (now - last > 250) { last = now; s->ApplyFilter(); UpdateLabel(); }
                }
                return 0;
            }
            break;
        }
        case WM_SETCURSOR: {
            POINT pt; ::GetCursorPos(&pt);
            ::ScreenToClient(h, &pt);
            if (pt.y < 6) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.y < 6) {
                resizing_ = true;
                splitHot_ = true;
                POINT sp; ::GetCursorPos(&sp);
                dragStartScreenY_ = sp.y;
                RECT rc; ::GetWindowRect(h, &rc);
                dragStartH_ = rc.bottom - rc.top;
                ::SetCapture(h);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (resizing_) {
                POINT sp; ::GetCursorPos(&sp);
                int newH = dragStartH_ + (dragStartScreenY_ - sp.y);
                newH = std::max(90, std::min(1400, newH));
                if (onHeightChange) onHeightChange(newH);
                return 0;
            }
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool hot = pt.y < 6;
            if (hot) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
            }
            if (hot != splitHot_) {
                splitHot_ = hot;
                RECT rc; ::GetClientRect(h, &rc);
                rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            if (splitHot_) {
                splitHot_ = false;
                RECT rc; ::GetClientRect(h, &rc);
                rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (resizing_) { resizing_ = false; ::ReleaseCapture(); return 0; }
            break;
        }
        case WM_CAPTURECHANGED:
            resizing_ = false;
            break;
        case WM_PAINT: {
            ::DefWindowProcW(h, msg, wp, lp);
            HDC dc = ::GetDC(h);
            RECT rc; ::GetClientRect(h, &rc);
            RECT band{0, 0, rc.right, 5};
            HBRUSH b = ::CreateSolidBrush(splitHot_ || resizing_
                ? RGB(0xCC, 0xE3, 0xFF) : RGB(0xE9, 0xE9, 0xE9));
            ::FillRect(dc, &band, b);
            ::DeleteObject(b);
            HPEN pen = ::CreatePen(PS_SOLID, 1, splitHot_ || resizing_
                ? RGB(0x00, 0x78, 0xD4) : RGB(0xAC, 0xAC, 0xAC));
            HPEN old = (HPEN)::SelectObject(dc, pen);
            ::MoveToEx(dc, 0, 5, nullptr);
            ::LineTo(dc, rc.right, 5);
            ::SelectObject(dc, old);
            ::DeleteObject(pen);
            ::ReleaseDC(h, dc);
            return 0;
        }
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK LogPanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (LogPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->WndProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

void LogPanel::ApplyTheme(const ThemeDef& t) {
    colors_.bg = t.editorBg;
    colors_.fg = t.editorFg;
    colors_.hl = RGB(0xFF, 0xF1, 0x9C);
    for (auto& s : sessions_) if (s) s->SetTheme(colors_.bg, colors_.fg, colors_.hl);
}

} // namespace xfs
