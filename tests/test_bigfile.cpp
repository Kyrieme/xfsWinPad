// test_bigfile.cpp - unit tests for BigFileModel (paged mmap + sparse index)
#include "../src/bigfile/BigFileModel.h"
#include "../src/core/Util.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <string>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK2(cond, name) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, name); } } while (0)

static std::wstring TempPath(const char* tag) {
    wchar_t dir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, dir);
    static int seq = 0;
    wchar_t name[64];
    swprintf_s(name, L"bigfile_%S_%d_%d.tmp", tag, ::GetCurrentProcessId(), ++seq);
    return std::wstring(dir) + name;
}

static void WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w = 0;
    ::WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    ::CloseHandle(h);
}

static bool WaitForScan(BigFileModel& m, int timeoutMs = 10000) {
    for (int i = 0; i < timeoutMs / 20; ++i) {
        if (m.ScanDone()) return true;
        ::Sleep(20);
    }
    return false;
}

int main() {
    // --- 1. basic: 10 lines, tiny samples -----------------------------------
    {
        auto path = TempPath("basic");
        std::string content;
        for (int i = 0; i < 10; ++i) content += "line" + std::to_string(i) + "\n";
        WriteBytes(path, content);

        BigFileModel m;
        m.SetSampleBytes(16);
        m.SetViewBytes(4096);
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        CHECK(m.LineCount() == 10);
        for (int i = 0; i < 10; ++i) {
            std::string line;
            CHECK(m.GetLine(i, line));
            CHECK2(line == "line" + std::to_string(i), "basic line content");
        }
        std::string extra;
        CHECK(!m.GetLine(10, extra));   // out of range
        CHECK(!m.GetLine(9999, extra));
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 2. no trailing newline ---------------------------------------------
    {
        auto path = TempPath("noeol");
        WriteBytes(path, "alpha\nbeta\ngamma");

        BigFileModel m;
        m.SetSampleBytes(8);
        m.SetViewBytes(4096);
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        CHECK(m.LineCount() == 3);
        std::string line;
        CHECK(m.GetLine(2, line) && line == "gamma");
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 3. CRLF stripped ----------------------------------------------------
    {
        auto path = TempPath("crlf");
        WriteBytes(path, "one\r\ntwo\r\nthree\r\n");

        BigFileModel m;
        m.SetSampleBytes(8);
        m.SetViewBytes(4096);
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        CHECK(m.LineCount() == 3);
        std::string line;
        CHECK(m.GetLine(0, line) && line == "one");
        CHECK(m.GetLine(1, line) && line == "two");
        CHECK(m.GetLine(2, line) && line == "three");
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 4. empty file --------------------------------------------------------
    {
        auto path = TempPath("empty");
        WriteBytes(path, "");
        BigFileModel m;
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        CHECK(m.LineCount() == 0);
        std::string line;
        CHECK(!m.GetLine(0, line));
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 5. UTF-8 BOM excluded from line 0 ------------------------------------
    {
        auto path = TempPath("bom");
        WriteBytes(path, "\xEF\xBB\xBF" "first\nsecond\n");
        BigFileModel m;
        m.SetSampleBytes(8);
        m.SetViewBytes(4096);
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        std::string line;
        CHECK(m.GetLine(0, line) && line == "first");
        CHECK(m.GetLine(1, line) && line == "second");
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 6. ANSI (GBK) transcoded per line ------------------------------------
    {
        auto path = TempPath("ansi");
        // "中文" in GBK = D6 D0 CE C4
        std::string gbk = "\xD6\xD0\xCE\xC4 line\nmore\n";
        WriteBytes(path, gbk);
        BigFileModel m;
        m.SetSampleBytes(8);
        m.SetViewBytes(4096);
        CHECK(m.Open(path));
        CHECK(m.Encoding() == BigFileModel::Enc::Ansi);
        CHECK(WaitForScan(m));
        std::string line;
        CHECK(m.GetLine(0, line));
        CHECK2(line == "\xE4\xB8\xAD\xE6\x96\x87 line", "gbk->utf8 transcode");
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 7. long line far exceeding sampleBytes -------------------------------
    {
        auto path = TempPath("long");
        std::string content(200, 'x');
        content += "\nshort\n";
        WriteBytes(path, content);
        BigFileModel m;
        m.SetSampleBytes(16);
        m.SetViewBytes(4096);
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        CHECK(m.LineCount() == 2);
        std::string line;
        CHECK(m.GetLine(0, line));
        CHECK2(line.size() == 200, "long line intact");
        CHECK(m.GetLine(1, line) && line == "short");
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 8. random lookups across many samples (LF + CRLF mix) -----------------
    {
        auto path = TempPath("rand");
        std::string content;
        for (int i = 0; i < 5000; ++i) {
            content += "row-" + std::to_string(i) + (i % 2 ? "\r\n" : "\n");
        }
        WriteBytes(path, content);
        BigFileModel m;
        m.SetSampleBytes(1000);
        m.SetViewBytes(4096);
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        CHECK(m.LineCount() == 5000);
        int probe[] = {0, 1, 2, 2500, 4998, 4999};
        for (int idx : probe) {
            std::string line;
            CHECK(m.GetLine((unsigned long long)idx, line));
            CHECK2(line == "row-" + std::to_string(idx), "random lookup content");
        }
        for (int i = 0; i < 5000; i += 997) {
            std::string line;
            CHECK(m.GetLine((unsigned long long)i, line));
            CHECK2(line == "row-" + std::to_string(i), "stride lookup");
        }
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 9. UTF-16 rejected -----------------------------------------------------
    {
        auto path = TempPath("u16");
        std::wstring w = L"hello";
        std::string bytes("\xFF\xFE", 2);
        bytes.append((const char*)w.data(), w.size() * 2);
        WriteBytes(path, bytes);
        BigFileModel m;
        CHECK(!m.Open(path));
        CHECK(m.Error().find(L"UTF-16") != std::wstring::npos);
        m.Close();
        ::DeleteFileW(path.c_str());
    }
    // --- 10. multibyte UTF-8 across scan windows --------------------------------
    {
        auto path = TempPath("multi");
        // many 3-byte chars per line, multiple lines; tiny windows force
        // window-boundary crossings inside multibyte runs (scan counts only
        // 0x0A bytes, which are never part of a UTF-8 multibyte sequence)
        std::string content;
        for (int ln = 0; ln < 5; ++ln) {
            for (int r = 0; r < 400; ++r) content += "\xE4\xB8\xAD";   // 中
            content += "\n";
        }
        WriteBytes(path, content);
        BigFileModel m;
        m.SetSampleBytes(700);
        m.SetViewBytes(2048);
        CHECK(m.Open(path));
        CHECK(WaitForScan(m));
        CHECK(m.LineCount() == 5);
        std::string line;
        for (int ln = 0; ln < 5; ++ln) {
            CHECK(m.GetLine((unsigned long long)ln, line));
            CHECK2(line.size() == 400 * 3, "multibyte line size");
        }
        m.Close();
        ::DeleteFileW(path.c_str());
    }

    // --- 11. >2GB sparse file e2e (NTFS sparse; skips when unsupported) -------
    {
        auto path = TempPath("sparse3g");
        HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            printf("test_bigfile: sparse case SKIP (create failed)\n");
        } else {
            DWORD bytes = 0;
            BOOL ok = ::DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0,
                                        nullptr, 0, &bytes, nullptr);
            if (!ok) {
                printf("test_bigfile: sparse case SKIP (non-NTFS)\n");
                ::CloseHandle(h);
            } else {
                const unsigned long long kStep = 1ULL << 20;          // 1MB
                const unsigned long long kTotal = 3ULL << 30;         // 3GB
                LARGE_INTEGER end{};
                end.QuadPart = (LONGLONG)kTotal;
                ::SetFilePointerEx(h, end, nullptr, FILE_BEGIN);
                ::SetEndOfFile(h);

                auto writeAt = [&](unsigned long long off, const char* s) {
                    LARGE_INTEGER li{};
                    li.QuadPart = (LONGLONG)off;
                    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
                        printf("writeAt seek fail off=%llu gle=%u\n", off, ::GetLastError());
                    DWORD w = 0;
                    if (!::WriteFile(h, s, (DWORD)strlen(s), &w, nullptr))
                        printf("writeAt write fail off=%llu gle=%u\n", off, ::GetLastError());
                    else if (w != (DWORD)strlen(s))
                        printf("writeAt short write off=%llu w=%u\n", off, w);
                };
                // line 0 = "HEAD"; lines 1..N-2 = ~1MB NUL gaps each ending
                // with "L<k>"; last line = "TAIL"
                writeAt(0, "HEAD\n");
                unsigned long long nMarks = kTotal / kStep - 1;       // 3071
                for (unsigned long long k = 1; k <= nMarks; ++k) {
                    std::string mark = "L" + std::to_string(k) + "\n";
                    writeAt(k * kStep, mark.c_str());
                }
                writeAt(kTotal - 8, "\nTAIL---");
                ::CloseHandle(h);

                BigFileModel m;
                m.SetSampleBytes(64 << 10);
                m.SetViewBytes(256 << 20);
                CHECK(m.Open(path));
                CHECK(WaitForScan(m, 60000));
                unsigned long long expect = nMarks + 3;   // HEAD + 3071 marks + gap-line + TAIL
                CHECK(m.LineCount() == expect);

                std::string line;
                CHECK(m.GetLine(0, line) && line == "HEAD");
                // mark line k: content = "L<k>" — everything before it in the
                // line is the NUL gap of the previous 1MB block
                unsigned long long mid = 1500;
                CHECK(m.GetLine(mid, line));
                std::string want = "L" + std::to_string(mid);
                CHECK2(line.size() > want.size() &&
                       line.compare(line.size() - want.size(), want.size(), want) == 0,
                       "middle line ends with mark");
                // the gap line after the last mark (NULs up to the TAIL newline)
                CHECK(m.GetLine(expect - 2, line));
                CHECK2(!line.empty() && line[0] == '\0' && line.back() == '\0',
                       "gap line is NULs");
                CHECK(m.GetLine(expect - 1, line) && line == "TAIL---");

                m.Close();
                ::DeleteFileW(path.c_str());
                printf("test_bigfile: 3GB sparse case PASS (%llu lines)\n", expect);
            }
        }
    }

    printf(g_fail == 0 ? "test_bigfile: ALL PASS\n" : "test_bigfile: %d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
