#pragma once
// xfsWinPad - chunked raw->UTF-8 decoding for huge memory-mapped files
#include "Encoding.h"
#include <algorithm>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {

// --- chunked raw→UTF-8 decoding for huge mmap'd files -------------------------
// UTF-16LE/BE and ANSI/DBCS can't be fed straight into Scintilla. This decoder
// converts in ~8MB raw chunks, holding back boundary bytes that may split a
// code unit (odd UTF-16 byte, lone high surrogate, DBCS lead byte). Peak
// memory stays at one chunk instead of raw+wide+utf8 full copies.
class StreamDecoder {
public:
    StreamDecoder(const char* data, size_t size, encoding::EncodingType type,
                  size_t chunkRaw = 8u << 20)
        : data_(data), size_(size), type_(type), chunk_(chunkRaw) {}

    // Next UTF-8 chunk; empty result = end of input. An empty return never
    // happens while input remains: if everything was held back we keep pulling.
    std::string Next() {
        for (;;) {
            bool finalChunk = pos_ >= size_;
            if (finalChunk && carry_.empty()) return {};
            size_t len = finalChunk ? 0 : (std::min)(chunk_, size_ - pos_);
            std::string raw(std::move(carry_));   // carry stays in original order
            carry_.clear();
            if (len) {
                raw.append(data_ + pos_, len);
                pos_ += len;
            }

            bool isU16 = (type_ == encoding::EncodingType::UTF16LE ||
                          type_ == encoding::EncodingType::UTF16BE);
            if (!finalChunk && isU16) {
                // compute total hold-back: odd byte + trailing high surrogate
                size_t keep = raw.size() & 1;
                if (raw.size() - keep >= 2) {
                    size_t u = raw.size() - keep - 2;
                    unsigned lo = (unsigned char)raw[u];
                    unsigned hi = (unsigned char)raw[u + 1];
                    unsigned unit = (type_ == encoding::EncodingType::UTF16LE)
                                        ? (lo | (hi << 8))
                                        : ((lo << 8) | hi);
                    if (unit >= 0xD800 && unit <= 0xDBFF) keep += 2;
                }
                if (keep) {
                    carry_.assign(raw.end() - (long)keep, raw.end());
                    raw.resize(raw.size() - keep);
                }
            } else if (!finalChunk && type_ == encoding::EncodingType::ANSI) {
                // walk DBCS pairing to the last complete char boundary; a lead
                // byte ending the chunk is held back (harmless for SBCS pages)
                if (carry_.size() < 64) {   // malformed-input runaway guard
                    size_t i = 0;
                    while (i < raw.size()) {
                        unsigned char b = (unsigned char)raw[i];
                        if (b >= 0x81 && b <= 0xFE) {
                            if (i + 1 >= raw.size()) {
                                carry_.assign(1, (char)b);
                                raw.resize(i);
                                break;
                            }
                            i += 2;
                        } else {
                            i += 1;
                        }
                    }
                }
            }

            if (raw.empty()) continue;   // everything held back; pull more

            if (isU16) {
                if (type_ == encoding::EncodingType::UTF16BE) {
                    for (size_t i = 0; i + 1 < raw.size(); i += 2)
                        std::swap(raw[i], raw[i + 1]);
                }
                return Utf16LeToUtf8(raw.data(), raw.size() / 2);
            }
            if (type_ == encoding::EncodingType::UTF8 ||
                type_ == encoding::EncodingType::UTF8BOM)
                return raw;   // already UTF-8: pass through
            return AnsiToUtf8(raw.data(), raw.size());
        }
    }

private:
    static std::string Utf16LeToUtf8(const char* bytes, size_t units) {
        if (!units) return {};
        // WideCharToMultiByte 的 cchWideChar 是 int，所以这里必然要收窄；显式转型
        // 把"有意收窄"写出来，否则 MSVC 会报 C4267（size_t -> int）。
        const int wlen = (int)units;
        int need = ::WideCharToMultiByte(CP_UTF8, 0,
            reinterpret_cast<const wchar_t*>(bytes), wlen, nullptr, 0, nullptr, nullptr);
        std::string out((size_t)(std::max)(need, 0), '\0');
        if (need > 0)
            ::WideCharToMultiByte(CP_UTF8, 0,
                reinterpret_cast<const wchar_t*>(bytes), wlen, out.data(), need, nullptr, nullptr);
        return out;
    }
    static std::string AnsiToUtf8(const char* bytes, size_t size) {
        if (!size) return {};
        int wneed = ::MultiByteToWideChar(CP_ACP, 0, bytes, (int)size, nullptr, 0);
        if (wneed <= 0) return {};
        std::wstring wide((size_t)wneed, L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, bytes, (int)size, wide.data(), wneed);
        int uneed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wneed,
                                          nullptr, 0, nullptr, nullptr);
        std::string out((size_t)(std::max)(uneed, 0), '\0');
        if (uneed > 0)
            ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wneed, out.data(), uneed,
                                  nullptr, nullptr);
        return out;
    }

    const char* data_;
    size_t size_;
    encoding::EncodingType type_;
    size_t chunk_;
    size_t pos_ = 0;
    std::string carry_;
};

// Load cap: Scintilla positions are 32-bit by default; keep huge files
// truncatable at a line boundary instead of failing mid-load.
constexpr size_t kMaxLoadBytes = 2000ULL << 20;   // 2000 MB

// Cut a byte budget back to the last newline so a truncated load never ends
// mid-line. Returns the adjusted length (<= budget).
inline size_t TruncateAtLine(const char* data, size_t size, size_t budget) {
    if (size <= budget) return size;
    size_t cut = budget;
    while (cut > 0 && data[cut - 1] != '\n') --cut;
    return cut > 0 ? cut : budget;
}

} // namespace xfs