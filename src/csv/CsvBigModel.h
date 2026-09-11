#pragma once
// xfsWinPad - CsvBigModel: >64MB CSV 的只读虚拟模型（批次 36）。
//
// 一次前台字节级索引扫描（RFC 4180 引号态感知）记录每行起始字节偏移；
// 显示时按需物化单行（mmap 切片 → 解码 → SplitRow）。内存 = 8B/行索引，
// 百万行文件不再要求整表进内存。
//
// v1 边界（与 BigFileModel 一致）：
// - 编码支持 UTF-8（含 BOM）与 ANSI（CP_ACP 逐行转码）；UTF-16 拒绝打开；
// - 只读：无编辑/排序/过滤/保存（面板侧降级，见 CsvPanel::LoadBig）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../encoding/Encoding.h"

namespace xfs {
namespace csv {

class CsvBigModel {
public:
    bool Open(const std::wstring& path);   // mmap + 一次索引扫描
    void Close();
    bool IsValid() const { return base_ != nullptr; }

    const std::wstring& Path() const { return path_; }
    unsigned long long FileSize() const { return size_; }
    size_t RowCount() const { return rowOff_.empty() ? 0 : rowOff_.size() - 1; }
    int ColCount() const { return cols_; }       // 各行最大格数（含表头）
    wchar_t Delim() const { return delim_; }
    encoding::EncodingType Encoding() const { return enc_; }
    const std::wstring& Error() const { return error_; }

    // 物化行 r（0 起，含表头行）→ 宽字符单元格序列（RFC 4180 切分）。
    // UI 线程顺序访问，内置单行缓存。
    bool Row(size_t r, std::vector<std::wstring>& cells) const;

    // 无缓存物化（批次 37）：供后台过滤线程使用——不读写单行缓存，
    // 只依赖 Open 之后不可变的映射/索引/编码状态，可安全并发。
    bool RowUncached(size_t r, std::vector<std::wstring>& cells) const;

    static constexpr unsigned long long kMaxRowBytes = 4ULL << 20;   // 单行护栏

private:
    std::wstring path_;
    std::wstring error_;
    void* fileH_ = nullptr;    // HANDLE
    void* mapH_ = nullptr;     // HANDLE
    const unsigned char* base_ = nullptr;
    unsigned long long size_ = 0;
    unsigned long long dataStart_ = 0;   // BOM 之后
    std::vector<unsigned long long> rowOff_;   // 行起始偏移 + 哨兵 size_
    int cols_ = 0;
    wchar_t delim_ = L',';
    encoding::EncodingType enc_ = encoding::EncodingType::UTF8;

    // 单行缓存（仅 UI 线程访问，无锁）
    mutable size_t cacheRow_ = (size_t)-1;
    mutable std::vector<std::wstring> cacheCells_;
};

} // namespace csv
} // namespace xfs
