// xfsbench.cpp - offline benchmark for xfsWinPad core data-path layers.
//
// Measures, on synthetic data generated under %TEMP%\xfsbench:
//   readfile_seq      classic CreateFileW+ReadFile loop        (MB/s)
//   mmap_touch        MappedFile open + touch every page      (MB/s)
//   detect_encoding   encoding::DetectEncoding over the file  (MB/s)
//   valid_utf8        encoding::IsValidUtf8 over the file     (MB/s)
//   utf32_roundtrip   Utf32FromUtf16 + Utf16FromUtf32         (MB/s)
//   bigfile_scan      BigFileModel background index scan      (MB/s)
//   bigfile_getline   random BigFileModel::GetLine            (ns/line)
//   csv_scan          CsvBigModel::Open index scan            (MB/s)
//   csv_row           random CsvBigModel::Row materialization (us/row)
//
// Usage: xfsbench [--size-mb N] [--csv] [--quick]
//   --size-mb N   text/CSV data size in MB (default 64, min 8)
//   --csv         machine output only: "xfsbench,<name>,<value>,<unit>" lines
//   --quick       run with 16 MB and reduced iterations
//
// Deterministic (xorshift64 seed fixed). Never part of ctest; run manually
// or from .github/workflows/ci.yml.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/bigfile/BigFileModel.h"
#include "../src/core/Util.h"
#include "../src/csv/CsvBigModel.h"
#include "../src/encoding/Encoding.h"

namespace {

// ---------------------------------------------------------------- timing --
double gFreq = 0.0;

struct Timer {
    LARGE_INTEGER t0;
    Timer() { Start(); }
    void Start() { QueryPerformanceCounter(&t0); }
    double Ms() const {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        return (t1.QuadPart - t0.QuadPart) * 1000.0 / gFreq;
    }
};

// ---------------------------------------------------------------- output --
bool gCsv = false;

// Human-readable progress lines; in --csv mode they go to stderr so stdout
// stays purely machine-parseable (ci.yml tees stdout into benchmark.csv).
void Info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (gCsv) {
        vfprintf(stderr, fmt, args);
    } else {
        vprintf(fmt, args);
    }
    va_end(args);
}

void Report(const char* name, double value, const char* unit) {
    char line[256];
    if (gCsv) {
        snprintf(line, sizeof(line), "xfsbench,%s,%.2f,%s", name, value, unit);
    } else {
        snprintf(line, sizeof(line), "[bench] %-18s %10.2f %s", name, value, unit);
    }
    printf("%s\n", line);
    fflush(stdout);
}

// ------------------------------------------------------------ utils --
unsigned long long gRng = 0x9E3779B97F4A7C15ull;

unsigned long long NextRand() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return gRng;
}

std::wstring TempDir() {
    wchar_t p[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, p);
    std::wstring dir = std::wstring(p, n) + L"xfsbench";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Synthetic UTF-8 line: ASCII code + CJK tokens (raw UTF-8 bytes below).
const unsigned char kZh1[] = {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87};          // CJK token 1
const unsigned char kZh2[] = {0xE6, 0xB5, 0x8B, 0xE8, 0xAF, 0x95};          // CJK token 2

std::string MakeTextChunk() {
    // ~16 lines, 40..130 bytes each, mixed ASCII + CJK, '\n' terminated.
    std::string chunk;
    char ascii[160];
    for (int i = 0; i < 16; i++) {
        int n = snprintf(ascii, sizeof(ascii),
                         "int v%d = compute(arg%d, %d); // ", i, i, i * 7);
        chunk.append(ascii, n);
        chunk.append(reinterpret_cast<const char*>(kZh1), sizeof(kZh1));
        n = snprintf(ascii, sizeof(ascii), " token_%d ", i * 31);
        chunk.append(ascii, n);
        chunk.append(reinterpret_cast<const char*>(kZh2), sizeof(kZh2));
        n = snprintf(ascii, sizeof(ascii), " tail%d.pad%08d\n", i, i);
        chunk.append(ascii, n);
    }
    return chunk;
}

std::string MakeCsvRowChunk(unsigned long long seed) {
    // One CSV row with quoted fields containing commas and escaped quotes.
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "\"f%d,q%d\",\"v \\\"q\\\" %d\",%d.%02d,2026-09-%02d,host_%d\n",
                     (int)(seed % 97), (int)(seed % 89), (int)(seed % 1000),
                     (int)(seed % 5000), (int)(seed % 100), (int)(seed % 28) + 1,
                     (int)(seed % 13));
    return std::string(buf, n);
}

