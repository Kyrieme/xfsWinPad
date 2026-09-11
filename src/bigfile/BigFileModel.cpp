#include "BigFileModel.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../encoding/Encoding.h"

#include <algorithm>
#include <atomic>

namespace xfs {

namespace {

constexpr unsigned long long kAllocGran = 64ULL << 10;   // MapViewOfFile granularity

unsigned long long AlignDown(unsigned long long v, unsigned long long a) {
    return v & ~(a - 1);
}

// MapViewOfFile offset must be DWORD-split; sizes are SIZE_T.
const unsigned char* RawMapView(void* mapping, unsigned long long off, size_t len) {
    ULARGE_INTEGER u;
    u.QuadPart = off;
    auto* p = static_cast<const unsigned char*>(
        ::MapViewOfFile(mapping, FILE_MAP_READ, u.HighPart, u.LowPart, len));
    return p;
}

} // namespace

bool BigFileModel::Open(const std::wstring& path) {
    Close();
    error_.clear();

    fileH_ = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileH_ == INVALID_HANDLE_VALUE) {
        fileH_ = nullptr;
        error_ = L"无法打开文件 (CreateFile)";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(fileH_, &sz)) {
        error_ = L"无法读取文件大小";
        Close();
        return false;
    }
    fileSize_ = static_cast<unsigned long long>(sz.QuadPart);

    if (fileSize_ == 0) {
        // empty file: valid, zero lines, no mapping possible (gle=1006)
        enc_ = Enc::Utf8;
        path_ = path;
        std::lock_guard<std::mutex> lk(mtx_);
        samples_.assign(1, {0, 0});
        lineCount_ = 0;
        scanned_ = 0;
        longestLine_ = 0;
        done_ = true;
        return true;
    }
    mapping_ = ::CreateFileMappingW(static_cast<HANDLE>(fileH_), nullptr,
                                    PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) {
        error_ = L"创建文件映射失败";
        Close();
        return false;
    }

    // encoding probe on the head of the file (first view worth of bytes)
    Enc enc = Enc::Utf8;
    if (fileSize_ > 0) {
        size_t probeLen = static_cast<size_t>(
            std::min<unsigned long long>(fileSize_, 4ULL << 20));
        auto* probe = RawMapView(mapping_, 0, probeLen);
        if (probe) {
            auto det = encoding::DetectEncoding(reinterpret_cast<const char*>(probe), probeLen);
            if (det == encoding::EncodingType::UTF16LE ||
                det == encoding::EncodingType::UTF16BE) {
                ::UnmapViewOfFile(probe);
                error_ = L"大文件查看器 v1 暂不支持 UTF-16，请先转换为 UTF-8 或 ANSI";
                Close();
                return false;
            }
            if (det == encoding::EncodingType::ANSI)
                enc = Enc::Ansi;
            else if (det == encoding::EncodingType::UTF8BOM)
                enc = Enc::Utf8;   // BOM handled by skipping 3 bytes below
            ::UnmapViewOfFile(probe);
        }
    }
    enc_ = enc;

    // BOM skip: sample 0 starts after the UTF-8 BOM
    unsigned long long startOff = 0;
    if (fileSize_ >= 3) {
        auto* head = RawMapView(mapping_, 0, 3);
        if (head && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF)
            startOff = 3;
        if (head) ::UnmapViewOfFile(head);
    }

    path_ = path;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        samples_.clear();
        lineCount_ = 0;
        scanned_ = 0;
        longestLine_ = 0;
        done_ = false;
        samples_.push_back({startOff, 0});
    }
    stop_ = false;
    scanThread_ = std::thread(&BigFileModel::ScanLoop, this);
    return true;
}

void BigFileModel::Close() {
    stop_ = true;
    if (scanThread_.joinable())
        scanThread_.join();
    DropCaches();
    if (mapping_) { ::CloseHandle(static_cast<HANDLE>(mapping_)); mapping_ = nullptr; }
    if (fileH_) { ::CloseHandle(static_cast<HANDLE>(fileH_)); fileH_ = nullptr; }
    fileSize_ = 0;
    path_.clear();
}

