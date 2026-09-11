// StdfFile.cpp — STDF v4 解析层实现。
//
// v4 关键记录布局（REC_LEN 不含自身 4B 头；body = 记录头后第一字节偏移）：
//   FAR (0,10):  CPU_TYPE(1) STDF_VER(1)
//   MIR (1,10):  SETUP_T(U4) START_T(U4) STAT_NUM(U1) MODE_COD(C1) RTST_COD(C1)
//                PROT_COD(C1) BURN_STAT(C1) CMOD_COD(C1) LOT_ID(Cn) PART_TYP(Cn)
//                NODE_NAM(Cn) TSTR_TYP(Cn) JOB_NAM(Cn) JOB_REV(Cn) OPER_NAM(Cn)
//                EXEC_TYP(Cn) EXEC_VER(Cn) DAT_COD(Cn) TST_COD(Cn) ...
//   PCR (1,30):  HEAD_NUM(1) SITE_NUM(1) PART_CNT(U4) RTST_CNT(U4) ABRT_CNT(U4)
//                GOOD_CNT(U4) FUNC_CNT(U4)
//   HBR (1,40):  HEAD_NUM(1) SITE_NUM(1) HBIN_NUM(U2) HBIN_CNT(U4) PF(B1) HBIN_NAM(Cn)
//   SBR (1,50):  同 HBR（SBIN_*）
//   PIR (5,10):  HEAD_NUM(1) SITE_NUM(1)
//   PRR (5,20):  HEAD_NUM(1) SITE_NUM(1) PART_FLG(1) NUM_TEST(U2) HARD_BIN(U2)
//                SOFT_BIN(U2) X_COORD(I2) Y_COORD(I2) TEST_T(U4) PART_ID(Cn)
//                PART_TXT(Cn) PART_FIX(Bn)
//   WIR (2,10):  HEAD_NUM(1) SITE_NUM(1) START_T(U4) WAFER_ID(Cn) ...
//   WRR (2,20):  HEAD_NUM(1) SITE_NUM(1) FINISH_T(U4) PART_ID(Cn) RTST_CNT(U4)
//                ABRT_CNT(U4) GOOD_CNT(U4) FUNC_CNT(U4) WAFER_ID(Cn) FABWF_ID(Cn)...
//   PTR (15,10): TEST_NUM(U4) HEAD(1) SITE(1) TEST_FLG(1) PARM_FLG(1) RESULT(R4)
//                TEST_TXT(Cn) ALARM_ID(A*n) OPT_FLAG(B1) RES_SCAL(I1) LLM_SCAL(I1)
//                HLM_SCAL(I1) LO_LIMIT(R4) HI_LIMIT(R4) UNITS(Cn) ...
//   MPR (15,15): TEST_NUM(U4) HEAD(1) SITE(1) TEST_FLG(1) PARM_FLG(1)
//                TEST_TXT(Cn) ALARM_ID(A*n) OPT_FLAG(B1) RES_SCAL(I1) LLM_SCAL(I1)
//                HLM_SCAL(I1) LO_LIMIT(R4) HI_LIMIT(R4) UNITS(Cn) N_PARMS(U2)
//                RTN_STAT(B×N) RSLT_STAT(B×N)   —— MPR 无 R*4 数值结果！
//   位语义（v4 规范）：
//     TEST_FLG  bit2(0x04)=结果无效  bit6(0x40)=1 失败
//     PARM_FLG  bit3(0x08)=1 失败
//     PART_FLG  bit3(0x08)=1 pass（PRR）
//     OPT_FLAG  bit3(0x08)=无低限  bit4(0x10)=无高限
#include "StdfFile.h"
#include "../core/Log.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <limits>

