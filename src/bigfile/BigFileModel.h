#pragma once
// xfsWinPad - BigFileModel: paged read-only access to arbitrarily large text
// files (beyond the 2GB Scintilla cell-buffer limit).
//
// Design (TODO "Next batch" #2):
// - mmap is paged: only a moving window of bytes is mapped at any time, so
//   files of any size open instantly.
// - a sparse line index records {byte offset, line number} every sampleBytes_
//   bytes during one background full-file scan; random line lookup is a
//   binary search plus a bounded forward scan (<= sampleBytes_ + one line).
// - encoding: UTF-8 (incl. BOM) and ANSI (CP_ACP per-line transcode).
//   UTF-16 is rejected in v1 (clear error, files stay openable after
//   conversion).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xfs {

class BigFileModel {
public:
    BigFileModel() = default;
    ~BigFileModel() { Close(); }
    BigFileModel(const BigFileModel&) = delete;
    BigFileModel& operator=(const BigFileModel&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    bool IsValid() const { return fileH_ != nullptr; }

    const std::wstring& Path() const { return path_; }
    unsigned long long FileSize() const { return fileSize_; }

    enum class Enc { Utf8, Ansi };
    Enc Encoding() const { return enc_; }
    const std::wstring& Error() const { return error_; }

    // line count discovered so far; final once ScanDone()
    unsigned long long LineCount() const;
    bool ScanDone() const;
    double ScanProgress() const;                 // 0..1
    unsigned long long LongestLine() const;      // bytes between line breaks (approx)

    // materialize one line (no trailing newline; trailing '\r' stripped).
    // ANSI files are transcoded per line via CP_ACP -> UTF-8.
    // Lines longer than kMaxLineBytes are truncated (with no marker in v1).
    bool GetLine(unsigned long long lineNo, std::string& utf8Out) const;

    // test hooks (must be called before Open)
    void SetSampleBytes(unsigned long long n) { sampleBytes_ = n; }
    void SetViewBytes(unsigned long long n) { viewBytes_ = n; }

    static constexpr unsigned long long kMaxLineBytes = 4ULL << 20;

private:
    struct Sample {
        unsigned long long byteOff;
        unsigned long long lineNo;   // # of '\n' in [0, byteOff)
    };

    const unsigned char* MapWindow(unsigned long long fileOff, size_t* avail) const;
    void DropCaches() const;
    void ScanLoop();

    std::wstring path_;
    std::wstring error_;
    void* fileH_ = nullptr;         // HANDLE
    void* mapping_ = nullptr;       // HANDLE
    unsigned long long fileSize_ = 0;
    Enc enc_ = Enc::Utf8;

    unsigned long long sampleBytes_ = 64ULL << 10;
    unsigned long long viewBytes_ = 256ULL << 20;

    mutable std::mutex mtx_;
    std::vector<Sample> samples_;
    unsigned long long lineCount_ = 0;
    unsigned long long scanned_ = 0;
    unsigned long long longestLine_ = 0;
    bool done_ = false;

    std::thread scanThread_;
    std::atomic<bool> stop_{ false };

    // UI-thread paged read cache (scan thread maps its own windows)
    struct View {
        unsigned long long off = 0;
        const unsigned char* p = nullptr;
        size_t len = 0;
    };
    static constexpr int kViewCache = 3;
    mutable View cache_[kViewCache];
    mutable int cacheNext_ = 0;
};

} // namespace xfs
