#pragma once
// StdfFile.h — STDF v4 只读解析层（Phase 20）。
//
// 职责：把一个 .std/.stdf 文件 mmap 后单遍扫描，产出三块数据：
//   1. 记录索引  records_（offset/typ/sub/len）——原始记录 Tab 数据源；
//   2. 文件摘要  summary_（FAR/MIR/MRR/PCR/PRR/WIR/WRR/HBR/SBR 统计）；
//   3. 测试项聚合  tests_（PTR/MPR 按 TEST_NUM 去重的 min/max/mean/fail）。
//
// 设计约束：
//   * 无窗口依赖（可单测）；mmap 只读、零拷贝，扫描全程 O(N) 单遍；
//   * 未知 REC_TYP/SUB 按 REC_LEN 跳过（真样本存在非标记录，必须容忍）；
//   * 字段读取统一走 Reader（端序按 FAR 的 CPU_TYPE），越界返回 0/false；
//   * 文本列存 StrSlice（offset+len 指向 mmap 区），避免数百万字符串；
//   * part/good 计数以 PRR 为准（每 part 一条）；无 PRR 时回退 PCR 汇总。
//
// STDF v4 记录类型（REC_TYP/REC_SUB，十进制）：
//   FAR 0/10  ATR 0/20 | MIR 1/10  MRR 1/20  PCR 1/30  HBR 1/40  SBR 1/50
//   PMR 1/60  PGR 1/62  PLR 1/63  TSR 1/70 | WIR 2/10  WRR 2/20  WCR 2/30
//   PIR 5/10  PRR 5/20 | PTR 15/10  MPR 15/15  FTR 15/20  SDR 15/81
//   GDR 20/10  DTR 20/30 | BPS 50/10  EPS 60/10
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>
#include "../core/Util.h"

namespace xfs {
namespace stdf {

// mmap 区字节切片（StdfFile 存活期间有效）
struct StrSlice {
    uint32_t offset = 0;
    uint16_t len = 0;          // 0 = 不存在
    bool empty() const { return len == 0; }
};

// 记录索引条目（原始记录 Tab 数据源）
struct RecordIndexEntry {
    uint32_t offset;   // 记录头（REC_LEN 字段）偏移
    uint16_t len;      // REC_LEN（不含自身 4B 头）
    uint8_t  typ;
    uint8_t  sub;
};

// per-site 计数（PRR/PCR）
struct SiteCount {
    uint8_t  site = 0;
    uint32_t partCount = 0;
    uint32_t goodCount = 0;
};

// HBR/SBR bin 计数
struct BinCount {
    uint16_t num = 0;
    uint32_t count = 0;
    uint8_t  pf = 0;           // bit0/1: pass/fail 位（规范 bit3=pass? v1 存原始字节）
    StrSlice name;
};

// 一次扫描的全部产物
struct StdfSummary {
    // FAR
    uint8_t  cpuType = 0;      // 1/2 小端，4/6 大端
    uint8_t  stdfVersion = 0;  // 应为 4
    // MIR/MRR 摘要
    StrSlice lotId, partTyp, nodeNam, jobNam, jobRev, operNam, testCod;
    std::string startT;        // MIR START_T（本地时间字符串）
    std::string finishT;       // MRR FINISH_T
    StrSlice waferId;          // WRR WAFER_ID（有多片时为首片）
    // 统计
    uint32_t recCount = 0;
    uint32_t ptrCount = 0, mprCount = 0, ftrCount = 0;
    uint32_t pirCount = 0, prrCount = 0;
    uint32_t wirCount = 0, wrrCount = 0;
    uint32_t gdrCount = 0, dtrCount = 0;
    uint32_t partCount = 0, goodCount = 0;
    uint64_t dataSize = 0;
    std::vector<SiteCount> sites;
    std::vector<BinCount> hbins, sbins;
    // 未知记录直方图（诊断：typ<<8|sub → count）
    std::unordered_map<uint32_t, uint32_t> unknown;
};

// 测试项聚合（PTR/MPR）
struct TestItem {
    int64_t  testNum = 0;
    int      headSite = -1;        // 首见 HEAD*256+SITE
    uint8_t  kind = 0;             // 'P'=PTR 'M'=MPR
    StrSlice testTxt;
    StrSlice alarmId;
    StrSlice units;
    double   lo = 0, hi = 0;
    bool     hasLo = false, hasHi = false;
    double   minV = 0, maxV = 0, sumV = 0;
    bool     validSeen_ = false;    // min/max 是否已有有效样本（无效结果不进 min/max）
    uint32_t count = 0;            // PTR: 记录数；MPR: 记录数
    uint32_t failCount = 0;        // PTR: fail 记录数；MPR: 累计失败 pin 数
    double   Mean() const { return count ? sumV / count : 0; }
};

// 一颗芯片（PIR..PRR 区间）的完整测试记录（datalog 表行）
struct PartRow {
    uint32_t index = 0;            // 第几颗（PRR 顺序，0 起）
    uint8_t  site = 0;
    int16_t  x = -32768, y = -32768;   // 坐标（0x8000 = 无效）
    uint16_t hbin = 0, sbin = 0;
    bool     pass = false;
    uint32_t testMs = 0;           // PRR.TEST_T 测试耗时（毫秒）
    std::vector<float> results;    // 每 TestItem 一格；NaN=无效/缺失
};


// ---- Reader：端序感知的字段读取（越界一律返回 0/false）--------------------------
class Reader {
public:
    Reader() = default;
    Reader(const char* base, size_t size, bool littleEndian)
        : base_(base), size_(size), le_(littleEndian) {}

