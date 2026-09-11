// JsonLite.cpp — 极简 JSON 解析实现（详见 JsonLite.h）。
#include "JsonLite.h"
#include <cstdlib>

namespace xfs {
namespace json {

namespace {

struct Parser {
    const wchar_t* p = nullptr;
    const wchar_t* end = nullptr;

    void SkipWs() {
        while (p < end && (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')) ++p;
    }
    bool ParseString(std::wstring* out) {
        SkipWs();
        if (p >= end || *p != L'"') return false;
        ++p;
        std::wstring s;
        while (p < end) {
            wchar_t ch = *p++;
            if (ch == L'"') { *out = std::move(s); return true; }
            if (ch == L'\\' && p < end) {
                wchar_t e = *p++;
                switch (e) {
                    case L'"': s += L'"'; break;
                    case L'\\': s += L'\\'; break;
                    case L'/': s += L'/'; break;
                    case L'n': s += L'\n'; break;
                    case L't': s += L'\t'; break;
                    case L'r': s += L'\r'; break;
                    default: s += e; break;   // \uXXXX 暂不需要
                }
            } else s += ch;
        }
        return false;
    }
    bool ParseValue(Value* out) {
        SkipWs();
        if (p >= end) return false;
        wchar_t ch = *p;
        if (ch == L'{') {
            out->kind = Value::Obj; ++p;
            SkipWs();
            if (p < end && *p == L'}') { ++p; return true; }
            for (;;) {
                std::wstring key;
                if (!ParseString(&key)) return false;
                SkipWs();
                if (p >= end || *p != L':') return false;
                ++p;
                Value v;
                if (!ParseValue(&v)) return false;
                out->obj[key] = std::move(v);
                SkipWs();
                if (p < end && *p == L',') { ++p; continue; }
                if (p < end && *p == L'}') { ++p; return true; }
                return false;
            }
        }
        if (ch == L'[') {
            out->kind = Value::Arr; ++p;
            SkipWs();
            if (p < end && *p == L']') { ++p; return true; }
            for (;;) {
                Value v;
                if (!ParseValue(&v)) return false;
                out->arr.push_back(std::move(v));
                SkipWs();
                if (p < end && *p == L',') { ++p; continue; }
                if (p < end && *p == L']') { ++p; return true; }
                return false;
            }
        }
        if (ch == L'"') { out->kind = Value::Str; return ParseString(&out->str); }
        if (wcsncmp(p, L"true", 4) == 0)  { out->kind = Value::Bool; out->b = true;  p += 4; return true; }
        if (wcsncmp(p, L"false", 5) == 0) { out->kind = Value::Bool; out->b = false; p += 5; return true; }
        if (wcsncmp(p, L"null", 4) == 0)  { out->kind = Value::Null; p += 4; return true; }
        out->kind = Value::Num;
        wchar_t* stop = nullptr;
        out->num = wcstod(p, &stop);
        if (stop == p) return false;
        p = stop;
        return true;
    }
};

} // namespace

bool Parse(const std::wstring& text, Value* root) {
    Parser ps{ text.data(), text.data() + text.size() };
    if (!ps.ParseValue(root)) return false;
    return root->kind == Value::Obj;
}

const Value* Find(const Value& obj, const wchar_t* key) {
    if (obj.kind != Value::Obj) return nullptr;
    auto it = obj.obj.find(key);
    return it == obj.obj.end() ? nullptr : &it->second;
}

COLORREF ParseColor(const std::wstring& s, COLORREF fallback) {
    if (s.size() >= 7 && s[0] == L'#') {
        wchar_t* stop = nullptr;
        long v = wcstol(s.c_str() + 1, &stop, 16);
        if (*stop == L'\0' && v >= 0 && v <= 0xFFFFFF)
            return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }
    return fallback;
}

std::wstring ColorToWstr(COLORREF c) {
    wchar_t buf[16];
    swprintf(buf, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

} // namespace json
} // namespace xfs