// Write `data` to `path` (direct CreateFileW + WriteFile, overwrite).
bool WriteOut(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

// Ensure a file of exactly `size` bytes exists; regenerate if absent/short.
bool EnsureFile(const std::wstring& path, unsigned long long size,
                const std::string& chunk) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        unsigned long long hi = fa.nFileSizeHigh;
        if ((hi << 32) + fa.nFileSizeLow == size) return true;
    }
    std::string data;
    data.reserve((size_t)size + chunk.size());
    unsigned long long i = 0;
    unsigned long long rowSeed = 1;
    while (i < size) {
        unsigned long long take = std::min<unsigned long long>(chunk.size(), size - i);
        data.append(chunk, 0, (size_t)take);
        i += take;
        if (data.size() >= size) break;
        // For CSV keep rows uniform; for text the chunk ends with '\n' anyway.
        if (path.size() > 4 && path.compare(path.size() - 4, 4, L".csv") == 0) {
            data.append(MakeCsvRowChunk(rowSeed++));
        }
    }
    // Trim to exact size at a line boundary (last '\n'), then pad with a
    // filler line so the file is EXACTLY `size` bytes with complete lines.
    data.resize((size_t)size);
    size_t lastNl = data.find_last_of('\n');
    if (lastNl != std::string::npos && lastNl + 1 < data.size()) {
        data.resize(lastNl + 1);
    }
    if (data.size() < (size_t)size) {
        data.append((size_t)size - data.size() - 1, 'x');
        data += '\n';
    }
    return WriteOut(path, data);
}

