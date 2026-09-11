#pragma once
// xfsWinPad - I18n: logical-id string table loaded from lang/<code>.json
// (shipped next to the exe). Every user-visible UI string is a logical id;
// each language file maps id -> localized text. Tr(id) resolves against the
// active language, then falls back to the other shipped language, then to the
// id itself so a missing key is obvious during development.

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace xfs {

class I18n {
public:
    using Callback = std::function<void(const std::wstring& newCode)>;

    static I18n& Instance();

    // Loads lang/<code>.json from the directory next to the exe and makes
    // <code> the active language. Returns false when the file is unreadable
    // (previous dictionaries are kept). Fallback order after <code> is
    // "en" then "zh-CN", deduplicated.
    bool Load(const std::wstring& code);

    const std::wstring& Code() const { return code_; }

    // Resolves a logical id against the active language (with fallback).
    const wchar_t* Tr(const wchar_t* id) const;

    // Translates a format string then replaces {0} {1} ... with args.
    std::wstring Fmt(const wchar_t* id,
                     std::initializer_list<std::wstring> args) const;

    // Invoked after every successful language switch (UI rebuild hook).
    void AddCallback(Callback cb);

    // Overrides the directory that holds lang/*.json (default: exe dir).
    // Used by unit tests to point at a fixture folder.
    void SetLangDir(const std::wstring& dir);

private:
    I18n() = default;

    std::wstring langDir_;   // empty = exe dir
    std::wstring code_ = L"zh-CN";
    std::map<std::wstring, std::map<std::wstring, std::wstring>> dicts_;
    std::vector<Callback> cbs_;
};

// shorthand used at every call site
inline const wchar_t* Tr(const wchar_t* id) { return I18n::Instance().Tr(id); }

} // namespace xfs
