#pragma once
// xfsWinPad - chunked UTF-8 -> target-encoding conversion for streaming saves.
// Mirror of StreamDecoder: holds back boundary bytes between Feed() calls so a
// code point or surrogate pair is never split, and emits the target BOM once.
#include "Encoding.h"
#include "../core/Util.h"
#include <cstring>
#include <string>

namespace xfs {

// Streaming UTF-8 -> UTF8/UTF8BOM/UTF16LE/UTF16BE/UTF32LE/UTF32BE/ANSI/Big5/
// Shift-JIS/KOI8-R/ISO-8859-1 encoder (batch 39).
class StreamEncoder {
public:
    explicit StreamEncoder(encoding::EncodingType target) : type_(target) {}

    // Convert one UTF-8 chunk; encoded bytes are appended to `out`.
    void Feed(const char* data, size_t size, std::string& out) {
        if (!data || !size) return;
        std::string raw(std::move(carry_));
        carry_.clear();
        carryWide_ = false;
        raw.append(data, size);

        // back off an incomplete UTF-8 sequence at the tail so a code point
        // is never split across Feed() calls.
        // count trailing continuation bytes (10xxxxxx)
        size_t conts = 0;
        while (conts < 3 && (unsigned char)raw[raw.size() - 1 - conts] >= 0x80 &&
               ((unsigned char)raw[raw.size() - 1 - conts] & 0xC0) == 0x80)
            ++conts;
        size_t back = 0;
        if (conts == 0) {
            unsigned char b = (unsigned char)raw[raw.size() - 1];
            if (b >= 0xC2 && b <= 0xF4) back = 1;   // bare lead: sequence continues
        } else {
            unsigned char l = (unsigned char)raw[raw.size() - 1 - conts];
            int need = (l >= 0xF0) ? 3 : (l >= 0xE0) ? 2 : (l >= 0xC0) ? 1 : 0;
            if ((int)conts < need)
                back = conts + 1;                   // incomplete: hold lead+conts
            // complete sequence -> nothing to hold
        }
        if (back) {
            carry_.assign(raw.end() - (long)back, raw.end());
            raw.resize(raw.size() - back);
        }
        if (raw.empty()) return;

        EmitBomOnce(out);
        if (type_ == encoding::EncodingType::UTF8 ||
            type_ == encoding::EncodingType::UTF8BOM) {
            out.append(raw);
            return;
        }

        // UTF-8 -> UTF-16
        int wneed = ::MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(),
                                          nullptr, 0);
        if (wneed <= 0) return;
        std::wstring wide((size_t)wneed, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(),
                              wide.data(), wneed);

        // hold a trailing high surrogate (its pair continues in the next chunk)
        if (wide[wneed - 1] >= 0xD800 && wide[wneed - 1] <= 0xDBFF) {
            carry_.assign((const char*)&wide[wneed - 1], 2);
            carryWide_ = true;
            --wneed;
        }
        if (wneed > 0) AppendWide(wide.data(), wneed, out);
    }

    // Flush any held-back bytes at end of input.
    void Flush(std::string& out) {
        if (carry_.empty()) return;
        std::string tail(std::move(carry_));
        carry_.clear();
        bool wide = carryWide_;
        carryWide_ = false;
        EmitBomOnce(out);
        if (!wide) {
            // incomplete UTF-8 sequence: let the OS map it (default char)
            int wneed = ::MultiByteToWideChar(CP_UTF8, 0, tail.data(),
                                              (int)tail.size(), nullptr, 0);
            if (wneed <= 0) return;
            std::wstring w((size_t)wneed, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, tail.data(), (int)tail.size(),
                                  w.data(), wneed);
            if (type_ == encoding::EncodingType::UTF8 ||
                type_ == encoding::EncodingType::UTF8BOM) {
                out.append(tail);
            } else {
                AppendWide(w.data(), wneed, out);
            }
            return;
        }
        // one trailing wchar (lone high surrogate at EOF)
        if (type_ == encoding::EncodingType::UTF8 ||
            type_ == encoding::EncodingType::UTF8BOM) {
            out.append(tail);   // pass through as-is (malformed input)
        } else {
            wchar_t w;
            memcpy(&w, tail.data(), 2);
            AppendWide(&w, 1, out);
        }
    }

private:
    void AppendWide(const wchar_t* w, int n, std::string& out) {
        if (type_ == encoding::EncodingType::UTF16LE ||
            type_ == encoding::EncodingType::UTF16BE) {
            size_t base = out.size();
            out.resize(base + (size_t)n * 2);
            for (int i = 0; i < n; ++i) {
                unsigned short u = (unsigned short)w[i];
                if (type_ == encoding::EncodingType::UTF16LE) {
                    out[base + i * 2]     = (char)(u & 0xFF);
                    out[base + i * 2 + 1] = (char)(u >> 8);
                } else {
                    out[base + i * 2]     = (char)(u >> 8);
                    out[base + i * 2 + 1] = (char)(u & 0xFF);
                }
            }
            return;
        }
        if (type_ == encoding::EncodingType::UTF32LE ||
            type_ == encoding::EncodingType::UTF32BE) {
            // batch 39: recombine surrogate pairs (Feed holds back a trailing
            // high surrogate, so a lone low surrogate here means broken input)
            std::wstring wide(w, w + n);
            out.append(encoding::Utf32FromUtf16(wide,
                                                type_ == encoding::EncodingType::UTF32BE));
            return;
        }
        int cp = (int)encoding::CodepageOf(type_);
        if (cp == 0) cp = CP_ACP;   // ANSI or unknown fallback
        int aneed = ::WideCharToMultiByte(cp, 0, w, n,
                                          nullptr, 0, nullptr, nullptr);
        if (aneed <= 0) return;
        size_t base = out.size();
        out.resize(base + (size_t)aneed, '\0');
        ::WideCharToMultiByte(cp, 0, w, n, out.data() + base, aneed,
                              nullptr, nullptr);
    }
    void EmitBomOnce(std::string& out) {
        if (bomDone_) return;
        bomDone_ = true;
        switch (type_) {
            case encoding::EncodingType::UTF8BOM:
                out += "\xEF\xBB\xBF";
                break;
            case encoding::EncodingType::UTF16LE:
                out += "\xFF\xFE";
                break;
            case encoding::EncodingType::UTF16BE:
                out += "\xFE\xFF";
                break;
            case encoding::EncodingType::UTF32LE:
                // append with explicit length: += would strlen-truncate at NUL
                out.append("\xFF\xFE\x00\x00", 4);
                break;
            case encoding::EncodingType::UTF32BE:
                out.append("\x00\x00\xFE\xFF", 4);
                break;
            default:
                break;
        }
    }

    encoding::EncodingType type_;
    std::string carry_;
    bool carryWide_ = false;   // carry_ holds 1 wchar (surrogate) vs utf-8 bytes
    bool bomDone_ = false;
};

} // namespace xfs