namespace xfs {
namespace stdf {

namespace {

inline bool Bit(uint8_t v, int bit) { return (v >> bit) & 1; }

// TEST_TXT 内容的 64 位 FNV-1a（TEST_NUM=0 的测试机用文本区分测试项）
uint64_t TextKey(const StrSlice& s, const char* base) {
    if (!s.len || !base) return 0;
    uint64_t h = 1469598103934665603ULL;
    for (uint16_t i = 0; i < s.len; ++i) {
        h ^= static_cast<unsigned char>(base[s.offset + i]);
        h *= 1099511628211ULL;
    }
    return h;
}

// STDF v4 时间：Unix epoch 秒（1970-01-01 起）→ 本地时间字符串
//（真样本验证：ADS 0x6A6A2D9D=2026-07-30、EVA 0x64359548=2023-04-10 均吻合）
std::string FmtStdfTime(uint32_t secs) {
    if (secs == 0) return {};
    time_t t = static_cast<time_t>(secs);
    tm lt{};
    if (localtime_s(&lt, &t) != 0) return {};
    char buf[40];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    return buf;
}

} // namespace

// ---- StrSlice → UTF-16 ----------------------------------------------------------
std::wstring StdfFile::ToString(const StrSlice& s, const char* base) {
    if (!s.len || !base) return {};
    std::wstring out;
    out.reserve(s.len);
    for (uint16_t i = 0; i < s.len; ++i)
        out += static_cast<wchar_t>(static_cast<unsigned char>(base[s.offset + i]));
    return out;
}

// ---- 记录可读名 -------------------------------------------------------------------
const char* StdfFile::RecordName(uint8_t typ, uint8_t sub) {
    switch (typ) {
        case 0:
            if (sub == 10) return "FAR";
            if (sub == 15) return "VUR";
            if (sub == 20) return "ATR";
            break;
        case 1:
            switch (sub) {
                case 10: return "MIR";
                case 20: return "MRR";
                case 30: return "PCR";
                case 40: return "HBR";
                case 50: return "SBR";
                case 60: return "PMR";
                case 62: return "PGR";
                case 63: return "PLR";
                case 70: return "TSR";
            }
            break;
        case 2:
            if (sub == 10) return "WIR";
            if (sub == 20) return "WRR";
            if (sub == 30) return "WCR";
            break;
        case 5:
            if (sub == 10) return "PIR";
            if (sub == 20) return "PRR";
            break;
        case 15:
            if (sub == 10) return "PTR";
            if (sub == 15) return "MPR";
            if (sub == 20) return "FTR";
            if (sub == 81) return "SDR";
            break;
        case 20:
            if (sub == 10) return "GDR";
            if (sub == 30) return "DTR";
            break;
        case 50: if (sub == 10) return "BPS"; break;
        case 60: if (sub == 10) return "EPS"; break;
        default: break;
    }
    static char buf[16];
    snprintf(buf, sizeof(buf), "%u-%u", typ, sub);
    return buf;
}

// ---- 打开 + 单遍扫描 ---------------------------------------------------------------
bool StdfFile::Open(const std::wstring& path, std::wstring* err) {
    DWORD tOpen = ::GetTickCount();
    auto fail = [&](const wchar_t* why) {
        if (err) *err = why;
        Logger::Error("StdfFile: " + WideToUtf8(why) + " '" +
                      WideToUtf8(path) + "'");
        return false;
    };

    if (!map_.Open(path) || map_.Size() < 6)
        return fail(L"cannot map file (or too small)");
    const char* base = map_.Data();
    const size_t size = map_.Size();
    summary_.dataSize = size;

    // ---- FAR：REC_LEN 端序由 CPU_TYPE 决定，而 CPU_TYPE 在 FAR 体内。
    // FAR 体固定 2 字节 → FAR 的 REC_LEN 恒为 2。先小端试、不行再大端试。
    bool le = true;
    {
        uint8_t b0 = static_cast<uint8_t>(base[0]);
        uint8_t b1 = static_cast<uint8_t>(base[1]);
        uint16_t le16 = static_cast<uint16_t>(b0 | (b1 << 8));
        uint16_t be16 = static_cast<uint16_t>((b0 << 8) | b1);
        if (le16 == 2) {
            le = true;
        } else if (be16 == 2) {
            le = false;
        } else {
            return fail(L"first record is not FAR");
        }
        if (static_cast<uint8_t>(base[2]) != 0 ||
            static_cast<uint8_t>(base[3]) != 10)
            return fail(L"not a STDF file (no FAR)");
        summary_.cpuType = static_cast<uint8_t>(base[4]);
        summary_.stdfVersion = static_cast<uint8_t>(base[5]);
        if (summary_.stdfVersion != 4)
            return fail(L"unsupported STDF version");
    }
    Reader r(base, size, le);

    // ---- 主循环：长度驱动，未知类型跳过 -------------------------------------
    size_t off = 0;
    records_.reserve(size / 64 + 16);   // PTR 密集文件均值 ~45B/条，64 是安全低估

    while (off + 4 <= size) {
        uint16_t rl = 0;
        if (!r.U2(off, &rl)) break;
        uint8_t typ = 0, sub = 0;
        r.U1(off + 2, &typ);
        r.U1(off + 3, &sub);
        // 空记录/截断记录终止（防死循环）
        if (rl == 0 && typ == 0 && sub == 0) break;
        if (off + 4 + rl > size) {
            Logger::Error("StdfFile: truncated record at " + std::to_string(off) +
                          " (len=" + std::to_string(rl) + ", " +
                          std::to_string(size - off) + " bytes to eof)");
            break;
        }

        RecordIndexEntry e;
        e.offset = static_cast<uint32_t>(off);
        e.len = rl;
        e.typ = typ;
        e.sub = sub;
        records_.push_back(e);
        ++summary_.recCount;

        const size_t body = off + 4;
        switch (typ) {
            case 0:
                // FAR 已处理；ATR 跳过
                break;
            case 1:
                if (sub == 10) ParseMir(r, body);
                else if (sub == 20) {
                    uint32_t fin = 0;
                    if (r.U4(body, &fin)) summary_.finishT = FmtStdfTime(fin);
                }
                else if (sub == 30) ParsePcr(r, body);
                else if (sub == 40) ParseHbrSbr(r, body, true);
                else if (sub == 50) ParseHbrSbr(r, body, false);
                else if (sub == 70) ++summary_.gdrCount;   // TSR 仅计数
                else ++summary_.unknown[(static_cast<uint32_t>(typ) << 8) | sub];
                break;
            case 2:
                if (sub == 10) { ++summary_.wirCount; ParseWirWrr(r, body, true); }
                else if (sub == 20) { ++summary_.wrrCount; ParseWirWrr(r, body, false); }
                else ++summary_.unknown[(static_cast<uint32_t>(typ) << 8) | sub];
                break;
            case 5:
                if (sub == 10) {
                    ++summary_.pirCount;
                    // PIR：为该 site 开一个 datalog 行（PRR 收尾）
                    uint8_t site = 0;
                    if (r.U1(body + 1, &site)) {
                        PartRow row;
                        row.index = (uint32_t)parts_.size();
                        row.site = site;
                        row.results.resize(tests_.size(),
                                          std::numeric_limits<float>::quiet_NaN());
                        parts_.push_back(std::move(row));
                        openPart_[site] = (int32_t)parts_.size() - 1;
                    }
                }
                else if (sub == 20) ParsePrr(r, body, body + rl);
                else ++summary_.unknown[(static_cast<uint32_t>(typ) << 8) | sub];
                break;
            case 15:
                if (sub == 10) { ++summary_.ptrCount; ParsePtr(r, body, body + rl); }
                else if (sub == 15) { ++summary_.mprCount; ParseMpr(r, body, body + rl); }
                else if (sub == 20) ++summary_.ftrCount;
                else ++summary_.unknown[(static_cast<uint32_t>(typ) << 8) | sub];
                break;
            case 20:
                if (sub == 10) ++summary_.gdrCount;
                else if (sub == 30) ++summary_.dtrCount;
                else ++summary_.unknown[(static_cast<uint32_t>(typ) << 8) | sub];
                break;
            default:
                ++summary_.unknown[(static_cast<uint32_t>(typ) << 8) | sub];
                break;
        }
        off += 4 + rl;
    }

    if (summary_.recCount == 0)
        return fail(L"no records found");

    ReconcileCounts();
    DWORD t0 = ::GetTickCount();
    FinishTestColumns();
    Logger::Debug("StdfFile::FinishTestColumns tests=" +
                  std::to_string(tests_.size()) + " parts=" +
                  std::to_string(parts_.size()) + " " +
                  std::to_string(::GetTickCount() - t0) + " ms");

    if (err) err->clear();
    Logger::Info("StdfFile: scanned " + WideToUtf8(path) + " (" +
                 std::to_string(size) + " bytes, " +
                 std::to_string(summary_.recCount) + " records, " +
                 std::to_string(tests_.size()) + " test items, " +
                 std::to_string(summary_.unknown.size()) + " unknown types) " +
                 std::to_string(::GetTickCount() - tOpen) + " ms");
    return true;
}

// ---- PRR/PCR 计数对账 ------------------------------------------------------------
void StdfFile::ReconcileCounts() {
    if (summary_.prrCount > 0) return;   // PRR 为准，PCR 只做 per-site 补充
    // 无 PRR（如 final test 只写 PCR）：用 PCR 汇总
    for (const auto& p : pcrSites_) {
        summary_.partCount += p.partCount;
        summary_.goodCount += p.goodCount;
        summary_.sites.push_back(p);
    }
}

// ---- 测试项列序：执行序（首次出现顺序）——测试机 ATE CSV 同序 ------------------------
void StdfFile::FinishTestColumns() {
    testOrder_.resize(tests_.size());
    for (uint32_t i = 0; i < testOrder_.size(); ++i) testOrder_[i] = i;
    // datalog 行在扫描中按当时 tests_.size() 扩容；测试项全部收齐后统一补齐
    for (auto& row : parts_) {
        if (row.results.size() < tests_.size())
            row.results.resize(tests_.size(),
                              std::numeric_limits<float>::quiet_NaN());
    }
    // lo > hi：limit 写反（真机见过：EVA RVCC 全部记录一致写 lo=1e7/hi=0）→ 交换显示。
    // 不做"pass 结果须落在限内"的裁剪：判定可能走 bin 而非 limit（A08 实证 pass die
    // 值 -1.50 仍 pass），测试机自己的 CSV 也是按程序里写的原值显示。
    for (auto& t : tests_) {
        if (t.hasLo && t.hasHi && t.lo > t.hi) {
            double tmp = t.lo; t.lo = t.hi; t.hi = tmp;
        }
    }
}

// ---- MIR（摘要字段）--------------------------------------------------------------
void StdfFile::ParseMir(Reader& r, size_t body) {
    size_t p = body;
    uint32_t setupT = 0, startT = 0;
    r.U4(p, &setupT); p += 4;
    r.U4(p, &startT); p += 4;
    summary_.startT = FmtStdfTime(startT ? startT : setupT);
    p += 1;              // STAT_NUM
    p += 6;              // 6×C1：MODE_COD RTST_COD PROT_COD BURN_STAT CMOD_COD + 1
                         //（第 6 个字节规范名存疑，但两台真实测试机（NI STS/其他）
                         // 一致写 6 字节：ADS 样本 6-C1 下 job 字段 len=34 恰为
                         // "ADS6102_..._loop10" 全长、EVA 样本 LOT_ID="KB38035-"
                         // 恰 8 字节与文件名吻合；5-C1 解释会产生嵌 \0 的脏切片）

    auto takeCn = [&](StrSlice* s) {
        uint8_t n = 0;
        if (!r.U1(p, &n)) { s->len = 0; ++p; return; }
        ++p;
        r.Cn(p, n, s);
        p += n;
    };
    takeCn(&summary_.lotId);       // LOT_ID
    takeCn(&summary_.partTyp);     // PART_TYP
    takeCn(&summary_.nodeNam);     // NODE_NAM
    StrSlice discard;
    takeCn(&discard);              // TSTR_TYP
    takeCn(&summary_.jobNam);      // JOB_NAM
    takeCn(&summary_.jobRev);      // JOB_REV
    takeCn(&summary_.operNam);     // OPER_NAM
    takeCn(&discard);              // EXEC_TYP
    takeCn(&discard);              // EXEC_VER
    takeCn(&discard);              // DAT_COD
    takeCn(&summary_.testCod);     // TST_COD
}

// ---- PCR（per-site 计数；PRR 存在时仅入 pcrSites_ 备用）---------------------------
void StdfFile::ParsePcr(Reader& r, size_t body) {
    uint8_t site = 0;
    uint32_t partCnt = 0, goodCnt = 0;
    if (!r.U1(body + 1, &site)) return;
    r.U4(body + 2, &partCnt);
    r.U4(body + 14, &goodCnt);   // PART_CNT@2 RTST@6 ABRT@10 GOOD@14
    SiteCount* dst = nullptr;
    for (auto& s : pcrSites_)
        if (s.site == site) { dst = &s; break; }
    if (!dst) {
        pcrSites_.push_back({});
        dst = &pcrSites_.back();
        dst->site = site;
    }
    dst->partCount += partCnt;
    dst->goodCount += goodCnt;
}

// ---- PRR（part 聚合 + per-site + datalog 行收尾）-----------------------------------
void StdfFile::ParsePrr(Reader& r, size_t body, size_t bodyEnd) {
    ++summary_.prrCount;
    uint8_t site = 0, flg = 0;
    if (!r.U1(body + 1, &site)) return;   // HEAD@0 SITE@1
    if (!r.U1(body + 2, &flg)) return;    // PART_FLG@2
    // PART_FLG bit3(0x08)=1 → pass
    bool pass = Bit(flg, 3);
    summary_.partCount += 1;
    if (pass) summary_.goodCount += 1;
    SiteCount* dst = nullptr;
    for (auto& s : summary_.sites)
        if (s.site == site) { dst = &s; break; }
    if (!dst) {
        summary_.sites.push_back({});
        dst = &summary_.sites.back();
        dst->site = site;
    }
    dst->partCount += 1;
    if (pass) dst->goodCount += 1;

    // datalog 行收尾：把 PRR 的 bin/坐标/耗时回填到该 site 的活动 part
    auto it = openPart_.find(site);
    if (it == openPart_.end() || it->second < 0) return;
    PartRow& row = parts_[(size_t)it->second];
    uint16_t hb = 0, sb = 0;
    r.U2(body + 5, &hb);                 // HARD_BIN@5
    r.U2(body + 7, &sb);                 // SOFT_BIN@7
    uint16_t xv = 0, yv = 0;
    r.U2(body + 9, &xv);                 // X_COORD@9
    r.U2(body + 11, &yv);                // Y_COORD@11
    uint32_t tms = 0;
    r.U4(body + 13, &tms);               // TEST_T@13
    row.hbin = hb;
    row.sbin = sb;
    row.x = (int16_t)xv;
    row.y = (int16_t)yv;
    row.testMs = tms;
    row.pass = pass;
    row.results.resize(tests_.size(), std::numeric_limits<float>::quiet_NaN());
    // 长度不足的 PRR（无 TEST_T）：剩余字段以哨兵值兜底
    (void)bodyEnd;
    it->second = -1;                      // 该 site 的 part 已收尾
}

// ---- HBR/SBR（bin 计数）------------------------------------------------------------
void StdfFile::ParseHbrSbr(Reader& r, size_t body, bool hbr) {
    // HEAD(1) SITE(1) BIN_NUM(U2) BIN_CNT(U4) PF(1) NAME(Cn)
    uint16_t num = 0;
    uint32_t cnt = 0;
    if (!r.U2(body + 2, &num)) return;
    if (!r.U4(body + 4, &cnt)) return;
    BinCount b;
    b.num = num;
    b.count = cnt;
    r.U1(body + 8, &b.pf);
    uint8_t n = 0;
    if (r.U1(body + 9, &n)) {
        r.Cn(body + 10, n, &b.name);
    }
    auto& v = hbr ? summary_.hbins : summary_.sbins;
    for (auto& e : v)
        if (e.num == num) { e.count += cnt; return; }
    v.push_back(b);
}

// ---- WIR/WRR（计数 + WRR 的 WAFER_ID）----------------------------------------------
void StdfFile::ParseWirWrr(Reader& r, size_t body, bool wir) {
    if (wir) {
        // WIR: HEAD(1) SITE(1) START_T(U4) WAFER_ID(Cn)
        if (summary_.waferId.empty()) {
            uint8_t n = 0;
            if (r.U1(body + 6, &n)) r.Cn(body + 7, n, &summary_.waferId);
        }
        return;
    }
    // WRR: HEAD(1) SITE(1) FINISH_T(U4) PART_ID(Cn) RTST(U4) ABRT(U4)
    //      GOOD(U4) FUNC(U4) WAFER_ID(Cn) FABWF_ID(Cn)...
    size_t p = body + 6;
    uint8_t n = 0;
    if (!r.U1(p, &n)) return;
    ++p;
    p += n;                // PART_ID
    p += 16;               // RTST/ABRT/GOOD/FUNC
    if (summary_.waferId.empty()) {
        if (r.U1(p, &n)) r.Cn(p + 1, n, &summary_.waferId);
    }
}

// ---- PTR（测试项聚合核心）----------------------------------------------------------
void StdfFile::ParsePtr(Reader& r, size_t body, size_t bodyEnd) {
    size_t p = body;
    uint32_t testNum = 0;
    if (!r.U4(p, &testNum)) return;
    p += 4;
    uint8_t head = 0, site = 0, tflg = 0, pflg = 0;
    r.U1(p, &head); ++p;
    r.U1(p, &site); ++p;
    r.U1(p, &tflg); ++p;
    r.U1(p, &pflg); ++p;
    float result = 0;
    const bool resultValid = !Bit(tflg, 2);   // TEST_FLG bit2=结果无效
    r.R4(p, &result);
    p += 4;

    uint8_t tn1 = 0, tn2 = 0;
    StrSlice testTxt, alarmId, units;
    if (!r.U1(p, &tn1)) return;
    ++p;
    r.Cn(p, tn1, &testTxt);
    p += tn1;
    if (!r.U1(p, &tn2)) return;
    ++p;
    r.Cn(p, tn2, &alarmId);
    p += tn2;

    // OPT_FLAG（1 字节；记录到此结束也合法）
    uint8_t optFlag = 0;
    const bool hasOpt = r.U1(p, &optFlag);
    if (hasOpt) ++p;

    float lo = 0, hi = 0;
    bool hasLoVal = false, hasHiVal = false;
    if (hasOpt && p + 3 + 8 <= bodyEnd) {
        p += 3;                               // RES_SCAL/LLM_SCAL/HLM_SCAL
        r.R4(p, &lo);
        r.R4(p + 4, &hi);
        p += 8;
        // 真实测试机（ADS/EVA 样本）把 OPT_FLAG bit3/4 乱置成"无 low/high"却仍写入
        // 有效数值——按用户要求以数据为准：只要 8 字节物理存在且是"可-looking"浮点
        // 就采用（NaN/Inf/|v|>1e30 或 ==0 的哨兵除外）。ResultValid 才有意义：结果
        // 无效时 limit 字节可能是未初始化垃圾。
        if (resultValid) {
            auto plausible = [](float v) {
                return v == v && v > -1e30f && v < 1e30f && v != 0.0f;
            };
            hasLoVal = plausible(lo);
            hasHiVal = plausible(hi);
        }
    }
    // UNITS Cn
    if (p + 1 <= bodyEnd) {
        uint8_t un = 0;
        if (r.U1(p, &un) && p + 1 + un <= bodyEnd) {
            r.Cn(p + 1, un, &units);
            p += 1 + un;
        }
    }

    // ---- 聚合：key = (id<<1)，id = TEST_NUM；TEST_NUM=0 时用 TEST_TXT 哈希
    //（EVA 真样本：测试机全程写 TEST_NUM=0，文本是唯一区分）
    const uint64_t idNum = testNum ? (static_cast<uint64_t>(testNum) << 1)
                                   : (TextKey(testTxt, r.Base()) << 1);
    const uint64_t key = idNum;
    auto it = testIndex_.find(key);
    TestItem* item = nullptr;
    if (it == testIndex_.end()) {
        tests_.push_back({});
        item = &tests_.back();
        testIndex_[key] = tests_.size() - 1;
        lastTestIdx_ = tests_.size() - 1;
        item->testNum = static_cast<int64_t>(testNum);
        item->kind = 'P';
        item->headSite = head * 256 + site;
        item->testTxt = testTxt;
        item->alarmId = alarmId;
        item->units = units;
        item->lo = lo;
        item->hi = hi;
        // Limit 解析无视 OPT_FLAG bit3/4（真机乱置），只看物理字节是否可-looking；
        // 首个有效记录建立基准，后续记录仅在基准缺失时补填（多数派语义）。
        item->hasLo = hasLoVal;
        item->hasHi = hasHiVal;
        item->minV = resultValid ? result : 0.0;
        item->maxV = resultValid ? result : 0.0;
        item->validSeen_ = resultValid;   // min/max 只统计有效结果
    } else {
        item = &tests_[it->second];
        lastTestIdx_ = it->second;
        if (resultValid) {
            if (!item->validSeen_) { item->minV = item->maxV = result; item->validSeen_ = true; }
            else {
                if (result < item->minV) item->minV = result;
                if (result > item->maxV) item->maxV = result;
            }
        }
        if (!item->hasLo && hasLoVal) { item->lo = lo; item->hasLo = true; }
        if (!item->hasHi && hasHiVal) { item->hi = hi; item->hasHi = true; }
    }
    item->count += 1;
    if (resultValid) item->sumV += result;
    // 失败：TEST_FLG bit6 或 PARM_FLG bit3（不同测试机只置其一）
    if (Bit(tflg, 6) || Bit(pflg, 3)) item->failCount += 1;

    // datalog：结果写进该 site 当前活动 part 的行
    //（lastTestIdx_ 在 testIndex_ 查找后设置，见上；注意 push_back 未扩容）
    {
        auto it2 = openPart_.find(site);
        if (it2 != openPart_.end() && it2->second >= 0) {
            PartRow& row = parts_[(size_t)it2->second];
            if (lastTestIdx_ < tests_.size()) {
                // 行创建之后新出现的测试项：扩一格
                if (row.results.size() < tests_.size())
                    row.results.resize(tests_.size(),
                                      std::numeric_limits<float>::quiet_NaN());
                row.results[lastTestIdx_] =
                    resultValid ? result : std::numeric_limits<float>::quiet_NaN();
            }
        }
    }
}

// ---- MPR（多 pin 状态；无数值结果）--------------------------------------------------
void StdfFile::ParseMpr(Reader& r, size_t body, size_t bodyEnd) {
    size_t p = body;
    uint32_t testNum = 0;
    if (!r.U4(p, &testNum)) return;
    p += 4;
    p += 4;               // HEAD/SITE/TEST_FLG/PARM_FLG

    uint8_t tn1 = 0, tn2 = 0;
    StrSlice testTxt, alarmId, units;
    if (!r.U1(p, &tn1)) return;
    ++p;
    r.Cn(p, tn1, &testTxt);
    p += tn1;
    if (!r.U1(p, &tn2)) return;
    ++p;
    r.Cn(p, tn2, &alarmId);
    p += tn2;

    uint8_t optFlag = 0;
    const bool hasOpt = r.U1(p, &optFlag);
    if (hasOpt) ++p;

    float lo = 0, hi = 0;
    bool hasLoVal = false, hasHiVal = false;
    if (hasOpt && p + 3 + 8 <= bodyEnd) {
        p += 3;
        r.R4(p, &lo);
        r.R4(p + 4, &hi);
        p += 8;
        // 同 PTR：无视 OPT_FLAG bit3/4（真机乱置），只看浮点是否可-looking
        auto plausible = [](float v) {
            return v == v && v > -1e30f && v < 1e30f && v != 0.0f;
        };
        hasLoVal = plausible(lo);
        hasHiVal = plausible(hi);
    }
    if (p + 1 <= bodyEnd) {
        uint8_t un = 0;
        if (r.U1(p, &un) && p + 1 + un <= bodyEnd) {
            r.Cn(p + 1, un, &units);
            p += 1 + un;
        }
    }

    // N_PARMS(U2) + RTN_STAT(B×N) + RSLT_STAT(B×N)
    uint16_t nparms = 0;
    if (!r.U2(p, &nparms) || nparms == 0) return;
    p += 2;
    const size_t stats = p + static_cast<size_t>(nparms);   // RSLT_STAT 起点

    // ---- 聚合：key = (id<<1)|1（MPR），id 同 PTR 规则
    const uint64_t key = (testNum ? (static_cast<uint64_t>(testNum) << 1)
                                  : (TextKey(testTxt, r.Base()) << 1)) | 1;
    auto it = testIndex_.find(key);
    TestItem* item = nullptr;
    if (it == testIndex_.end()) {
        tests_.push_back({});
        item = &tests_.back();
        testIndex_[key] = tests_.size() - 1;
        item->testNum = static_cast<int64_t>(testNum);
        item->kind = 'M';
        item->testTxt = testTxt;
        item->alarmId = alarmId;
        item->units = units;
        item->lo = lo;
        item->hi = hi;
        item->hasLo = hasLoVal;
        item->hasHi = hasHiVal;
    } else {
        item = &tests_[it->second];
        if (!item->hasLo && hasLoVal) { item->lo = lo; item->hasLo = true; }
        if (!item->hasHi && hasHiVal) { item->hi = hi; item->hasHi = true; }
    }
    item->count += 1;
    // RSLT_STAT bit0：1=pass 0=fail；统计失败 pin 数
    uint32_t fails = 0;
    for (uint16_t i = 0; i < nparms; ++i) {
        uint8_t st = 0;
        if (r.U1(stats + i, &st) && !Bit(st, 0)) ++fails;
    }
    item->failCount += fails;
}

// ---- Datalog CSV（ATE log 格式）-----------------------------------------------------
std::string StdfFile::ToDatalogCsv(const StdfFile& sf,
                                   const std::vector<int32_t>* rowMap) {
    const auto& parts = sf.Parts();
    const auto& order = sf.TestColumnOrder();
    const auto& tests = sf.Tests();
    const char* base = sf.Base();

    std::string out;
    out.reserve(64 * 1024 * 1024);
    auto cell = [&](const std::wstring& v) {   // CSV 转义 + UTF-8
        std::string u = WideToUtf8(v);
        if (u.find(',') != std::string::npos ||
            u.find('"') != std::string::npos ||
            u.find('\n') != std::string::npos) {
            out += '"';
            for (char c : u) {
                if (c == '"') out += "\"\"";
                else out += c;
            }
            out += '"';
        } else {
            out += u;
        }
    };
    auto row = [&] { if (!out.empty() && out.back() != '\n') out += "\r\n"; };
    wchar_t bb[48];

    // 行 1：表头（固定列 + 测试项名）
    out += "Test_time(ms),Test_no,Site,X_POS,Y_POS,HW_bin,SW_bin,Result";
    for (size_t c = 0; c < order.size(); ++c) {
        out += ",";
        std::wstring name = StdfFile::ToString(tests[order[c]].testTxt, base);
        if (name.empty()) {
            swprintf_s(bb, L"#%lld", (long long)tests[order[c]].testNum);
            name = bb;
        }
        cell(name);
    }
    out += "\r\n";
    // 行 2：单位（8 个固定列中 2-8 留空）
    out += "Unit,,,,,,,";
    for (size_t c = 0; c < order.size(); ++c) {
        out += ",";
        cell(StdfFile::ToString(tests[order[c]].units, base));
    }
    out += "\r\n";
    // 行 3：low limit
    out += "Low Limit,,,,,,,";
    for (size_t c = 0; c < order.size(); ++c) {
        out += ",";
        if (tests[order[c]].hasLo) {
            swprintf_s(bb, L"%.6g", tests[order[c]].lo);
            cell(bb);
        }
    }
    out += "\r\n";
    // 行 4：high limit
    out += "High Limit,,,,,,,";
    for (size_t c = 0; c < order.size(); ++c) {
        out += ",";
        if (tests[order[c]].hasHi) {
            swprintf_s(bb, L"%.6g", tests[order[c]].hi);
            cell(bb);
        }
    }
    out += "\r\n";
    // 行 5+：每颗芯片（X/Y=-32768、bin=0xFFFF 为无效哨兵，留空）
    auto emit = [&](const PartRow& p) {
        swprintf_s(bb, L"%u", p.testMs); out += WideToUtf8(bb); out += ",";
        swprintf_s(bb, L"%u", p.index);   out += WideToUtf8(bb); out += ",";
        swprintf_s(bb, L"%u", p.site);    out += WideToUtf8(bb); out += ",";
        if (p.x != -32768) { swprintf_s(bb, L"%d", p.x); out += WideToUtf8(bb); }
        out += ",";
        if (p.y != -32768) { swprintf_s(bb, L"%d", p.y); out += WideToUtf8(bb); }
        out += ",";
        if (p.hbin != 0xFFFF) { swprintf_s(bb, L"%u", p.hbin); out += WideToUtf8(bb); }
        out += ",";
        if (p.sbin != 0xFFFF) { swprintf_s(bb, L"%u", p.sbin); out += WideToUtf8(bb); }
        out += ",";
        out += p.pass ? "Pass" : "Fail";
        for (size_t c = 0; c < order.size() && c < p.results.size(); ++c) {
            out += ",";
            size_t ti = order[c];
            float v = ti < p.results.size() ? p.results[ti]
                                            : std::numeric_limits<float>::quiet_NaN();
            if (!std::isnan(v)) {
                swprintf_s(bb, L"%.6g", v);
                out += WideToUtf8(bb);
            }
        }
        out += "\r\n";
        row();
    };
    if (rowMap) {
        for (int32_t pi : *rowMap)
            if (pi >= 0 && (size_t)pi < parts.size()) emit(parts[(size_t)pi]);
    } else {
        for (const auto& p : parts) emit(p);
    }
    return out;
}

// ---- 测试项分布统计（Cp/Cpk/超限）---------------------------------------------------
StdfFile::ItemStats StdfFile::ComputeStats(const StdfFile& sf, size_t testIdx) {
    ItemStats st;
    const auto& tests = sf.Tests();
    if (testIdx >= tests.size()) return st;
    const TestItem& t = tests[testIdx];
    st.hasLo = t.hasLo;
    st.hasHi = t.hasHi;
    st.lo = t.lo;
    st.hi = t.hi;

    double sum = 0, sumSq = 0;
    double mn = 0, mx = 0;
    bool seen = false;
    for (const auto& p : sf.Parts()) {
        if (testIdx >= p.results.size()) continue;
        float f = p.results[testIdx];
        if (std::isnan(f)) continue;
        double v = f;
        if (!seen) { mn = mx = v; seen = true; }
        else { if (v < mn) mn = v; if (v > mx) mx = v; }
        sum += v;
        sumSq += v * v;
        ++st.n;
        if (st.hasLo && v < st.lo) ++st.overLo;
        if (st.hasHi && v > st.hi) ++st.overHi;
    }
    if (!st.n) return st;
    st.minV = mn;
    st.maxV = mx;
    st.mean = sum / st.n;
    double var = sumSq / st.n - st.mean * st.mean;
    st.sigma = var > 0 ? std::sqrt(var) : 0;

    if (st.hasLo && st.hasHi && st.sigma > 0) {
        st.Cp = (st.hi - st.lo) / (6 * st.sigma);
        st.Cpk = (std::min)(st.hi - st.mean, st.mean - st.lo) / (3 * st.sigma);
    } else if (st.hasHi && st.sigma > 0) {
        st.Cpk = (st.hi - st.mean) / (3 * st.sigma);
    } else if (st.hasLo && st.sigma > 0) {
        st.Cpk = (st.mean - st.lo) / (3 * st.sigma);
    }
    return st;
}

// ---- AI 统计块（批次 28）-----------------------------------------------------------

// 生成给 AI 的 STDF 统计块：英文数据行（模型解析稳定），只含分析所需最小集：
// 产量/yield、per-site、HW/SW bin（数量降序取前几）、失败测试项（failCount
// 降序取前 10）。空字段/空段整行省略，控制块体积（prompt 预算）。
// bin 的 pass/fail 判定沿用 STDF v4 HBR/SBR 规范 bit3（1=pass）。
std::wstring StdfFile::FormatAiStatsBlock(const std::wstring& path) const {
    auto U = [](unsigned long long v) {
        wchar_t b[32]; swprintf(b, 32, L"%llu", v); return std::wstring(b);
    };
    auto G = [](double v) {
        if (std::isnan(v)) return std::wstring(L"nan");
        wchar_t b[40]; swprintf(b, 40, L"%.4g", v); return std::wstring(b);
    };
    auto Pct = [](unsigned long long n, unsigned long long d) {
        if (!d) return std::wstring(L"0");
        wchar_t b[32]; swprintf(b, 32, L"%.2f", 100.0 * (double)n / (double)d);
        return std::wstring(b);
    };

    std::wstring o;
    o += L"[STDF test data]\n";
    o += L"File: " + path + L"\n";

    // 标识行：Lot/Product/Job/JobRev/Node/Wafer，空段省略、有值才拼
    {
        std::wstring id;
        auto add = [&](const wchar_t* k, const StrSlice& sl) {
            std::wstring v = ToString(sl, Base());
            if (v.empty()) return;
            if (!id.empty()) id += L" | ";
            id += std::wstring(k) + L": " + v;
        };
        add(L"Lot", summary_.lotId);
        add(L"Product", summary_.partTyp);
        add(L"Job", summary_.jobNam);
        add(L"JobRev", summary_.jobRev);
        add(L"Node", summary_.nodeNam);
        add(L"Wafer", summary_.waferId);
        if (!id.empty()) o += id + L"\n";
    }
    if (!summary_.startT.empty() || !summary_.finishT.empty()) {
        // 时间戳已是本地时间 UTF-8 字符串；非 ASCII 直通可读性足够（AI 场景）
        std::string s8 = summary_.startT.empty() ? "-" : summary_.startT;
        std::string f8 = summary_.finishT.empty() ? "-" : summary_.finishT;
        o += L"Start: ";
        for (unsigned char c : s8) o += wchar_t(c);
        o += L" | Finish: ";
        for (unsigned char c : f8) o += wchar_t(c);
        o += L"\n";
    }
    o += L"Parts tested: " + U(summary_.partCount) +
         L" | Pass: " + U(summary_.goodCount) +
         L" | Fail: " + U(summary_.partCount >= summary_.goodCount
                             ? summary_.partCount - summary_.goodCount : 0) +
         L" | Yield: " + Pct(summary_.goodCount, summary_.partCount) + L"%\n";
    if (!summary_.sites.empty()) {
        o += L"Per-site (site=parts/pass):";
        for (const auto& sc : summary_.sites)
            o += L" " + U(sc.site) + L"=" + U(sc.partCount) + L"/" + U(sc.goodCount);
        o += L"\n";
    }
    // bin 表：count 降序取前 cap 个（HW 8 / SW 5，控块体积）
    auto bins = [&](const wchar_t* title, const std::vector<BinCount>& v,
                    size_t cap) {
        if (v.empty()) return;
        std::vector<const BinCount*> p;
        for (const auto& b : v) p.push_back(&b);
        std::stable_sort(p.begin(), p.end(),
                         [](const BinCount* a, const BinCount* b) {
                             return a->count > b->count;
                         });
        o += std::wstring(title) + L":\n";
        size_t n = std::min(cap, p.size());
        for (size_t i = 0; i < n; ++i) {
            const auto* b = p[i];
            bool pass = (b->pf & 0x08) != 0;
            o += L"  bin " + U(b->num) + L" x" + U(b->count) +
                 L" " + (pass ? L"[PASS]" : L"[FAIL]");
            std::wstring nm = ToString(b->name, Base());
            if (!nm.empty()) o += L" " + nm;
            o += L"\n";
        }
    };
    bins(L"Hardware bins (top by count)", summary_.hbins, 8);
    bins(L"Software bins (top by count)", summary_.sbins, 5);

    // 失败测试项：failCount 降序取前 10。MPR 的 failCount 是失败 pin 累计
    // （不是芯片数），行内注明 pin-fails 避免模型误读。
    std::vector<const TestItem*> fails;
    for (const auto& t : tests_)
        if (t.failCount > 0) fails.push_back(&t);
    if (fails.empty()) {
        o += L"Failing tests: none\n";
    } else {
        std::stable_sort(fails.begin(), fails.end(),
                         [](const TestItem* a, const TestItem* b) {
                             return a->failCount > b->failCount;
                         });
        o += L"Failing tests (top by fail count):\n";
        size_t n = std::min<size_t>(fails.size(), 10);
        for (size_t i = 0; i < n; ++i) {
            const auto* t = fails[i];
            std::wstring nm = ToString(t->testTxt, Base());
            if (nm.empty()) nm = L"-";
            o += L"  TEST " + U((unsigned long long)t->testNum) +
                 L" " + std::wstring(t->kind == 'M' ? L"MPR" : L"PTR") +
                 L" \"" + nm + L"\"";
            std::wstring un = ToString(t->units, Base());
            if (!un.empty()) o += L" units=" + un;
            if (t->kind == 'M')
                o += L" pin-fails=" + U(t->failCount) + L"/" + U(t->count);
            else
                o += L" fails=" + U(t->failCount) + L"/" + U(t->count) +
                     L" (" + Pct(t->failCount, t->count) + L"%)";
            o += L" mean=" + G(t->Mean());
            if (t->hasLo || t->hasHi) {
                o += L" limits=[" + std::wstring(t->hasLo ? G(t->lo) : L"-") +
                     L"," + std::wstring(t->hasHi ? G(t->hi) : L"-") + L"]";
            }
            o += L"\n";
        }
        if (fails.size() > n)
            o += L"  (" + U(fails.size() - n) + L" more failing tests omitted)\n";
    }
    if (!summary_.unknown.empty()) {
        o += L"Unknown record types:";
        for (const auto& kv : summary_.unknown)
            o += L" " + U(kv.first >> 8) + L"-" + U(kv.first & 0xFF) +
                 L" x" + U(kv.second);
        o += L"\n";
    }
    return o;
}

} // namespace stdf
} // namespace xfs
