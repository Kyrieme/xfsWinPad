#include "CsvPrint.h"

#include <algorithm>

namespace xfs {
namespace csv {

std::vector<PrintPage> BuildPrintPages(const std::vector<int>& colWidthPx,
                                       int rowCount, int contentW,
                                       int contentH, int rowH) {
    std::vector<PrintPage> pages;
    if (colWidthPx.empty()) return pages;
    rowH = std::max(1, rowH);
    contentW = std::max(1, contentW);
    contentH = std::max(1, contentH);
    rowCount = std::max(0, rowCount);

    // 列组：贪心装填，单列超宽独占一组。
    struct ColGroup { int start, end; };
    std::vector<ColGroup> groups;
    int acc = 0;
    for (size_t c = 0; c < colWidthPx.size(); ++c) {
        int w = std::max(1, colWidthPx[c]);
        if (!groups.empty() && acc + w <= contentW) {
            groups.back().end = (int)c + 1;
            acc += w;
        } else {
            groups.push_back({(int)c, (int)c + 1});
            acc = w;
        }
    }

    // 行带：0 行也出一个头带（只印表头）。
    int rowsPerPage = std::max(1, contentH / rowH);
    int bandCount = std::max(1, (rowCount + rowsPerPage - 1) / rowsPerPage);
    for (int b = 0; b < bandCount; ++b) {
        int r0 = b * rowsPerPage;
        int r1 = std::min(rowCount, r0 + rowsPerPage);
        for (size_t g = 0; g < groups.size(); ++g) {
            PrintPage p;
            p.rowStart = r0;
            p.rowEnd = r1;
            p.colStart = groups[g].start;
            p.colEnd = groups[g].end;
            p.firstColGroup = (g == 0);
            pages.push_back(p);
        }
    }
    return pages;
}

} // namespace csv
} // namespace xfs
