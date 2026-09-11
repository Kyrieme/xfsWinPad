#include "Encoding.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string_view>

namespace xfs {
namespace encoding {

bool IsValidUtf8(const char* data, size_t size) {
    if (size == 0) return true;
    // MultiByteToWideChar takes an int; validate >2GB buffers in 1GB slices.
    // A slice boundary can never split a valid sequence into two invalid
    // halves *and* pass both (continuation bytes alone fail MB_ERR_INVALID_CHARS),
    // but a split inside a multi-byte char would falsely report failure, so
    // back off to the previous lead byte (0xxxxxxx or 11xxxxxx) when slicing.
    const size_t kSlice = 1ULL << 30;
    size_t off = 0;
    while (off < size) {
        size_t len = (std::min)(kSlice, size - off);
        if (off + len < size) {
            // back off to last lead byte so the next slice starts on one
            while (len > 0 && (static_cast<unsigned char>(data[off + len]) & 0xC0) == 0x80)
                --len;
            if (len == 0) return false;   // >1GB of continuation bytes
        }
        int r = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      data + off, (int)len, nullptr, 0);
        if (r <= 0) return false;
        off += len;
    }
    return true;
}

EncodingType DetectUtf16NoBom(const char* data, size_t size) {
    // Heuristic: ASCII-dominant UTF-16 has NUL bytes concentrated on one side.
    if (size < 4 || (size & 1)) return EncodingType::ANSI;
    // Sampling is enough: NUL distribution is uniform across ATE/log files.
    // Full scans cost ~GB/s; sample keeps huge mmap detection at ~0 cost.
    const size_t kSample = (std::min)(size, (size_t)(1 << 20)) & ~size_t(1);
    size_t nulEven = 0, nulOdd = 0;
    for (size_t i = 0; i < kSample; i += 2) {
        if (data[i] == '\0') ++nulEven;
        if (data[i + 1] == '\0') ++nulOdd;
    }
    size_t n = kSample;
    if (nulOdd >= n / 4 && nulOdd > nulEven * 2) return EncodingType::UTF16LE;
    if (nulEven >= n / 4 && nulEven > nulOdd * 2) return EncodingType::UTF16BE;
    return EncodingType::ANSI;
}

bool LooksLikeUtf16NoBom(const char* data, size_t size) {
    EncodingType t = DetectUtf16NoBom(data, size);
    return t == EncodingType::UTF16LE || t == EncodingType::UTF16BE;
}

bool LooksLikeUtf16NoBom(const std::string& raw) {
    return LooksLikeUtf16NoBom(raw.data(), raw.size());
}

DecodedText DecodeToUtf8(const std::string& raw) {
    DecodedText out;

    if (raw.size() >= 3 &&
        (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF) {
        out.encoding = EncodingType::UTF8BOM;
        std::string body = raw.substr(3);
        if (IsValidUtf8(body.data(), body.size())) {
            out.utf8 = std::move(body);
        } else {
            out.encoding = EncodingType::ANSI;
            out.utf8 = WideToUtf8(xfs::Utf8ToWide(raw, CP_ACP));
        }
        return out;
    }
    // UTF-32 BOMs must be tested BEFORE the 2-byte UTF-16 checks:
    // UTF-32 LE BOM starts with FF FE (UTF-16 LE BOM prefix).
    if (raw.size() >= 4 && (unsigned char)raw[0] == 0xFF &&
        (unsigned char)raw[1] == 0xFE && (unsigned char)raw[2] == 0x00 &&
        (unsigned char)raw[3] == 0x00) {
        out.encoding = EncodingType::UTF32LE;
        out.utf8 = WideToUtf8(Utf16FromUtf32(raw.substr(4), false, false));
        return out;
    }
    if (raw.size() >= 4 && (unsigned char)raw[0] == 0x00 &&
        (unsigned char)raw[1] == 0x00 && (unsigned char)raw[2] == 0xFE &&
        (unsigned char)raw[3] == 0xFF) {
        out.encoding = EncodingType::UTF32BE;
        out.utf8 = WideToUtf8(Utf16FromUtf32(raw.substr(4), true, false));
        return out;
    }
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        out.encoding = EncodingType::UTF16LE;
        out.utf8 = WideToUtf8(std::wstring(
            reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2));
        return out;
    }
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        out.encoding = EncodingType::UTF16BE;
        std::string swapped(raw.size() - 2, '\0');
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            swapped[i - 2] = raw[i + 1];
            swapped[i - 1] = raw[i];
        }
        out.utf8 = WideToUtf8(std::wstring(
            reinterpret_cast<const wchar_t*>(swapped.data()), swapped.size() / 2));
        return out;
    }

    EncodingType u16 = DetectUtf16NoBom(raw.data(), raw.size());
    if (u16 == EncodingType::UTF16LE || u16 == EncodingType::UTF16BE) {
        std::string bytes(raw);
        if (u16 == EncodingType::UTF16BE) {
            for (size_t i = 0; i + 1 < bytes.size(); i += 2)
                std::swap(bytes[i], bytes[i + 1]);
        }
        out.encoding = u16;
        out.utf8 = WideToUtf8(std::wstring(
            reinterpret_cast<const wchar_t*>(bytes.data()), bytes.size() / 2));
        return out;
    }
    if (IsValidUtf8(raw.data(), raw.size())) {
        out.encoding = EncodingType::UTF8;
        out.utf8 = raw;
        return out;
    }

    out.encoding = EncodingType::ANSI;
    out.utf8 = WideToUtf8(xfs::Utf8ToWide(raw, CP_ACP));
    return out;
}

