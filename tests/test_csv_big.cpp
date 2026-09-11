// xfsWinPad - CsvBigModel 单测（批次 36）：行索引/物化/编码/引号态边界
#include "../src/csv/CsvBigModel.h"
#include "../src/csv/CsvBigFilter.h"
#include "../src/csv/CsvParser.h"
#include "../src/core/Util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace xfs;
using namespace xfs::csv;

static int g_failed = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failed; \
    } } while (0)

namespace {
namespace fs = std::filesystem;

std::wstring MakeTemp(const std::string& stem) {
    wchar_t dir[MAX_PATH];
    ::GetTempPathW(MAX_PATH, dir);
    std::wstring p = std::wstring(dir) + Utf8ToWide("csvbig_" + stem + ".csv");
    return p;
}

void WriteBytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

std::wstring Join(const std::vector<std::wstring>& cells) {
    std::wstring s;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i) s += L'|';
        s += cells[i];
    }
    return s;
}
} // namespace

int main() {
    // ---- 1. 基础 CRLF + 尾换行不产生幽灵行 --------------------------------
    {
        std::wstring p = MakeTemp("basic");
        WriteBytes(p, "a,b,c\r\n1,2,3\r\nx,y,z\r\n");
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.RowCount() == 3);
        CHECK(m.ColCount() == 3);
        CHECK(m.Delim() == L',');
        std::vector<std::wstring> c;
        CHECK(m.Row(0, c)); CHECK(Join(c) == L"a|b|c");
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"1|2|3");
        CHECK(m.Row(2, c)); CHECK(Join(c) == L"x|y|z");
        CHECK(!m.Row(3, c));   // 越界
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 2. 末行无换行 / 空末行 / 全空行 ----------------------------------
    {
        std::wstring p = MakeTemp("tail");
        WriteBytes(p, "a,b\n1,2");
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.RowCount() == 2);
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"1|2");
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    {
        std::wstring p = MakeTemp("tailnl");
        WriteBytes(p, "a,b\n,\n");   // 第二行 = 两个空格
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.RowCount() == 2);
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"|");
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 3. 引号内换行：行边界不被切断（核心差异 vs 按行切模型）-----------
    {
        std::wstring p = MakeTemp("qnl");
        WriteBytes(p, "a,b\n\"line1\nline2\",z\n\"x\"\"y\",2\n");
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.RowCount() == 3);   // 引号内 \n 不算行尾
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"line1\nline2|z");
        CHECK(m.Row(2, c)); CHECK(Join(c) == L"x\"y|2");
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 4. 引号内分隔符不计数；分号/制表符探测 ---------------------------
    {
        std::wstring p = MakeTemp("semi");
        WriteBytes(p, "a;b\n1;\"x;y\"\n");
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.Delim() == L';');
        CHECK(m.ColCount() == 2);
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"1|x;y");
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    {
        std::wstring p = MakeTemp("tab");
        WriteBytes(p, "a\tb\tc\n1\t2\t3\n");
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.Delim() == L'\t');
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 5. 短行：cols = 各行最大格数，物化越界列返回缺省 -----------------
    {
        std::wstring p = MakeTemp("short");
        WriteBytes(p, "a,b,c\n1\nx,y\n");
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.ColCount() == 3);
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"1");
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 6. UTF-8 BOM ------------------------------------------------------
    {
        std::wstring p = MakeTemp("bom");
        std::string bytes = "\xEF\xBB\xBF" "name,qty\nalpha,10\n";
        WriteBytes(p, bytes);
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.RowCount() == 2);
        std::vector<std::wstring> c;
        CHECK(m.Row(0, c)); CHECK(Join(c) == L"name|qty");   // BOM 不入首格
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 7. ANSI（无效 UTF-8 的 GBK 字节）---------------------------------
    {
        std::wstring p = MakeTemp("ansi");
        // "名字,值" 的 GBK 编码（C3 FB D7 D6 = 名字 的 GBK 前两字节...）
        // 直接用 0x80+ 高位字节序列：BB B6 D3 AD = "欢迎" GBK
        std::string bytes = "huan,1\n\xBB\xB6\xD3\xAD,2\n";
        WriteBytes(p, bytes);
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.Encoding() == encoding::EncodingType::ANSI);
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c));
        CHECK(c[0] == Utf8ToWide("\xBB\xB6\xD3\xAD", CP_ACP) ||
              c[0].size() == 2);   // ACP 随区域而变，长度 2 是 GBK 的稳定信号
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 8. UTF-16 拒绝 / 空文件拒绝 --------------------------------------
    {
        std::wstring p = MakeTemp("u16");
        WriteBytes(p, std::string("\xFF\xFE" "a\x00,\x00b\x00", 10));
        CsvBigModel m;
        CHECK(!m.Open(p));
        CHECK(!m.Error().empty());
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    {
        std::wstring p = MakeTemp("empty");
        WriteBytes(p, "");
        CsvBigModel m;
        CHECK(!m.Open(p));
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 9. 行内缓存：两次取同一行内容一致 --------------------------------
    {
        std::wstring p = MakeTemp("cache");
        WriteBytes(p, "a,b\n1,2\n3,4\n");
        CsvBigModel m;
        CHECK(m.Open(p));
        std::vector<std::wstring> c1, c2;
        CHECK(m.Row(1, c1));
        CHECK(m.Row(1, c2));
        CHECK(Join(c1) == Join(c2));
        CHECK(m.Row(2, c2)); CHECK(Join(c2) == L"3|4");
        CHECK(m.Row(1, c2)); CHECK(Join(c2) == L"1|2");   // 回跳
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 10. 裸 CR 行尾（CR-only）-----------------------------------------
    {
        std::wstring p = MakeTemp("cr");
        WriteBytes(p, "a,b\r1,2\r");
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.RowCount() == 2);
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"1|2");
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 11. 裸引号在格中间（非格首）：不进入引号态（与 Parse 一致）-------
    {
        std::wstring p = MakeTemp("midq");
        WriteBytes(p, "a,b\nx\"y,z\n");
        CsvBigModel m;
        CHECK(m.Open(p));
        std::vector<std::wstring> c;
        CHECK(m.Row(1, c)); CHECK(Join(c) == L"x\"y|z");
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 12. SplitRow 抽出件（批次 36 导出）回环 --------------------------
    {
        std::wstring arena;
        std::vector<std::pair<uint32_t, uint32_t>> cells;
        SplitRow(L"p,\"q,\",\"\"\"r\"\"\",", L',', arena, cells);
        CHECK(cells.size() == 4);   // p / q, / "r" / 尾空格
        std::vector<std::wstring> v;
        for (auto& c : cells)
            v.emplace_back(arena, c.first, c.second);
        CHECK(v[0] == L"p");
        CHECK(v[1] == L"q,");
        CHECK(v[2] == L"\"r\"");
        CHECK(v[3] == L"");
    }

    // ---- 13. 后台流式过滤（批次 37）：命中/进度/完成/空词 ------------------
    {
        std::wstring p = MakeTemp("filter");
        {
            std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc);
            f << "name,tag\n";
            for (int i = 1; i <= 1000; ++i) {
                if (i % 50 == 0) f << "row" << i << ",hit" << (i / 50) << "\n";
                else f << "row" << i << ",miss\n";
            }
        }
        CsvBigModel m;
        CHECK(m.Open(p));
        CHECK(m.RowCount() == 1001);
        {
            CsvBigFilter flt;
            flt.Start(&m, L"hit");
            CHECK(flt.Needle() == L"hit");
            // 等待完成（最多 10s）
            for (int t = 0; t < 100 && flt.Running(); ++t) ::Sleep(100);
            CHECK(!flt.Running());
            CHECK(flt.Progress() >= 1.0);
            CHECK(flt.MatchCount() == 20);   // 1000/50 = 20 行命中 "hit"
            CHECK(flt.RowAt(0) == 50);       // 首个命中 = 原始行 50
            CHECK(flt.RowAt(19) == 1000);
            // 重启换词
            flt.Start(&m, L"row999");
            for (int t = 0; t < 100 && flt.Running(); ++t) ::Sleep(100);
            CHECK(flt.MatchCount() == 1);
            CHECK(flt.RowAt(0) == 999);
            // 空词：立即停止，无结果
            flt.Start(&m, L"");
            CHECK(!flt.Running());
            CHECK(flt.MatchCount() == 0);
        }
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    // ---- 14. 过滤中途 Stop（大文件上快速终止）-----------------------------
    {
        std::wstring p = MakeTemp("stop");
        {
            std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc);
            f << "a,b\n";
            for (int i = 1; i <= 200000; ++i) f << "x" << i << ",y\n";
        }
        CsvBigModel m;
        CHECK(m.Open(p));
        {
            CsvBigFilter flt;
            flt.Start(&m, L"zzz-nomatch");
            ::Sleep(30);          // 让线程跑一小段
            flt.Stop();           // 中途终止（析构同样安全）
            CHECK(!flt.Running());
        }
        m.Close();
        std::error_code ec; std::filesystem::remove(p, ec);
    }

    if (g_failed == 0) std::printf("test_csv_big: all passed\n");
    else std::printf("test_csv_big: %d FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
