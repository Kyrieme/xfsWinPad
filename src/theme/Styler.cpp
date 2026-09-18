// Styler.cpp — 语法配色数据层实现（设置系统设计笔记 阶段 2a）。
// stylers.json 的读写基于共享的 core/JsonLite；颜色统一 "#RRGGBB" 字符串。
#include "Styler.h"
#include "Theme.h"
#include "../core/JsonLite.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

namespace xfs {

// ---- 角色名映射 -------------------------------------------------------------

namespace {
const char* const kRoleNames[SR_COUNT] = {
    "comment", "string", "number", "keyword", "keyword2",
    "operator", "class", "preproc", "special",
    "pass", "fail", "dim",
    // 批次 73：向量语义四色
    "vector", "expect", "both", "ctrl",
};

// 词法器名是 ASCII；wstring 没有 char* 构造，逐字符加宽即可
std::wstring Widen(const char* s) {
    std::wstring w;
    if (s)
        for (const char* p = s; *p; ++p) w += wchar_t((unsigned char)*p);
    return w;
}
std::string Narrow(const wchar_t* w) {
    std::string s;
    if (w)
        for (const wchar_t* p = w; *p; ++p) s += (char)(*p & 0xFF);
    return s;
}
} // namespace

const char* StyleRoleName(int role) {
    return (role >= 0 && role < SR_COUNT) ? kRoleNames[role] : "";
}

int StyleRoleFromName(const std::string& name) {
    for (int i = 0; i < SR_COUNT; ++i)
        if (name == kRoleNames[i]) return i;
    return -1;
}

// ---- JSON → StyleOverride -----------------------------------------------------

namespace {

void ApplyStyleObj(const json::Value& o, StyleOverride* so) {
    so->defined = true;
    if (const json::Value* v = json::Find(o, L"colorStyle"))
        if (v->kind == json::Value::Num) so->colorStyle = (int)v->num;
    if (const json::Value* v = json::Find(o, L"fg"))
        if (v->kind == json::Value::Str) so->fg = json::ParseColor(v->str, so->fg);
    if (const json::Value* v = json::Find(o, L"bg"))
        if (v->kind == json::Value::Str) so->bg = json::ParseColor(v->str, so->bg);
    if (const json::Value* v = json::Find(o, L"bold"))
        if (v->kind == json::Value::Bool) so->bold = v->b;
    if (const json::Value* v = json::Find(o, L"italic"))
        if (v->kind == json::Value::Bool) so->italic = v->b;
    if (const json::Value* v = json::Find(o, L"underline"))
        if (v->kind == json::Value::Bool) so->underline = v->b;
    if (const json::Value* v = json::Find(o, L"font"))
        if (v->kind == json::Value::Str) so->font = v->str;
    if (const json::Value* v = json::Find(o, L"size"))
        if (v->kind == json::Value::Num) so->fontSize = (int)v->num;
}

} // namespace

// ---- 查询/解析链 -------------------------------------------------------------

const StyleOverride& StylerStore::Override(const char* lexer, int role) const {
    static const StyleOverride kNone;
    if (!lexer) return kNone;
    return OverrideW(Widen(lexer).c_str(), role);
}

const StyleOverride& StylerStore::OverrideW(const wchar_t* lexer, int role) const {
    static const StyleOverride kNone;
    if (!lexer) return kNone;
    auto it = langs_.find(lexer);
    if (it == langs_.end()) return kNone;
    auto it2 = it->second.find(role);
    return it2 == it->second.end() ? kNone : it2->second;
}

COLORREF StylerStore::ResolveFg(const char* lexer, int role, const ThemeDef& theme) const {
    // 1) Global override
    if (global_.enableFg) return global_.fg;
    // 2) 语言覆盖（colorStyle 语义见 Styler.h 文件头）
    const StyleOverride& so = Override(lexer, role);
    if (so.defined) {
        if (so.colorStyle == 1 || so.colorStyle == -1) return so.fg;  // fg 用覆盖值（含 fg+bg 双选）
        if (so.colorStyle == 0) return theme.editorFg;                // 全继承
    }
    // 3) 主题角色色
    switch (role) {
        case SR_Comment:  return theme.comment;
        case SR_String:   return theme.str;
        case SR_Number:   return theme.number;
        case SR_Keyword:  return theme.keyword;
        case SR_Keyword2: return theme.keyword2;
        case SR_Operator: return theme.op;
        case SR_Class:    return theme.cls;
        case SR_Preproc:  return theme.preproc;
        case SR_Special:  return theme.special;
        case SR_Pass:     return theme.pass;
        case SR_Fail:     return theme.fail;
        case SR_Dim:      return theme.dim;
        case SR_Vector:   return theme.vector;
        case SR_Expect:   return theme.expect;
        case SR_Both:     return theme.both;
        case SR_Ctrl:     return theme.ctrl;
    }
    return theme.editorFg;
}

COLORREF StylerStore::ResolveFgW(const wchar_t* lexer, int role, const ThemeDef& theme) const {
    return ResolveFg(Narrow(lexer).c_str(), role, theme);
}

COLORREF StylerStore::ResolveBgW(const wchar_t* lexer, int role, const ThemeDef& theme) const {
    return ResolveBg(Narrow(lexer).c_str(), role, theme);
}

COLORREF StylerStore::ResolveBg(const char* lexer, int role, const ThemeDef& theme) const {
    if (global_.enableBg) return global_.bg;
    const StyleOverride& so = Override(lexer, role);
    if (so.defined) {
        if (so.colorStyle == 2 || so.colorStyle == -1) return so.bg;  // bg 用覆盖值（含 fg+bg 双选）
        if (so.colorStyle == 0) return theme.editorBg;                // 全继承
        // 1：fg 覆盖，bg 跟主题编辑器底色
    }
    return theme.editorBg;
}

void StylerStore::ResolveFont(const char* lexer, const ThemeDef& /*theme*/,
                              std::wstring* fontOut, int* sizeOut,
                              bool* boldOut, bool* italicOut, bool* underlineOut) const {
    // 字体覆盖链：语言级整行字体 > 默认。
    const std::wstring* lf = lexer ? LanguageFont(lexer) : nullptr;
    if (lf && !lf->empty()) *fontOut = *lf;
    const int lsize = lexer ? LanguageFontSize(lexer) : 0;
    if (lsize > 0) *sizeOut = lsize;
    *boldOut = global_.bold;
    *italicOut = global_.italic;
    *underlineOut = global_.underline;
}

// ---- 修改 ---------------------------------------------------------------------

StyleOverride& StylerStore::MutableOverride(const char* lexer, int role) {
    return langs_[Widen(lexer)][role];
}

StyleOverride& StylerStore::MutableOverrideW(const wchar_t* lexer, int role) {
    return langs_[lexer][role];
}

void StylerStore::ClearOverride(const char* lexer, int role) {
    if (lexer) ClearOverrideW(Widen(lexer).c_str(), role);
}

void StylerStore::ClearOverrideW(const wchar_t* lexer, int role) {
    if (!lexer) return;
    auto it = langs_.find(lexer);
    if (it == langs_.end()) return;
    it->second.erase(role);
    if (it->second.empty()) langs_.erase(it);
}

void StylerStore::ClearLexer(const char* lexer) {
    if (lexer) langs_.erase(Widen(lexer));
}

// ---- 每语言整行字体 -----------------------------------------------------------

const std::wstring* StylerStore::LanguageFont(const char* lexer) const {
    if (!lexer) return nullptr;
    auto it = langFonts_.find(Widen(lexer));
    return it != langFonts_.end() ? &it->second.first : nullptr;
}

int StylerStore::LanguageFontSize(const char* lexer) const {
    if (!lexer) return 0;
    auto it = langFonts_.find(Widen(lexer));
    return it != langFonts_.end() ? it->second.second : 0;
}

void StylerStore::SetLanguageFont(const char* lexer, const std::wstring& font, int size) {
    if (lexer) langFonts_[Widen(lexer)] = { font, size };
}

void StylerStore::ClearLanguageFont(const char* lexer) {
    if (lexer) langFonts_.erase(Widen(lexer));
}

// ---- 用户扩展名 ---------------------------------------------------------------

const char* StylerStore::UserLexerForExt(const char* ext) const {
    if (!ext) return nullptr;
    auto it = userExts_.find(ext);
    return it != userExts_.end() ? it->second.c_str() : nullptr;
}

void StylerStore::SetUserExt(const char* ext, const char* lexer) {
    if (!ext) return;
    if (lexer && lexer[0])
        userExts_[ext] = lexer;
    else
        userExts_.erase(ext);
}

bool StylerStore::HasLexer(const char* lexer) const {
    return lexer && langs_.count(Widen(lexer)) > 0;
}

std::vector<std::wstring> StylerStore::Languages() const {
    std::vector<std::wstring> out;
    for (const auto& kv : langs_) out.push_back(kv.first);
    return out;
}

// ---- 持久化 ---------------------------------------------------------------------

const wchar_t* StylerStore::FilePath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    static const std::wstring path = base + L"\\xfsWinPad\\stylers.json";
    return path.c_str();
}

