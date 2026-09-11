#pragma once
// xfsWinPad - file-extension to (lexer, keywords) mapping
// Real keyword seeds per language; expanded continuously in later phases.

#include <string>
#include <vector>

namespace xfs {

struct LanguageInfo {
    const wchar_t* const* extensions;   // null-terminated array, without dot, lowercase
    const char* lexerName;              // Lexilla lexer name; nullptr = plain text
    const char* keywords[2];            // keyword sets 0 and 1; may be null
};

// Returns nullptr when unknown (plain text).
const LanguageInfo* DetectLanguage(const std::wstring& fileName);

// lexer 名 → 行注释前缀（批次 29「切换行注释」用）；
// nullptr = 该语言无行注释（css/html/xml/diff/markdown/纯文本）。
const char* LineCommentToken(const char* lexerName);

// One entry of the top-level "Language" menu.
struct LanguageMenuItem {
    const wchar_t* label;       // menu display name
    const char* lexerName;      // Lexilla name; nullptr = plain text
    const char* keywords[2];    // keyword sets 0 and 1; may be null
};

// Null-terminated catalog of common programming languages for the "Language"
// menu. Index equals the command offset from Cmd::LangFirst.
const LanguageMenuItem* LanguageMenuCatalog();

} // namespace xfs
