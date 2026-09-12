// xfsWinPad - CsvPanel 实现（批次 32/33）
#include "CsvPanel.h"

#include <windowsx.h>

#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../csv/CsvBigModel.h"
#include "../csv/CsvPrint.h"
#include "../encoding/Encoding.h"
#include "../theme/Theme.h"
#include "InputBox.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <cstdlib>

namespace xfs {

namespace {

constexpr wchar_t kCsvPanelClass[] = L"xfsWinPadCsvPanel";
constexpr int ID_LABEL = 1380;
constexpr int ID_CLOSE = 1381;
constexpr int ID_FILTEREDIT = 1382;
constexpr int ID_COUNTLBL = 1383;
constexpr int ID_LIST = 1384;
constexpr int ID_SAVE = 1385;
constexpr int ID_CELLEDIT = 1386;
constexpr int ID_CTX_ADDROW = 1390;
constexpr int ID_CTX_DELROW = 1391;
constexpr int ID_CTX_ADDCOL = 1392;
constexpr int ID_CTX_DELCOL = 1393;
constexpr int ID_CTX_GOTO = 1394;
constexpr int ID_CTX_EXPORT = 1395;
constexpr int ID_CTX_PRINT = 1396;
constexpr UINT_PTR kFilterTimer = 5;
constexpr UINT_PTR kBigPollTimer = 6;   // 大文件过滤轮询（批次 37）
constexpr UINT_PTR kEditSubclassId = 20260933;
constexpr UINT_PTR kListSubclassId = 20260934;

// 文本护栏：>64MB 的表格体验失控（解析秒级 + 内存翻倍），首片直接拒绝
constexpr size_t kMaxCsvBytes = 64ull * 1024 * 1024;
// 列数护栏：ListView 列太多本身不可用
constexpr int kMaxCols = 128;

std::wstring GetWndText(HWND h) {
    int len = ::GetWindowTextLengthW(h);
    if (len <= 0) return L"";
    std::wstring s((size_t)len + 1, L'\0');
    int got = ::GetWindowTextW(h, s.data(), len + 1);
    s.resize((size_t)std::max(0, got));
    return s;
}

} // namespace

bool CsvPanel::Create(HWND parent, HINSTANCE hInst) {
    if (hwnd_) return true;
    inst_ = hInst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kCsvPanelClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    int dpi = ::GetDpiForWindow(parent);
    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    hwnd_ = ::CreateWindowExW(0, kCsvPanelClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 600, 240, parent,
                              (HMENU)(INT_PTR)1107, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", Tr(L"panel.csv"),
                               WS_CHILD | WS_VISIBLE | SS_NOTIFY, 6, 8, 300, 18,
                               hwnd_, (HMENU)(INT_PTR)ID_LABEL, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.csv.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 2, 76, 22,
                                  hwnd_, (HMENU)(INT_PTR)ID_CLOSE, hInst, nullptr);
    saveBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.csv.save"),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 2, 76, 22,
                                 hwnd_, (HMENU)(INT_PTR)ID_SAVE, hInst, nullptr);
    filterEdit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    ES_AUTOHSCROLL,
                                    0, 4, 240, 24,
                                    hwnd_, (HMENU)(INT_PTR)ID_FILTEREDIT,
                                    hInst, nullptr);
    countLabel_ = ::CreateWindowExW(0, L"STATIC", L"",
                                    WS_CHILD | WS_VISIBLE | SS_NOTIFY, 0, 8, 260, 18,
                                    hwnd_, (HMENU)(INT_PTR)ID_COUNTLBL, hInst, nullptr);

    list_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
                              0, 0, 100, 100,
                              hwnd_, (HMENU)(INT_PTR)ID_LIST, hInst, nullptr);
    if (!list_) return false;
    ::SendMessageW(list_, WM_SETFONT, (WPARAM)font_, TRUE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT |
                                             LVS_EX_DOUBLEBUFFER |
                                             LVS_EX_GRIDLINES);
    SetWindowSubclass(list_, ListProcThunk, kListSubclassId, (DWORD_PTR)this);

    for (HWND h : { label_, closeBtn_, saveBtn_, filterEdit_, countLabel_ })
        if (h) ::SendMessageW(h, WM_SETFONT, (WPARAM)font_, TRUE);
    UpdateSaveState();
    return true;
}

void CsvPanel::Destroy() {
    if (filterTimer_) { ::KillTimer(hwnd_, kFilterTimer); filterTimer_ = 0; }
    if (hwnd_) { ::KillTimer(hwnd_, kBigPollTimer); }
    bigFilter_.Stop();
    CommitCellEdit(false);
    if (list_) RemoveWindowSubclass(list_, ListProcThunk, kListSubclassId);
    for (HWND h : { filterEdit_, countLabel_, list_, label_, saveBtn_, closeBtn_ }) {
        if (h) { ::DestroyWindow(h); h = nullptr; }
    }
    filterEdit_ = countLabel_ = list_ = nullptr;
    label_ = closeBtn_ = saveBtn_ = nullptr;
    viewRows_.clear();
    data_.reset();
    big_.reset();
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
    if (bgBrush_) { ::DeleteObject(bgBrush_); bgBrush_ = nullptr; }
}

bool CsvPanel::HasFocus() const {
    HWND f = ::GetFocus();
    return hwnd_ && (f == hwnd_ || ::IsChild(hwnd_, f));
}

CsvPanel::CsvPanel() = default;

CsvPanel::~CsvPanel() { Destroy(); }

void CsvPanel::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
}

