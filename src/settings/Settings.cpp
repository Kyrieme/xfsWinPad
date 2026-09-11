#include "Settings.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <string>

namespace xfs {
namespace {

// ---- minimal flat-JSON writer ----------------------------------------------

std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\t': out += L"\\t";  break;
            default:    out += c; break;
        }
    }
    return out;
}

// ---- minimal flat-JSON reader (tolerant; unknown keys ignored) ---------------

struct Cursor {
    const wchar_t* p;
    const wchar_t* end;
};

void SkipWs(Cursor& c) {
    while (c.p < c.end && (*c.p == L' ' || *c.p == L'\t' || *c.p == L'\r' || *c.p == L'\n'))
        ++c.p;
}

bool ReadString(Cursor& c, std::wstring* out) {
    SkipWs(c);
    if (c.p >= c.end || *c.p != L'"') return false;
    ++c.p;
    std::wstring s;
    while (c.p < c.end) {
        wchar_t ch = *c.p++;
        if (ch == L'"') { *out = std::move(s); return true; }
        if (ch == L'\\' && c.p < c.end) {
            wchar_t e = *c.p++;
            switch (e) {
                case L'"': s += L'"'; break;
                case L'\\': s += L'\\'; break;
                case L'/': s += L'/'; break;
                case L'n': s += L'\n'; break;
                case L't': s += L'\t'; break;
                case L'r': s += L'\r'; break;
                default: s += e; break;   // \uXXXX not needed for our keys
            }
        } else {
            s += ch;
        }
    }
    return false;
}

bool ReadInt(Cursor& c, int* out) {
    SkipWs(c);
    bool neg = false;
    if (c.p < c.end && *c.p == L'-') { neg = true; ++c.p; }
    if (c.p >= c.end || *c.p < L'0' || *c.p > L'9') return false;
    long long v = 0;
    while (c.p < c.end && *c.p >= L'0' && *c.p <= L'9') {
        v = v * 10 + (*c.p - L'0');
        if (v > 100000000) v = 100000000; // clamp
        ++c.p;
    }
    *out = (int)(neg ? -v : v);
    return true;
}

bool ReadBool(Cursor& c, bool* out) {
    SkipWs(c);
    if (c.p + 4 <= c.end && wcsncmp(c.p, L"true", 4) == 0)  { c.p += 4; *out = true;  return true; }
    if (c.p + 5 <= c.end && wcsncmp(c.p, L"false", 5) == 0) { c.p += 5; *out = false; return true; }
    return false;
}

// Skips over any JSON value (string / number / literal / nested object/array).
bool SkipValue(Cursor& c) {
    SkipWs(c);
    if (c.p >= c.end) return false;
    wchar_t ch = *c.p;
    if (ch == L'"') { std::wstring s; return ReadString(c, &s); }
    if (ch == L'{' || ch == L'[') {
        const wchar_t open = ch;
        const wchar_t close = (ch == L'{') ? L'}' : L']';
        int depth = 0;
        bool inStr = false;
        while (c.p < c.end) {
            wchar_t x = *c.p++;
            if (inStr) {
                if (x == L'\\' && c.p < c.end) ++c.p;
                else if (x == L'"') inStr = false;
                continue;
            }
            if (x == L'"') inStr = true;
            else if (x == open) ++depth;
            else if (x == close) { if (--depth == 0) return true; }
        }
        return false;
    }
    // number / true / false / null
    while (c.p < c.end && *c.p != L',' && *c.p != L'}') ++c.p;
    return true;
}

struct KeyVal {
    const wchar_t* key;
    enum Kind { Str, Int, Bool } kind;
    void* target;
};

} // namespace

std::wstring SettingsFilePath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    ::CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    return base + L"\\xfsWinPad\\settings.json";
}

