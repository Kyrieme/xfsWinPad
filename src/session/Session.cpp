#include "Session.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

namespace xfs {

std::wstring SessionFilePath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    ::CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    return base + L"\\xfsWinPad\\session.json";
}

// --- flat JSON (same minimal format as Settings) ------------------------------

namespace {

struct Cursor { const wchar_t* p; const wchar_t* end; };

void SkipWs(Cursor& c) {
    while (c.p < c.end && (*c.p == L' ' || *c.p == L'\t' || *c.p == L'\r' || *c.p == L'\n'))
        ++c.p;
}

bool ReadString(Cursor& c, std::wstring* out) {
    SkipWs(c);
    if (c.p >= c.end || *c.p != L'"') return false;
    ++c.p;
    out->clear();
    while (c.p < c.end) {
        wchar_t ch = *c.p++;
        if (ch == L'"') return true;
        if (ch == L'\\' && c.p < c.end) {
            wchar_t e = *c.p++;
            switch (e) {
                case L'"': *out += L'"'; break;
                case L'\\': *out += L'\\'; break;
                case L'/': *out += L'/'; break;
                case L'n': *out += L'\n'; break;
                case L'r': *out += L'\r'; break;
                case L't': *out += L'\t'; break;
                case L'b': *out += L'\b'; break;
                case L'f': *out += L'\f'; break;
                case L'u': {
                    // \uXXXX (exact four hex digits, no surrogate pairing:
                    // session text comes from our own Escape())
                    unsigned v = 0;
                    for (int k = 0; k < 4 && c.p < c.end; ++k) {
                        wchar_t h = *c.p++;
                        v <<= 4;
                        if (h >= L'0' && h <= L'9') v |= (unsigned)(h - L'0');
                        else if (h >= L'a' && h <= L'f') v |= (unsigned)(h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') v |= (unsigned)(h - L'A' + 10);
                    }
                    *out += (wchar_t)v;
                    break;
                }
                default: *out += e; break;
            }
        } else {
            *out += ch;
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
        if (v > 100000000) v = 100000000;
        ++c.p;
    }
    *out = (int)(neg ? -v : v);
    return true;
}

void SkipValue(Cursor& c) {
    SkipWs(c);
    if (c.p >= c.end) return;
    wchar_t ch = *c.p;
    if (ch == L'"') { std::wstring s; ReadString(c, &s); return; }
    if (ch == L'{' || ch == L'[') {
        wchar_t open = ch, close = (ch == L'{') ? L'}' : L']';
        int depth = 0; bool inStr = false;
        while (c.p < c.end) {
            wchar_t x = *c.p++;
            if (inStr) { if (x == L'\\' && c.p < c.end) ++c.p; else if (x == L'"') inStr = false; continue; }
            if (x == L'"') inStr = true;
            else if (x == open) ++depth;
            else if (x == close) { if (--depth == 0) return; }
        }
        return;
    }
    while (c.p < c.end && *c.p != L',' && *c.p != L'}') ++c.p;
}

std::wstring Escape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            case L'\b': out += L"\\b"; break;
            case L'\f': out += L"\\f"; break;
            default:
                if (c < 0x20) {
                    // \uXXXX for remaining control chars
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\u%04X", (unsigned)c);
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

} // namespace

bool SessionSave(const std::wstring& path, const SessionState& s) {
    std::wstring j = L"{\r\n  \"active\": " + std::to_wstring(s.activeIndex) +
                     L",\r\n  \"activeView\": " +
                     L"\"" + (s.activeView == 1 ? L"right" : L"left") + L"\",\r\n" +
                     L"  \"active1\": " + std::to_wstring(s.activeIndex1) +
                     L",\r\n  \"entries\": [\r\n";
    for (size_t i = 0; i < s.entries.size(); ++i) {
        const auto& e = s.entries[i];
        j += L"    { \"path\": \"" + Escape(e.path) +
             L"\", \"line\": " + std::to_wstring(e.line) +
             L", \"col\": " + std::to_wstring(e.col);
        if (e.locked) j += L", \"locked\": 1";
        if (e.lang >= 0) j += L", \"lang\": " + std::to_wstring(e.lang);
        if (!e.name.empty() || !e.text.empty()) {
            // untitled snapshot: keep the tab label and the full text
            j += L", \"name\": \"" + Escape(e.name) +
                 L"\", \"text\": \"" + Escape(e.text) + L"\"";
        }
        j += L" }";
        if (i + 1 < s.entries.size()) j += L",";
        j += L"\r\n";
    }
    j += L"  ],\r\n  \"entries1\": [\r\n";
    for (size_t i = 0; i < s.entries1.size(); ++i) {
        const auto& e = s.entries1[i];
        j += L"    { \"path\": \"" + Escape(e.path) +
             L"\", \"line\": " + std::to_wstring(e.line) +
             L", \"col\": " + std::to_wstring(e.col);
        if (e.locked) j += L", \"locked\": 1";
        if (e.lang >= 0) j += L", \"lang\": " + std::to_wstring(e.lang);
        if (!e.name.empty() || !e.text.empty()) {
            j += L", \"name\": \"" + Escape(e.name) +
                 L"\", \"text\": \"" + Escape(e.text) + L"\"";
        }
        j += L" }";
        if (i + 1 < s.entries1.size()) j += L",";
        j += L"\r\n";
    }
    j += L"  ]\r\n}\r\n";

    std::string utf8 = WideToUtf8(j);
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    ::CloseHandle(h);
    return written == utf8.size();
}

bool SessionLoad(const std::wstring& path, SessionState* out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    std::string raw((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    ::ReadFile(h, raw.data(), (DWORD)raw.size(), &got, nullptr);
    ::CloseHandle(h);
    raw.resize(got);

    // skip BOM
    size_t off = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        off = 3;

    std::wstring json = Utf8ToWide(raw.substr(off));
    Cursor c{json.data(), json.data() + json.size()};
    SkipWs(c);
    if (c.p >= c.end || *c.p != L'{') return false;
    ++c.p;

    // Parse an "entries"-style array into `outVec`. Shared by the
    // left ("entries") and right ("entries1") arrays.
    auto parseEntries = [&](std::vector<SessionEntry>* outVec) -> bool {
        SkipWs(c);
        if (c.p >= c.end || *c.p != L'[') { SkipValue(c); return false; }
        ++c.p;
        for (;;) {
            SkipWs(c);
            if (c.p >= c.end || *c.p == L']') break;
            if (*c.p == L',') { ++c.p; continue; }
            if (*c.p != L'{') { SkipValue(c); continue; }
            ++c.p;
            SessionEntry e;
            for (;;) {
                SkipWs(c);
                if (c.p >= c.end || *c.p == L'}') break;
                if (*c.p == L',') { ++c.p; continue; }
                std::wstring ek;
                if (!ReadString(c, &ek)) break;
                SkipWs(c);
                if (c.p >= c.end || *c.p != L':') break;
                ++c.p; SkipWs(c);
                if (ek == L"path")      ReadString(c, &e.path);
                else if (ek == L"line") ReadInt(c, &e.line);
                else if (ek == L"col")  ReadInt(c, &e.col);
                else if (ek == L"locked") {
                    int v = 0;
                    ReadInt(c, &v);
                    e.locked = (v != 0);
                }
                else if (ek == L"name") ReadString(c, &e.name);
                else if (ek == L"text") ReadString(c, &e.text);
                else if (ek == L"lang") ReadInt(c, &e.lang);
                else SkipValue(c);
            }
            if (!e.path.empty() || !e.text.empty()) outVec->push_back(std::move(e));
            SkipWs(c);
            if (c.p < c.end && *c.p == L'}') ++c.p;
        }
        SkipWs(c);
        if (c.p < c.end && *c.p == L']') ++c.p;   // consume closing bracket
        return true;
    };

    for (;;) {
        SkipWs(c);
        if (c.p >= c.end || *c.p == L'}') break;
        if (*c.p == L',') { ++c.p; continue; }

        std::wstring key;
        SkipWs(c);
        if (!ReadString(c, &key)) break;
        SkipWs(c);
        if (c.p >= c.end || *c.p != L':') break;
        ++c.p; SkipWs(c);

        if (key == L"active") {
            ReadInt(c, &out->activeIndex);
        } else if (key == L"active1") {
            ReadInt(c, &out->activeIndex1);
        } else if (key == L"activeView") {
            std::wstring v;
            if (ReadString(c, &v))
                out->activeView = (v == L"right") ? 1 : 0;
        } else if (key == L"entries") {
            parseEntries(&out->entries);
        } else if (key == L"entries1") {
            parseEntries(&out->entries1);
        } else {
            SkipValue(c);
        }
    }
    return !out->entries.empty() || !out->entries1.empty();
}

} // namespace xfs
