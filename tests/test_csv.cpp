// xfsWinPad - CSV 解析/排序/过滤单测（批次 32/33）
#include "../src/csv/CsvParser.h"
#include "../src/csv/CsvPrint.h"
#include "../src/core/Util.h"

#include <cstdio>
#include <string>

using namespace xfs;
using namespace xfs::csv;

static int g_failed = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static std::wstring Join(const CsvData& d, size_t row) {
    std::wstring s;
    for (size_t c = 0; c < d.cols; ++c) {
        if (c) s += L"|";
        s += d.Cell(row, c);
    }
    return s;
}

int main() {
    // ---- 基本解析：逗号 + 引号 + 转义 + CRLF/LF 混排 ----------------------
    {
        CsvData d = Parse(L"a,b,c\r\n1,\"x,y\",\"he said \"\"hi\"\"\"\n2,3,4", -1);
        CHECK(d.RowCount() == 3);
        CHECK(d.cols == 3);
        CHECK(d.delim == L',');
        CHECK(Join(d, 0) == L"a|b|c");
        CHECK(Join(d, 1) == L"1|x,y|he said \"hi\"");
        CHECK(Join(d, 2) == L"2|3|4");
    }
    // ---- 尾行无换行 + 文件以换行结尾不补空行 ------------------------------
    {
        CsvData d = Parse(L"p,q\n1,2\n3,4\n", -1);
        CHECK(d.RowCount() == 3);
        CsvData e = Parse(L"p,q\n1,2\n3,4", -1);
        CHECK(e.RowCount() == 3);
        CHECK(Join(e, 2) == L"3|4");
    }
    // ---- 分隔符自动探测：分号 vs 制表 -------------------------------------
    {
        CsvData d = Parse(L"a;b;c\n1;2;3\n4;5;6", -1);
        CHECK(d.delim == L';');
        CHECK(d.cols == 3);
        CsvData t = Parse(L"a\tb\tc\n1\t2\t3", -1);
        CHECK(t.delim == L'\t');
        // 引号内分隔符不计数（首行 "1,2";3 → 逗号在引号内，分号在外）
        CsvData q = Parse(L"\"1,2\";3\nx,y", -1);
        CHECK(q.delim == L';');
        CHECK(q.cols == 2);
        CHECK(Join(q, 0) == L"1,2|3");
    }
    // ---- BOM 跳过 + 单列 + maxRows 截断 ------------------------------------
    {
        CsvData d = Parse(L"\uFEFFonly-one\n2", -1);
        CHECK(d.RowCount() == 2);
        CHECK(d.cols == 1);
        CHECK(Join(d, 0) == L"only-one");
        CsvData m = Parse(L"a,b\n1,2\n3,4\n5,6", 2);   // 表头 + 1 数据行
        CHECK(m.RowCount() == 2);
        CHECK(Join(m, 1) == L"1|2");
    }
    // ---- 短行：越界列返回空视图 -------------------------------------------
    {
        CsvData d = Parse(L"a,b,c\n1,2", -1);
        CHECK(d.cols == 3);
        CHECK(d.Cell(1, 2).empty());
    }
    // ---- 排序：数值感知 + 混排（数值在前）+ 降序 ---------------------------
    // 注意：row 0 是表头，rows 必须覆盖全部 6 行（0..5）
    {
        CsvData d = Parse(L"n\n10\n2\n1.5\n-3\nabc\n", -1);
        CHECK(d.RowCount() == 6);
        std::vector<uint32_t> rows{0, 1, 2, 3, 4, 5};
        SortRows(d, 0, false, rows);
        // 数值：-3, 1.5, 2, 10；abc 非数值排最后
        CHECK(d.Cell(rows[0], 0) == L"-3");
        CHECK(d.Cell(rows[1], 0) == L"1.5");
        CHECK(d.Cell(rows[2], 0) == L"2");
        CHECK(d.Cell(rows[3], 0) == L"10");
        CHECK(d.Cell(rows[4], 0) == L"abc");
        CHECK(d.Cell(rows[5], 0) == L"n");
        SortRows(d, 0, true, rows);
        CHECK(d.Cell(rows[0], 0) == L"n");
        CHECK(d.Cell(rows[1], 0) == L"abc");
        CHECK(d.Cell(rows[5], 0) == L"-3");
    }
    // ---- 排序：stable（相等键保持原顺序）+ 非法列号 no-op ------------------
    {
        CsvData d = Parse(L"k,v\n1,a\n1,b\n0,c\n", -1);
        CHECK(d.RowCount() == 4);
        std::vector<uint32_t> rows{0, 1, 2, 3};
        SortRows(d, 0, false, rows);
        CHECK(d.Cell(rows[0], 1) == L"c");
        CHECK(d.Cell(rows[1], 1) == L"a");
        CHECK(d.Cell(rows[2], 1) == L"b");
        CHECK(d.Cell(rows[3], 1) == L"v");   // 表头行按文本排最后
        std::vector<uint32_t> keep{2, 1, 0};
        SortRows(d, 9, false, keep);
        CHECK(keep[0] == 2 && keep[1] == 1 && keep[2] == 0);
    }
    // ---- 过滤：大小写不敏感 + 空词全选 + 多列命中 --------------------------
    {
        CsvData d = Parse(L"Name,City\nAlice,beijing\nBOB,Shanghai\nCarol,Beijing\n", -1);
        auto all = FilterRows(d, L"");
        CHECK(all.size() == 4);
        auto f1 = FilterRows(d, L"beijing");
        CHECK(f1.size() == 2);   // 两行数据（大小写不敏感）；表头 City 不命中
        auto f2 = FilterRows(d, L"ALI");
        CHECK(f2.size() == 1);
        CHECK(f2[0] == 1);
        auto f3 = FilterRows(d, L"zzz");
        CHECK(f3.empty());
    }
    // ---- Cell 越界安全 ------------------------------------------------------
    {
        CsvData d = Parse(L"a,b", -1);
        CHECK(d.Cell(99, 0).empty());
        CHECK(d.Cell(0, 99).empty());
    }
    // ---- SetCell（批次 33）：等长/变长/跨行偏移平移 --------------------------
    {
        CsvData d = Parse(L"a,b,c\n1,2,3\nx,y,z\n", -1);
        d.SetCell(1, 1, L"22");           // 变长 +1
        CHECK(Join(d, 1) == L"1|22|3");
        CHECK(Join(d, 2) == L"x|y|z");    // 后续行偏移已平移
        d.SetCell(0, 0, L"HEADER");       // 表头行同样可改
        CHECK(d.Cell(0, 0) == L"HEADER");
        d.SetCell(2, 2, L"zzz");
        CHECK(Join(d, 1) == L"1|22|3");   // 后改前面行不影响已改内容
        CHECK(Join(d, 2) == L"x|y|zzz");
        d.SetCell(0, 0, L"A");            // 缩短（delta < 0）
        CHECK(d.Cell(0, 0) == L"A");
        CHECK(Join(d, 1) == L"1|22|3");
        d.SetCell(99, 0, L"no");          // 越界静默
        d.SetCell(1, 99, L"no");
        CHECK(Join(d, 1) == L"1|22|3");
        // 含分隔符/引号/换行的值原样写入（序列化时才加引号）
        d.SetCell(1, 0, L"x,\"q\"\n2");
        CHECK(d.Cell(1, 0) == L"x,\"q\"\n2");
    }
    // ---- SerializeCsv（批次 33）：回环 + 引号规则 + 行尾/尾换行保真 ----------
    {
        CsvData d = Parse(L"Name,Qty,Note\r\nalpha,10,\"x,y\"\r\nbeta,2,\"he said \"\"hi\"\"\"\r\n", -1);
        std::string out = SerializeCsv(d, L',', "\r\n", true);
        // 语义等价回环：再解析后单元格一致
        std::wstring back = Utf8ToWide(out);
        CsvData d2 = Parse(back, -1);
        CHECK(d2.RowCount() == d.RowCount());
        CHECK(d2.cols == d.cols);
        CHECK(Join(d2, 0) == Join(d, 0));
        CHECK(Join(d2, 1) == Join(d, 1));
        CHECK(Join(d2, 2) == Join(d, 2));
        // 引号规则：含逗号/引号/换行的格加引号 + " 翻倍；普通格不加
        CHECK(out.find("alpha,10,\"x,y\"") != std::string::npos);
        CHECK(out.find("\"he said \"\"hi\"\"\"") != std::string::npos);
        CHECK(out.rfind("\r\n") == out.size() - 2);   // 尾换行保真
        // LF 行尾 + 无尾换行
        CsvData e = Parse(L"a,b\n1,2\n3,4", -1);
        std::string out2 = SerializeCsv(e, L',', "\n", false);
        CHECK(out2 == "a,b\n1,2\n3,4");
        // 分号分隔符
        CsvData s = Parse(L"a;b\n1;2", -1);
        std::string out3 = SerializeCsv(s, L';', "\n", false);
        CHECK(out3 == "a;b\n1;2");
        // 逗号不是活动分隔符 → 值里无需加引号（回环等价）
        CsvData m = Parse(L"a\tb\n1,2\t3", -1);
        std::string out4 = SerializeCsv(m, L'\t', "\n", false);
        CHECK(out4 == "a\tb\n1,2\t3");
    }
    // ---- 行操作（批次 34）：InsertRow / DeleteRow / SetCell 短行补列 ---------
    {
        CsvData d = Parse(L"a,b,c\n1,2,3\nx,y,z\n", -1);
        d.InsertRow(1);                          // 表头后插空行
        CHECK(d.RowCount() == 4);
        CHECK(Join(d, 1) == L"||");              // 3 个空格
        d.SetCell(1, 1, L"NEW");                 // 空行直接可写
        CHECK(Join(d, 1) == L"|NEW|");
        CHECK(Join(d, 2) == L"1|2|3");           // 后续行不受影响
        d.DeleteRow(2);                          // 删掉 1|2|3 行
        CHECK(d.RowCount() == 3);
        CHECK(Join(d, 1) == L"|NEW|");
        CHECK(Join(d, 2) == L"x|y|z");
        // 序列化回环
        std::string out = SerializeCsv(d, L',', "\n", false);
        CsvData back = Parse(Utf8ToWide(out), -1);
        CHECK(back.RowCount() == 3);
        CHECK(Join(back, 1) == L"|NEW|");
        // 表头行也可删
        CsvData h = Parse(L"H1,H2\n1,2\n", -1);
        h.DeleteRow(0);
        CHECK(h.RowCount() == 1);
        CHECK(Join(h, 0) == L"1|2");
        // 末尾追加（row == RowCount）
        CsvData ap = Parse(L"a,b\n1,2", -1);
        ap.InsertRow(ap.RowCount());
        CHECK(ap.RowCount() == 3);
        ap.SetCell(2, 0, L"tail");
        CHECK(Join(ap, 2) == L"tail|");
        CHECK(ap.Cell(2, 1).empty());
        // 越界静默
        ap.InsertRow(99);
        ap.DeleteRow(99);
        CHECK(ap.RowCount() == 3);
        // SetCell 短行补列：1 列行写到第 3 列
        CsvData s = Parse(L"a,b,c\n1\n", -1);
        s.SetCell(1, 2, L"x");
        CHECK(Join(s, 1) == L"1||x");
        std::string so = SerializeCsv(s, L',', "\n", false);
        CHECK(so == "a,b,c\n1,,x");
    }
    // ---- 列操作（批次 35）：InsertCol / DeleteCol ----------------------------
    {
        CsvData d = Parse(L"a,b,c\n1,2,3\nx,y\n", -1);   // 第二数据行是短行
        d.InsertCol(1);                                   // 在 b 前插空列
        CHECK(d.cols == 4);
        CHECK(Join(d, 0) == L"a||b|c");                   // 表头行插格
        CHECK(Join(d, 1) == L"1||2|3");
        CHECK(Join(d, 2) == L"x||y|");                    // 该行有 col1 格 → y 右移
        d.SetCell(1, 1, L"N");                            // 新列可写
        CHECK(Join(d, 1) == L"1|N|2|3");
        d.SetCell(2, 1, L"S");
        CHECK(Join(d, 2) == L"x|S|y|");
        // 序列化回环
        std::string out = SerializeCsv(d, L',', "\n", false);
        CsvData back = Parse(Utf8ToWide(out), -1);
        CHECK(back.cols == 4);
        CHECK(Join(back, 0) == Join(d, 0));
        CHECK(Join(back, 1) == Join(d, 1));
        CHECK(Join(back, 2) == Join(d, 2));
        // 删列
        CsvData e = Parse(L"a,b,c\n1,2,3\nx,y\n", -1);
        e.DeleteCol(1);                                   // 删掉 b 列
        CHECK(e.cols == 2);
        CHECK(Join(e, 0) == L"a|c");
        CHECK(Join(e, 1) == L"1|3");
        CHECK(Join(e, 2) == L"x|");                       // 该行的 y 在 col1 被删
        // 删首列（边界值平移路径）
        CsvData f = Parse(L"a,b\n1,2\n3,4\n", -1);
        f.DeleteCol(0);
        CHECK(f.cols == 1);
        CHECK(Join(f, 0) == L"b");
        CHECK(Join(f, 1) == L"2");
        CHECK(Join(f, 2) == L"4");
        // 越界静默 + 末尾追列
        CsvData g = Parse(L"a,b\n1,2\n", -1);
        g.InsertCol(99);                                  // col > cols → no-op
        CHECK(g.cols == 2);
        g.InsertCol(g.cols);                              // 末尾追列
        CHECK(g.cols == 3);
        CHECK(Join(g, 1) == L"1|2|");
        g.DeleteCol(9);                                   // 越界 no-op
        CHECK(g.cols == 3);
    }
    {   // 批次 43：SerializeRows（导出选中行复用同一引号规则）
        std::vector<std::vector<std::wstring>> rows = {
            { L"b", L"\"x,y\"" }, { L"2", L"tab\tsep" }, { L"", L"q\"uote" },
        };
        std::string out = SerializeRows(rows, L',', "\n");
        CHECK(out == "b,\"\"\"x,y\"\"\"\n2,tab\tsep\n,\"q\"\"uote\"");
        CsvData back = Parse(Utf8ToWide(out), -1);        // 回环：解析回来等价
        CHECK(back.cols == 2);
        CHECK(back.Cell(0, 1) == L"\"x,y\"");
        CHECK(back.Cell(1, 1) == L"tab\tsep");
        CHECK(back.Cell(2, 1) == L"q\"uote");
        std::string lf = SerializeRows(rows, L';', "\n");  // 活动分隔符换分号
        CHECK(lf.find("x,y") != std::string::npos);          // 逗号仍加引号
        CHECK(lf.find("tab\tsep") != std::string::npos);     // tab 非活动 → 裸出
        CHECK(SerializeRows({}, L',', "\r\n").empty());
    }
    {   // 批次 44：BuildPrintPages（列组装填 + 行带分页 + 页序）
        // 单列不超宽、行数正好一屏：一页
        auto p1 = BuildPrintPages({ 100, 100 }, 3, 250, 60, 20);
        CHECK(p1.size() == 1);
        CHECK(p1[0].rowStart == 0 && p1[0].rowEnd == 3);
        CHECK(p1[0].colStart == 0 && p1[0].colEnd == 2);

        // 宽表：内容宽 250，三列各 100 → 列组成 [0,2) [2,3)
        auto p2 = BuildPrintPages({ 100, 100, 100 }, 2, 250, 100, 20);
        CHECK(p2.size() == 2);
        CHECK(p2[0].colEnd == 2 && p2[0].firstColGroup);
        CHECK(p2[1].colStart == 2 && !p2[1].firstColGroup);
        CHECK(p2[0].rowEnd == 2 && p2[1].rowEnd == 2);  // 同一行带内列组相邻

        // 长表行带分页：content 60 / rowH 20 = 每页 3 行，10 行 → 4 带
        auto p3 = BuildPrintPages({ 80 }, 10, 200, 60, 20);
        CHECK(p3.size() == 4);
        CHECK(p3[0].rowStart == 0 && p3[0].rowEnd == 3);
        CHECK(p3[3].rowStart == 9 && p3[3].rowEnd == 10);

        // 二维：3 行带(每带 40/20=2 行) × 2 列组 = 6 页，行带优先顺序
        auto p4 = BuildPrintPages({ 100, 100, 100 }, 6, 250, 40, 20);
        CHECK(p4.size() == 6);
        CHECK(p4[0].rowEnd == 2 && p4[0].colEnd == 2);   // band0 group0
        CHECK(p4[1].rowEnd == 2 && p4[1].colStart == 2); // band0 group1
        CHECK(p4[2].rowStart == 2 && p4[2].firstColGroup);
        CHECK(p4[5].rowStart == 4 && p4[5].colStart == 2);

        // 空列宽表 → 无页；0 行 → 一个表头页
        CHECK(BuildPrintPages({}, 5, 200, 100, 20).empty());
        auto p5 = BuildPrintPages({ 50 }, 0, 200, 100, 20);
        CHECK(p5.size() == 1 && p5[0].rowStart == 0 && p5[0].rowEnd == 0);

        // 超宽单列（>contentW）独占一组不丢弃
        auto p6 = BuildPrintPages({ 500, 40 }, 1, 100, 100, 20);
        CHECK(p6.size() == 2);
        CHECK(p6[0].colStart == 0 && p6[0].colEnd == 1);
        CHECK(p6[1].colStart == 1 && p6[1].colEnd == 2);
    }

    if (g_failed == 0) std::printf("test_csv: all passed\n");
    else std::printf("test_csv: %d FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