    bool U1(size_t off, uint8_t* v) const {
        if (off >= size_) return false;
        *v = static_cast<uint8_t>(base_[off]);
        return true;
    }
    bool U2(size_t off, uint16_t* v) const {
        if (off + 2 > size_) return false;
        uint8_t a = static_cast<uint8_t>(base_[off]);
        uint8_t b = static_cast<uint8_t>(base_[off + 1]);
        *v = le_ ? static_cast<uint16_t>(a | (b << 8))
                 : static_cast<uint16_t>((a << 8) | b);
        return true;
    }
    bool U4(size_t off, uint32_t* v) const {
        if (off + 4 > size_) return false;
        uint8_t b0 = static_cast<uint8_t>(base_[off]);
        uint8_t b1 = static_cast<uint8_t>(base_[off + 1]);
        uint8_t b2 = static_cast<uint8_t>(base_[off + 2]);
        uint8_t b3 = static_cast<uint8_t>(base_[off + 3]);
        *v = le_ ? (static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) |
                    (static_cast<uint32_t>(b2) << 16) | (static_cast<uint32_t>(b3) << 24))
                 : (static_cast<uint32_t>(b3) | (static_cast<uint32_t>(b2) << 8) |
                    (static_cast<uint32_t>(b1) << 16) | (static_cast<uint32_t>(b0) << 24));
        return true;
    }
    bool I1(size_t off, int8_t* v) const {
        uint8_t u; if (!U1(off, &u)) return false;
        *v = static_cast<int8_t>(u); return true;
    }
    bool I2(size_t off, int16_t* v) const {
        uint16_t u; if (!U2(off, &u)) return false;
        *v = static_cast<int16_t>(u); return true;
    }
    // IEEE754 binary32/binary64
    bool R4(size_t off, float* v) const {
        uint32_t bits;
        if (!U4(off, &bits)) return false;
        memcpy(v, &bits, 4);
        return true;
    }
    bool R8(size_t off, double* v) const {
        if (off + 8 > size_) return false;
        uint64_t bits = 0;
        if (le_) {
            for (int i = 7; i >= 0; --i)
                bits = (bits << 8) | static_cast<uint8_t>(base_[off + i]);
        } else {
            for (int i = 0; i < 8; ++i)
                bits = (bits << 8) | static_cast<uint8_t>(base_[off + i]);
        }
        memcpy(v, &bits, 8);
        return true;
    }
    // Cn/A*n：n 字节文本切片
    bool Cn(size_t off, uint16_t n, StrSlice* s) const {
        if (off + n > size_) return false;
        s->offset = static_cast<uint32_t>(off);
        s->len = n;
        return true;
    }
    bool Bn(size_t off, uint16_t n, StrSlice* s) const { return Cn(off, n, s); }

    size_t Size() const { return size_; }
    const char* Base() const { return base_; }
    bool Little() const { return le_; }

private:
    const char* base_ = nullptr;
    size_t size_ = 0;
    bool le_ = true;
};

// ---- StdfFile：打开 + 扫描 ------------------------------------------------------
class StdfFile {
public:
    StdfFile() = default;
    ~StdfFile() = default;
    StdfFile(const StdfFile&) = delete;
    StdfFile& operator=(const StdfFile&) = delete;

