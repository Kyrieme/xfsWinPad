// I18n.cpp - implementation of the logical-id string table.
#include "I18n.h"
#include "JsonLite.h"
#include "Log.h"
#include "Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>

namespace xfs {

I18n& I18n::Instance() {
    static I18n inst;
    return inst;
}

namespace {

// Directory holding the exe, e.g. "...\xfsWinPad\". Used to locate lang\*.json
// which CMake copies next to the binary (like Scintilla.dll / Lexilla.dll).
std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    const size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"" : p.substr(0, slash + 1);
}

// Parses one language file into `out`. Returns false on read/parse failure.
bool LoadDict(const std::wstring& dir, const std::wstring& code,
              std::map<std::wstring, std::wstring>* out) {
    const std::wstring path = dir + L"lang\\" + code + L".json";
    std::string raw;
    if (!ReadFileBytes(path, raw)) return false;
    std::wstring j = Utf8ToWide(raw);
    if (!j.empty() && j[0] == 0xFEFF) j.erase(0, 1);

    json::Value root;
    if (!json::Parse(j, &root)) return false;

    out->clear();
    for (const auto& [k, v] : root.obj)
        if (v.kind == json::Value::Str) (*out)[k] = v.str;
    return true;
}

} // namespace

void I18n::SetLangDir(const std::wstring& dir) {
    langDir_ = dir;
}

bool I18n::Load(const std::wstring& code) {
    const std::wstring dir = langDir_.empty() ? ExeDir() : langDir_;
    std::map<std::wstring, std::wstring> fresh;
    if (!LoadDict(dir, code, &fresh)) {
        Logger::Warn("I18n::Load failed for " + WideToUtf8(code));
        return false;
    }

    // Also load the fallback dictionaries (en and zh-CN) if present.
    std::map<std::wstring, std::map<std::wstring, std::wstring>> dicts;
    dicts[code] = std::move(fresh);
    for (const wchar_t* other : {L"en", L"zh-CN"}) {
        std::wstring oc = other;
        if (oc == code) continue;
        std::map<std::wstring, std::wstring> d;
        if (LoadDict(dir, oc, &d)) dicts[oc] = std::move(d);
    }

    dicts_ = std::move(dicts);
    code_ = code;
    for (auto& cb : cbs_) cb(code_);
    return true;
}

const wchar_t* I18n::Tr(const wchar_t* id) const {
    // active language first, then "en", then "zh-CN"
    static const wchar_t* order[] = {L"en", L"zh-CN"};
    const auto it = dicts_.find(code_);
    if (it != dicts_.end()) {
        const auto& hit = it->second.find(id);
        if (hit != it->second.end()) return hit->second.c_str();
    }
    for (const wchar_t* oc : order) {
        const auto dit = dicts_.find(oc);
        if (dit == dicts_.end()) continue;
        const auto& hit = dit->second.find(id);
        if (hit != dit->second.end()) return hit->second.c_str();
    }
    return id;
}

std::wstring I18n::Fmt(const wchar_t* id,
                       std::initializer_list<std::wstring> args) const {
    std::wstring t = Tr(id);
    std::wstring out;
    out.reserve(t.size() + 16);
    for (size_t i = 0; i < t.size();) {
        if (t[i] == L'{' && i + 2 < t.size() && t[i + 2] == L'}') {
            const wchar_t d = t[i + 1];
            if (d >= L'0' && d <= L'9') {
                const int idx = d - L'0';
                size_t n = 0;
                for (const auto& a : args) {
                    if (n == (size_t)idx) { out += a; break; }
                    ++n;
                }
                i += 3;
                continue;
            }
        }
        out += t[i++];
    }
    return out;
}

void I18n::AddCallback(Callback cb) {
    if (cb) cbs_.push_back(std::move(cb));
}

} // namespace xfs