void BigFileModel::DropCaches() const {
    for (auto& v : cache_) {
        if (v.p) { ::UnmapViewOfFile(v.p); v.p = nullptr; }
        v.off = 0; v.len = 0;
    }
    cacheNext_ = 0;
}

// Maps a window covering fileOff (64KB-aligned start), returns pointer to the
// byte at fileOff and the number of valid bytes from there to the window end.
const unsigned char* BigFileModel::MapWindow(unsigned long long fileOff, size_t* avail) const {
    if (fileOff >= fileSize_) { *avail = 0; return nullptr; }
    unsigned long long win = AlignDown(fileOff, kAllocGran);
    // window must cover fileOff: allocation granularity may push the aligned
    // start up to 64KB behind it, so extend the length accordingly
    unsigned long long len = (fileOff - win) + viewBytes_;
    len = std::min<unsigned long long>(len, fileSize_ - win);

    for (int i = 0; i < kViewCache; ++i) {
        auto& v = cache_[i];
        if (v.p && fileOff >= v.off && fileOff < v.off + v.len) {
            *avail = static_cast<size_t>(v.off + v.len - fileOff);
            return v.p + (fileOff - v.off);
        }
    }
    auto& slot = cache_[cacheNext_];
    cacheNext_ = (cacheNext_ + 1) % kViewCache;
    if (slot.p) ::UnmapViewOfFile(slot.p);
    slot.p = RawMapView(mapping_, win, static_cast<size_t>(len));
    if (!slot.p) { *avail = 0; return nullptr; }
    slot.off = win;
    slot.len = static_cast<size_t>(len);
    *avail = static_cast<size_t>(win + len - fileOff);
    return slot.p + (fileOff - win);
}

unsigned long long BigFileModel::LineCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return lineCount_;
}

bool BigFileModel::ScanDone() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return done_;
}

double BigFileModel::ScanProgress() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fileSize_ == 0) return 1.0;
    return static_cast<double>(scanned_) / static_cast<double>(fileSize_);
}

unsigned long long BigFileModel::LongestLine() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return longestLine_;
}

void BigFileModel::ScanLoop() {
    // Sequential paged scan: one mapped window at a time. Counts '\n' bytes,
    // records a {offset, lineNo} sample each sampleBytes_ boundary and the
    // longest line seen. Uses its own views (never the UI cache).
    unsigned long long nextSample;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        nextSample = AlignDown(samples_[0].byteOff + sampleBytes_, sampleBytes_);
    }
    unsigned long long pos = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pos = samples_[0].byteOff;
    }
    unsigned long long line = 0;          // complete lines before pos
    unsigned long long lineStart = pos;
    unsigned long long longest = 0;

    while (pos < fileSize_ && !stop_) {
        unsigned long long win = AlignDown(pos, kAllocGran);
        size_t len = static_cast<size_t>(std::min<unsigned long long>(
            (pos - win) + viewBytes_, fileSize_ - win));
        auto* base = RawMapView(mapping_, win, len);
        if (!base) break;
        size_t off = static_cast<size_t>(pos - win);
        const unsigned char* p = base + off;
        size_t remain = len - off;

        for (;;) {
            const void* hit = ::memchr(p, '\n', remain);
            size_t nlPos = hit ? static_cast<size_t>(static_cast<const unsigned char*>(hit) - p)
                               : remain;
            // newline positions may carry samples inside the line they close
            while (nextSample <= pos + nlPos && nextSample <= fileSize_) {
                unsigned long long so = nextSample;
                std::lock_guard<std::mutex> lk(mtx_);
                samples_.push_back({so, line});
                nextSample += sampleBytes_;
            }
            if (!hit) {
                pos += nlPos;
                break;
            }
            unsigned long long lineLen = pos + nlPos - lineStart;
            if (lineLen > longest) longest = lineLen;
            ++line;
            pos += nlPos + 1;
            lineStart = pos;
            p += nlPos + 1;
            remain -= nlPos + 1;
            if (remain == 0) break;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            scanned_ = pos;
            longestLine_ = longest;
            // partial trailing line not counted until the final update
            lineCount_ = line + (pos >= fileSize_ && pos > lineStart ? 1 : 0);
        }
        ::UnmapViewOfFile(base);
    }

    if (!stop_) {
        std::lock_guard<std::mutex> lk(mtx_);
        // EOF: final line without trailing newline counts as one line
        if (fileSize_ > 0 && pos >= fileSize_ && pos > lineStart)
            lineCount_ = line + 1;
        else
            lineCount_ = line;
        longestLine_ = longest;
        scanned_ = fileSize_;
        done_ = true;
    }
}