    // 打开并全量扫描。失败原因进 err（可空）。扫描成功返回 true
    //（哪怕文件里含大量非标记录——按长度跳过）。
    bool Open(const std::wstring& path, std::wstring* err = nullptr);

    const StdfSummary& Summary() const { return summary_; }
    const std::vector<RecordIndexEntry>& Records() const { return records_; }
    const std::vector<TestItem>& Tests() const { return tests_; }
    // datalog 表：每颗芯片一行（PIR..PRR 区间的 PTR 结果矩阵）
    const std::vector<PartRow>& Parts() const { return parts_; }
    // 测试项列顺序：TEST_NUM 升序，同号按首见顺序（CSV 表列序）
    const std::vector<uint32_t>& TestColumnOrder() const { return testOrder_; }

    // mmap 基址（StdfPanel 解码 StrSlice 用；Open 成功后有效）
    const char* Base() const { return map_.Data(); }

    // StrSlice → UTF-16（STDF 文本应为 ASCII；非 ASCII 字节按 latin1 透传）
    static std::wstring ToString(const StrSlice& s, const char* base);

    // 把 datalog 矩阵导出为 ATE log 格式 CSV 文本（UTF-8）。
    // 行1 表头 / 行2 单位 / 行3 low / 行4 high / 行5+ 数据；可单测。
    // rowMap 非空时仅导出其中的 Parts() 下标行（视图过滤），否则全量。
    static std::string ToDatalogCsv(const StdfFile& sf,
                                    const std::vector<int32_t>* rowMap = nullptr);

    // 批次 28：给 AI 的 STDF 统计块（英文数据行，prompt 用）。
    // yield/per-site/HW-SW bin（数量降序取前几）/失败测试项（failCount
    // 降序取前 10）。空段省略控体积；可单测（配合合成样本）。
    std::wstring FormatAiStatsBlock(const std::wstring& path) const;

    // 单个测试项的分布统计（从结果矩阵现算；NaN 跳过）。
    struct ItemStats {
        uint32_t n = 0;          // 有效样本数
        double mean = 0, sigma = 0, minV = 0, maxV = 0;
        double lo = 0, hi = 0;   // 限值（无则 hasLo/hasHi=false）
        bool hasLo = false, hasHi = false;
        uint32_t overLo = 0, overHi = 0;  // 超出限值样本数
        double Cp = 0, Cpk = 0;  // hasLo&&hasHi 时 Cp；有任一时 Cpk
    };
    static ItemStats ComputeStats(const StdfFile& sf, size_t testIdx);

    // 记录类型可读名（"PTR"/"MPR"/"GDR"/"10-30"）
    static const char* RecordName(uint8_t typ, uint8_t sub);

private:
    void ParseMir(Reader& r, size_t body);
    void ParsePcr(Reader& r, size_t body);
    void ParsePrr(Reader& r, size_t body, size_t bodyEnd);
    void ParseHbrSbr(Reader& r, size_t body, bool hbr);
    void ParseWirWrr(Reader& r, size_t body, bool wir);
    void ParsePtr(Reader& r, size_t body, size_t bodyEnd);
    void ParseMpr(Reader& r, size_t body, size_t bodyEnd);
    void ReconcileCounts();    // 扫描后：无 PRR 时用 PCR 汇总
    void FinishTestColumns();  // 扫描后：testOrder_ 排序（TEST_NUM 升序、同号按首见）

    MappedFile map_;
    StdfSummary summary_;
    std::vector<RecordIndexEntry> records_;
    std::vector<TestItem> tests_;
    std::vector<PartRow> parts_;        // datalog 行
    std::vector<uint32_t> testOrder_;   // tests_ 下标 → CSV 列序
    std::vector<SiteCount> pcrSites_;   // PCR 原始计数（PRR 缺失时兜底）
    // 扫描期：每 site 的"当前 part"（PIR 开 part，PRR 收尾）
    // key=site, value=parts_ 下标；-1=无活动 part
    std::unordered_map<uint8_t, int32_t> openPart_;
    // PTR 解析测试项时把下标回填到这里（part 结果行用）
    size_t lastTestIdx_ = 0;
    // key = (testNum << 1) | isMpr
    std::unordered_map<uint64_t, size_t> testIndex_;
};

} // namespace stdf
} // namespace xfs