DecodedText DecodeWithEncoding(const std::string& raw, EncodingType forced) {
    DecodedText out;
    out.encoding = forced;

    auto stripLeBom = [&](const std::string& bytes, bool swap) -> std::string {
        size_t off = 0;
        if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF &&
            (unsigned char)bytes[1] == 0xFE)
            off = 2; // UTF-16 LE BOM
        std::string body = bytes.substr(off);
        if (!swap) return body;
        std::string swapped(body.size(), '\0');
        for (size_t i = 0; i + 1 < body.size(); i += 2) {
            swapped[i] = body[i + 1];
            swapped[i + 1] = body[i];
        }
        return swapped;
    };

    switch (forced) {
        case EncodingType::UTF8BOM: {
            if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
                (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
                out.utf8 = raw.substr(3);
            else
                out.utf8 = raw;
            return out;
        }
        case EncodingType::UTF16LE: {
            std::string body = stripLeBom(raw, false);
            out.utf8 = WideToUtf8(std::wstring(
                reinterpret_cast<const wchar_t*>(body.data()), body.size() / 2));
            return out;
        }
        case EncodingType::UTF16BE: {
            std::string body = stripLeBom(raw, true);
            out.utf8 = WideToUtf8(std::wstring(
                reinterpret_cast<const wchar_t*>(body.data()), body.size() / 2));
            return out;
        }
        case EncodingType::ANSI:
            out.utf8 = WideToUtf8(xfs::Utf8ToWide(raw, CP_ACP));
            return out;
        case EncodingType::UTF32LE:
        case EncodingType::UTF32BE: {
            bool be = (forced == EncodingType::UTF32BE);
            std::string body = raw;
            // strip BOM if the file carries one (both orientations)
            if (body.size() >= 4 &&
                ((be && (unsigned char)body[0] == 0x00 &&
                        (unsigned char)body[1] == 0x00 &&
                        (unsigned char)body[2] == 0xFE &&
                        (unsigned char)body[3] == 0xFF) ||
                 (!be && (unsigned char)body[0] == 0xFF &&
                        (unsigned char)body[1] == 0xFE &&
                        (unsigned char)body[2] == 0x00 &&
                        (unsigned char)body[3] == 0x00)))
                body = body.substr(4);
            out.utf8 = WideToUtf8(Utf16FromUtf32(body, be, false));
            return out;
        }
        case EncodingType::Big5:
        case EncodingType::ShiftJIS:
        case EncodingType::KOI8R:
        case EncodingType::ISO88591:
            out.utf8 = WideToUtf8(xfs::Utf8ToWide(raw, CodepageOf(forced)));
            return out;
        case EncodingType::UTF8:
        default:
            out.utf8 = raw;
            return out;
    }
}