bool SettingsLoad(const std::wstring& path, AppSettings* out) {
    std::string rawUtf8;
    if (!ReadFileBytes(path, rawUtf8)) return false;
    std::wstring json = Utf8ToWide(rawUtf8);
    if (!json.empty() && json[0] == 0xFEFF)
        json.erase(0, 1);   // tolerate UTF-8 BOM written by external editors

    Cursor c{json.data(), json.data() + json.size()};
    SkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return false;
    ++c.p;

    KeyVal table[] = {
        {L"fontName", KeyVal::Str,  &out->fontName},
        {L"fontSize", KeyVal::Int,  &out->fontSize},
        {L"tabWidth", KeyVal::Int,  &out->tabWidth},
        {L"wrapOn",   KeyVal::Bool, &out->wrapOn},
        {L"caretWidth", KeyVal::Int, &out->caretWidth},
        {L"currentLineHighlight", KeyVal::Bool, &out->currentLineHighlight},
        {L"showLineNumber", KeyVal::Bool, &out->showLineNumber},
        {L"autoIndent", KeyVal::Bool, &out->autoIndent},
        {L"autoComplete", KeyVal::Bool, &out->autoComplete},
        {L"defaultEol", KeyVal::Int, &out->defaultEol},
        {L"autosaveEnabled", KeyVal::Bool, &out->autosaveEnabled},
        {L"autosaveSeconds", KeyVal::Int, &out->autosaveSeconds},
        {L"theme",    KeyVal::Str,  &out->theme},
        {L"autoCloseBrackets", KeyVal::Bool, &out->autoCloseBrackets},
        {L"termFontName", KeyVal::Str, &out->termFontName},
        {L"termFontSize", KeyVal::Int, &out->termFontSize},
        {L"hasWindow",KeyVal::Bool, &out->hasWindow},
        {L"winX",     KeyVal::Int,  &out->winX},
        {L"winY",     KeyVal::Int,  &out->winY},
        {L"winW",     KeyVal::Int,  &out->winW},
        {L"winH",     KeyVal::Int,  &out->winH},
        {L"winMax",   KeyVal::Bool, &out->winMax},
        {L"projectRoot", KeyVal::Str, &out->projectRoot},
        {L"explorerVisible", KeyVal::Bool, &out->explorerVisible},
        {L"hexPanelH", KeyVal::Int, &out->hexPanelH},
        {L"resultsPanelH", KeyVal::Int, &out->resultsPanelH},
        {L"logPanelH", KeyVal::Int, &out->logPanelH},
        {L"terminalPanelH", KeyVal::Int, &out->terminalPanelH},
        {L"aiPanelW", KeyVal::Int, &out->aiPanelW},
        {L"aiPanelVisible", KeyVal::Bool, &out->aiPanelVisible},
        {L"aiAttachContext", KeyVal::Bool, &out->aiAttachContext},
        {L"aiAutoApprove", KeyVal::Bool, &out->aiAutoApprove},
        {L"aiModel",  KeyVal::Str,  &out->aiModel},
        {L"aiBackend", KeyVal::Str, &out->aiBackend},
        {L"aiEndpoint", KeyVal::Str, &out->aiEndpoint},
        {L"aiApiKey", KeyVal::Str,  &out->aiApiKey},
        {L"aiLocalModel", KeyVal::Str, &out->aiLocalModel},
        {L"showStatusBar", KeyVal::Bool, &out->showStatusBar},
        {L"showTabBar", KeyVal::Bool, &out->showTabBar},
        {L"showToolbar", KeyVal::Bool, &out->showToolbar},
        {L"recentFilesMax", KeyVal::Int, &out->recentFilesMax},
        {L"braceMatch", KeyVal::Bool, &out->braceMatch},
        {L"indentGuides", KeyVal::Bool, &out->indentGuides},
        {L"showWhitespace", KeyVal::Bool, &out->showWhitespace},
        {L"searchMatchCase", KeyVal::Bool, &out->searchMatchCase},
        {L"searchWholeWord", KeyVal::Bool, &out->searchWholeWord},
        {L"fullPathTitle", KeyVal::Bool, &out->fullPathTitle},
        {L"autoDetectLang", KeyVal::Bool, &out->autoDetectLang},
        {L"uiLang",   KeyVal::Str,  &out->uiLang},
    };

    for (;;) {
        SkipWs(c);
        if (c.p >= c.end) return false;
        if (*c.p == L'}') return true;
        if (*c.p == L',') { ++c.p; continue; }

        std::wstring key;
        if (!ReadString(c, &key)) return false;
        SkipWs(c);
        if (c.p >= c.end || *c.p != L':') return false;
        ++c.p;

        KeyVal* hit = nullptr;
        for (auto& kv : table)
            if (key == kv.key) { hit = &kv; break; }

        if (!hit) {
            SkipValue(c);
            continue;
        }
        switch (hit->kind) {
            case KeyVal::Str:  ReadString(c, (std::wstring*)hit->target); break;
            case KeyVal::Bool: ReadBool(c, (bool*)hit->target);           break;
            case KeyVal::Int:
            default:           ReadInt(c, (int*)hit->target);             break;
        }
    }
}