bool CsvPanel::Load(const std::wstring& filePath) {
    // 小文件路径：退出可能残留的大文件模式
    if (big_) {
        big_.reset();
        bigFilter_.Stop();
        if (filterEdit_) ::EnableWindow(filterEdit_, TRUE);
    }
    // ---- 读文件（cap 64MB）------------------------------------------------
    HANDLE h = ::CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ |
                             FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        ::CloseHandle(h);
        return false;
    }
    if ((unsigned long long)sz.QuadPart > kMaxCsvBytes) {
        ::CloseHandle(h);
        return LoadBig(filePath);   // 大文件 → 只读虚拟模式（批次 36）
    }
    std::string raw((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = ::ReadFile(h, raw.data(), (DWORD)raw.size(), &got, nullptr);
    // 冲突检测基线：同一句柄取真实写入时间（外部改动会在 DoSave 拦下）
    FILETIME writeTime{};
    (void)writeTime;
    loadSize_ = 0;
    loadTime_ = {};
    BY_HANDLE_FILE_INFORMATION fi{};
    if (::GetFileInformationByHandle(h, &fi)) {
        loadTime_ = fi.ftLastWriteTime;
        loadSize_ = ((long long)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    }
    ::CloseHandle(h);
    if (!ok || got != raw.size()) return false;

    // ---- 解码（复用编辑器同一套探测：BOM/UTF-8/UTF-16/ANSI）--------------
    DWORD t0 = ::GetTickCount();
    DecodedText dec = encoding::DecodeToUtf8(raw);
    std::wstring wide = Utf8ToWide(dec.utf8);

    // ---- 行尾/尾换行还原（保存回写保真）----------------------------------
    newline_ = "\r\n";
    for (wchar_t ch : wide) {
        if (ch == L'\r') { newline_ = "\r\n"; break; }
        if (ch == L'\n') { newline_ = "\n"; break; }
    }
    trailingNewline_ = !wide.empty() &&
                       (wide.back() == L'\n' || wide.back() == L'\r');

    // ---- 解析（护栏 200 万行）--------------------------------------------
    csv::CsvData d = csv::Parse(wide, 2000000);
    if (d.RowCount() == 0) return false;
    int truncCol = 0;
    if ((int)d.cols > kMaxCols) { truncCol = (int)d.cols; d.cols = kMaxCols; }

    data_ = std::make_unique<csv::CsvData>(std::move(d));
    path_ = filePath;
    cols_ = (int)data_->cols;
    sortCol_ = -1;
    sortDesc_ = false;
    enc_ = dec.encoding;
    dirty_ = false;

    // ---- 列与视图 ---------------------------------------------------------
    // 重建列（Load 可被不同文件重复调用）
    while (ListView_DeleteColumn(list_, 0)) {}
    BuildColumns();
    ::SetWindowTextW(filterEdit_, L"");
    RebuildView();
    UpdateSaveState();

    DWORD dt = ::GetTickCount() - t0;
    Logger::Info("CsvPanel: loaded " + WideToUtf8(filePath) + " " +
                 std::to_string(data_->RowCount()) + " rows x " +
                 std::to_string(cols_) + " cols, enc=" +
                 std::to_string((int)dec.encoding) +
                 (truncCol ? (", cols truncated from " + std::to_string(truncCol))
                           : std::string()) +
                 ", " + std::to_string(dt) + " ms");
    ::ShowWindow(hwnd_, SW_SHOW);
    return true;
}

// --- 大文件只读模式（批次 36）：>64MB 的 CSV，mmap 行索引 + 按需物化 -------

bool CsvPanel::LoadBig(const std::wstring& filePath) {
    auto m = std::make_unique<csv::CsvBigModel>();
    DWORD t0 = ::GetTickCount();
    if (!m->Open(filePath)) {
        Logger::Error("CsvPanel: big open failed " + WideToUtf8(filePath) +
                      " (" + WideToUtf8(m->Error()) + ")");
        return false;
    }
    big_ = std::move(m);
    data_.reset();
    path_ = filePath;
    cols_ = std::min<int>((int)big_->ColCount(), kMaxCols);
    sortCol_ = -1;
    sortDesc_ = false;
    lastSubItem_ = -1;
    enc_ = big_->Encoding() == encoding::EncodingType::ANSI
        ? encoding::EncodingType::ANSI : encoding::EncodingType::UTF8;
    dirty_ = false;
    if (filterEdit_) ::EnableWindow(filterEdit_, TRUE);   // 批次 37：大文件支持过滤

    while (ListView_DeleteColumn(list_, 0)) {}
    BuildColumns();
    ::SetWindowTextW(filterEdit_, L"");
    RebuildView();
    UpdateSaveState();

    Logger::Info("CsvPanel: big-loaded " + WideToUtf8(filePath) + " " +
                 std::to_string(big_->RowCount()) + " rows x " +
                 std::to_string(cols_) + " cols, size=" +
                 std::to_string(big_->FileSize()) + ", " +
                 std::to_string(::GetTickCount() - t0) + " ms");
    ::ShowWindow(hwnd_, SW_SHOW);
    return true;
}

void CsvPanel::BuildColumns() {
    if (big_) {
        if (!list_) return;
        int dpi = ::GetDpiForWindow(hwnd_);
        std::vector<std::wstring> row;
        big_->Row(0, row);   // 表头行
        size_t probe = std::min<size_t>(big_->RowCount(), 200);
        for (int c = 0; c < cols_; ++c) {
            size_t mx = c < row.size() ? row[c].size() : 0;
            for (size_t r = 1; r < probe; ++r) {
                big_->Row(r, row);
                if (c < (int)row.size())
                    mx = std::max(mx, row[(size_t)c].size());
            }
            int w = MulDiv((int)std::min<size_t>(mx, 60) * 7 + 30, dpi, 96);
            w = std::max(MulDiv(48, dpi, 96), std::min(w, MulDiv(320, dpi, 96)));
            std::wstring head = c < (int)row.size() ? row[(size_t)c] : L"";
            if (head.empty()) head = L"(col " + std::to_wstring(c + 1) + L")";
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = head.data();
            col.cx = w;
            ListView_InsertColumn(list_, c, &col);
        }
        return;
    }
    if (!data_ || !list_) return;
    int dpi = ::GetDpiForWindow(hwnd_);
    // 宽度启发：表头与首屏 200 行同列最大字符数 × 7 逻辑像素 + 边距
    size_t probe = std::min<size_t>(data_->RowCount(), 200);
    for (int c = 0; c < cols_; ++c) {
        size_t mx = data_->Cell(0, c).size();   // 表头行（row 0）
        for (size_t r = 1; r < probe; ++r)
            mx = std::max(mx, data_->Cell(r, c).size());
        int w = MulDiv((int)std::min<size_t>(mx, 60) * 7 + 30, dpi, 96);
        w = std::max(MulDiv(48, dpi, 96), std::min(w, MulDiv(320, dpi, 96)));
        std::wstring head(data_->Cell(0, c));
        if (head.empty()) head = L"(col " + std::to_wstring(c + 1) + L")";
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = head.data();
        col.cx = w;
        ListView_InsertColumn(list_, c, &col);
    }
}

std::wstring CsvPanel::CellText(int row, int col) const {
    if (row < 0 || (size_t)row >= viewRows_.size() && !big_) return L"";
    if (big_) {
        // 过滤态走 bigFilter_ 结果表路由；非过滤态走恒等映射 viewRows_
        size_t orig;
        if (!bigFilter_.Needle().empty()) {
            if (row < 0 || (size_t)row >= bigFilter_.MatchCount()) return L"";
            orig = bigFilter_.RowAt((size_t)row);
        } else {
            if (row < 0 || (size_t)row >= viewRows_.size()) return L"";
            orig = viewRows_[(size_t)row];
        }
        std::vector<std::wstring> cells;
        if (!big_->Row(orig, cells) || col >= (int)cells.size()) return L"";
        return cells[(size_t)col];
    }
    // viewRows_[row] = 原始行号（表头行不进显示序列）
    size_t orig = viewRows_[(size_t)row];
    if (!data_) return L"";
    std::wstring_view v = data_->Cell(orig, (size_t)col);
    return std::wstring(v);
}

void CsvPanel::RebuildView() {
    if (big_) {
        std::wstring q = filterEdit_ ? GetWndText(filterEdit_) : L"";
        if (q.empty()) {
            // 无过滤：全部数据行恒等映射
            bigFilter_.Stop();
            ::KillTimer(hwnd_, kBigPollTimer);
            viewRows_.clear();
            viewRows_.reserve(big_->RowCount() ? big_->RowCount() - 1 : 0);
            for (size_t r = 1; r < big_->RowCount(); ++r)
                viewRows_.push_back((uint32_t)r);
            ListView_SetItemCountEx(list_, (int)viewRows_.size(), LVSICF_NOINVALIDATEALL);
        }
        // 有过滤词：由 StartBigFilter/轮询驱动（RebuildView 不重启扫描）
        UpdateCountLabel();
        UpdateSortArrows();
        return;
    }
    if (!data_ || !list_) return;
    std::wstring q = filterEdit_ ? GetWndText(filterEdit_) : L"";
    viewRows_ = csv::FilterRows(*data_, q);   // 原始行号（0 = 表头行）
    // 表头行不算数据行：过滤词命中表头时也不显示它（列标题已有）
    if (!viewRows_.empty() && viewRows_.front() == 0)
        viewRows_.erase(viewRows_.begin());
    if (sortCol_ >= 0 && sortCol_ < cols_)
        csv::SortRows(*data_, sortCol_, sortDesc_, viewRows_);
    ListView_SetItemCountEx(list_, (int)viewRows_.size(), LVSICF_NOINVALIDATEALL);
    UpdateCountLabel();
    UpdateSortArrows();
}

// --- 大文件后台过滤（批次 37）----------------------------------------------

void CsvPanel::StartBigFilter() {
    if (!big_ || !list_) return;
    std::wstring q = filterEdit_ ? GetWndText(filterEdit_) : L"";
    if (q.empty()) { RebuildView(); return; }   // 清空 → 回恒等映射
    viewRows_.clear();
    bigFilter_.Start(big_.get(), q);
    ListView_SetItemCountEx(list_, (int)bigFilter_.MatchCount(), LVSICF_NOINVALIDATEALL);
    UpdateCountLabel();
    ::SetTimer(hwnd_, kBigPollTimer, 200, nullptr);
}

void CsvPanel::PollBigFilter() {
    if (!big_ || !list_) return;
    if (bigFilter_.Needle().empty()) {
        ::KillTimer(hwnd_, kBigPollTimer);
        return;
    }
    ListView_SetItemCountEx(list_, (int)bigFilter_.MatchCount(), LVSICF_NOINVALIDATEALL);
    UpdateCountLabel();
    if (!bigFilter_.Running()) {
        ::KillTimer(hwnd_, kBigPollTimer);
        ::InvalidateRect(list_, nullptr, FALSE);
        Logger::Info("CsvPanel: big filter [" + WideToUtf8(bigFilter_.Needle()) +
                     "] done, " + std::to_string(bigFilter_.MatchCount()) +
                     " matches");
    }
}

void CsvPanel::UpdateCountLabel() {
    if (!countLabel_) return;
    if (big_) {
        std::wstring s;
        if (!bigFilter_.Needle().empty()) {
            unsigned long long m = bigFilter_.MatchCount();
            if (bigFilter_.Running()) {
                s = I18n::Instance().Fmt(L"csv.scanning",
                      { std::to_wstring((int)(bigFilter_.Progress() * 100.0)),
                        std::to_wstring(m) });
            } else {
                s = I18n::Instance().Fmt(L"csv.count",
                      { std::to_wstring(m), std::to_wstring(cols_),
                        std::to_wstring(m) });
            }
        } else {
            unsigned long long rows = big_->RowCount();
            unsigned long long dataRows = rows ? rows - 1 : 0;
            s = rows == 0
                ? Tr(L"csv.nodata")
                : I18n::Instance().Fmt(L"csv.count",
                      { std::to_wstring(dataRows), std::to_wstring(cols_),
                        std::to_wstring(viewRows_.size()) });
        }
        s += std::wstring(L"  (") + Tr(L"csv.readonly") + L")";
        ::SetWindowTextW(countLabel_, s.c_str());
        return;
    }
    if (!data_) return;
    unsigned long long rows = data_ ? data_->RowCount() : 0;
    unsigned long long shown = viewRows_.size();
    // rows 含表头行：数据行数 = rows - 1
    unsigned long long dataRows = rows ? rows - 1 : 0;
    std::wstring s = rows == 0
        ? Tr(L"csv.nodata")
        : I18n::Instance().Fmt(L"csv.count",
              { std::to_wstring(dataRows), std::to_wstring(cols_),
                std::to_wstring(shown) });
    if (dirty_) s += L"  *";
    ::SetWindowTextW(countLabel_, s.c_str());
}

void CsvPanel::UpdateSaveState() {
    if (saveBtn_) ::EnableWindow(saveBtn_, dirty_ ? TRUE : FALSE);
    UpdateCountLabel();
}

void CsvPanel::UpdateSortArrows() {
    if (!list_) return;
    HWND hdr = ListView_GetHeader(list_);
    if (!hdr) return;
    for (int i = 0; i < cols_; ++i) {
        wchar_t b[256];
        HDITEMW it{};
        it.mask = HDI_TEXT;
        it.pszText = b;
        it.cchTextMax = 256;
        if (!Header_GetItem(hdr, i, &it)) continue;
        size_t len = wcslen(b);
        if (len >= 2 && (b[len - 1] == 0x25B2 || b[len - 1] == 0x25BC))
            b[len - 2] = 0;
        if (i == sortCol_) wcscat_s(b, sortDesc_ ? L" ▼" : L" ▲");
        it.pszText = b;
        it.cchTextMax = (int)wcslen(b);
        Header_SetItem(hdr, i, &it);
    }
}

void CsvPanel::Layout(int w, int h) {
    if (!hwnd_) return;
    int dpi = ::GetDpiForWindow(hwnd_);
    int m = MulDiv(6, dpi, 96);
    int closeW = MulDiv(76, dpi, 96);
    int saveW = MulDiv(64, dpi, 96);
    int filterW = MulDiv(240, dpi, 96);
    int cntW = MulDiv(300, dpi, 96);
    int lblW = std::max(m, w - closeW - saveW - filterW - cntW - m * 6);
    // 头部单行：标签/计数 (y=6,h=18)、过滤框 (y=3,h=24)、保存/关闭钮 (y=3,h=22)
    // ——96dpi 下垂直中心全部落在 y≈15，与 StdfPanel 头部几何一致
    if (label_) ::MoveWindow(label_, m, MulDiv(6, dpi, 96),
                             lblW, MulDiv(18, dpi, 96), TRUE);
    int fx = m + lblW + m;
    if (filterEdit_) ::MoveWindow(filterEdit_, fx, MulDiv(3, dpi, 96),
                                  filterW, MulDiv(24, dpi, 96), TRUE);
    if (countLabel_) ::MoveWindow(countLabel_, fx + filterW + m,
                                  MulDiv(6, dpi, 96), cntW,
                                  MulDiv(18, dpi, 96), TRUE);
    if (closeBtn_) ::MoveWindow(closeBtn_, w - closeW - m, MulDiv(3, dpi, 96),
                                closeW, MulDiv(22, dpi, 96), TRUE);
    if (saveBtn_) ::MoveWindow(saveBtn_, w - closeW - saveW - m * 2,
                               MulDiv(3, dpi, 96), saveW, MulDiv(22, dpi, 96), TRUE);
    int listTop = MulDiv(32, dpi, 96);
    if (list_) ::MoveWindow(list_, m, listTop, w - m * 2,
                            std::max(10, h - listTop - m), TRUE);
}

void CsvPanel::ApplyTheme(const ThemeDef& t) {
    colors_.bg = t.editorBg;
    colors_.fg = t.editorFg;
    if (bgBrush_) { ::DeleteObject(bgBrush_); bgBrush_ = nullptr; }
    if (list_) {
        ListView_SetBkColor(list_, colors_.bg);
        ListView_SetTextBkColor(list_, colors_.bg);
        ListView_SetTextColor(list_, colors_.fg);
        ::InvalidateRect(list_, nullptr, TRUE);
    }
    if (hwnd_) ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void CsvPanel::Retranslate() {
    if (label_) ::SetWindowTextW(label_, Tr(L"panel.csv"));
    if (closeBtn_) ::SetWindowTextW(closeBtn_, Tr(L"panel.csv.close"));
    if (saveBtn_) ::SetWindowTextW(saveBtn_, Tr(L"panel.csv.save"));
    ::SendMessageW(filterEdit_, EM_SETCUEBANNER, TRUE, (LPARAM)Tr(L"csv.filterhint"));
    UpdateCountLabel();
}

LRESULT CsvPanel::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kFilterTimer) {
                ::KillTimer(hwnd_, kFilterTimer);
                filterTimer_ = 0;
                if (big_) StartBigFilter();
                else RebuildView();
                return 0;
            }
            if (wp == kBigPollTimer) {
                PollBigFilter();
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ::ScreenToClient(h, &pt);
            if (pt.y < 6) return HTCLIENT;
            break;
        }
        case WM_SETCURSOR: {
            POINT pt; ::GetCursorPos(&pt);
            ::ScreenToClient(h, &pt);
            if (pt.y < 6) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.y < 6) {
                resizing_ = true;
                POINT sp; ::GetCursorPos(&sp);
                dragStartScreenY_ = sp.y;
                RECT rc; ::GetWindowRect(h, &rc);
                dragStartH_ = rc.bottom - rc.top;
                ::SetCapture(h);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (resizing_) {
                POINT sp; ::GetCursorPos(&sp);
                int newH = dragStartH_ + (dragStartScreenY_ - sp.y);
                newH = std::max(90, std::min(1400, newH));
                if (onHeightChange) onHeightChange(newH);
                return 0;
            }
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool hot = pt.y < 6;
            if (hot) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
            }
            if (hot != splitHot_) {
                splitHot_ = hot;
                RECT rc; ::GetClientRect(h, &rc);
                rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (splitHot_) {
                splitHot_ = false;
                RECT rc; ::GetClientRect(h, &rc);
                rc.bottom = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        case WM_LBUTTONUP:
            if (resizing_) { resizing_ = false; ::ReleaseCapture(); return 0; }
            break;
        case WM_CAPTURECHANGED:
            resizing_ = false;
            break;
        case WM_PAINT: {
            DefWindowProcW(h, msg, wp, lp);
            HDC dc = ::GetDC(h);
            RECT rc; ::GetClientRect(h, &rc);
            RECT band{ 0, 0, rc.right, 5 };
            HBRUSH b = ::CreateSolidBrush(splitHot_ || resizing_
                ? RGB(0xCC, 0xE3, 0xFF) : RGB(0xE9, 0xE9, 0xE9));
            ::FillRect(dc, &band, b);
            ::DeleteObject(b);
            HPEN pen = ::CreatePen(PS_SOLID, 1, splitHot_ || resizing_
                ? RGB(0x00, 0x78, 0xD4) : RGB(0xAC, 0xAC, 0xAC));
            HPEN old = (HPEN)::SelectObject(dc, pen);
            ::MoveToEx(dc, 0, 5, nullptr);
            ::LineTo(dc, rc.right, 5);
            ::SelectObject(dc, old);
            ::DeleteObject(pen);
            ::ReleaseDC(h, dc);
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (nm->code == LVN_GETDISPINFO && nm->idFrom == ID_LIST) {
                NMLVDISPINFOW* di = (NMLVDISPINFOW*)lp;
                if (di->item.mask & LVIF_TEXT) {
                    std::wstring s = CellText(di->item.iItem, di->item.iSubItem);
                    int n = (int)s.size();
                    int cap = (int)di->item.cchTextMax - 1;
                    if (cap > 0) {
                        if (n > cap) n = cap;
                        memcpy(di->item.pszText, s.data(), n * sizeof(wchar_t));
                        di->item.pszText[n] = 0;
                    }
                }
                return 0;
            }
            if (nm->code == LVN_COLUMNCLICK && nm->idFrom == ID_LIST) {
                if (big_) return 0;   // 大文件只读：不排序（物化全表代价失控）
                NMLISTVIEW* cl = (NMLISTVIEW*)lp;
                int c = cl->iSubItem;
                if (sortCol_ == c) sortDesc_ = !sortDesc_;
                else { sortCol_ = c; sortDesc_ = false; }
                RebuildView();
                Logger::Debug("CsvPanel: sort col=" + std::to_string(sortCol_) +
                              (sortDesc_ ? " desc" : " asc"));
                return 0;
            }
            if (nm->code == NM_DBLCLK && nm->idFrom == ID_LIST) {
                NMLISTVIEW* cl = (NMLISTVIEW*)lp;
                if (cl->iItem >= 0 && cl->iSubItem >= 0)
                    BeginCellEdit(cl->iItem, cl->iSubItem);
                return 0;
            }
            if (nm->code == NM_CLICK && nm->idFrom == ID_LIST) {
                NMLISTVIEW* cl = (NMLISTVIEW*)lp;
                if (cl->iSubItem >= 0) lastSubItem_ = cl->iSubItem;
                break;   // 交给 ListView 默认处理（选择/焦点）
            }
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wp) == ID_CLOSE && onClose) { onClose(); return 0; }
            if (LOWORD(wp) == ID_SAVE) { DoSave(); return 0; }
            if (LOWORD(wp) == ID_CTX_ADDROW) { AddRowAt(); return 0; }
            if (LOWORD(wp) == ID_CTX_DELROW) { DeleteSelectedRow(); return 0; }
            if (LOWORD(wp) == ID_CTX_ADDCOL) { AddColAt(); return 0; }
            if (LOWORD(wp) == ID_CTX_DELCOL) { DeleteColAt(); return 0; }
            if (LOWORD(wp) == ID_CTX_GOTO) { GoToRow(); return 0; }
            if (LOWORD(wp) == ID_CTX_EXPORT) { ExportSelected(); return 0; }
            if (LOWORD(wp) == ID_CTX_PRINT) { PrintTable(); return 0; }
            if (LOWORD(wp) == ID_FILTEREDIT && HIWORD(wp) == EN_CHANGE) {
                if (filterTimer_) ::KillTimer(hwnd_, kFilterTimer);
                ::SetTimer(hwnd_, filterTimer_ = kFilterTimer, 250, nullptr);
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            ::SetTextColor(dc, colors_.fg);
            ::SetBkColor(dc, colors_.bg);
            if (!bgBrush_) bgBrush_ = ::CreateSolidBrush(colors_.bg);
            return (LRESULT)bgBrush_;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            ::SetTextColor(dc, colors_.fg);
            ::SetBkColor(dc, colors_.bg);
            if (!bgBrush_) bgBrush_ = ::CreateSolidBrush(colors_.bg);
            return (LRESULT)bgBrush_;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// --- 单元格就地编辑（批次 33）----------------------------------------------

void CsvPanel::BeginCellEdit(int viewRow, int col) {
    if (!data_ || !list_ || col < 0 || col >= cols_) return;
    if (viewRow < 0 || (size_t)viewRow >= viewRows_.size()) return;
    CommitCellEdit(true);   // 若已有编辑框先提交

    // 取该格显示矩形（list 客户区坐标 → 面板客户区坐标）。
    // LVM_GETSUBITEMRECT 约定：发送前 rc.left 携带 LVIR_* 代码。
    RECT rc{};
    rc.left = LVIR_BOUNDS;
    if (!::SendMessageW(list_, LVM_GETSUBITEMRECT,
                        (WPARAM)viewRow, (LPARAM)&rc)) return;
    POINT ptTL{ rc.left, rc.top };
    POINT ptBR{ rc.right, rc.bottom };
    ::MapWindowPoints(list_, hwnd_, &ptTL, 1);
    ::MapWindowPoints(list_, hwnd_, &ptBR, 1);

    editViewRow_ = viewRow;
    editCol_ = col;
    std::wstring text = CellText(viewRow, col);

    int edH = std::max(20, (int)(ptBR.y - ptTL.y));
    cellEdit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  ES_AUTOHSCROLL,
                                  ptTL.x, ptTL.y,
                                  std::max(40, (int)(ptBR.x - ptTL.x)), edH,
                                  hwnd_, (HMENU)(INT_PTR)ID_CELLEDIT,
                                  inst_, nullptr);
    if (!cellEdit_) { editViewRow_ = editCol_ = -1; return; }
    ::SendMessageW(cellEdit_, WM_SETFONT, (WPARAM)font_, TRUE);
    ::SetFocus(cellEdit_);
    ::SendMessageW(cellEdit_, EM_SETSEL, 0, -1);   // 全选（与标签编辑一致）
    SetWindowSubclass(cellEdit_, EditProcThunk, kEditSubclassId,
                      (DWORD_PTR)this);
}

void CsvPanel::CommitCellEdit(bool commit) {
    if (!cellEdit_) return;
    HWND ed = cellEdit_;
    cellEdit_ = nullptr;   // 先清防重入（WM_KILLFOCUS 会再触发）
    RemoveWindowSubclass(ed, EditProcThunk, kEditSubclassId);
    std::wstring text;
    if (commit) text = GetWndText(ed);
    int vr = editViewRow_, col = editCol_;
    editViewRow_ = editCol_ = -1;
    ::DestroyWindow(ed);
    if (!commit || vr < 0 || (size_t)vr >= viewRows_.size()) return;
    int origRow = viewRows_[vr];
    std::wstring old(data_->Cell(origRow, (size_t)col));
    if (text == old) return;   // 无变化不算脏
    data_->SetCell((size_t)origRow, (size_t)col, text);
    dirty_ = true;
    UpdateSaveState();
    ::InvalidateRect(list_, nullptr, FALSE);
    Logger::Debug("CsvPanel: cell edit r=" + std::to_string(origRow) +
                  " c=" + std::to_string(col));
}

LRESULT CALLBACK CsvPanel::EditProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref) {
    auto* self = (CsvPanel*)ref;
    if (!self) return ::DefSubclassProc(h, m, wp, lp);
    return self->EditProc(h, m, wp, lp, id, ref);
}

LRESULT CsvPanel::EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp,
                           UINT_PTR id, DWORD_PTR ref) {
    switch (m) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN) { CommitCellEdit(true); return 0; }
            if (wp == VK_ESCAPE) { CommitCellEdit(false); return 0; }
            break;
        case WM_KILLFOCUS:
            CommitCellEdit(true);
            return 0;
    }
    return DefSubclassProc(h, m, wp, lp);
}

