#pragma once
// xfsWinPad - CSV 表格打印分页（批次 44）。纯逻辑无 GDI：列宽由调用方
// 传入（屏幕列宽已换算到打印 DC 像素），返回页序列（行带 × 列组）。
// 表头行由调用方每页叠加，故 contentH 需先扣除表头高度。

#include <vector>

namespace xfs {
namespace csv {

struct PrintPage {
    int rowStart = 0, rowEnd = 0;   // [rowStart,rowEnd) 数据行（不含表头）
    int colStart = 0, colEnd = 0;   // [colStart,colEnd) 列组
    bool firstColGroup = false;     // 本行带内的第一个列组（页序校验用）
};

// colWidthPx 每列显示宽；空列宽表 → 无页。rowH/contentW/contentH 至少为 1
// （内部夹取）。单列超宽独占一列组。页序：行带优先（同带内列组相邻）。
std::vector<PrintPage> BuildPrintPages(const std::vector<int>& colWidthPx,
                                       int rowCount, int contentW,
                                       int contentH, int rowH);

} // namespace csv
} // namespace xfs
