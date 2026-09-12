// xfsWinPad - CSV 解析与表格运算实现（批次 32）
#include "CsvParser.h"

#include "../core/Util.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace xfs {
namespace csv {

std::wstring_view CsvData::Cell(size_t row, size_t col) const {
    if (row >= RowCount()) return {};
    const uint32_t begin = rowStart[row];
    const uint32_t end = rowStart[row + 1];
    if (col >= (size_t)(end - begin)) return {};   // 短行越界列 = 空
    const auto& c = cells[begin + col];
    return std::wstring_view(arena.data() + c.first, c.second);
}

void CsvData::SetCell(size_t row, size_t col, std::wstring_view v) {
    if (row >= RowCount()) return;
    const uint32_t begin = rowStart[row];
    const uint32_t end = rowStart[row + 1];
    // 目标列超出该行格数（短行）→ 行尾补空格（零长度，arena 不动），
    // 后续所有行的 rowStart 边界右移 k。批次 34：短行补列。
    if (col >= (size_t)(end - begin)) {
        const size_t k = (size_t)col + 1 - (end - begin);
        cells.insert(cells.begin() + end, k, { (uint32_t)arena.size(), 0 });
        for (size_t i = row + 1; i < rowStart.size(); ++i)
            rowStart[i] += (uint32_t)k;
    }
    auto& c = cells[rowStart[row] + col];
    const size_t oldOff = c.first;
    const size_t oldLen = c.second;
    const long long delta = (long long)v.size() - (long long)oldLen;
    if (delta == 0) {
        if (!v.empty())
            memcpy(arena.data() + oldOff, v.data(), v.size() * sizeof(wchar_t));
        return;
    }
    // 拼接 arena 并把此后所有单元格偏移平移 delta。
    // 注意：oldLen==0（空格）时 oldOff == shiftEnd，编辑格自身会被扫到——
    // 必须跳过自身（其余同偏移的零格仍需右移，它们排在被编辑格之后）。
    arena.replace(oldOff, oldLen, std::wstring(v));
    c.second = (uint32_t)v.size();
    const size_t shiftEnd = oldOff + oldLen;
    const size_t selfIdx = (size_t)(&c - cells.data());
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i == selfIdx) continue;
        auto& cc = cells[i];
        if (cc.first >= shiftEnd) cc.first = (uint32_t)(cc.first + delta);
    }
    // rowStart 存的是单元格序号区间（非字节偏移），无需调整
}

void CsvData::InsertRow(size_t row) {
    if (row > RowCount()) return;
    // 新行 = cols 个零长度格（arena 不动，偏移指向末尾）；
    // rowStart 在边界处插入同值并在其后整体 +cols
    const size_t at = rowStart[row];   // 插入点的单元格序号
    cells.insert(cells.begin() + at, cols, { (uint32_t)arena.size(), 0 });
    rowStart.insert(rowStart.begin() + (row + 1), rowStart[row]);
    for (size_t i = row + 1; i < rowStart.size(); ++i)
        rowStart[i] += (uint32_t)cols;
}

void CsvData::DeleteRow(size_t row) {
    if (row >= RowCount()) return;
    const uint32_t begin = rowStart[row];
    const uint32_t end = rowStart[row + 1];
    // arena 拼接：该行的字节跨度 = 首格偏移 .. 尾格末端（全零格行跨度为 0）
    size_t firstOff = SIZE_MAX, lastEnd = 0;
    for (uint32_t i = begin; i < end; ++i) {
        firstOff = std::min(firstOff, (size_t)cells[i].first);
        lastEnd = std::max(lastEnd, (size_t)cells[i].first + cells[i].second);
    }
    if (lastEnd > firstOff && firstOff != SIZE_MAX) {
        arena.erase(firstOff, lastEnd - firstOff);
        const size_t shiftEnd = lastEnd;
        const long long delta = -(long long)(lastEnd - firstOff);
        for (auto& cc : cells)
            if (cc.first >= shiftEnd) cc.first = (uint32_t)(cc.first + delta);
    }
    cells.erase(cells.begin() + begin, cells.begin() + end);
    rowStart.erase(rowStart.begin() + (row + 1));
    const long long k = (long long)(end - begin);
    for (size_t i = row + 1; i < rowStart.size(); ++i)
        rowStart[i] = (uint32_t)(rowStart[i] - k);
}

void CsvData::InsertCol(size_t col) {
    if (col > cols) return;
    // 末行 → 首行：后行插格只会平移更大的边界值，前行边界保持有效
    for (size_t r = RowCount(); r-- > 0;) {
        const uint32_t begin = rowStart[r];
        const uint32_t end = rowStart[r + 1];
        if ((size_t)(end - begin) <= col) continue;   // 短行：新列在越界空区
        cells.insert(cells.begin() + begin + col,
                     { (uint32_t)arena.size(), 0 });
        for (size_t i = r + 1; i < rowStart.size(); ++i)
            rowStart[i] += 1;
    }
    cols += 1;
}