bool StylerStore::Load(const std::wstring& path) {
    Reset();
    std::string raw;
    if (!ReadFileBytes(path, raw)) return false;
    std::wstring j = Utf8ToWide(raw);
    if (!j.empty() && j[0] == 0xFEFF) j.erase(0, 1);

    json::Value root;
    if (!json::Parse(j, &root)) return false;

    if (const json::Value* g = json::Find(root, L"global")) {
        if (const json::Value* v = json::Find(*g, L"enableFg"))
            if (v->kind == json::Value::Bool) global_.enableFg = v->b;
        if (const json::Value* v = json::Find(*g, L"enableBg"))
            if (v->kind == json::Value::Bool) global_.enableBg = v->b;
        if (const json::Value* v = json::Find(*g, L"fg"))
            if (v->kind == json::Value::Str) global_.fg = json::ParseColor(v->str, global_.fg);
        if (const json::Value* v = json::Find(*g, L"bg"))
            if (v->kind == json::Value::Str) global_.bg = json::ParseColor(v->str, global_.bg);
        if (const json::Value* v = json::Find(*g, L"bold"))
            if (v->kind == json::Value::Bool) global_.bold = v->b;
        if (const json::Value* v = json::Find(*g, L"italic"))
            if (v->kind == json::Value::Bool) global_.italic = v->b;
        if (const json::Value* v = json::Find(*g, L"underline"))
            if (v->kind == json::Value::Bool) global_.underline = v->b;
    }

    const json::Value* langs = json::Find(root, L"languages");
    if (!langs || langs->kind != json::Value::Obj) return true;   // 只有 global 也算成功
    for (const auto& kv : langs->obj) {
        if (kv.second.kind != json::Value::Obj) continue;
        std::map<int, StyleOverride> dst;
        if (const json::Value* styles = json::Find(kv.second, L"styles")) {
            for (const auto& sv : styles->obj) {
                std::string rn(sv.first.begin(), sv.first.end());
                int role = StyleRoleFromName(rn);
                if (role < 0) continue;
                StyleOverride so;
                ApplyStyleObj(sv.second, &so);
                dst[role] = so;
            }
        }
        if (!dst.empty()) langs_[kv.first] = std::move(dst);
        // 语言级整行字体（可选）
        std::wstring lf; int lsize = 0;
        if (const json::Value* v = json::Find(kv.second, L"font"))
            if (v->kind == json::Value::Str) lf = v->str;
        if (const json::Value* v = json::Find(kv.second, L"size"))
            if (v->kind == json::Value::Num) lsize = (int)v->num;
        if (!lf.empty() || lsize > 0) langFonts_[kv.first] = { lf, lsize };
    }

    // 用户扩展名 → 词法器（独立顶层块）
    if (const json::Value* exts = json::Find(root, L"userExts")) {
        if (exts->kind == json::Value::Obj) {
            for (const auto& kv : exts->obj) {
                if (kv.second.kind != json::Value::Str || kv.second.str.empty()) continue;
                std::string ext(kv.first.begin(), kv.first.end());
                std::string lex(kv.second.str.begin(), kv.second.str.end());
                userExts_[ext] = lex;
            }
        }
    }
    return true;
}