std::string EncodeFromUtf8(const std::string& utf8, EncodingType target) {
    switch (target) {
        case EncodingType::UTF8:
            return utf8;
        case EncodingType::UTF8BOM: {
            static const char bom[3] = {(char)0xEF, (char)0xBB, (char)0xBF};
            return std::string(bom, 3) + utf8;
        }
        case EncodingType::UTF16LE:
        case EncodingType::UTF16BE: {
            std::wstring wide = xfs::Utf8ToWide(utf8);
            std::string out;
            // keep the BOM: detection strips it on load, so a round trip
            // through edit/save must re-add it or the encoding is lost
            if (target == EncodingType::UTF16LE) {
                out.push_back((char)0xFF); out.push_back((char)0xFE);
            } else {
                out.push_back((char)0xFE); out.push_back((char)0xFF);
            }
            for (size_t i = 0; i < wide.size(); ++i) {
                unsigned short u = (unsigned short)wide[i];
                if (target == EncodingType::UTF16LE) {
                    out.push_back((char)(u & 0xFF));
                    out.push_back((char)(u >> 8));
                } else {
                    out.push_back((char)(u >> 8));
                    out.push_back((char)(u & 0xFF));
                }
            }
            return out;
        }
        case EncodingType::ANSI:
        case EncodingType::Big5:
        case EncodingType::ShiftJIS:
        case EncodingType::KOI8R:
        case EncodingType::ISO88591:
            return WideToUtf8(xfs::Utf8ToWide(utf8), CodepageOf(target));
        case EncodingType::UTF32LE:
        case EncodingType::UTF32BE: {
            // keep the BOM: detection strips it on load, so a round trip
            // through edit/save must re-add it or the encoding is lost
            std::string body = Utf32FromUtf16(xfs::Utf8ToWide(utf8),
                                              target == EncodingType::UTF32BE);
            static const char bom32le[4] = {(char)0xFF, (char)0xFE, 0, 0};
            static const char bom32be[4] = {0, 0, (char)0xFE, (char)0xFF};
            return std::string(target == EncodingType::UTF32BE ? bom32be
                                                               : bom32le, 4) +
                   body;
        }
        default:
            return utf8;
    }
}

