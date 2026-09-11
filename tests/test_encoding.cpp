// test_encoding.cpp - unit tests for xfs encoding detection/conversion
#include "../src/encoding/Encoding.h"
#include "../src/encoding/StreamDecoder.h"
#include "../src/encoding/StreamEncoder.h"
#include "../src/core/Util.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace xfs;
using namespace xfs::encoding;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK2(cond, name, chunk) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s chunk=%zu\n", __FILE__, __LINE__, name, chunk); } } while (0)

int main() {
    // --- UTF-8 plain ---------------------------------------------------------
    {
        DecodedText d = DecodeToUtf8("hello \xE4\xBD\xA0\xE5\xA5\xBD");
        CHECK(d.encoding == EncodingType::UTF8);
        CHECK(d.utf8 == "hello \xE4\xBD\xA0\xE5\xA5\xBD");
    }
    // --- UTF-8 BOM -----------------------------------------------------------
    {
        std::string raw = "\xEF\xBB\xBF" "abc";
        DecodedText d = DecodeToUtf8(raw);
        CHECK(d.encoding == EncodingType::UTF8BOM);
        CHECK(d.utf8 == "abc");
        // forced decode also strips BOM when present, keeps bytes when absent
        DecodedText f1 = DecodeWithEncoding(raw, EncodingType::UTF8BOM);
        CHECK(f1.utf8 == "abc");
        DecodedText f2 = DecodeWithEncoding("abc", EncodingType::UTF8BOM);
        CHECK(f2.utf8 == "abc" && f2.encoding == EncodingType::UTF8BOM);
    }
    // --- UTF-16 LE with BOM ----------------------------------------------------
    {
        std::wstring w = L"hi \x4F60";
        std::string bytes("\xFF\xFE", 2);
        bytes.append((const char*)w.data(), w.size() * 2);
        DecodedText d = DecodeToUtf8(bytes);
        CHECK(d.encoding == EncodingType::UTF16LE);
        CHECK(d.utf8 == WideToUtf8(w));
        DecodedText f = DecodeWithEncoding(bytes, EncodingType::UTF16LE);
        CHECK(f.utf8 == WideToUtf8(w));
    }
    // --- UTF-16 BE with BOM ----------------------------------------------------
    {
        std::wstring w = L"BE\x4F60";
        std::string bytes("\xFE\xFF", 2);
        for (wchar_t wc : w) {
            bytes.push_back((char)((wc >> 8) & 0xFF));
            bytes.push_back((char)(wc & 0xFF));
        }
        DecodedText d = DecodeToUtf8(bytes);
        CHECK(d.encoding == EncodingType::UTF16BE);
        CHECK(d.utf8 == WideToUtf8(w));
    }
    // --- ANSI fallback (active code page) ---------------------------------------
    {
        const unsigned char gbkNi[] = {0xC4, 0xE3}; // "你" in CP936
        std::string raw((const char*)gbkNi, 2);
        UINT acp = GetACP();
        if (acp == 936) {
            DecodedText d = DecodeToUtf8(raw);
            CHECK(d.encoding == EncodingType::ANSI);
            CHECK(d.utf8 == "\xE4\xBD\xA0");
            DecodedText f = DecodeWithEncoding(raw, EncodingType::ANSI);
            CHECK(f.utf8 == "\xE4\xBD\xA0");
        }
    }
    // --- encode round trips -------------------------------------------------------
    {
        std::string utf8 = "round\xE4\xBD\xA0trip";
        std::string le = EncodeFromUtf8(utf8, EncodingType::UTF16LE);
        DecodedText back = DecodeToUtf8(le);
        CHECK(back.encoding == EncodingType::UTF16LE);
        CHECK(back.utf8 == utf8);
    }
    {
        std::string utf8 = "x\xE4\xBD\xA0y";
        UINT acp = GetACP();
        if (acp == 936) {
            std::string ansi = EncodeFromUtf8(utf8, EncodingType::ANSI);
            CHECK(ansi == std::string("x\xC4\xE3y", 4));
        }
    }
    {
        std::string out = EncodeFromUtf8("z", EncodingType::UTF8BOM);
        CHECK(out.size() == 4 && (unsigned char)out[0] == 0xEF &&
              (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF &&
              out[3] == 'z');
    }
    // --- UTF-16 BE encode (BOM is re-added: load strips it, save restores it) ---
    {
        std::string utf8 = "be\xE4\xBD\xA0";
        std::string be = EncodeFromUtf8(utf8, EncodingType::UTF16BE);
        CHECK(be.size() % 2 == 0);
        CHECK((unsigned char)be[0] == 0xFE && (unsigned char)be[1] == 0xFF);
        std::wstring w = Utf8ToWide(utf8);
        CHECK(be.size() == w.size() * 2 + 2);
        for (size_t i = 0; i < w.size(); ++i) {
            unsigned short u = (unsigned short)w[i];
            CHECK((unsigned char)be[2 + i*2] == (u >> 8));
            CHECK((unsigned char)be[3 + i*2] == (u & 0xFF));
        }
    }
    // --- UTF-16 LE encode (BOM) + full decode round trip --------------------------
    {
        std::string utf8 = "le\xE4\xBD\xA0";
        std::string le = EncodeFromUtf8(utf8, EncodingType::UTF16LE);
        CHECK((unsigned char)le[0] == 0xFF && (unsigned char)le[1] == 0xFE);
        DecodedText rt = DecodeToUtf8(le);
        CHECK(rt.encoding == EncodingType::UTF16LE);
        CHECK(rt.utf8 == utf8);
    }

    // --- DetectEncoding + BomSkip over raw buffers --------------------------------
    {
        CHECK(DetectEncoding("plain text", 10) == EncodingType::UTF8);
        std::string bom = "\xEF\xBB\xBF" "abc";
        CHECK(DetectEncoding(bom.data(), bom.size()) == EncodingType::UTF8BOM);
        CHECK(BomSkip(EncodingType::UTF8BOM) == 3);
        std::string le16("\xFF\xFE", 2);
        CHECK(DetectEncoding(le16.data(), le16.size()) == EncodingType::UTF16LE);
        CHECK(BomSkip(EncodingType::UTF16LE) == 2);
        std::string be16("\xFE\xFF", 2);
        CHECK(DetectEncoding(be16.data(), be16.size()) == EncodingType::UTF16BE);
        // invalid UTF-8 (a lone continuation byte) -> ANSI fallback
        std::string bad = "\x80\x81";
        CHECK(DetectEncoding(bad.data(), bad.size()) == EncodingType::ANSI);
        // no-BOM UTF-16 heuristic: NUL-phase decides LE/BE
        CHECK(DetectEncoding("h\0i\0", 4) == EncodingType::UTF16LE);
        CHECK(DetectEncoding("\0h\0i", 4) == EncodingType::UTF16BE);
        // sampled UTF16-no-BOM heuristic: NUL pattern beyond the 1MB sample
        // window must still be detected from the head sample itself
        {
            std::string big16;
            big16.resize(3u << 20);          // 3 MB UTF-16LE-shaped: 'A' + NUL
            for (size_t i = 0; i < big16.size(); i += 2) {
                big16[i] = 'A';
                big16[i + 1] = '\0';
            }
            CHECK(DetectEncoding(big16.data(), big16.size()) == EncodingType::UTF16LE);
            // opposite NUL phase (BE-shaped)
            std::string big16be = big16;
            for (size_t i = 0; i < big16be.size(); i += 2)
                std::swap(big16be[i], big16be[i + 1]);
            CHECK(DetectEncoding(big16be.data(), big16be.size()) == EncodingType::UTF16BE);
        }
        // IsValidUtf8 slices: a valid multi-byte char run must stay valid,
        // a lone continuation byte must stay invalid (no size-int truncation)
        {
            std::string mb = "\xE4\xBD\xA0\xE5\xA5\xBD";   // 你好
            CHECK(IsValidUtf8(mb.data(), mb.size()));
            std::string bad = "a\x80" "b";
            CHECK(!IsValidUtf8(bad.data(), bad.size()));
        }
    }

    // --- MappedFile: round-trip a real temp file ----------------------------------
    {
        std::wstring tmp = L"xfs_test_mmap.txt";
        FILE* f = nullptr;
        _wfopen_s(&f, tmp.c_str(), L"wb");
        CHECK(f != nullptr);
        if (f) {
            const char payload[] = "line1\r\nline2\n\xE4\xBD\xA0";
            fwrite(payload, 1, sizeof(payload) - 1, f);
            fclose(f);

            MappedFile m;
            CHECK(m.Open(tmp));
            CHECK(m.IsValid());
            CHECK(m.Size() == sizeof(payload) - 1);
            if (m.IsValid()) {
                // exactly the bytes we wrote are visible in the zero-copy view
                CHECK(memcmp(m.Data(), payload, m.Size()) == 0);
            }
            m.Close();
            CHECK(!m.IsValid());
            ::DeleteFileW(tmp.c_str());
        }
    }

    // --- StreamDecoder: chunk boundary correctness ------------------------------
    {
        // UTF-16LE: two surrogate pairs (U+1F600) + 'A'; every chunk size from
        // 2..10 bytes must reconstruct the same UTF-8 (F0 9F 98 80 x2 + 'A')
        std::string u16;
        auto unit16 = [&](unsigned v) { u16 += (char)(v & 0xFF); u16 += (char)(v >> 8); };
        unit16(0xD83D); unit16(0xDE00);
        unit16(0xD83D); unit16(0xDE00);
        unit16(0x0041);
        const unsigned char expEmoji[] = {0xF0, 0x9F, 0x98, 0x80, 0xF0, 0x9F,
                                          0x98, 0x80, 'A'};
        for (size_t chunk = 2; chunk <= 10; ++chunk) {
            StreamDecoder sd(u16.data(), u16.size(),
                             encoding::EncodingType::UTF16LE, chunk);
            std::string all;
            for (;;) {
                std::string c = sd.Next();
                if (c.empty()) break;
                all += c;
            }
            CHECK(all.size() == 9);
            if (all.size() == 9)
                CHECK(memcmp(all.data(), expEmoji, 9) == 0);
        }
        // UTF-16BE 'h''i' -> "hi"
        std::string be;
        be += (char)0x00; be += 'h';
        be += (char)0x00; be += 'i';
        StreamDecoder sbe(be.data(), be.size(), encoding::EncodingType::UTF16BE, 3);
        std::string allbe;
        for (;;) {
            std::string c = sbe.Next();
            if (c.empty()) break;
            allbe += c;
        }
        CHECK(allbe == "hi");
        // GBK DBCS pair split across every chunk size (你 = C4 E3, 好 = BA C3)
        // CP936-only: on other ANSI locales (e.g. CP1252 CI runners) these
        // bytes decode as Latin characters, so the CJK expectation fails
        if (GetACP() == 936) {
            std::string gbk = "\xC4\xE3\xBA\xC3hi";
            const unsigned char expGbk[] = {0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD, 'h', 'i'};
            for (size_t chunk = 1; chunk <= 6; ++chunk) {
                StreamDecoder sd(gbk.data(), gbk.size(),
                                 encoding::EncodingType::ANSI, chunk);
                std::string all;
                for (;;) {
                    std::string c = sd.Next();
                    if (c.empty()) break;
                    all += c;
                }
                CHECK(all.size() == 8);
                if (all.size() == 8)
                    CHECK(memcmp(all.data(), expGbk, 8) == 0);
            }
        }
    }

    // --- TruncateAtLine: line-boundary load cap ---------------------------------
    {
        std::string lines = "aaa\nbbbb\ncc\n";
        CHECK(TruncateAtLine(lines.data(), lines.size(), 100) == lines.size());  // under cap
        CHECK(TruncateAtLine(lines.data(), lines.size(), 9) == 9);   // ends on '\n'
        CHECK(TruncateAtLine(lines.data(), lines.size(), 8) == 4);   // back to 1st '\n'+1
        CHECK(TruncateAtLine(lines.data(), lines.size(), 6) == 4);
        CHECK(TruncateAtLine(lines.data(), lines.size(), 2) == 2);   // no '\n' yet -> budget
    }

    // --- StreamEncoder <-> StreamDecoder round trip (any chunking) ---------------
    {
        // input: ascii + CJK + 4-byte emoji + BMP symbol; every encoder target
        std::string u8 = "abc\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x98\x80\xe2\x9c\x93xyz";
        struct Case {
            encoding::EncodingType t;
            const char* name;
        };
        Case cases[] = {
            { encoding::EncodingType::UTF8,    "utf8" },
            { encoding::EncodingType::UTF8BOM, "utf8bom" },
            { encoding::EncodingType::UTF16LE, "utf16le" },
            { encoding::EncodingType::UTF16BE, "utf16be" },
            { encoding::EncodingType::ANSI,    "ansi" },   // CP936 covers CJK, not emoji
        };
        for (auto& c : cases) {
            for (size_t chunk = 1; chunk <= 9; ++chunk) {
                StreamEncoder enc(c.t);
                std::string bytes;
                for (size_t off = 0; off < u8.size(); off += chunk) {
                    size_t n = (std::min)(chunk, u8.size() - off);
                    enc.Feed(u8.data() + off, n, bytes);
                }
                enc.Flush(bytes);
                // decode back; strip the target BOM first (the loader does
                // the same via BomSkip)
                size_t skip = BomSkip(c.t);
                StreamDecoder sd(bytes.data() + skip, bytes.size() - skip,
                                 c.t, chunk);
                std::string back;
                for (;;) {
                    std::string part = sd.Next();
                    if (part.empty()) break;
                    back += part;
                }
                if (c.t == encoding::EncodingType::ANSI) {
                    // unmappable chars turn to '?' on any ANSI locale; the
                    // CJK-survival assertion only holds when ANSI is CP936
                    CHECK(back.compare(0, 3, "abc") == 0);   // ascii head survives
                    CHECK(back.back() == 'z');
                    if (GetACP() == 936)                     // CJK survives
                        CHECK(back.find("\xe4\xbd\xa0\xe5\xa5\xbd") !=
                              std::string::npos);
                } else {
                    bool ok = back == u8;
                    CHECK2(ok, c.name, chunk);
                }
            }
        }
    }

    // --- batch 39: UTF-32 LE BOM auto-detect + forced decode -----------------
    {
        // \U0002F800 via explicit surrogate pair: \x eats all hex digits
        std::wstring w = L"A\x4F60\xD87E\xDC00";   // BMP + astral plane (U+2F800)
        std::string body = Utf32FromUtf16(w, false);
        std::string raw("\xFF\xFE\x00\x00", 4);
        raw += body;
        DecodedText d = DecodeToUtf8(raw);
        CHECK(d.encoding == EncodingType::UTF32LE);
        CHECK(d.utf8 == WideToUtf8(w));
        DecodedText f = DecodeWithEncoding(raw, EncodingType::UTF32LE);
        CHECK(f.utf8 == WideToUtf8(w));
        // round trip via EncodeFromUtf8 (re-adds the BOM)
        CHECK(EncodeFromUtf8(WideToUtf8(w), EncodingType::UTF32LE) == raw);
        CHECK(BomSkip(EncodingType::UTF32LE) == 4);
    }
    // --- batch 39: UTF-32 BE --------------------------------------------------
    {
        std::wstring w = L"be\x4F60";
        std::string raw("\x00\x00\xFE\xFF", 4);
        raw += Utf32FromUtf16(w, true);
        DecodedText d = DecodeToUtf8(raw);
        CHECK(d.encoding == EncodingType::UTF32BE);
        CHECK(d.utf8 == WideToUtf8(w));
        DecodedText f = DecodeWithEncoding(raw, EncodingType::UTF32BE);
        CHECK(f.utf8 == WideToUtf8(w));
        CHECK(EncodeFromUtf8(WideToUtf8(w), EncodingType::UTF32BE) == raw);
        CHECK(BomSkip(EncodingType::UTF32BE) == 4);
    }
    // --- batch 39: Shift-JIS (cp 932) ------------------------------------------
    {
        // "日本語" in Shift-JIS = 93 FA 96 7B 8C EA
        std::string sjis("\x93\xFA\x96\x7B\x8C\xEA", 6);
        std::string u8 = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";  // 日本語 utf8
        DecodedText f = DecodeWithEncoding(sjis, EncodingType::ShiftJIS);
        CHECK(f.utf8 == u8);
        CHECK(EncodeFromUtf8(u8, EncodingType::ShiftJIS) == sjis);
        // a Shift-JIS byte pair must NOT be misdetected as UTF-16
        CHECK(DecodeToUtf8(sjis).encoding == EncodingType::ANSI);
    }
    // --- batch 39: Big5 (cp 950) ------------------------------------------------
    {
        // "你好" in Big5 = A7 41 A6 6E
        std::string big5("\xA7\x41\xA6\x6E", 4);
        std::string u8 = "\xE4\xBD\xA0\xE5\xA5\xBD";  // 你好 utf8
        DecodedText f = DecodeWithEncoding(big5, EncodingType::Big5);
        CHECK(f.utf8 == u8);
        CHECK(EncodeFromUtf8(u8, EncodingType::Big5) == big5);
    }
    // --- batch 39: KOI8-R (cp 20866) --------------------------------------------
    {
        // "Привет" in KOI8-R = F0 D2 C9 D7 C5 D4
        std::string koi("\xF0\xD2\xC9\xD7\xC5\xD4", 6);
        std::string u8 = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
        DecodedText f = DecodeWithEncoding(koi, EncodingType::KOI8R);
        CHECK(f.utf8 == u8);
        CHECK(EncodeFromUtf8(u8, EncodingType::KOI8R) == koi);
    }
    // --- batch 39: ISO-8859-1 (cp 28591) ----------------------------------------
    {
        // "café" = 63 61 66 E9
        std::string lat("caf\xe9", 4);
        std::string u8 = "caf\xC3\xA9";
        DecodedText f = DecodeWithEncoding(lat, EncodingType::ISO88591);
        CHECK(f.utf8 == u8);
        CHECK(EncodeFromUtf8(u8, EncodingType::ISO88591) == lat);
    }
    // --- batch 39: names + codepages -------------------------------------------
    {
        CHECK(std::wstring(DisplayName(EncodingType::UTF32LE)) == L"UTF-32 LE");
        CHECK(std::wstring(DisplayName(EncodingType::Big5)) == L"Big5");
        CHECK(std::wstring(DisplayName(EncodingType::ShiftJIS)) == L"Shift-JIS");
        CHECK(std::wstring(DisplayName(EncodingType::KOI8R)) == L"KOI8-R");
        CHECK(std::wstring(DisplayName(EncodingType::ISO88591)) == L"ISO-8859-1");
        CHECK(FromDisplayName(L"Shift-JIS") == EncodingType::ShiftJIS);
        CHECK(FromDisplayName(L"UTF-32 BE") == EncodingType::UTF32BE);
        CHECK(CodepageOf(EncodingType::Big5) == 950);
        CHECK(CodepageOf(EncodingType::ShiftJIS) == 932);
        CHECK(CodepageOf(EncodingType::KOI8R) == 20866);
        CHECK(CodepageOf(EncodingType::ISO88591) == 28591);
        CHECK(CodepageOf(EncodingType::ANSI) == CP_ACP);
    }
    // --- batch 39: StreamEncoder new targets, odd chunk sizes ------------------
    // each encoding gets text it can map losslessly (CJK 大字符集例外)
    {
        struct Case { EncodingType t; const char* name; const char* u8;
                      size_t len; };
        Case cases[] = {
            { EncodingType::UTF32LE, "utf32le",
              "abc\xE4\xBD\xA0\xE5\xA5\xBDz", 10 },
            { EncodingType::UTF32BE, "utf32be",
              "abc\xE4\xBD\xA0\xE5\xA5\xBDz", 10 },
            { EncodingType::ShiftJIS, "sjis",
              "abc\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9Ez", 13 },  // 日本語
            { EncodingType::Big5, "big5",
              "abc\xE4\xBD\xA0\xE5\xA5\xBDz", 10 },              // 你好
            { EncodingType::KOI8R, "koi8",
              "abc\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82z", 16 },  // Привет
            { EncodingType::ISO88591, "latin1",
              "abccaf\xC3\xA9z", 9 },                            // café
        };
        for (auto& c : cases) {
            std::string u8(c.u8, c.len);
            for (size_t chunk = 1; chunk <= 13; ++chunk) {
                StreamEncoder enc(c.t);
                std::string bytes;
                for (size_t off = 0; off < u8.size(); off += chunk) {
                    size_t n = (std::min)(chunk, u8.size() - off);
                    enc.Feed(u8.data() + off, n, bytes);
                }
                enc.Flush(bytes);
                size_t skip = BomSkip(c.t);
                // decode whole buffer back (small text: no streaming needed)
                std::string body = bytes.substr(skip);
                std::string back;
                if (c.t == EncodingType::UTF32LE || c.t == EncodingType::UTF32BE) {
                    back = WideToUtf8(Utf16FromUtf32(
                        body, c.t == EncodingType::UTF32BE, false));
                } else {
                    back = WideToUtf8(Utf8ToWide(body, CodepageOf(c.t)));
                }
                bool ok = back == u8;
                if (!ok) {
                    printf("DBG %s chunk=%zu got %zu bytes:", c.name, chunk,
                           bytes.size());
                    for (size_t k = 0; k < bytes.size() && k < 32; ++k)
                        printf(" %02X", (unsigned char)bytes[k]);
                    printf("\n");
                }
                CHECK2(ok, c.name, chunk);
            }
        }
    }

    if (g_fail == 0) { printf("ALL ENCODING TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