bool StylerStore::Save(const std::wstring& path) const {
    std::wstring j = L"{\r\n  \"global\": {\r\n";
    j += std::wstring(L"    \"enableFg\": ") + (global_.enableFg ? L"true" : L"false") + L",\r\n";
    j += L"    \"fg\": \"" + json::ColorToWstr(global_.fg) + L"\",\r\n";
    j += std::wstring(L"    \"enableBg\": ") + (global_.enableBg ? L"true" : L"false") + L",\r\n";
    j += L"    \"bg\": \"" + json::ColorToWstr(global_.bg) + L"\",\r\n";
    j += std::wstring(L"    \"bold\": ")      + (global_.bold ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"    \"italic\": ")    + (global_.italic ? L"true" : L"false") + L",\r\n";
    j += std::wstring(L"    \"underline\": ") + (global_.underline ? L"true" : L"false") + L"\r\n";
    j += L"  },\r\n  \"languages\": {";
    // 收集所有语言键（langs_ + langFonts_）
    std::set<std::wstring> allKeys;
    for (const auto& kv : langs_) allKeys.insert(kv.first);
    for (const auto& kv : langFonts_) allKeys.insert(kv.first);
    bool firstLang = true;
    for (const auto& key : allKeys) {
        if (!firstLang) j += L",";
        firstLang = false;
        j += L"\r\n    \"" + key + L"\": {\r\n      \"styles\": {";
        auto lit = langs_.find(key);
        if (lit != langs_.end()) {
            bool firstStyle = true;
            for (const auto& sv : lit->second) {
                if (!firstStyle) j += L",";
                firstStyle = false;
                const StyleOverride& o = sv.second;
                j += L"\r\n        \"" + Widen(StyleRoleName(sv.first)) + L"\": {";
                j += L"\"colorStyle\": " + std::to_wstring(o.colorStyle);
                j += L", \"fg\": \"" + json::ColorToWstr(o.fg) + L"\"";
                j += L", \"bg\": \"" + json::ColorToWstr(o.bg) + L"\"";
                j += std::wstring(L", \"bold\": ")      + (o.bold ? L"true" : L"false");
                j += std::wstring(L", \"italic\": ")    + (o.italic ? L"true" : L"false");
                j += std::wstring(L", \"underline\": ") + (o.underline ? L"true" : L"false");
                if (!o.font.empty()) j += L", \"font\": \"" + o.font + L"\"";
                if (o.fontSize > 0)  j += L", \"size\": " + std::to_wstring(o.fontSize);
                j += L" }";
            }
        }
        j += L"\r\n      }";
        // 语言级整行字体
        auto fit = langFonts_.find(key);
        if (fit != langFonts_.end()) {
            if (!fit->second.first.empty())
                j += L",\r\n      \"font\": \"" + fit->second.first + L"\"";
            if (fit->second.second > 0)
                j += L",\r\n      \"size\": " + std::to_wstring(fit->second.second);
        }
        j += L"\r\n    }";
    }
    j += (firstLang ? L"}" : L"\r\n  }");
    // 用户扩展名（顶层独立块）
    if (!userExts_.empty()) {
        j += L",\r\n  \"userExts\": {";
        bool firstExt = true;
        for (const auto& kv : userExts_) {
            if (!firstExt) j += L",";
            firstExt = false;
            std::wstring ext(kv.first.begin(), kv.first.end());
            std::wstring lex(kv.second.begin(), kv.second.end());
            j += L"\r\n    \"" + ext + L"\": \"" + lex + L"\"";
        }
        j += L"\r\n  }";
    }
    j += L"\r\n}";

    std::string utf8 = WideToUtf8(j);
    return WriteFileBytes(path, utf8.data(), utf8.size());
}

