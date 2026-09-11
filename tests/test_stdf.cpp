// test_stdf.cpp — STDF v4 解析层单测（Phase 20）。
//
// 覆盖：
//   1. 合成小端流：FAR/MIR/PIR/PTR(聚合 min/max/mean/fail)/PRR(pass|fail)/
//      PCR(PRR 存在时不重复计数)/HBR/SBR/WIR/WRR/GDR/未知记录(10-30, 99-99)/
//      MPR(RSLT_STAT 失败 pin 统计)/MRR —— 索引、摘要、聚合全断言；
//   2. 合成大端流（CPU_TYPE=4）：REC_LEN/字段端序翻转正确；
//   3. 截断记录（尾部垃圾）优雅终止；
//   4. 非 STDF 文件拒绝；
//   5. 真实样本（temp/ 小样本，存在时才跑）：记录数/测试项数/耗时。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/stdf/StdfFile.h"
#include "../src/core/Util.h"

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---- 字节流构造器（支持端序）----------------------------------------------------
struct Buf {
    std::string b;
    bool be = false;
    explicit Buf(bool bigEndian = false) : be(bigEndian) {}

    void Raw(const char* p, size_t n) { b.append(p, n); }
    void U1(unsigned v) { b.push_back(static_cast<char>(v & 0xFF)); }
    void U2(unsigned v) {
        if (be) { b.push_back(static_cast<char>((v >> 8) & 0xFF)); b.push_back(static_cast<char>(v & 0xFF)); }
        else    { b.push_back(static_cast<char>(v & 0xFF)); b.push_back(static_cast<char>((v >> 8) & 0xFF)); }
    }
    void U4(unsigned v) {
        if (be) { for (int i = 3; i >= 0; --i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
        else    { for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
    }
    void R4(float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        U4(bits);
    }
    void Cn(const char* s) {
        size_t n = strlen(s);
        U1(n);
        Raw(s, n);
    }
    void Pad(int n) { for (int i = 0; i < n; ++i) U1(0); }
    // 追加完整记录（REC_LEN 编码随端序）
    void Rec(unsigned typ, unsigned sub, const std::string& body) {
        U2(static_cast<unsigned>(body.size()));
        U1(typ);
        U1(sub);
        Raw(body.data(), body.size());
    }
    template <typename F>
    void RecBody(unsigned typ, unsigned sub, F&& fill) {
        Buf body(be);
        fill(body);
        Rec(typ, sub, body.b);
    }
};

static bool WriteTemp(const wchar_t* tag, const std::string& data, std::wstring* out) {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"xfs_stdf_test_" + tag + L"_" +
                       std::to_wstring(GetCurrentProcessId()) + L".std";
    if (!WriteFileBytes(dir, data.data(), data.size())) return false;
    *out = dir;
    return true;
}

// ---- 合成小端样本 ---------------------------------------------------------------
static std::string BuildLeSample() {
    Buf f(false);

    // FAR: CPU_TYPE=2(LE) STDF_VER=4
    f.Rec(0, 10, std::string{static_cast<char>(2), static_cast<char>(4)});

    // MIR（时间戳 = Unix epoch 秒，真样本验证过）
    f.RecBody(1, 10, [](Buf& b) {
        b.U4(1786253561u);      // SETUP_T
        b.U4(1786254161u);      // START_T
        b.U1(1);                // STAT_NUM
        b.Pad(6);               // 6×C1（真样本验证的字节数）
        b.Cn("LOT123");         // LOT_ID
        b.Cn("CHIP");           // PART_TYP
        b.Cn("TESTER1");        // NODE_NAM
        b.Cn("IGXL");           // TSTR_TYP
        b.Cn("job.v1");         // JOB_NAM
        b.Cn("");               // JOB_REV
        b.Cn("op");             // OPER_NAM
        b.Cn("");               // EXEC_TYP
        b.Cn("");               // EXEC_VER
        b.Cn("");               // DAT_COD
        b.Cn("tc9");            // TST_COD
    });

    // PIR
    f.RecBody(5, 10, [](Buf& b) { b.U1(1); b.U1(0); });

    // PTR #100 ×3：1.5(pass) 1.2(pass) 1.8(fail)
    auto ptr100 = [&](float res, unsigned pflg) {
        f.RecBody(15, 10, [&](Buf& b) {
            b.U4(100); b.U1(1); b.U1(0);
            b.U1(0);            // TEST_FLG
            b.U1(pflg);         // PARM_FLG
            b.R4(res);
            b.Cn("vdd_test");
            b.Cn("");           // ALARM_ID
            b.U1(0);            // OPT_FLAG（低/高限有效）
            b.U1(0); b.U1(0); b.U1(0);   // RES_SCAL LLM_SCAL HLM_SCAL
            b.R4(1.0f); b.R4(2.0f);      // LO HI
            b.Cn("V");          // UNITS
        });
    };
    ptr100(1.5f, 0);
    ptr100(1.2f, 0);
    ptr100(1.8f, 0x08);   // PARM_FLG bit3 = fail

    // PTR #200（无限值记录：OPT_FLAG bit3|bit4 → 无低/高限）
    f.RecBody(15, 10, [&](Buf& b) {
        b.U4(200); b.U1(1); b.U1(0);
        b.U1(0); b.U1(0);
        b.R4(3.0f);
        b.Cn("open_test");
        b.Cn("");
        b.U1(0x18);         // OPT_FLAG bit3|bit4 → 无低/高限
        b.Cn("A");          // UNITS
    });

    // MPR #300：4 pin，RSLT_STAT = {1,0,1,1} → 1 个失败 pin
    f.RecBody(15, 15, [&](Buf& b) {
        b.U4(300); b.U1(1); b.U1(0);
        b.U1(0); b.U1(0);
        b.Cn("io_scan");
        b.Cn("");
        b.U1(0);
        b.U1(0); b.U1(0); b.U1(0);
        b.R4(0.0f); b.R4(5.0f);
        b.Cn("mA");
        b.U2(4);            // N_PARMS
        b.Pad(4);           // RTN_STAT
        b.U1(1); b.U1(0); b.U1(1); b.U1(1);   // RSLT_STAT
    });

    // PRR ×2：site0 pass / site1 fail
    f.RecBody(5, 20, [&](Buf& b) {
        b.U1(1); b.U1(0);
        b.U1(0x08);         // PART_FLG bit3 = pass
        b.U2(10);           // NUM_TEST
        b.U2(1);            // HARD_BIN
        b.U2(2);            // SOFT_BIN
        b.U2(0); b.U2(0);   // X Y
        b.U4(5);            // TEST_T
        b.Cn("P001");
        b.Cn("");
        b.Cn("");
    });
    f.RecBody(5, 20, [&](Buf& b) {
        b.U1(1); b.U1(1);
        b.U1(0x00);         // fail
        b.U2(10);
        b.U2(2); b.U2(0);
        b.U2(0); b.U2(0);
        b.U4(5);
        b.Cn("P002");
        b.Cn("");
        b.Cn("");
    });

    // PCR site 0（PRR 已计数 → 不应重复计入 part/good）
    f.RecBody(1, 30, [](Buf& b) {
        b.U1(1); b.U1(0);
        b.U4(5); b.U4(0); b.U4(0); b.U4(4); b.U4(0);
    });

    // HBR / SBR（PF 字段：规范 bit3=1 为 pass，故 pass bin 写 8）
    f.RecBody(1, 40, [&](Buf& b) {
        b.U1(1); b.U1(0); b.U2(1); b.U4(1); b.U1(8); b.Cn("pass_bin");
    });
    f.RecBody(1, 50, [&](Buf& b) {
        b.U1(1); b.U1(0); b.U2(10); b.U4(1); b.U1(0); b.Cn("");
    });

    // WIR / WRR（时间戳 Unix epoch）
    f.RecBody(2, 10, [&](Buf& b) {
        b.U1(1); b.U1(0); b.U4(1786253761u); b.Cn("W01");
    });
    f.RecBody(2, 20, [&](Buf& b) {
        b.U1(1); b.U1(0); b.U4(1786254561u);
        b.Cn("P001");                   // PART_ID
        b.Pad(16);                      // RTST ABRT GOOD FUNC
        b.Cn("W01");                    // WAFER_ID
        b.Cn(""); b.Cn("");             // FABWF_ID FRAME_ID
    });

    // GDR + 未知记录（真样本见 10-30；再补 99-99）+ FTR + MRR
    f.RecBody(20, 10, [](Buf& b) { b.U2(0); b.Cn("x"); });
    f.RecBody(10, 30, [](Buf& b) { b.Pad(8); });
    f.RecBody(99, 99, [](Buf& b) { b.Pad(8); });
    f.RecBody(15, 20, [](Buf& b) { b.U4(100); b.U1(1); b.U1(0); b.Pad(24); });
    f.RecBody(1, 20, [](Buf& b) { b.U4(1786254961u); });

    return f.b;
}

// ---- 合成大端样本 ---------------------------------------------------------------
static std::string BuildBeSample() {
    Buf f(true);
    // FAR: REC_LEN=2 在 BE 下是 00 02；CPU_TYPE=4(BE) STDF_VER=4
    f.Rec(0, 10, std::string{static_cast<char>(4), static_cast<char>(4)});
    // PTR：全部字段 BE
    f.RecBody(15, 10, [&](Buf& b) {
        b.U4(100); b.U1(1); b.U1(2);
        b.U1(0); b.U1(0);
        b.R4(2.25f);
        b.Cn("be_test");
        b.Cn("");
        b.U1(0);
        b.U1(0); b.U1(0); b.U1(0);
        b.R4(1.0f); b.R4(3.0f);
        b.Cn("V");
    });
    return f.b;
}

// ---- 用例 -----------------------------------------------------------------------
static void RunLeSample() {
    std::wstring path;
    CHECK(WriteTemp(L"le", BuildLeSample(), &path));
    stdf::StdfFile sf;
    std::wstring err;
    CHECK(sf.Open(path, &err));
    CHECK(err.empty());
    const auto& s = sf.Summary();

    // 索引
    const size_t expectRecs = 20;   // FAR MIR PIR PTR×4 MPR PRR×2 PCR HBR SBR WIR WRR GDR UNK(10-30) UNK(99-99) FTR MRR
    CHECK(s.recCount == expectRecs);
    CHECK(sf.Records().size() == s.recCount);
    CHECK(s.ptrCount == 4);
    CHECK(s.mprCount == 1);
    CHECK(s.pirCount == 1);
    CHECK(s.prrCount == 2);
    CHECK(s.wirCount == 1);
    CHECK(s.wrrCount == 1);
    CHECK(s.gdrCount == 1);
    CHECK(s.cpuType == 2);
    CHECK(s.stdfVersion == 4);
    // 未知记录：10-30 与 99-99 各 1
    CHECK(s.unknown.size() == 2);
    CHECK(s.unknown.at(10u * 256u + 30u) == 1);
    CHECK(s.unknown.at(99u * 256u + 99u) == 1);
    // 记录名
    CHECK(strcmp(stdf::StdfFile::RecordName(15, 10), "PTR") == 0);
    CHECK(strcmp(stdf::StdfFile::RecordName(10, 30), "10-30") == 0);

    // 摘要（MIR/MRR/WRR）
    const char* base = nullptr;   // mmap 内部指针；用 Tests() 的切片经 StdfFile::ToString
    base = sf.Summary().dataSize ? nullptr : nullptr;   // 占位：切片基址经 Open 后有效
    // part/good：PRR 为准（PCR 的 5/4 不计入）
    CHECK(s.partCount == 2);
    CHECK(s.goodCount == 1);
    CHECK(s.sites.size() == 2);
    // site0: 1/1, site1: 1/0
    bool site0 = false, site1 = false;
    for (const auto& sc : s.sites) {
        if (sc.site == 0) { site0 = sc.partCount == 1 && sc.goodCount == 1; }
        if (sc.site == 1) { site1 = sc.partCount == 1 && sc.goodCount == 0; }
    }
    CHECK(site0);
    CHECK(site1);

    // HBR/SBR
    CHECK(s.hbins.size() == 1);
    CHECK(s.hbins[0].num == 1 && s.hbins[0].count == 1);
    CHECK(s.sbins.size() == 1 && s.sbins[0].num == 10);

    // 测试项聚合
    const auto& tests = sf.Tests();
    CHECK(tests.size() == 3);   // #100(P) #200(P) #300(M)
    const stdf::TestItem* t100 = nullptr;
    const stdf::TestItem* t200 = nullptr;
    const stdf::TestItem* t300 = nullptr;
    for (const auto& t : tests) {
        if (t.testNum == 100) t100 = &t;
        else if (t.testNum == 200) t200 = &t;
        else if (t.testNum == 300) t300 = &t;
    }
    CHECK(t100 && t200 && t300);
    CHECK(t100->kind == 'P');
    CHECK(t100->count == 3);
    CHECK(t100->failCount == 1);
    CHECK(std::fabs(t100->minV - 1.2f) < 1e-6);
    CHECK(std::fabs(t100->maxV - 1.8f) < 1e-6);
    CHECK(std::fabs(t100->Mean() - 1.5) < 1e-6);
    CHECK(t100->hasLo && t100->hasHi);
    CHECK(std::fabs(t100->lo - 1.0f) < 1e-6);
    CHECK(std::fabs(t100->hi - 2.0f) < 1e-6);
    CHECK(t200->count == 1 && !t200->hasLo && !t200->hasHi);
    CHECK(std::fabs(t200->maxV - 3.0f) < 1e-6);
    CHECK(t300->kind == 'M');
    CHECK(t300->count == 1);
    CHECK(t300->failCount == 1);   // RSLT_STAT {1,0,1,1} → 1 fail

    // 文本切片（需 mmap 基址——重新打开验证，利用 StdfFile::ToString）
    {
        stdf::StdfFile sf2;
        CHECK(sf2.Open(path));
        const char* b2 = nullptr;
        // 通过一条已知记录验证 ToString：用 MIR lotId —— 从 Summary 拿切片。
        // mmap 基址不暴露；改用全文件 bytes 复核切片内容。
        std::string raw;
        CHECK(ReadFileBytes(path, raw));
        b2 = raw.data();
        const auto& s2 = sf2.Summary();
        CHECK(stdf::StdfFile::ToString(s2.lotId, b2) == L"LOT123");
        CHECK(stdf::StdfFile::ToString(s2.partTyp, b2) == L"CHIP");
        CHECK(stdf::StdfFile::ToString(s2.jobNam, b2) == L"job.v1");
        CHECK(stdf::StdfFile::ToString(s2.operNam, b2) == L"op");
        CHECK(stdf::StdfFile::ToString(s2.testCod, b2) == L"tc9");
        CHECK(stdf::StdfFile::ToString(s2.waferId, b2) == L"W01");
        CHECK(!s2.startT.empty());
        CHECK(!s2.finishT.empty());
        for (const auto& t : sf2.Tests()) {
            if (t.testNum == 100)
                CHECK(stdf::StdfFile::ToString(t.testTxt, b2) == L"vdd_test");
            if (t.testNum == 200) {
                CHECK(stdf::StdfFile::ToString(t.testTxt, b2) == L"open_test");
                CHECK(stdf::StdfFile::ToString(t.units, b2) == L"A");
            }
            if (t.testNum == 300)
                CHECK(stdf::StdfFile::ToString(t.testTxt, b2) == L"io_scan");
        }

        // ---- ComputeStats（S4 统计：mean/σ/Cp/Cpk/超限）----
        // 合成 LE 样本只有 1 颗 die（3 条 PTR#100 相互覆盖 → results[0]=1.8）
        size_t idx100 = sf2.Tests().size(), idx200 = sf2.Tests().size(),
               idx300 = sf2.Tests().size();
        for (size_t i = 0; i < sf2.Tests().size(); ++i) {
            if (sf2.Tests()[i].testNum == 100) idx100 = i;
            else if (sf2.Tests()[i].testNum == 200) idx200 = i;
            else if (sf2.Tests()[i].testNum == 300) idx300 = i;
        }
        CHECK(idx100 < sf2.Tests().size());
        auto st = stdf::StdfFile::ComputeStats(sf2, idx100);
        CHECK(st.n == 1);
        CHECK(std::fabs(st.mean - 1.8) < 1e-6);
        CHECK(st.sigma == 0);
        CHECK(std::fabs(st.minV - 1.8) < 1e-6);
        CHECK(std::fabs(st.maxV - 1.8) < 1e-6);
        CHECK(st.hasLo && st.hasHi);
        CHECK(st.overLo == 0 && st.overHi == 0);
        CHECK(st.Cp == 0 && st.Cpk == 0);   // 单样本 σ=0 → 无 Cp/Cpk
        // 空索引/越界防御
        auto stBad = stdf::StdfFile::ComputeStats(sf2, 9999);
        CHECK(stBad.n == 0);
    }
    DeleteFileW(path.c_str());
}

// ---- 多 die 统计样本：4 颗 die 的 #100 = {0.9, 1.5, 1.8, 2.1}，限值 [1,2] ----
static std::string BuildStatsSample() {
    Buf f(false);
    f.Rec(0, 10, std::string{static_cast<char>(2), static_cast<char>(4)});
    auto die = [&](float v, bool withLimits) {
        f.RecBody(5, 10, [](Buf& b) { b.U1(1); b.U1(0); });   // PIR
        f.RecBody(15, 10, [&](Buf& b) {
            b.U4(100); b.U1(1); b.U1(0);
            b.U1(0); b.U1(0);
            b.R4(v);
            b.Cn("stat_test");
            b.Cn("");
            if (withLimits) {
                b.U1(0); b.U1(0); b.U1(0); b.U1(0);
                b.R4(1.0f); b.R4(2.0f);
                b.Cn("V");
            } else {
                b.U1(0x18);
                b.Cn("V");
            }
        });
        f.RecBody(5, 20, [](Buf& b) {
            b.U1(1); b.U1(0);
            b.U1(0x08);         // pass
            b.U2(1);
            b.U2(1); b.U2(0);
            b.U2(0); b.U2(0);
            b.U4(5);
            b.Cn("D");
            b.Cn(""); b.Cn("");
        });
    };
    die(0.9f, true);    // 超下限
    die(1.5f, true);
    die(1.8f, true);
    die(2.1f, true);    // 超上限
    return f.b;
}

static void RunStatsSample() {
    std::wstring path;
    CHECK(WriteTemp(L"stats", BuildStatsSample(), &path));
    stdf::StdfFile sf;
    CHECK(sf.Open(path));
    CHECK(sf.Tests().size() == 1);
    CHECK(sf.Parts().size() == 4);
    auto st = stdf::StdfFile::ComputeStats(sf, 0);
    CHECK(st.n == 4);
    CHECK(std::fabs(st.mean - 1.575) < 1e-6);
    // σ = sqrt(0.7875/4) = 0.4437096…
    CHECK(std::fabs(st.sigma - 0.4437096) < 1e-5);
    CHECK(std::fabs(st.minV - 0.9) < 1e-6);
    CHECK(std::fabs(st.maxV - 2.1) < 1e-6);
    CHECK(st.hasLo && st.hasHi);
    CHECK(st.overLo == 1 && st.overHi == 1);
    // Cp = 1/(6σ) = 0.3756291… ; Cpk = 0.425/(3σ) = 0.3192757…
    CHECK(std::fabs(st.Cp - 0.3756291) < 1e-5);
    CHECK(std::fabs(st.Cpk - 0.3192757) < 1e-5);

    // ---- CSV 过滤导出（rowMap 行子集）----
    std::string full = stdf::StdfFile::ToDatalogCsv(sf);
    {
        size_t lines = 0;
        for (size_t i = 0; i < full.size(); ++i)
            if (full[i] == '\n') ++lines;
        CHECK(lines == 8);   // 4 头行 + 4 die
    }
    std::vector<int32_t> subset{ 1, 3 };
    std::string part = stdf::StdfFile::ToDatalogCsv(sf, &subset);
    {
        size_t lines = 0;
        for (size_t i = 0; i < part.size(); ++i)
            if (part[i] == '\n') ++lines;
        CHECK(lines == 6);   // 4 头行 + 2 die
    }
    // 子集行内容 = 全量里对应行（第 5 行起第 2 与第 4 行）
    auto lineAt = [](const std::string& s, size_t idx) {
        size_t start = 0;
        for (size_t i = 0; i < idx; ++i) {
            size_t e = s.find('\n', start);
            if (e == std::string::npos) return std::string();
            start = e + 1;
        }
        size_t e = s.find('\n', start);
        return s.substr(start, e == std::string::npos ? std::string::npos
                                                      : e - start);
    };
    CHECK(lineAt(part, 4) == lineAt(full, 5));
    CHECK(lineAt(part, 5) == lineAt(full, 7));
    DeleteFileW(path.c_str());
}

static void RunBeSample() {
    std::wstring path;
    CHECK(WriteTemp(L"be", BuildBeSample(), &path));
    stdf::StdfFile sf;
    CHECK(sf.Open(path));
    const auto& s = sf.Summary();
    CHECK(s.cpuType == 4);
    CHECK(s.ptrCount == 1);
    CHECK(sf.Tests().size() == 1);
    const auto& t = sf.Tests()[0];
    CHECK(t.testNum == 100);
    CHECK(std::fabs(t.maxV - 2.25f) < 1e-6);
    CHECK(std::fabs(t.hi - 3.0f) < 1e-6);
    CHECK(std::fabs(t.lo - 1.0f) < 1e-6);
    std::string raw;
    CHECK(ReadFileBytes(path, raw));
    CHECK(stdf::StdfFile::ToString(t.testTxt, raw.data()) == L"be_test");
    DeleteFileW(path.c_str());
}

static void RunTruncated() {
    Buf f(false);
    f.Rec(0, 10, std::string{static_cast<char>(2), static_cast<char>(4)});
    std::string partial;
    partial.push_back(static_cast<char>(100));   // REC_LEN 低位 =100
    partial.push_back(0);
    partial.push_back(static_cast<char>(15));
    partial.push_back(static_cast<char>(10));
    partial.append("JUST10BYTE");
    f.b += partial;
    std::wstring path;
    CHECK(WriteTemp(L"trunc", f.b, &path));
    stdf::StdfFile sf;
    CHECK(sf.Open(path));        // 容忍截断：已完整记录仍可用
    CHECK(sf.Summary().recCount == 1);   // 只有 FAR 完整
    CHECK(sf.Tests().empty());
    DeleteFileW(path.c_str());
}

static void RunReject() {
    // 文本文件
    std::wstring path;
    CHECK(WriteTemp(L"txt", "hello world this is not stdf at all......", &path));
    stdf::StdfFile sf;
    std::wstring err;
    CHECK(!sf.Open(path, &err));
    CHECK(!err.empty());
    DeleteFileW(path.c_str());
    // 太小的文件
    CHECK(WriteTemp(L"tiny", "\x02", &path));
    stdf::StdfFile sf2;
    CHECK(!sf2.Open(path));
    DeleteFileW(path.c_str());
}

// ---- AI 统计块（批次 28）-------------------------------------------------------
static void RunAiStatsBlock() {
    std::wstring path;
    CHECK(WriteTemp(L"aiblk", BuildLeSample(), &path));
    stdf::StdfFile sf;
    CHECK(sf.Open(path));
    std::wstring blk = sf.FormatAiStatsBlock(path);

    // 块头与文件行
    CHECK(blk.find(L"[STDF test data]\n") == 0);
    CHECK(blk.find(L"File: " + path) != std::wstring::npos);
    // MIR 标识行（Lot/Product/Job 有值；空段 JobRev 不出现）
    CHECK(blk.find(L"Lot: LOT123") != std::wstring::npos);
    CHECK(blk.find(L"Product: CHIP") != std::wstring::npos);
    CHECK(blk.find(L"Job: job.v1") != std::wstring::npos);
    CHECK(blk.find(L"JobRev") == std::wstring::npos);
    // 产量行：2 parts / 1 pass / 1 fail / 50.00%
    CHECK(blk.find(L"Parts tested: 2 | Pass: 1 | Fail: 1 | Yield: 50.00%")
          != std::wstring::npos);
    // per-site：site0=1/1 site1=1/0
    CHECK(blk.find(L"Per-site (site=parts/pass): 0=1/1 1=1/0")
          != std::wstring::npos);
    // bin：HBIN num1 count1 pass 名 pass_bin；SBR num10 fail 无名
    CHECK(blk.find(L"Hardware bins (top by count):\n  bin 1 x1 [PASS] pass_bin")
          != std::wstring::npos);
    CHECK(blk.find(L"Software bins (top by count):\n  bin 10 x1 [FAIL]")
          != std::wstring::npos);
    // 失败测试项段：#100 PTR fails=1/3 (33.33%) mean=1.5 limits=[1,2]；
    // MPR #300 用 pin-fails=1/1；无限值的 #200 不进失败段
    CHECK(blk.find(L"Failing tests (top by fail count):") != std::wstring::npos);
    CHECK(blk.find(L"TEST 100 PTR \"vdd_test\" units=V fails=1/3 (33.33%)"
                   L" mean=1.5 limits=[1,2]") != std::wstring::npos);
    CHECK(blk.find(L"TEST 300 MPR \"io_scan\" units=mA pin-fails=1/1")
          != std::wstring::npos);
    CHECK(blk.find(L"TEST 200") == std::wstring::npos);
    CHECK(blk.find(L"Failing tests: none") == std::wstring::npos);
    // 未知记录段：10-30 与 99-99 各 1
    CHECK(blk.find(L"Unknown record types: 10-30 x1 99-99 x1")
          != std::wstring::npos);

    // 无失败样本 → "Failing tests: none"
    std::wstring path2;
    CHECK(WriteTemp(L"aiblk2", BuildStatsSample(), &path2));
    stdf::StdfFile sf2;
    CHECK(sf2.Open(path2));
    std::wstring blk2 = sf2.FormatAiStatsBlock(path2);
    CHECK(blk2.find(L"Failing tests: none") != std::wstring::npos);
    CHECK(blk2.find(L"Failing tests (top by fail count):") == std::wstring::npos);
    // 4 颗全 pass：yield 100%
    CHECK(blk2.find(L"Parts tested: 4 | Pass: 4 | Fail: 0 | Yield: 100.00%")
          != std::wstring::npos);
    DeleteFileW(path.c_str());
    DeleteFileW(path2.c_str());
}

#ifdef XFS_STDF_SAMPLE
static void RunRealSample() {
    std::wstring path;
    for (const char* p = XFS_STDF_SAMPLE; *p; ++p) path += static_cast<wchar_t>(*p);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        printf("SKIP real sample not found\n");
        return;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    stdf::StdfFile sf;
    if (!sf.Open(path)) {
        ++g_fail;
        printf("FAIL real sample open\n");
        return;
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
    const auto& s = sf.Summary();
    CHECK(s.recCount == 33879);
    CHECK(s.ptrCount == 27225);
    CHECK(sf.Records().size() == 33879);
    CHECK(!sf.Tests().empty());
    CHECK(!s.startT.empty());
    printf("real sample: %llu records, %zu test items, %.1f ms\n",
           static_cast<unsigned long long>(s.recCount), sf.Tests().size(), ms);

    // ---- datalog 矩阵（PIR..PRR 结果表）----
    const auto& parts = sf.Parts();
    const auto& order = sf.TestColumnOrder();
    CHECK(parts.size() == 1);          // SAMPLE-A 样本只有 1 颗（PIR/PRR 各 1）
    CHECK(order.size() == sf.Tests().size());
    // 列序 = 执行序（首见序）——与测试机 ATE CSV 一致
    for (size_t i = 0; i < order.size(); ++i)
        CHECK(order[i] == i);
    // 该颗结果应填满（首测试项数值有效）
    CHECK(parts[0].results.size() >= 1);
    CHECK(!std::isnan(parts[0].results[order[0]]));

    // ---- CSV 导出格式 ----
    std::string csv = stdf::StdfFile::ToDatalogCsv(sf);
    CHECK(csv.find("Test_time(ms),Test_no,Site,X_POS,Y_POS,HW_bin,SW_bin,Result") == 0);
    CHECK(csv.find("\nUnit") != std::string::npos);
    CHECK(csv.find("\nLow Limit") != std::string::npos);
    CHECK(csv.find("\nHigh Limit") != std::string::npos);
    CHECK(csv.find("Pass") != std::string::npos || csv.find("Fail") != std::string::npos);
    // 每行列数一致（= 8 固定 + N 测试项）
    {
        size_t lineStart = 0, ln = 0;
        size_t expected = 0;
        for (;;) {
            size_t e = csv.find('\n', lineStart);
            if (e == std::string::npos) break;
            size_t commas = 0;
            for (size_t i = lineStart; i < e; ++i)
                if (csv[i] == ',') ++commas;
            if (ln == 0) expected = commas;
            else if (ln < 4 || ln >= 4) {
                // 数据行可能因末尾空单元格省略尾逗号？——我们固定全写，列数恒定
                CHECK(commas == expected);
            }
            ++ln;
            lineStart = e + 1;
        }
        CHECK(ln == 5);   // 4 头行 + 1 数据行
    }
    printf("datalog: %zu parts x %zu tests, csv %zu bytes\n",
           parts.size(), order.size(), csv.size());
}
#endif

int main() {
    printf("== test_stdf ==\n");
    RunLeSample();
    RunBeSample();
    RunTruncated();
    RunReject();
    RunStatsSample();
    RunAiStatsBlock();
#ifdef XFS_STDF_SAMPLE
    RunRealSample();
#endif
    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