// Best-of-k throughput for a body that processes the whole buffer.
// Body returns false on failure (abort).
template <typename Body>
bool Throughput(const char* name, int iters, size_t bytes, Body body) {
    double bestMs = 1e30;
    for (int i = 0; i < iters; i++) {
        Timer t;
        if (!body()) {
            fprintf(stderr, "[FAIL] %s: body returned false\n", name);
            return false;
        }
        double ms = t.Ms();
        if (ms < bestMs) bestMs = ms;
    }
    double mbps = (bytes / (1024.0 * 1024.0)) / (bestMs / 1000.0);
    Report(name, mbps, "MB/s");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    gFreq = (double)f.QuadPart;

    unsigned long long sizeMb = 64;
    int iters = 3;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--csv") {
            gCsv = true;
        } else if (a == "--quick") {
            sizeMb = 16;
            iters = 2;
        } else if (a == "--size-mb" && i + 1 < argc) {
            sizeMb = (unsigned long long)atoll(argv[++i]);
            if (sizeMb < 8) sizeMb = 8;
        } else {
            fprintf(stderr, "usage: xfsbench [--size-mb N] [--csv] [--quick]\n");
            return 2;
        }
    }

    const std::wstring dir = TempDir();
    const std::wstring textPath = dir + L"\\bench_text_u8.txt";
    const std::wstring csvPath = dir + L"\\bench_data.csv";
    const unsigned long long textSize = sizeMb << 20;
    const unsigned long long csvSize = std::max<unsigned long long>(sizeMb >> 1, 8) << 20;

    Info("[xfsbench] data dir: %ls\n", dir.c_str());
    Info("[xfsbench] text %llu MB, csv %llu MB, iters %d\n",
         textSize >> 20, csvSize >> 20, iters);

    Timer gen;
    if (!EnsureFile(textPath, textSize, MakeTextChunk())) {
        fprintf(stderr, "[FAIL] cannot generate text file\n");
        return 1;
    }
    if (!EnsureFile(csvPath, csvSize, MakeCsvRowChunk(1))) {
        fprintf(stderr, "[FAIL] cannot generate csv file\n");
        return 1;
    }
    Info("[xfsbench] data ready in %.0f ms\n\n", gen.Ms());

    // ------------------------------------------------ readfile_seq ----------
    {
        std::string raw;
        raw.resize((size_t)textSize);
        std::vector<char> buf(1 << 20);   // heap: a 1MB stack buffer overflows
        if (!Throughput("readfile_seq", iters, (size_t)textSize, [&]() {
                HANDLE h = CreateFileW(textPath.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                if (h == INVALID_HANDLE_VALUE) return false;
                size_t off = 0;
                DWORD got = 0;
                while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0) {
                    memcpy(raw.data() + off, buf.data(), got);
                    off += got;
                }
                CloseHandle(h);
                return off == (size_t)textSize;
            }))
            return 1;
    }

    // ------------------------------------------------ mmap_touch ------------
    {
        if (!Throughput("mmap_touch", iters, (size_t)textSize, [&]() {
                xfs::MappedFile mf;
                if (!mf.Open(textPath) || !mf.IsValid()) return false;
                unsigned long long sum = 0;
                const char* p = mf.Data();
                size_t n = mf.Size();
                // Touch every 4KB page (representative page-in cost).
                for (size_t i = 0; i < n; i += 4096) sum += (unsigned char)p[i];
                if (n && sum == 0xDEADBEEFull) return false; // keep work observable
                mf.Close();
                return true;
            }))
            return 1;
    }

    // Whole buffer in memory for the CPU-path benches.
    std::string raw;
    if (!xfs::ReadFileBytes(textPath, raw) || raw.size() != (size_t)textSize) {
        fprintf(stderr, "[FAIL] cannot read generated text back\n");
        return 1;
    }

    // ------------------------------------------------ detect_encoding -------
    if (!Throughput("detect_encoding", iters, raw.size(), [&]() {
            volatile int e = (int)xfs::encoding::DetectEncoding(raw.data(), raw.size());
            return e >= 0;
        }))
        return 1;

    // ------------------------------------------------ valid_utf8 ------------
    if (!Throughput("valid_utf8", iters, raw.size(), [&]() {
            return xfs::encoding::IsValidUtf8(raw.data(), raw.size());
        }))
        return 1;

    // ------------------------------------------------ utf32_roundtrip -------
    {
        const size_t sample = std::min<size_t>(raw.size(), 8u << 20);
        std::wstring wide = xfs::Utf8ToWide(raw.substr(0, sample));
        std::string u32le;
        std::wstring back;
        if (!Throughput("utf32_roundtrip", iters, sample, [&]() {
                u32le = xfs::encoding::Utf32FromUtf16(wide, false);
                back = xfs::encoding::Utf16FromUtf32(u32le, false, true);
                return back == wide;
            }))
            return 1;
    }

    // ------------------------------------------------ bigfile_scan ----------
    unsigned long long lineCount = 0;
    {
        Timer t;
        xfs::BigFileModel model;
        model.Open(textPath);
        if (!model.IsValid()) {
            fprintf(stderr, "[FAIL] BigFileModel open: %ls\n", model.Error().c_str());
            return 1;
        }
        while (!model.ScanDone()) Sleep(10);
        double ms = t.Ms();
        lineCount = model.LineCount();
        double mbps = ((double)model.FileSize() / (1024.0 * 1024.0)) / (ms / 1000.0);
        Report("bigfile_scan", mbps, "MB/s");
        if (lineCount == 0) {
            fprintf(stderr, "[FAIL] BigFileModel found no lines\n");
            return 1;
        }

        // -------------------------------------------- bigfile_getline -----
        const int kGets = 2000;
        Timer tg;
        unsigned long long hits = 0, totalLen = 0;
        for (int i = 0; i < kGets; i++) {
            unsigned long long lineNo = NextRand() % lineCount;
            std::string line;
            if (model.GetLine(lineNo, line)) {
                hits++;
                totalLen += line.size();
            }
        }
        double nsPer = tg.Ms() * 1e6 / kGets;
        Info("[xfsbench] bigfile_getline hits=%llu/%d avglen=%.0f\n",
             hits, kGets, (double)totalLen / (hits ? hits : 1));
        if (hits * 100 < kGets * 99) {
            fprintf(stderr, "[FAIL] GetLine success rate below 99%%\n");
            return 1;
        }
        Report("bigfile_getline", nsPer, "ns/line");
    }

    // ------------------------------------------------ csv_scan --------------
    {
        Timer t;
        xfs::csv::CsvBigModel csv;
        csv.Open(csvPath);
        if (!csv.IsValid()) {
            fprintf(stderr, "[FAIL] CsvBigModel open: %ls\n", csv.Error().c_str());
            return 1;
        }
        double ms = t.Ms();
        double mbps = ((double)csv.FileSize() / (1024.0 * 1024.0)) / (ms / 1000.0);
        Report("csv_scan", mbps, "MB/s");
        size_t rows = csv.RowCount();
        Info("[xfsbench] csv rows=%zu cols=%d\n", rows, csv.ColCount());
        if (rows == 0) {
            fprintf(stderr, "[FAIL] CsvBigModel found no rows\n");
            return 1;
        }

        // -------------------------------------------- csv_row -------------
        const int kRows = 1000;
        Timer tr;
        unsigned long long cells = 0;
        for (int i = 0; i < kRows; i++) {
            size_t r = (size_t)(NextRand() % rows);
            std::vector<std::wstring> row;
            if (csv.Row(r, row)) cells += row.size();
        }
        double usPer = tr.Ms() * 1000.0 / kRows;
        if (cells == 0) {
            fprintf(stderr, "[FAIL] no CSV cells materialized\n");
            return 1;
        }
        Report("csv_row", usPer, "us/row");
    }

    Info("\n[xfsbench] done\n");
    return 0;
}