void StylerStore::Reset() {
    global_ = GlobalOverride{};
    langs_.clear();
    langFonts_.clear();
    userExts_.clear();
}

StylerStore& GlobalStyler() {
    static StylerStore t;
    return t;
}

// 词法器家族清单：与 Editor.cpp 的 kFamilies 表保持同一组名字
namespace {
const char* const kLexerFamilies[] = {
    "cpp", "python", "hypertext", "xml", "css", "json",
    "yaml", "sql", "bash", "powershell", "batch",
    // 批次 72：ATE 族（自研 ILexer5，样式号从 64 起编）
    "ate_pattern", "stil", "ate_log",
    // 批次 73：Chroma 3380 的 .dec / .pln
    "chroma_dec", "chroma_plan",
};
// 从表长推导计数：原来在两处各写了一次字面量 14，加一族就得记得改两处，
// 漏一处就是「对话框少一项」或「越界返回空串」的静默 bug。
const int kLexerFamilyCount = (int)(sizeof(kLexerFamilies) / sizeof(kLexerFamilies[0]));
} // namespace

int StylerStore::LexerFamilyCount() { return kLexerFamilyCount; }

const char* StylerStore::LexerFamilyName(int i) {
    return (i >= 0 && i < kLexerFamilyCount) ? kLexerFamilies[i] : "";
}

} // namespace xfs