void CsvData::DeleteCol(size_t col) {
    if (cols == 0 || col >= cols) return;
    for (size_t r = RowCount(); r-- > 0;) {
        const uint32_t begin = rowStart[r];
        const uint32_t end = rowStart[r + 1];
        if ((size_t)(end - begin) <= col) continue;   // 短行无此列
        const size_t idx = begin + col;
        const size_t off = cells[idx].first;
        const size_t len = cells[idx].second;
        cells.erase(cells.begin() + idx);
        for (size_t i = r + 1; i < rowStart.size(); ++i)
            rowStart[i] -= 1;
        if (len > 0) {
            arena.erase(off, len);
            const size_t shiftEnd = off + len;
            for (auto& cc : cells)
                if (cc.first >= shiftEnd)
                    cc.first = (uint32_t)(cc.first - len);
        }
    }
    cols -= 1;
}

wchar_t ProbeDelim(std::wstring_view text) {
    bool inQ = false;
    long long cntComma = 0, cntSemi = 0, cntTab = 0;
    size_t i = (text.size() && text[0] == 0xFEFF) ? 1 : 0;
    for (; i < text.size() && text[i] != L'\n'; ++i) {
        wchar_t c = text[i];
        if (c == L'"') inQ = !inQ;
        else if (!inQ) {
            if (c == L',') ++cntComma;
            else if (c == L';') ++cntSemi;
            else if (c == L'\t') ++cntTab;
        }
    }
    wchar_t delim = L',';
    long long best = cntComma;
    if (cntSemi > best) { best = cntSemi; delim = L';'; }
    if (cntTab > best) { best = cntTab; delim = L'\t'; }
    // 没有任何分隔符 → 保持逗号（整行单列）
    return delim;
}

void SplitRow(std::wstring_view line, wchar_t delim,
              std::wstring& arena,
              std::vector<std::pair<uint32_t, uint32_t>>& cells) {
    std::wstring cur;
    bool inQ = false;
    bool cellOpenQ = false;
    bool pending = true;   // 行首/分隔符之后都挂着一格（可能是空格）
    auto endCell = [&]() {
        uint32_t off = (uint32_t)arena.size();
        arena += cur;
        cur.clear();
        cells.emplace_back(off, (uint32_t)(arena.size() - off));
        cellOpenQ = false;
        pending = false;
    };
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        wchar_t c = line[i];
        if (inQ) {
            if (c == L'"') {
                if (i + 1 < n && line[i + 1] == L'"') { cur += L'"'; i += 2; continue; }
                inQ = false; ++i; continue;
            }
            cur += c; ++i; continue;
        }
        if (c == L'"' && cur.empty() && !cellOpenQ) {
            inQ = true; cellOpenQ = true; ++i; continue;
        }
        if (c == delim) { endCell(); pending = true; ++i; continue; }
        cur += c; ++i;
    }
    if (!cur.empty() || cellOpenQ || pending) endCell();
}

CsvData Parse(std::wstring_view text, long long maxRows) {
    // ---- 分隔符：首个（非空）行内、引号外出现次数最多者 -------------------
    wchar_t delim = ProbeDelim(text);

    CsvData d;
    d.delim = delim;
    d.rowStart.push_back(0);
    std::wstring cur;
    bool inQ = false;        // 引号态
    bool cellOpenQ = false;  // 本格以引号开头（决定收尾是否保留字面引号）

    auto endCell = [&]() {
        uint32_t off = (uint32_t)d.arena.size();
        d.arena += cur;
        cur.clear();
        d.cells.emplace_back(off, (uint32_t)(d.arena.size() - off));
        cellOpenQ = false;
    };

    size_t i = (text.size() && text[0] == 0xFEFF) ? 1 : 0;
    const size_t n = text.size();
    long long rowsDone = 0;
    while (i < n) {
        wchar_t c = text[i];
        if (inQ) {
            if (c == L'"') {
                if (i + 1 < n && text[i + 1] == L'"') { cur += L'"'; i += 2; continue; }
                inQ = false; ++i; continue;   // 收引号（其后才是真正的分隔/行尾）
            }
            cur += c; ++i; continue;
        }
        if (c == L'"' && cur.empty() && !cellOpenQ) {
            inQ = true; cellOpenQ = true; ++i; continue;
        }
        if (c == delim) { endCell(); ++i; continue; }
        if (c == L'\r' || c == L'\n') {
            if (c == L'\r' && i + 1 < n && text[i + 1] == L'\n') ++i;
            endCell();
            d.rowStart.push_back((uint32_t)d.cells.size());
            ++rowsDone;
            if (maxRows > 0 && rowsDone >= maxRows) { i = n; break; }
            ++i; continue;
        }
        cur += c; ++i;
    }
    // 末行无换行符收尾：还有挂起内容才算一行（文件以换行结尾则不补空行）
    if (!cur.empty() || cellOpenQ ||
        d.cells.size() > (size_t)d.rowStart.back()) {
        endCell();
        d.rowStart.push_back((uint32_t)d.cells.size());
    }

    // 最大列数
    size_t mx = 0;
    for (size_t r = 0; r < d.RowCount(); ++r)
        mx = std::max(mx, (size_t)(d.rowStart[r + 1] - d.rowStart[r]));
    d.cols = mx;
    return d;
}

