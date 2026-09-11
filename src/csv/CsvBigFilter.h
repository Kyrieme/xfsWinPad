#pragma once
// xfsWinPad - CsvBigFilter: 大文件 CSV 的后台流式过滤（批次 37）。
//
// 工作线程逐行物化 + 大小写不敏感子串匹配，命中行号渐进写入共享结果表；
// UI 以计时器轮询进度/命中数并刷新虚拟列表。并发约定：
// - CsvBigModel 在 Open 之后只读（base_/rowOff_/编码/分隔符均不可变），
//   工作线程走 RowUncached()（不碰 UI 线程的单行缓存）；
// - matches_ 全部访问加锁；进度/停止为原子量。
//
// 语义对齐小文件路径 FilterRows：needle 为空 = 不过滤（不启动线程）；
// 命中 = 任意单元格含 needle（大小写不敏感）；表头行（0）不参与。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xfs {
namespace csv {

class CsvBigModel;

class CsvBigFilter {
public:
    CsvBigFilter() = default;
    ~CsvBigFilter();                       // 停止并 join
    CsvBigFilter(const CsvBigFilter&) = delete;
    CsvBigFilter& operator=(const CsvBigFilter&) = delete;

    // 启动（或以新词重启）后台扫描。needle 为空时只停止旧扫描。
    void Start(const CsvBigModel* model, std::wstring needle);
    void Stop();                           // 请求停止并 join（幂等）

    bool Running() const { return running_.load(); }
    double Progress() const;               // 0..1（扫描行数/总行数）
    size_t MatchCount() const;             // 已命中行数（渐进可用）
    size_t RowAt(size_t i) const;          // 第 i 个命中的原始行号
    const std::wstring& Needle() const { return needle_; }

private:
    void ScanLoop(const CsvBigModel* model, std::wstring needleLower);

    std::thread worker_;
    mutable std::mutex mtx_;
    std::vector<uint32_t> matches_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<unsigned long long> scanned_{ 0 };
    unsigned long long total_ = 0;         // 候选行数（不含表头）
    std::wstring needle_;
};

} // namespace csv
} // namespace xfs