// --- 保存回写（批次 33）-----------------------------------------------------

bool CsvPanel::DoSave() {
    if (!data_ || path_.empty()) return false;
    CommitCellEdit(true);

    // 外部修改冲突检测：基线时间/大小与当前盘上状态比对
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (::GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fa)) {
        ULARGE_INTEGER sz{ fa.nFileSizeLow, fa.nFileSizeHigh };
        if (::CompareFileTime(&fa.ftLastWriteTime, &loadTime_) != 0 ||
            (long long)sz.QuadPart != loadSize_) {
            std::wstring msg = Tr(L"msg.csvextern");
            msg += L"\n\n";
            msg += path_;
            if (::MessageBoxW(hwnd_, msg.c_str(), L"xfsWinPad CSV",
                              MB_YESNO | MB_ICONWARNING) != IDYES)
                return false;
        }
    }

    std::string utf8 = csv::SerializeCsv(*data_, data_->delim,
                                         newline_.c_str(), trailingNewline_);
    std::string bytes = encoding::EncodeFromUtf8(utf8, enc_);
    HANDLE h = ::CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::wstring msg = Tr(L"msg.csvsavefail");
        msg += L"\n\n";
        msg += path_;
        ::MessageBoxW(hwnd_, msg.c_str(), L"xfsWinPad CSV",
                      MB_OK | MB_ICONERROR);
        Logger::Error("CsvPanel: save failed gle=" +
                      std::to_string(::GetLastError()));
        return false;
    }
    DWORD written = 0;
    BOOL wok = ::WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    ::CloseHandle(h);
    if (!wok || written != bytes.size()) {
        ::MessageBoxW(hwnd_, Tr(L"msg.csvsavefail"), L"xfsWinPad CSV",
                      MB_OK | MB_ICONERROR);
        Logger::Error("CsvPanel: save short write " +
                      std::to_string(written) + "/" +
                      std::to_string(bytes.size()));
        return false;
    }

    // 刷新基线（避免下次保存把自己的写入当外部修改）
    if (::GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fa)) {
        loadTime_ = fa.ftLastWriteTime;
        ULARGE_INTEGER sz{ fa.nFileSizeLow, fa.nFileSizeHigh };
        loadSize_ = (long long)sz.QuadPart;
    }
    dirty_ = false;
    UpdateSaveState();
    Logger::Info("CsvPanel: saved " + WideToUtf8(path_) + " bytes=" +
                 std::to_string(bytes.size()));
    if (onSaved) onSaved(path_);
    return true;
}