namespace {

// 宽松数值判定：可解析为 double 且整体消费（允许首尾空白；长度护栏 64）
bool TryNum(std::wstring_view v, double& out) {
    size_t b = 0, e = v.size();
    while (b < e && iswspace(v[b])) ++b;
    while (e > b && iswspace(v[e - 1])) --e;
    if (e - b == 0 || e - b > 64) return false;
    wchar_t buf[65];
    memcpy(buf, v.data() + b, (e - b) * sizeof(wchar_t));
    buf[e - b] = 0;
    wchar_t* end = nullptr;
    double val = wcstod(buf, &end);
    if (end == buf) return false;
    while (*end && iswspace(*end)) ++end;
    if (*end) return false;
    out = val;
    return true;
}

inline wchar_t LowerW(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
}

// 无分配的大小写不敏感子串匹配
bool ContainsCI(std::wstring_view hay, std::wstring_view needleLower) {
    if (needleLower.empty()) return true;
    if (hay.size() < needleLower.size()) return false;
    const size_t span = hay.size() - needleLower.size();
    for (size_t s = 0; s <= span; ++s) {
        size_t k = 0;
        while (k < needleLower.size() &&
               LowerW(hay[s + k]) == needleLower[k]) ++k;
        if (k == needleLower.size()) return true;
    }
    return false;
}

} // namespace

int CompareCells(std::wstring_view a, std::wstring_view b) {
    double da = 0, db = 0;
    bool na = TryNum(a, da), nb = TryNum(b, db);
    if (na && nb) {
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }
    // 数值与非数值混排：数值一律排在文本前（Excel 风格），文本间走字典序
    if (na != nb) return na ? -1 : 1;
    size_t mn = std::min(a.size(), b.size());
    for (size_t i = 0; i < mn; ++i) {
        wchar_t ca = LowerW(a[i]), cb = LowerW(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

void SortRows(const CsvData& d, int col, bool desc,
              std::vector<uint32_t>& rows) {
    if (col < 0 || (size_t)col >= d.cols) return;
    std::stable_sort(rows.begin(), rows.end(),
                     [&](uint32_t a, uint32_t b) {
                         int c = CompareCells(d.Cell(a, (size_t)col),
                                              d.Cell(b, (size_t)col));
                         return desc ? c > 0 : c < 0;
                     });
}

std::vector<uint32_t> FilterRows(const CsvData& d, std::wstring_view needle) {
    std::vector<uint32_t> out;
    const size_t rows = d.RowCount();
    out.reserve(rows);
    if (needle.empty()) {
        for (size_t r = 0; r < rows; ++r) out.push_back((uint32_t)r);
        return out;
    }
    std::wstring q(needle);
    for (wchar_t& c : q) c = LowerW(c);
    for (size_t r = 0; r < rows; ++r) {
        const uint32_t begin = d.rowStart[r];
        const uint32_t end = d.rowStart[r + 1];
        bool hit = false;
        for (uint32_t ci = begin; ci < end && !hit; ++ci) {
            const auto& c = d.cells[ci];
            if (ContainsCI(std::wstring_view(d.arena.data() + c.first, c.second), q))
                hit = true;
        }
        if (hit) out.push_back((uint32_t)r);
    }
    return out;
}

static void AppendCellUtf8(std::string& out, std::wstring_view v, wchar_t delim) {
    bool need = false;
    for (wchar_t ch : v) {
        if (ch == delim || ch == L'"' || ch == L'\r' || ch == L'\n') {
            need = true;
            break;
        }
    }
    std::string cell8 = WideToUtf8(std::wstring(v));
    if (!need) {
        out += cell8;
        return;
    }
    out.push_back('"');
    for (char ch : cell8) {
        if (ch == '"') out.push_back('"');
        out.push_back(ch);
    }
    out.push_back('"');
}

std::string SerializeCsv(const CsvData& d, wchar_t delim,
                         const char* newline, bool trailingNewline) {
    std::string out;
    const size_t rows = d.RowCount();
    for (size_t r = 0; r < rows; ++r) {
        if (r) out += newline;
        for (size_t c = 0; c < d.cols; ++c) {
            if (c) out.push_back((char)delim);   // 支持的分隔符均为 ASCII
            AppendCellUtf8(out, d.Cell(r, c), delim);
        }
    }
    if (trailingNewline && rows > 0) out += newline;
    return out;
}

std::string SerializeRows(const std::vector<std::vector<std::wstring>>& rows,
                          wchar_t delim, const char* newline) {
    std::string out;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r) out += newline;
        for (size_t c = 0; c < rows[r].size(); ++c) {
            if (c) out.push_back((char)delim);
            AppendCellUtf8(out, rows[r][c], delim);
        }
    }
    return out;
}

} // namespace csv
} // namespace xfs