bool BigFileModel::GetLine(unsigned long long lineNo, std::string& utf8Out) const {
    utf8Out.clear();
    if (!IsValid() || fileSize_ == 0) return false;

    Sample lo{0, 0};
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (lineNo >= lineCount_) return false;
        // last sample with lineNo < target: scanning forward from it crosses
        // (target - lineNo) newlines and lands exactly at the line start.
        // Samples with lineNo == target sit mid-line and must not anchor us.
        auto it = std::lower_bound(samples_.begin(), samples_.end(), lineNo,
            [](const Sample& s, unsigned long long n) { return s.lineNo < n; });
        if (it == samples_.begin()) lo = samples_.front();
        else lo = *(it - 1);
    }

    // forward scan from lo: count newlines until lineNo newlines seen, then
    // capture until the next newline (paged reads)
    unsigned long long pos = lo.byteOff;
    unsigned long long seen = lo.lineNo;
    unsigned long long lineStart = 0;
    bool haveStart = false;
    std::string out;

    while (pos < fileSize_) {
        size_t avail = 0;
        auto* p = MapWindow(pos, &avail);
        if (!p || avail == 0) break;
        size_t cap = static_cast<size_t>(std::min<unsigned long long>(
            avail, fileSize_ - pos));
        const unsigned char* q = p;
        size_t remain = cap;
        for (;;) {
            const void* hit = ::memchr(q, '\n', remain);
            size_t nlPos = hit ? static_cast<size_t>(static_cast<const unsigned char*>(hit) - q)
                               : remain;
            if (!haveStart) {
                if (seen == lineNo) {
                    lineStart = pos + static_cast<unsigned long long>(cap - remain);
                    haveStart = true;
                } else if (hit) {
                    ++seen;
                }
            }
            if (haveStart) {
                size_t room = static_cast<size_t>(kMaxLineBytes - out.size());
                size_t add = std::min(nlPos, room);
                out.append(reinterpret_cast<const char*>(q), add);
                if (hit) {
                    pos = fileSize_;   // done
                    break;
                }
                if (out.size() >= kMaxLineBytes) {
                    pos = fileSize_;
                    break;
                }
            }
            if (!hit) {
                pos += nlPos;
                break;
            }
            // newline found while still counting
            q += nlPos + 1;
            remain -= nlPos + 1;
            pos += nlPos + 1;
            if (remain == 0) break;
        }
    }

    if (!haveStart) return false;
    if (!out.empty() && out.back() == '\r') out.pop_back();

    if (enc_ == Enc::Ansi) {
        int wlen = ::MultiByteToWideChar(CP_ACP, 0, out.data(), (int)out.size(), nullptr, 0);
        std::wstring wide((size_t)std::max(wlen, 0), L'\0');
        if (wlen > 0)
            ::MultiByteToWideChar(CP_ACP, 0, out.data(), (int)out.size(), &wide[0], wlen);
        utf8Out = WideToUtf8(wide);
    } else {
        utf8Out = std::move(out);
    }
    return true;
}

} // namespace xfs
