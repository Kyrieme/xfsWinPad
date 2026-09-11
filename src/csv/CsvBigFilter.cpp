// xfsWinPad - CsvBigFilter 实现（批次 37）
#include "CsvBigFilter.h"

#include "CsvBigModel.h"
#include "../core/Util.h"

#include <algorithm>
#include <cwctype>

namespace xfs {
namespace csv {

namespace {

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        else if (c >= 0x80) c = (wchar_t)towlower(c);
    }
    return s;
}

} // namespace

CsvBigFilter::~CsvBigFilter() { Stop(); }

void CsvBigFilter::Start(const CsvBigModel* model, std::wstring needle) {
    Stop();
    needle_ = needle;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        matches_.clear();
    }
    scanned_.store(0);
    if (needle.empty() || !model || model->RowCount() == 0) return;
    stop_.store(false);
    total_ = model->RowCount() - 1;   // 跳过表头行
    running_.store(true);
    worker_ = std::thread(&CsvBigFilter::ScanLoop, this, model, ToLower(needle));
}

void CsvBigFilter::Stop() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

double CsvBigFilter::Progress() const {
    if (total_ == 0) return 1.0;
    double p = (double)scanned_.load() / (double)total_;
    return std::min(1.0, p);
}

size_t CsvBigFilter::MatchCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return matches_.size();
}

size_t CsvBigFilter::RowAt(size_t i) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return i < matches_.size() ? matches_[i] : 0;
}

void CsvBigFilter::ScanLoop(const CsvBigModel* model, std::wstring needleLower) {
    const size_t rows = model->RowCount();
    std::vector<std::wstring> cells;
    std::vector<uint32_t> batch;
    batch.reserve(256);
    const size_t rowsMinus = rows ? rows - 1 : 0;
    for (size_t r = 1; r < rows; ++r) {   // 表头行（0）不参与
        if (stop_.load()) break;
        if (!model->RowUncached(r, cells)) { ++scanned_; continue; }
        bool hit = false;
        for (const auto& c : cells) {
            if (c.empty()) continue;
            std::wstring low = ToLower(c);
            if (low.find(needleLower) != std::wstring::npos) { hit = true; break; }
        }
        if (hit) {
            batch.push_back((uint32_t)r);
            if (batch.size() >= 256) {
                std::lock_guard<std::mutex> lk(mtx_);
                matches_.insert(matches_.end(), batch.begin(), batch.end());
                batch.clear();
            }
        }
        if ((r & 0xFF) == 0) scanned_.store(r - 1);   // 每 256 行报进度
    }
    if (!batch.empty()) {
        std::lock_guard<std::mutex> lk(mtx_);
        matches_.insert(matches_.end(), batch.begin(), batch.end());
    }
    scanned_.store(rowsMinus);
    running_.store(false);
}

} // namespace csv
} // namespace xfs