bool SettingsSave(const std::wstring& path, const AppSettings& s) {
    std::wstring j = L"{\r\n";
    j += L"  \"fontName\": \"" + Escape(s.fontName) + L"\",\r\n";
    j += L"  \"theme\": \"" + Escape(s.theme) + L"\",\r\n";
    auto num = [](const wchar_t* k, long long v, bool last) {
        return std::wstring(L"  \"") + k + L"\": " + std::to_wstring(v) + (last ? L"\r\n" : L",\r\n");
    };
    j += num(L"fontSize", s.fontSize, false);
    j += num(L"tabWidth", s.tabWidth, false);
    j += std::wstring(L"  \"wrapOn\": ")   + (s.wrapOn    ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"caretWidth\": ") + (s.caretWidth ? std::to_wstring(s.caretWidth) : L"2") + L",\r\n";
    j += std::wstring(L"  \"currentLineHighlight\": ") + (s.currentLineHighlight ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"showLineNumber\": ") + (s.showLineNumber ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"autoIndent\": ") + (s.autoIndent ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"autoComplete\": ") + (s.autoComplete ? L"true" : L"false") + L",\r\n";
    j += num(L"defaultEol", s.defaultEol, false);
    j += std::wstring(L"  \"autosaveEnabled\": ") + (s.autosaveEnabled ? L"true" : L"false") + L",\r\n";
    j += num(L"autosaveSeconds", s.autosaveSeconds, false);
    j += std::wstring(L"  \"autoCloseBrackets\": ") + (s.autoCloseBrackets ? L"true" : L"false") + L",\r\n";
    j += L"  \"termFontName\": \"" + Escape(s.termFontName) + L"\",\r\n";
    j += num(L"termFontSize", s.termFontSize, false);
    j += std::wstring(L"  \"hasWindow\": ")+ (s.hasWindow  ? L"true" : L"false") + L",\r\n";
    j += num(L"winX", s.winX, false);
    j += num(L"winY", s.winY, false);
    j += num(L"winW", s.winW, false);
    j += num(L"winH", s.winH, false);
    j += std::wstring(L"  \"winMax\": ")   + (s.winMax ? L"true" : L"false") + L",\r\n";
    j += L"  \"projectRoot\": \"" + Escape(s.projectRoot) + L"\",\r\n";
    j += std::wstring(L"  \"explorerVisible\": ") + (s.explorerVisible ? L"true" : L"false") + L",\r\n";
    j += num(L"hexPanelH", s.hexPanelH, false);
    j += num(L"resultsPanelH", s.resultsPanelH, false);
    j += num(L"logPanelH", s.logPanelH, false);
    j += num(L"terminalPanelH", s.terminalPanelH, false);
    j += num(L"aiPanelW", s.aiPanelW, false);
    j += std::wstring(L"  \"aiPanelVisible\": ") + (s.aiPanelVisible ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"aiAttachContext\": ") + (s.aiAttachContext ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"aiAutoApprove\": ") + (s.aiAutoApprove ? L"true" : L"false") + L",\r\n";
    j += L"  \"aiModel\": \"" + Escape(s.aiModel) + L"\",\r\n";
    j += L"  \"aiBackend\": \"" + Escape(s.aiBackend) + L"\",\r\n";
    j += L"  \"aiEndpoint\": \"" + Escape(s.aiEndpoint) + L"\",\r\n";
    j += L"  \"aiApiKey\": \"" + Escape(s.aiApiKey) + L"\",\r\n";
    j += L"  \"aiLocalModel\": \"" + Escape(s.aiLocalModel) + L"\",\r\n";
    j += std::wstring(L"  \"showStatusBar\": ")  + (s.showStatusBar  ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"showTabBar\": ")     + (s.showTabBar     ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"showToolbar\": ")    + (s.showToolbar    ? L"true" : L"false") + L",\r\n";
    j += num(L"recentFilesMax", s.recentFilesMax, false);
    j += std::wstring(L"  \"braceMatch\": ")     + (s.braceMatch     ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"indentGuides\": ")   + (s.indentGuides   ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"showWhitespace\": ") + (s.showWhitespace ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"searchMatchCase\": ") + (s.searchMatchCase ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"searchWholeWord\": ") + (s.searchWholeWord ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"fullPathTitle\": ")  + (s.fullPathTitle  ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"  \"autoDetectLang\": ") + (s.autoDetectLang ? L"true" : L"false") + L",\r\n";
    j += L"  \"uiLang\": \"" + Escape(s.uiLang) + L"\"\r\n";
    j += L"}\r\n";

    std::string utf8 = WideToUtf8(j);
    return WriteFileBytes(path, utf8.data(), utf8.size());
}

} // namespace xfs