// --- 行操作（批次 34）-------------------------------------------------------

void CsvPanel::AddRowAt() {
    if (!data_) return;
    CommitCellEdit(true);
    int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    size_t origRow = data_->RowCount();   // 无选中 → 末尾追加
    if (sel >= 0 && (size_t)sel < viewRows_.size())
        origRow = viewRows_[sel] + 1;     // 插到选中行之后（原始行号空间）
    data_->InsertRow(origRow);
    dirty_ = true;
    RebuildView();
    UpdateSaveState();
    Logger::Debug("CsvPanel: row inserted at " + std::to_string(origRow));
}

void CsvPanel::DeleteSelectedRow() {
    if (!data_) return;
    CommitCellEdit(true);
    int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (sel < 0 || (size_t)sel >= viewRows_.size()) return;
    size_t origRow = viewRows_[sel];
    if (origRow == 0) return;   // 表头行不进 viewRows_，防御
    data_->DeleteRow(origRow);
    dirty_ = true;
    RebuildView();
    UpdateSaveState();
    // 尽量让选中停在原位置
    int rows = (int)viewRows_.size();
    if (rows > 0) {
        int keep = std::min(sel, rows - 1);
        ListView_SetItemState(list_, keep,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
    Logger::Debug("CsvPanel: row deleted at " + std::to_string(origRow));
}

void CsvPanel::ShowRowMenu(int xScreen, int yScreen) {
    if ((!data_ && !big_) || !list_) return;
    HMENU m = ::CreatePopupMenu();
    if (!m) return;
    if (data_) {   // 编辑类操作只在可写小文件模式提供
        ::AppendMenuW(m, MF_STRING, ID_CTX_ADDROW, Tr(L"csv.addrow"));
        int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        ::AppendMenuW(m, MF_STRING | (sel >= 0 ? 0 : MF_GRAYED),
                      ID_CTX_DELROW, Tr(L"csv.delrow"));
        ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(m, MF_STRING, ID_CTX_ADDCOL, Tr(L"csv.addcol"));
        ::AppendMenuW(m, MF_STRING | (lastSubItem_ >= 0 ? 0 : MF_GRAYED),
                      ID_CTX_DELCOL, Tr(L"csv.delcol"));
    }
    // 批次 43：跳转/导出（只读操作，大文件模式同样可用）
    if (data_) ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(m, MF_STRING, ID_CTX_GOTO, Tr(L"csv.goto"));
    int selCount = (int)SendMessageW(list_, LVM_GETSELECTEDCOUNT, 0, 0);
    ::AppendMenuW(m, MF_STRING | (selCount > 0 ? 0 : MF_GRAYED),
                  ID_CTX_EXPORT, Tr(L"csv.exportsel"));
    ::AppendMenuW(m, MF_STRING, ID_CTX_PRINT, Tr(L"csv.print"));   // 批次 44
    if (xScreen == -1 || yScreen == -1) {   // 键盘呼出 → 列表中央
        RECT rc{};
        ::GetWindowRect(list_, &rc);
        xScreen = (rc.left + rc.right) / 2;
        yScreen = (rc.top + rc.bottom) / 2;
    }
    // 命令以 WM_COMMAND(ID_CTX_*) 发回面板
    ::TrackPopupMenu(m, TPM_RIGHTBUTTON, xScreen, yScreen, 0, hwnd_, nullptr);
    ::DestroyMenu(m);
}

LRESULT CALLBACK CsvPanel::ListProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref) {
    auto* self = (CsvPanel*)ref;
    if (!self) return ::DefSubclassProc(h, m, wp, lp);
    return self->ListProc(h, m, wp, lp, id, ref);
}

LRESULT CsvPanel::ListProc(HWND h, UINT m, WPARAM wp, LPARAM lp,
                           UINT_PTR id, DWORD_PTR ref) {
    switch (m) {
        case WM_KEYDOWN:
            if (wp == VK_INSERT) { AddRowAt(); return 0; }
            if (wp == VK_DELETE) { DeleteSelectedRow(); return 0; }
            if (wp == 'G' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
                GoToRow();
                return 0;
            }
            break;
        case WM_CONTEXTMENU:
            ShowRowMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
    }
    return DefSubclassProc(h, m, wp, lp);
}

// --- 列操作（批次 35）-------------------------------------------------------

void CsvPanel::AddColAt() {
    if (!data_) return;
    CommitCellEdit(true);
    size_t col = lastSubItem_ >= 0 ? (size_t)lastSubItem_ : data_->cols;
    data_->InsertCol(col);
    RebuildColumns();
    dirty_ = true;
    UpdateSaveState();
    Logger::Debug("CsvPanel: col inserted at " + std::to_string(col));
}

void CsvPanel::DeleteColAt() {
    if (!data_ || lastSubItem_ < 0 ||
        (size_t)lastSubItem_ >= data_->cols) return;
    CommitCellEdit(true);
    data_->DeleteCol((size_t)lastSubItem_);
    RebuildColumns();
    dirty_ = true;
    UpdateSaveState();
    Logger::Debug("CsvPanel: col deleted at " + std::to_string(lastSubItem_));
}

void CsvPanel::RebuildColumns() {
    if (!data_ || !list_) return;
    cols_ = (int)data_->cols;
    if (lastSubItem_ >= cols_) lastSubItem_ = cols_ - 1;
    sortCol_ = -1;   // 列结构变了，旧排序列号失效
    while (ListView_DeleteColumn(list_, 0)) {}
    BuildColumns();
    RebuildView();
}

// --- 跳转行 / 导出选中行（批次 43）------------------------------------------

void CsvPanel::GoToRow() {
    if (!list_) return;
    CommitCellEdit(true);
    int total = ListView_GetItemCount(list_);
    if (total <= 0) return;
    std::wstring val;
    if (!InputBox(hwnd_, inst_, Tr(L"csv.goto"), Tr(L"csv.gotolabel"), val))
        return;
    const wchar_t* s = val.c_str();
    wchar_t* end = nullptr;
    unsigned long long n = ::wcstoull(s, &end, 10);
    if (end == s || n < 1 || n > (unsigned long long)total) {
        Logger::Debug("CsvPanel: goto rejected [" + WideToUtf8(val) + "]");
        return;
    }
    int idx = (int)n - 1;   // 用户按显示行号 1 基输入
    ListView_EnsureVisible(list_, idx, FALSE);
    ListView_SetItemState(list_, idx,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    Logger::Info("CsvPanel: goto row " + std::to_string(n));
}

void CsvPanel::ExportSelected() {
    if ((!data_ && !big_) || !list_ || path_.empty()) return;
    CommitCellEdit(true);
    std::vector<std::vector<std::wstring>> rows;
    for (int r = -1;
         (r = ListView_GetNextItem(list_, r, LVNI_SELECTED)) >= 0;) {
        std::vector<std::wstring> row;
        for (int c = 0; c < cols_; ++c) row.push_back(CellText(r, c));
        rows.push_back(std::move(row));
    }
    if (rows.empty()) {
        Logger::Debug("CsvPanel: export no selection");
        return;
    }
    wchar_t delim = data_ ? data_->delim : big_->Delim();
    std::string utf8 = csv::SerializeRows(rows, delim, newline_.c_str());
    utf8 += newline_.c_str();   // 导出文件补末尾换行
    Logger::Info("CsvPanel: export begin rows=" + std::to_string(rows.size()));

    // 默认文件名：原文件同目录 <stem>_export.csv
    std::wstring dir = L".", stem = path_;
    size_t slash = path_.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir = path_.substr(0, slash);
        stem = path_.substr(slash + 1);
    }
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    std::wstring def = dir + L"\\" + stem + L"_export.csv";

    wchar_t buf[512] = L"";
    ::lstrcpynW(buf, def.c_str(), 512);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetAncestor(hwnd_, GA_ROOT);   // owner 必须是顶层，WS_CHILD 会导致对话框不可见
    ofn.lpstrFilter = L"CSV (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 512;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                OFN_OVERWRITEPROMPT;
    if (!::GetSaveFileNameW(&ofn)) {   // 取消静默；真错误记 cde 便于诊断
        DWORD cde = ::CommDlgExtendedError();
        if (cde) Logger::Warn("CsvPanel: export dialog error cde=" + std::to_string(cde));
        return;
    }

    HANDLE h = ::CreateFileW(buf, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::wstring msg = Tr(L"msg.csvsavefail");
        msg += L"\n\n";
        msg += buf;
        ::MessageBoxW(hwnd_, msg.c_str(), L"xfsWinPad CSV",
                      MB_OK | MB_ICONERROR);
        Logger::Error("CsvPanel: export open failed gle=" +
                      std::to_string(::GetLastError()));
        return;
    }
    DWORD written = 0;
    BOOL wok = ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written,
                           nullptr);
    ::CloseHandle(h);
    if (!wok || written != utf8.size()) {
        ::MessageBoxW(hwnd_, Tr(L"msg.csvsavefail"), L"xfsWinPad CSV",
                      MB_OK | MB_ICONERROR);
        Logger::Error("CsvPanel: export short write");
        return;
    }
    Logger::Info("CsvPanel: exported " + std::to_string(rows.size()) +
                 " rows -> " + WideToUtf8(buf));
}