EncodingType DetectEncoding(const char* data, size_t size) {
    if (size >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        return EncodingType::UTF8BOM;
    if (size >= 2 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE)
        return EncodingType::UTF16LE;
    if (size >= 2 && (unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF)
        return EncodingType::UTF16BE;

    EncodingType u16 = DetectUtf16NoBom(data, size);
    if (u16 == EncodingType::UTF16LE || u16 == EncodingType::UTF16BE)
        return u16;
    if (IsValidUtf8(data, size)) return EncodingType::UTF8;
    return EncodingType::ANSI;
}

size_t BomSkip(EncodingType t) {
    switch (t) {
        case EncodingType::UTF8BOM:  return 3;
        case EncodingType::UTF16LE:
        case EncodingType::UTF16BE:  return 2;
        case EncodingType::UTF32LE:
        case EncodingType::UTF32BE:  return 4;
        default:                     return 0;
    }
}

unsigned int CodepageOf(EncodingType t) {
    switch (t) {
        case EncodingType::ANSI:     return CP_ACP;
        case EncodingType::Big5:     return 950;
        case EncodingType::ShiftJIS: return 932;
        case EncodingType::KOI8R:    return 20866;
        case EncodingType::ISO88591: return 28591;
        default:                     return 0;
    }
}

std::string Utf32FromUtf16(const std::wstring& wide, bool bigEndian) {
    std::string out;
    out.reserve(wide.size() * 4);
    auto emit = [&](unsigned int cp) {
        if (bigEndian) {
            out.push_back((char)((cp >> 24) & 0xFF));
            out.push_back((char)((cp >> 16) & 0xFF));
            out.push_back((char)((cp >> 8) & 0xFF));
            out.push_back((char)(cp & 0xFF));
        } else {
            out.push_back((char)(cp & 0xFF));
            out.push_back((char)((cp >> 8) & 0xFF));
            out.push_back((char)((cp >> 16) & 0xFF));
            out.push_back((char)((cp >> 24) & 0xFF));
        }
    };
    for (size_t i = 0; i < wide.size(); ++i) {
        unsigned int u = (unsigned int)wide[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < wide.size() &&
            wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
            u = 0x10000 + ((u - 0xD800) << 10) + (unsigned)(wide[i + 1] - 0xDC00);
            ++i;   // consume the low surrogate
        }
        emit(u);
    }
    return out;
}

std::wstring Utf16FromUtf32(const std::string& bytes, bool bigEndian,
                            bool stripBom) {
    std::wstring out;
    size_t n = bytes.size() / 4;
    size_t off = 0;
    if (stripBom && n > 0) {
        unsigned int b0 = (unsigned char)bytes[0], b1 = (unsigned char)bytes[1];
        unsigned int b2 = (unsigned char)bytes[2], b3 = (unsigned char)bytes[3];
        bool bom = bigEndian ? (b0 == 0 && b1 == 0 && b2 == 0xFE && b3 == 0xFF)
                             : (b0 == 0xFF && b1 == 0xFE && b2 == 0 && b3 == 0);
        if (bom) { off = 1; --n; }
    }
    out.reserve(n);
    for (size_t i = off; i < off + n; ++i) {
        const unsigned char* p = (const unsigned char*)bytes.data() + i * 4;
        unsigned int u = bigEndian
            ? ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
              ((unsigned)p[2] << 8) | (unsigned)p[3]
            : ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) |
              ((unsigned)p[1] << 8) | (unsigned)p[0];
        if (u >= 0x10000) {
            u -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (u >> 10)));
            out.push_back((wchar_t)(0xDC00 + (u & 0x3FF)));
        } else {
            out.push_back((wchar_t)u);
        }
    }
    return out;
}

const wchar_t* DisplayName(EncodingType t) {
    switch (t) {
        case EncodingType::UTF8:     return L"UTF-8";
        case EncodingType::UTF8BOM:  return L"UTF-8 BOM";
        case EncodingType::UTF16LE:  return L"UTF-16 LE";
        case EncodingType::UTF16BE:  return L"UTF-16 BE";
        case EncodingType::ANSI:     return L"ANSI";
        case EncodingType::UTF32LE:  return L"UTF-32 LE";
        case EncodingType::UTF32BE:  return L"UTF-32 BE";
        case EncodingType::Big5:     return L"Big5";
        case EncodingType::ShiftJIS: return L"Shift-JIS";
        case EncodingType::KOI8R:    return L"KOI8-R";
        case EncodingType::ISO88591: return L"ISO-8859-1";
    }
    return L"UTF-8";
}

EncodingType FromDisplayName(const wchar_t* name) {
    if (wcscmp(name, L"UTF-8") == 0) return EncodingType::UTF8;
    if (wcscmp(name, L"UTF-8 BOM") == 0) return EncodingType::UTF8BOM;
    if (wcscmp(name, L"UTF-16 LE") == 0) return EncodingType::UTF16LE;
    if (wcscmp(name, L"UTF-16 BE") == 0) return EncodingType::UTF16BE;
    if (wcscmp(name, L"ANSI") == 0) return EncodingType::ANSI;
    if (wcscmp(name, L"UTF-32 LE") == 0) return EncodingType::UTF32LE;
    if (wcscmp(name, L"UTF-32 BE") == 0) return EncodingType::UTF32BE;
    if (wcscmp(name, L"Big5") == 0) return EncodingType::Big5;
    if (wcscmp(name, L"Shift-JIS") == 0) return EncodingType::ShiftJIS;
    if (wcscmp(name, L"KOI8-R") == 0) return EncodingType::KOI8R;
    if (wcscmp(name, L"ISO-8859-1") == 0) return EncodingType::ISO88591;
    return EncodingType::UTF8;
}

} // namespace encoding
} // namespace xfs
