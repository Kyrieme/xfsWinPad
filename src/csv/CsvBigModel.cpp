// xfsWinPad - CsvBigModel 实现（批次 36）
#include "CsvBigModel.h"

#include "../core/Util.h"
#include "CsvParser.h"

namespace xfs {
namespace csv {

bool CsvBigModel::Open(const std::wstring& path) {
    Close();
    error_.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ |
                             FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error_ = L"CreateFile failed";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        ::CloseHandle(h);
        error_ = L"empty file";
        return false;
    }
    size_ = (unsigned long long)sz.QuadPart;

    // 编码判定：采样头部 64KB。UTF-16 v1 拒绝（与 BigFileModel 同边界）。
    {
        std::string sample((size_t)std::min<unsigned long long>(size_, 64 << 10), '\0');
        DWORD got = 0;
        if (!::ReadFile(h, sample.data(), (DWORD)sample.size(), &got, nullptr) ||
            got != sample.size()) {
            ::CloseHandle(h);
            error_ = L"read sample failed";
            return false;
        }
        enc_ = encoding::DetectEncoding(sample.data(), sample.size());
        if (enc_ == encoding::EncodingType::UTF16LE ||
            enc_ == encoding::EncodingType::UTF16BE) {
            ::CloseHandle(h);
            error_ = L"UTF-16 not supported in big CSV v1";
            return false;
        }
    }

    HANDLE map = ::CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map) {
        ::CloseHandle(h);
        error_ = L"CreateFileMapping failed";
        return false;
    }
    const unsigned char* base =
        (const unsigned char*)::MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        ::CloseHandle(map);
        ::CloseHandle(h);
        error_ = L"MapViewOfFile failed";
        return false;
    }
    fileH_ = h;
    mapH_ = map;
    base_ = base;
    path_ = path;
    dataStart_ = encoding::BomSkip(enc_);   // ANSI/UTF8 无 BOM → 0

    // ---- 一次索引扫描：行边界（引号态感知）+ 各候选分隔符计数 -------------
    rowOff_.clear();
    rowOff_.push_back(dataStart_);
    long long firstRow[3] = { 0, 0, 0 };    // , ; \t（引号外）
    long long rowMax[3] = { 0, 0, 0 };
    long long cur[3] = { 0, 0, 0 };
    bool inQ = false;
    bool atCellStart = true;    // 引号只在格首生效（与 Parse 一致）
    bool firstRowDone = false;
    const unsigned long long n = size_;
    for (unsigned long long i = dataStart_; i < n; ++i) {
        const unsigned char b = base_[i];
        if (inQ) {
            if (b == '"') {
                if (i + 1 < n && base_[i + 1] == '"') { ++i; continue; }   // "" 转义
                inQ = false;
            }
            continue;
        }
        if (b == '"' && atCellStart) { inQ = true; atCellStart = false; continue; }
        if (b == ',') { ++cur[0]; atCellStart = true; continue; }
        if (b == ';') { ++cur[1]; atCellStart = true; continue; }
        if (b == '\t') { ++cur[2]; atCellStart = true; continue; }
        if (b == '\n') {
            for (int k = 0; k < 3; ++k) {
                if (cur[k] > rowMax[k]) rowMax[k] = cur[k];
                cur[k] = 0;
            }
            if (!firstRowDone) {
                for (int k = 0; k < 3; ++k) firstRow[k] = rowMax[k];
                firstRowDone = true;
            }
            atCellStart = true;
            if (i + 1 < n) rowOff_.push_back(i + 1);   // 下一行起始
            continue;
        }
        if (b == '\r' && (i + 1 >= n || base_[i + 1] != '\n')) {
            // 裸 CR 行尾（CR-only 文件）；\r\n 由 \n 分支统一处理
            for (int k = 0; k < 3; ++k) {
                if (cur[k] > rowMax[k]) rowMax[k] = cur[k];
                cur[k] = 0;
            }
            if (!firstRowDone) {
                for (int k = 0; k < 3; ++k) firstRow[k] = rowMax[k];
                firstRowDone = true;
            }
            atCellStart = true;
            if (i + 1 < n) rowOff_.push_back(i + 1);
            continue;
        }
        atCellStart = false;
    }
    // 末行计数入 max（哨兵 = size_，与"末行是否带换行"无关——
    // 带换行时最后一段为空行 [start,size_)，物化后为空格，符合 RFC）
    for (int k = 0; k < 3; ++k)
        if (cur[k] > rowMax[k]) rowMax[k] = cur[k];
    rowOff_.push_back(size_);

    // 分隔符：与 ProbeDelim 同规则，但探针取自首行计数的字节级对应物。
    // 无任何分隔符的文件保持逗号。
    delim_ = L',';
    long long best = firstRow[0];
    if (firstRow[1] > best) { best = firstRow[1]; delim_ = L';'; }
    if (firstRow[2] > best) { best = firstRow[2]; delim_ = L'\t'; }
    const int dIdx = delim_ == L',' ? 0 : (delim_ == L';' ? 1 : 2);
    cols_ = (int)std::min<long long>(rowMax[dIdx] + 1, 1 << 20);

    if (RowCount() == 0) {
        error_ = L"no rows";
        Close();
        return false;
    }
    return true;
}

void CsvBigModel::Close() {
    if (base_) { ::UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapH_) { ::CloseHandle(mapH_); mapH_ = nullptr; }
    if (fileH_) { ::CloseHandle(fileH_); fileH_ = nullptr; }
    rowOff_.clear();
    rowOff_.shrink_to_fit();
    cacheRow_ = (size_t)-1;
    cacheCells_.clear();
    cols_ = 0;
    size_ = 0;
    dataStart_ = 0;
    path_.clear();
}

bool CsvBigModel::Row(size_t r, std::vector<std::wstring>& cells) const {
    if (!base_ || r >= RowCount()) return false;
    if (cacheRow_ == r) {
        cells = cacheCells_;
        return true;
    }
    if (!RowUncached(r, cells)) return false;
    cacheRow_ = r;
    cacheCells_ = cells;
    return true;
}

bool CsvBigModel::RowUncached(size_t r, std::vector<std::wstring>& cells) const {
    cells.clear();
    if (!base_ || r >= RowCount()) return false;
    unsigned long long start = rowOff_[r];
    unsigned long long end = rowOff_[r + 1];
    // 去掉行尾换行字节（\r\n 或残留单字节）
    while (end > start && (base_[end - 1] == '\n' || base_[end - 1] == '\r'))
        --end;
    if (end - start > kMaxRowBytes) end = start + kMaxRowBytes;   // 单行护栏
    std::string bytes((const char*)base_ + start, (size_t)(end - start));
    std::wstring wide = (enc_ == encoding::EncodingType::ANSI)
        ? Utf8ToWide(bytes, CP_ACP)
        : Utf8ToWide(bytes);

    std::wstring arena;
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    SplitRow(wide, delim_, arena, spans);
    cells.reserve(spans.size());
    for (const auto& sp : spans)
        cells.emplace_back(arena, sp.first, sp.second);
    return true;
}

} // namespace csv
} // namespace xfs