// --- 打印表格（批次 44）------------------------------------------------------
// 打印当前视图（过滤/排序生效）：每页重复表头；列按显示宽贪心分组做
// 水平分页，行按行高垂直分页——分页数学在 csv::BuildPrintPages，纯逻辑已单测。
void CsvPanel::PrintTable() {
    if ((!data_ && !big_) || !list_) return;
    CommitCellEdit(true);
    int rows = ListView_GetItemCount(list_);
    if (cols_ <= 0) return;

    PRINTDLGW pd{};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = ::GetAncestor(hwnd_, GA_ROOT);
    pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nMinPage = 1; pd.nMaxPage = 1; pd.nFromPage = 1; pd.nToPage = 1;
    if (!::PrintDlgW(&pd) || !pd.hDC) {   // 取消静默；真错误记 cde
        DWORD cde = ::CommDlgExtendedError();
        if (cde) Logger::Warn("CsvPanel: print dialog error cde=" +
                              std::to_string(cde));
        return;
    }
    HDC pdc = pd.hDC;
    int dpiX = ::GetDeviceCaps(pdc, LOGPIXELSX);
    int dpiY = ::GetDeviceCaps(pdc, LOGPIXELSY);
    int pageW = ::GetDeviceCaps(pdc, HORZRES);
    int pageH = ::GetDeviceCaps(pdc, VERTRES);
    int margin = dpiX / 2;
    int contentW = pageW - 2 * margin;
    int contentH = pageH - 2 * margin;

    HFONT base = ::CreateFontW(-::MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
        L"Consolas");
    HFONT bold = ::CreateFontW(-::MulDiv(9, dpiY, 72), 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
        L"Consolas");
    HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
    if (!base || !bold || !pen) {
        if (base) ::DeleteObject(base);
        if (bold) ::DeleteObject(bold);
        if (pen) ::DeleteObject(pen);
        ::DeleteDC(pdc);
        return;
    }
    auto oldFont = (HFONT)::SelectObject(pdc, base);
    auto oldPen = (HPEN)::SelectObject(pdc, pen);
    ::SetBkMode(pdc, TRANSPARENT);
    TEXTMETRICW tm{};
    ::GetTextMetricsW(pdc, &tm);
    int rowH = tm.tmHeight + tm.tmExternalLeading + 4;
    int rowAreaH = contentH - 2 * rowH;   // 表头 1 行 + 页脚 1 行
    if (rowAreaH < rowH) rowAreaH = rowH;

    std::vector<int> colW(cols_);
    for (int c = 0; c < cols_; ++c)
        colW[c] = ::MulDiv((int)ListView_GetColumnWidth(list_, c), dpiX, 96);
    auto plan = csv::BuildPrintPages(colW, rows, contentW, rowAreaH, rowH);
    Logger::Info("CsvPanel: print begin rows=" + std::to_string(rows) +
                 " cols=" + std::to_string(cols_) +
                 " pages=" + std::to_string(plan.size()));
    if (plan.empty()) {
        ::SelectObject(pdc, oldFont);
        ::DeleteObject(base); ::DeleteObject(bold); ::DeleteObject(pen);
        ::DeleteDC(pdc);
        return;
    }

    std::vector<std::wstring> hdrText(cols_);
    HWND hdr = ListView_GetHeader(list_);
    if (hdr) {
        int hc = Header_GetItemCount(hdr);
        for (int c = 0; c < cols_ && c < hc; ++c) {
            wchar_t txt[256] = L"";
            HDITEMW hi{};
            hi.mask = HDI_TEXT;   // 与 UpdateSortArrows 一致（HDI_STRING 本 SDK 未定义）
            hi.pszText = txt;
            hi.cchTextMax = 256;
            if (Header_GetItem(hdr, c, &hi)) hdrText[c] = txt;
        }
    }

    std::wstring docName = path_.substr(path_.find_last_of(L"\\/") + 1);
    DOCINFOW di{};
    di.cbSize = sizeof(di);
    di.lpszDocName = docName.c_str();
    if (::StartDocW(pdc, &di) <= 0) {
        Logger::Error("CsvPanel: StartDoc failed gle=" +
                      std::to_string(::GetLastError()));
        ::SelectObject(pdc, oldFont);
        ::DeleteObject(base); ::DeleteObject(bold); ::DeleteObject(pen);
        ::DeleteDC(pdc);
        return;
    }

    int pageNum = 0;
    int totalPages = (int)plan.size();
    for (const auto& pg : plan) {
        ++pageNum;
        if (::StartPage(pdc) <= 0) break;
        RECT full{0, 0, pageW, pageH};
        ::FillRect(pdc, &full, (HBRUSH)::GetStockObject(WHITE_BRUSH));
        int groupW = 0;
        for (int c = pg.colStart; c < pg.colEnd; ++c) groupW += colW[c];
        int usedBottom = margin + rowH + (pg.rowEnd - pg.rowStart) * rowH;

        ::SelectObject(pdc, bold);
        int x = margin;
        for (int c = pg.colStart; c < pg.colEnd; ++c) {
            RECT rc{x, margin, x + colW[c], margin + rowH};
            ::DrawTextW(pdc, hdrText[c].c_str(), -1, &rc,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);
            x += colW[c];
        }
        ::SelectObject(pdc, base);
        int y = margin + rowH;
        for (int r = pg.rowStart; r < pg.rowEnd; ++r) {
            x = margin;
            for (int c = pg.colStart; c < pg.colEnd; ++c) {
                std::wstring t = CellText(r, c);
                RECT rc{x, y, x + colW[c], y + rowH};
                ::DrawTextW(pdc, t.c_str(), -1, &rc,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);
                x += colW[c];
            }
            ::MoveToEx(pdc, margin, y, nullptr);
            ::LineTo(pdc, margin + groupW, y);
            y += rowH;
        }
        x = margin;
        for (int c = pg.colStart; c < pg.colEnd; ++c) {
            ::MoveToEx(pdc, x, margin, nullptr);
            ::LineTo(pdc, x, usedBottom);
            x += colW[c];
        }
        ::MoveToEx(pdc, margin + groupW, margin, nullptr);
        ::LineTo(pdc, margin + groupW, usedBottom);

        wchar_t foot[64];
        swprintf_s(foot, L"%d / %d", pageNum, totalPages);
        RECT fr{margin, usedBottom + rowH / 2, margin + groupW,
                pageH - margin / 2};
        ::DrawTextW(pdc, docName.c_str(), -1, &fr,
                    DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT);
        ::DrawTextW(pdc, foot, -1, &fr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        ::EndPage(pdc);
    }
    ::EndDoc(pdc);
    ::SelectObject(pdc, oldFont);
    ::DeleteObject(base); ::DeleteObject(bold); ::DeleteObject(pen);
    ::DeleteDC(pdc);
    Logger::Info("CsvPanel: printed pages=" + std::to_string(pageNum));
}

LRESULT CALLBACK CsvPanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (CsvPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->WndProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

} // namespace xfs
