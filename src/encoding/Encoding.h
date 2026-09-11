#pragma once
// xfsWinPad - text encoding detection and conversion
//
// Supported on load:  UTF-8, UTF-8 BOM, UTF-16 LE/BE (BOM or heuristic),
//                     UTF-32 LE/BE (BOM), ANSI (active code page)
// Supported via convert / reload-as menu only (no auto-detect):
//                     Big5, Shift-JIS, KOI8-R, ISO-8859-1
// Internal representation is always UTF-8 (Scintilla code page 65001).

#include <string>
#include <cstdint>
#include <vector>

namespace xfs {

namespace encoding {

enum class EncodingType : int {
    UTF8 = 0,
    UTF8BOM,
    UTF16LE,
    UTF16BE,
    ANSI,
    // batch 39: appended at the end - ids are used as menu command offsets
    UTF32LE,
    UTF32BE,
    Big5,        // cp 950
    ShiftJIS,    // cp 932
    KOI8R,       // cp 20866
    ISO88591,    // cp 28591
};

} // namespace encoding

struct DecodedText {
    std::string utf8;
    encoding::EncodingType encoding = encoding::EncodingType::UTF8;
};

namespace encoding {

DecodedText DecodeToUtf8(const std::string& raw);
DecodedText DecodeWithEncoding(const std::string& raw, EncodingType forced);
std::string EncodeFromUtf8(const std::string& utf8, EncodingType target);

// Detect the encoding of a raw byte buffer without a full decode (BOM first,
// then UTF-8 validity, then a UTF-16-no-BOM heuristic). Used by the large-file
// mmap streaming path to decide whether raw bytes can be fed straight through.
EncodingType DetectEncoding(const char* data, size_t size);
// Strict UTF-8 validity check (>2GB-safe: validated in 1GB slices).
bool IsValidUtf8(const char* data, size_t size);
// UTF-16-no-BOM heuristic on raw bytes: UTF16LE / UTF16BE / ANSI (= no match).
// NUL-phase sampling, zero copies, works on mmap views.
EncodingType DetectUtf16NoBom(const char* data, size_t size);
// Boolean wrapper over DetectUtf16NoBom.
bool LooksLikeUtf16NoBom(const char* data, size_t size);
// Number of leading bytes to skip for the encoding's BOM (0 for UTF-8/ANSI).
size_t BomSkip(EncodingType t);

const wchar_t* DisplayName(EncodingType t);
EncodingType FromDisplayName(const wchar_t* name);
// Windows code page for the single-byte / double-byte menu encodings
// (ANSI -> CP_ACP, UTF families -> 0). StreamEncoder uses it for saves.
unsigned int CodepageOf(EncodingType t);
// UTF-16 <-> UTF-32 byte strings (LE/BE). Utf32FromUtf16 emits no BOM;
// Utf16FromUtf32 skips a leading 4-byte BOM when `stripBom` is set.
std::string Utf32FromUtf16(const std::wstring& wide, bool bigEndian);
std::wstring Utf16FromUtf32(const std::string& bytes, bool bigEndian,
                            bool stripBom);

} // namespace encoding
} // namespace xfs
