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

    // 打开失败的原因。本层**只说"是哪一类"，不产出人类可读文本** ——
    // 文案由展示层按语言键取（Tr），内核不依赖 I18n（与 language/ 各内核同口径）。
    // 失败类别是**枚举而不是字符串**，否则换语言时这里的中文会跟着漏进界面。
    enum class Err {
        None = 0,   // 未失败
        OpenFile,   // CreateFileW 失败
        FileSize,   // GetFileSizeEx 失败
        MapFailure, // CreateFileMappingW 失败
        Utf16,      // 文件头探测到 UTF-16，v1 不支持
    };
    Err Error() const { return err_; }
    // 语言无关的诊断名（写日志、基准工具用；不进界面）
    static const char* ErrName(Err e);

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
    Err err_ = Err::None;
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
