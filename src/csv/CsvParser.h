#pragma once
// xfsWinPad - CSV 解析与表格运算（批次 32，纯函数可单测，不依赖控件）。
//
// RFC 4180：引号字段（"" 转义、内嵌逗号/换行）、CRLF/LF/CR 行尾；
// 分隔符在 , ; \t 中按首行出现频次自动选择（Excel 语义）。
// 存储 = 连续 arena + (offset,len) 单元格索引，比逐格 std::wstring
// 轻一个量级（百万行 CSV 实测可承受）。

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xfs {
namespace csv {

struct CsvData {
    std::wstring arena;   // 全部单元格文本连续存放
    // 行 r 的单元格区间 = cells[rowStart[r], rowStart[r+1])
    std::vector<uint32_t> rowStart;
    // 行主序单元格 (arena 偏移, 长度)
    std::vector<std::pair<uint32_t, uint32_t>> cells;
    size_t cols = 0;      // 最大列数（解析时统计）
    wchar_t delim = L',';

    size_t RowCount() const { return rowStart.empty() ? 0 : rowStart.size() - 1; }
    std::wstring_view Cell(size_t row, size_t col) const;

    // 就地改写一个单元格（arena 拼接 + 后续单元格偏移整体平移，O(cells)）。
    // 行索引含表头（0 = 表头行）；目标列超出该行格数时自动补空格（短行补列）。
    void SetCell(size_t row, size_t col, std::wstring_view v);

    // 行操作（批次 34）。新行含 cols 个空格（保证 SetCell 可直接写入）；
    // InsertRow 在 row 之前插入（row == RowCount() 表示末尾追加）。
    // 删除行同步拼接 arena 并修正后续偏移；越界静默忽略。
    void InsertRow(size_t row);
    void DeleteRow(size_t row);

    // 列操作（批次 35）。InsertCol 在 col 之前插入空列（col == cols 为末尾
    // 追加）；短行（格数 ≤ col）不需要插格——新列对其就是"越界空格"。
    // DeleteCol 删除第 col 列：有该格的行删格并拼接 arena，短行跳过。
    // 两者都从末行向首行处理（后行插入/删除只平移更大的边界值）。
    void InsertCol(size_t col);
    void DeleteCol(size_t col);
};

// 解析。maxRows > 0 时只取前 maxRows 个完整行（余文丢弃，用于大文件护栏）。
CsvData Parse(std::wstring_view text, long long maxRows = -1);

// 分隔符探测：首行内（引号外）出现次数最多者（, ; \t，Excel 语义；
// 无任何分隔符 → 逗号）。批次 36 从 Parse 抽出，大文件模型复用。
wchar_t ProbeDelim(std::wstring_view text);

// 单行 RFC 4180 切分（批次 36，大文件模型复用；语义与 Parse 的逐格收集
// 完全一致：仅格首引号生效、"" 转义）。单元格内容追加进 arena（可能去引号
// 转义，无法原地引用），(offset,len) 追加进 cells。可重复调用累积。
void SplitRow(std::wstring_view line, wchar_t delim,
              std::wstring& arena,
              std::vector<std::pair<uint32_t, uint32_t>>& cells);

// 数值感知比较：两侧都能完整解析为 double（允许首尾空白）→ 数值比较；
// 否则不区分大小写的字典序（空串最小）。返回 <0/0/>0。
int CompareCells(std::wstring_view a, std::wstring_view b);

// 原位排序行号序列（可先经 FilterRows 过滤）。稳定排序。
void SortRows(const CsvData& d, int col, bool desc,
              std::vector<uint32_t>& rows);

// 大小写不敏感的「整格任意位置」子串过滤；needle 为空 = 原样返回全部。
std::vector<uint32_t> FilterRows(const CsvData& d, std::wstring_view needle);

// 序列化为 UTF-8 文本（批次 33 保存回写）。
// 引号规则：单元格含分隔符/双引号/CR/LF 时整体加引号，内部 " 翻倍（RFC 4180）；
// 其余原样。newline = "\r\n" 或 "\n"；trailingNewline 还原文件末尾的换行习惯。
std::string SerializeCsv(const CsvData& d, wchar_t delim,
                         const char* newline, bool trailingNewline);

} // namespace csv
} // namespace xfs
