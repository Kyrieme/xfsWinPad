#include "StdfPanel.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../theme/Theme.h"

#include <commctrl.h>
#include <windowsx.h>
#include <commdlg.h>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <algorithm>

namespace xfs {
namespace {
constexpr wchar_t kPanelClass[] = L"xfsWinPadStdfPanel";
constexpr int ID_LABEL = 1350;
constexpr int ID_CLOSE = 1351;
constexpr int ID_AIBTN = 1362;   // 批次 28「AI 分析」
constexpr int ID_TAB = 1352;
constexpr int ID_OVERVIEW = 1353;
constexpr int ID_LISTTESTS = 1354;
constexpr int ID_LISTRECS = 1355;
constexpr int ID_LISTLOG = 1356;
constexpr int ID_CSVBTN = 1357;
constexpr int ID_COMBO_RESULT = 1358;
constexpr int ID_COMBO_SITE = 1359;
constexpr int ID_COMBO_BIN = 1360;
constexpr int ID_COUNTLBL = 1361;
constexpr int ID_SEARCHEDIT = 1362;
constexpr int ID_STATSCOMBO = 1363;
constexpr int ID_STATSCANVAS = 1364;
constexpr int ID_STATSTEXT = 1365;
constexpr UINT_PTR kTabSubclassId = 0x53544432;   // 'STD2'

// Tab 2/3 列宽（96dpi 逻辑像素；Layout 时按 dpi 缩放）
constexpr int kTestCols[] = { 70, 46, 250, 64, 90, 90, 90, 90, 90, 70 };
constexpr int kTestColCount = 10;
constexpr int kRecCols[] = { 60, 90, 70, 90, 110 };
constexpr int kRecColCount = 5;

void AddCol(HWND lv, int i, const wchar_t* txt, int wLogical, int dpi) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<LPWSTR>(txt);
    col.cx = MulDiv(wLogical, dpi, 96);
    ListView_InsertColumn(lv, i, &col);
}
} // namespace

// ---------------------------------------------------------------- frame

bool StdfPanel::Create(HWND parent, HINSTANCE hInst) {
    if (hwnd_) return true;
    inst_ = hInst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPanelClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    int dpi = ::GetDpiForWindow(parent);
    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    hwnd_ = ::CreateWindowExW(0, kPanelClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 600, 240, parent,
                              (HMENU)(INT_PTR)1105, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", Tr(L"panel.stdf"),
                               WS_CHILD | WS_VISIBLE | SS_NOTIFY, 6, 8, 520, 18,
                               hwnd_, (HMENU)(INT_PTR)ID_LABEL, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.stdf.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 2, 76, 22,
                                  hwnd_, (HMENU)(INT_PTR)ID_CLOSE, hInst, nullptr);
    // 「AI 分析」（批次 28）：把当前 STDF 统计喂给 AI 面板问失效原因。
    // 未加载文件时禁用，Load 成功后启用。
    aiBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.stdf.ai"),
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 2, 84, 22,
                               hwnd_, (HMENU)(INT_PTR)ID_AIBTN, hInst, nullptr);
    if (label_) ::SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (closeBtn_) ::SendMessageW(closeBtn_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (aiBtn_) {
        ::SendMessageW(aiBtn_, WM_SETFONT, (WPARAM)font_, TRUE);
        ::EnableWindow(aiBtn_, FALSE);
    }

    tab_ = ::CreateWindowExW(0, WC_TABCONTROLW, nullptr,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
                             6, 26, 588, 200,
                             hwnd_, (HMENU)(INT_PTR)ID_TAB, hInst, nullptr);
    if (!tab_) return false;
    ::SendMessageW(tab_, WM_SETFONT, (WPARAM)font_, TRUE);
    // 子类化 tab 控件：Datalog 页按钮的 WM_COMMAND 父窗口是 tab_，转发回面板
    ::SetWindowSubclass(tab_, TabProcThunk, kTabSubclassId, (DWORD_PTR)this);
    // 四个 Tab 页（标题文本由 Retranslate 填）
    TCITEMW it{};
    it.mask = TCIF_TEXT;
    it.pszText = const_cast<LPWSTR>(L"");
    for (int i = 0; i < 5; ++i) TabCtrl_InsertItem(tab_, i, &it);
    Retranslate();   // 填 Tab 标题
    return true;
}

void StdfPanel::Destroy() {
    if (tab_) {
        ::RemoveWindowSubclass(tab_, TabProcThunk, kTabSubclassId);
    }
    for (HWND h : { overview_, listTests_, listRecs_, listLog_, csvBtn_,
                    comboResult_, comboSite_, comboBin_, countLabel_,
                    searchEdit_, statsCombo_, statsCanvas_, statsText_,
                    tab_, label_, closeBtn_, aiBtn_ }) {
        if (h) { ::DestroyWindow(h); h = nullptr; }
    }
    overview_ = listTests_ = listRecs_ = listLog_ = csvBtn_ = nullptr;
    comboResult_ = comboSite_ = comboBin_ = countLabel_ = nullptr;
    searchEdit_ = nullptr;
    testRows_.clear();
    rowMap_.clear();
    statsCombo_ = statsCanvas_ = statsText_ = nullptr;
    statsTestIdx_ = -1;
    tab_ = label_ = closeBtn_ = aiBtn_ = nullptr;
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
}

// tab 控件子类过程：把 Datalog 页按钮/下拉的 WM_COMMAND 转回面板 wndproc
LRESULT CALLBACK StdfPanel::TabProcThunk(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref) {
    auto* self = (StdfPanel*)ref;
    if (self && msg == WM_COMMAND) {
        // CSV 按钮 / 过滤下拉（父窗口=tab_）→ 面板统一处理
        switch (LOWORD(wp)) {
            case ID_CSVBTN: self->ExportDatalogCsv(); break;
            case ID_SEARCHEDIT:
                if (HIWORD(wp) == EN_CHANGE) self->QueueTestSearch();
                break;
            case ID_STATSCOMBO:
                if (HIWORD(wp) == CBN_SELCHANGE) self->OnStatsSelChange();
                break;
            case ID_COMBO_RESULT:
            case ID_COMBO_SITE:
            case ID_COMBO_BIN:
                if (HIWORD(wp) == CBN_SELCHANGE) self->RebuildRowMap();
                break;
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

bool StdfPanel::HasFocus() const {
    HWND f = ::GetFocus();
    return hwnd_ && (f == hwnd_ || f == overview_ || f == listTests_ ||
                     f == listRecs_ || f == listLog_ || f == csvBtn_ ||
                     ::IsChild(hwnd_, f));
}

void StdfPanel::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
}

void StdfPanel::EnsureTabs() {
    if (overview_ || !tab_) return;
    HINSTANCE hInst = inst_;
    int dpi = ::GetDpiForWindow(hwnd_);

    // Tab 1: 概览（只读 EDIT）
    overview_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                  ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                  0, 0, 100, 100,
                                  tab_, (HMENU)(INT_PTR)ID_OVERVIEW, hInst, nullptr);
    if (overview_) {
        ::SendMessageW(overview_, WM_SETFONT, (WPARAM)font_, TRUE);
        int m = MulDiv(8, dpi, 96);
        ::PostMessageW(overview_, EM_SETMARGINS, 0, MAKELPARAM(m, m));
    }

    // Tab 2: 测试项（虚拟 ListView；工具行 = 名称搜索框；列头可点击排序）
    listTests_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   LVS_REPORT | LVS_OWNERDATA,
                                   0, 0, 100, 100,
                                   tab_, (HMENU)(INT_PTR)ID_LISTTESTS, hInst, nullptr);
    if (listTests_) {
        ::SendMessageW(listTests_, WM_SETFONT, (WPARAM)font_, TRUE);
        ListView_SetExtendedListViewStyle(listTests_, LVS_EX_FULLROWSELECT |
                                                       LVS_EX_DOUBLEBUFFER |
                                                       LVS_EX_GRIDLINES);
        AddCol(listTests_, 0, Tr(L"stdf.col.testnum"), kTestCols[0], dpi);
        AddCol(listTests_, 1, Tr(L"stdf.col.kind"), kTestCols[1], dpi);
        AddCol(listTests_, 2, Tr(L"stdf.col.testtxt"), kTestCols[2], dpi);
        AddCol(listTests_, 3, Tr(L"stdf.col.units"), kTestCols[3], dpi);
        AddCol(listTests_, 4, Tr(L"stdf.col.n"), kTestCols[4], dpi);
        AddCol(listTests_, 5, Tr(L"stdf.col.fail"), kTestCols[5], dpi);
        AddCol(listTests_, 6, Tr(L"stdf.col.min"), kTestCols[6], dpi);
        AddCol(listTests_, 7, Tr(L"stdf.col.max"), kTestCols[7], dpi);
        AddCol(listTests_, 8, Tr(L"stdf.col.mean"), kTestCols[8], dpi);
        AddCol(listTests_, 9, Tr(L"stdf.col.limits"), kTestCols[9], dpi);
    }

    // Tab 2 工具行：测试名搜索框
    searchEdit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    ES_AUTOHSCROLL,
                                    0, 0, 240, 24,
                                    tab_, (HMENU)(INT_PTR)ID_SEARCHEDIT,
                                    hInst, nullptr);
    if (searchEdit_) ::SendMessageW(searchEdit_, WM_SETFONT, (WPARAM)font_, TRUE);

    // Tab 5: 统计（直方图 + Cp/Cpk）
    statsCombo_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                    CBS_DROPDOWNLIST,
                                    0, 0, 340, 300,
                                    tab_, (HMENU)(INT_PTR)ID_STATSCOMBO,
                                    hInst, nullptr);
    statsText_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                   ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                   0, 0, 100, 100,
                                   tab_, (HMENU)(INT_PTR)ID_STATSTEXT,
                                   hInst, nullptr);
    statsCanvas_ = ::CreateWindowExW(0, L"STATIC", L"",
                                     WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                                     0, 0, 100, 100,
                                     tab_, (HMENU)(INT_PTR)ID_STATSCANVAS,
                                     hInst, nullptr);
    for (HWND h : { statsCombo_, statsText_, statsCanvas_ })
        if (h) ::SendMessageW(h, WM_SETFONT, (WPARAM)font_, TRUE);

    // Tab 3: 原始记录（虚拟 ListView）
    listRecs_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   LVS_REPORT | LVS_OWNERDATA | LVS_NOSORTHEADER,
                                   0, 0, 100, 100,
                                   tab_, (HMENU)(INT_PTR)ID_LISTRECS, hInst, nullptr);
    if (listRecs_) {
        ::SendMessageW(listRecs_, WM_SETFONT, (WPARAM)font_, TRUE);
        ListView_SetExtendedListViewStyle(listRecs_, LVS_EX_FULLROWSELECT |
                                                      LVS_EX_DOUBLEBUFFER);
        AddCol(listRecs_, 0, L"#", kRecCols[0], dpi);
        AddCol(listRecs_, 1, Tr(L"stdf.col.recname"), kRecCols[1], dpi);
        AddCol(listRecs_, 2, Tr(L"stdf.col.typsub"), kRecCols[2], dpi);
        AddCol(listRecs_, 3, Tr(L"stdf.col.offset"), kRecCols[3], dpi);
        AddCol(listRecs_, 4, Tr(L"stdf.col.length"), kRecCols[4], dpi);
    }

    // Tab 4: Datalog（虚拟 ListView；工具行 = 过滤下拉 + 另存为 CSV 按钮）
    csvBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"stdf.savecsv"),
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                4, 4, 130, 24,
                                tab_, (HMENU)(INT_PTR)ID_CSVBTN, hInst, nullptr);
    if (csvBtn_) ::SendMessageW(csvBtn_, WM_SETFONT, (WPARAM)font_, TRUE);
    comboResult_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                     CBS_DROPDOWNLIST,
                                     0, 0, 90, 200,
                                     tab_, (HMENU)(INT_PTR)ID_COMBO_RESULT,
                                     hInst, nullptr);
    comboSite_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                   CBS_DROPDOWNLIST,
                                   0, 0, 80, 200,
                                   tab_, (HMENU)(INT_PTR)ID_COMBO_SITE,
                                   hInst, nullptr);
    comboBin_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                  CBS_DROPDOWNLIST,
                                  0, 0, 110, 200,
                                  tab_, (HMENU)(INT_PTR)ID_COMBO_BIN,
                                  hInst, nullptr);
    countLabel_ = ::CreateWindowExW(0, L"STATIC", L"",
                                    WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                    0, 0, 160, 24,
                                    tab_, (HMENU)(INT_PTR)ID_COUNTLBL,
                                    hInst, nullptr);
    for (HWND c : { comboResult_, comboSite_, comboBin_ })
        if (c) ::SendMessageW(c, WM_SETFONT, (WPARAM)font_, TRUE);
    if (countLabel_) ::SendMessageW(countLabel_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (comboResult_) {
        ComboBox_ResetContent(comboResult_);
        ComboBox_AddString(comboResult_, Tr(L"stdf.flt.all"));
        ComboBox_AddString(comboResult_, Tr(L"stdf.flt.pass"));
        ComboBox_AddString(comboResult_, Tr(L"stdf.flt.fail"));
        ComboBox_SetCurSel(comboResult_, 0);
    }
    listLog_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 LVS_REPORT | LVS_OWNERDATA | LVS_NOSORTHEADER,
                                 0, 0, 100, 100,
                                 tab_, (HMENU)(INT_PTR)ID_LISTLOG, hInst, nullptr);
    if (listLog_) {
        ::SendMessageW(listLog_, WM_SETFONT, (WPARAM)font_, TRUE);
        // GRIDLINES 在千列宽表下显著拖慢滚动（实测 1000 列滚轮 30ms→0.8ms），
        // 仅保留全行选择 + 双缓冲
        ListView_SetExtendedListViewStyle(listLog_, LVS_EX_FULLROWSELECT |
                                                     LVS_EX_DOUBLEBUFFER);
    }
    SwitchTab(curTab_);
}

void StdfPanel::SwitchTab(int idx) {
    curTab_ = idx;
    if (!overview_) return;   // EnsureTabs 未跑
    DWORD t0 = ::GetTickCount();
    if (overview_) ::ShowWindow(overview_, idx == 0 ? SW_SHOW : SW_HIDE);
    if (listTests_) ::ShowWindow(listTests_, idx == 1 ? SW_SHOW : SW_HIDE);
    if (searchEdit_) ::ShowWindow(searchEdit_, idx == 1 ? SW_SHOW : SW_HIDE);
    if (listRecs_) ::ShowWindow(listRecs_, idx == 2 ? SW_SHOW : SW_HIDE);
    if (listLog_) ::ShowWindow(listLog_, idx == 3 ? SW_SHOW : SW_HIDE);
    if (csvBtn_) ::ShowWindow(csvBtn_, idx == 3 ? SW_SHOW : SW_HIDE);
    if (comboResult_) ::ShowWindow(comboResult_, idx == 3 ? SW_SHOW : SW_HIDE);
    if (comboSite_) ::ShowWindow(comboSite_, idx == 3 ? SW_SHOW : SW_HIDE);
    if (comboBin_) ::ShowWindow(comboBin_, idx == 3 ? SW_SHOW : SW_HIDE);
    if (countLabel_) ::ShowWindow(countLabel_, idx == 3 ? SW_SHOW : SW_HIDE);
    if (statsCombo_) ::ShowWindow(statsCombo_, idx == 4 ? SW_SHOW : SW_HIDE);
    if (statsCanvas_) ::ShowWindow(statsCanvas_, idx == 4 ? SW_SHOW : SW_HIDE);
    if (statsText_) ::ShowWindow(statsText_, idx == 4 ? SW_SHOW : SW_HIDE);
    if (idx == 3 && sf_) {
        xfs::Logger::Debug("StdfPanel::SwitchTab datalog parts=" +
                           std::to_string(sf_->Parts().size()) + " cols=" +
                           std::to_string(sf_->TestColumnOrder().size()) + " " +
                           std::to_string(::GetTickCount() - t0) + " ms");
    }
}

void StdfPanel::Layout(int w, int h) {
    if (!hwnd_) return;
    int dpi = ::GetDpiForWindow(hwnd_);
    ::MoveWindow(label_, 8, 9, std::max(0, w - MulDiv(210, dpi, 96)), 18, TRUE);
    ::MoveWindow(closeBtn_, std::max(0, w - MulDiv(90, dpi, 96)), 3,
                 MulDiv(84, dpi, 96), MulDiv(22, dpi, 96), TRUE);
    ::MoveWindow(aiBtn_, std::max(0, w - MulDiv(184, dpi, 96)), 3,
                 MulDiv(84, dpi, 96), MulDiv(22, dpi, 96), TRUE);
    if (tab_) {
        ::MoveWindow(tab_, 6, 26, std::max(0, w - 12), std::max(0, h - 32), TRUE);
        EnsureTabs();
        RECT rc; ::GetClientRect(tab_, &rc);
        TabCtrl_AdjustRect(tab_, FALSE, &rc);
        int tw = std::max(0L, rc.right - rc.left);
        int th = std::max(0L, rc.bottom - rc.top);
        if (overview_) ::MoveWindow(overview_, rc.left, rc.top, tw, th, TRUE);
        if (listTests_) {
            int searchH = MulDiv(28, dpi, 96);
            int listH = std::max(0, th - searchH);
            if (searchEdit_)
                ::MoveWindow(searchEdit_, rc.left, rc.top, MulDiv(260, dpi, 96),
                             searchH, TRUE);
            ::MoveWindow(listTests_, rc.left, rc.top + searchH, tw, listH, TRUE);
        }
        if (listRecs_) ::MoveWindow(listRecs_, rc.left, rc.top, tw, th, TRUE);
        if (statsCombo_) {
            int cbH = MulDiv(28, dpi, 96);
            int remaining = std::max(0, th - cbH);
            int textH = MulDiv(150, dpi, 96);
            int canvasH = std::max(0, remaining - textH);
            ::MoveWindow(statsCombo_, rc.left, rc.top, MulDiv(360, dpi, 96), cbH, TRUE);
            if (statsCanvas_)
                ::MoveWindow(statsCanvas_, rc.left, rc.top + cbH, tw, canvasH, TRUE);
            if (statsText_)
                ::MoveWindow(statsText_, rc.left, rc.top + cbH + canvasH,
                             tw, textH, TRUE);
        }
        if (listLog_ || csvBtn_) {
            int btnH = MulDiv(28, dpi, 96);
            int listH = std::max(0, th - btnH);
            int x = rc.left;
            auto mv = [&](HWND h, int w) {
                if (h) { ::MoveWindow(h, x, rc.top, MulDiv(w, dpi, 96), btnH, TRUE);
                         x += MulDiv(w + 6, dpi, 96); }
            };
            mv(comboResult_, 90);
            mv(comboSite_, 80);
            mv(comboBin_, 110);
            if (countLabel_) {
                ::MoveWindow(countLabel_, x, rc.top, MulDiv(150, dpi, 96), btnH, TRUE);
                x += MulDiv(150 + 6, dpi, 96);
            }
            if (csvBtn_) {
                int bw = MulDiv(130, dpi, 96);
                int bx = x > rc.right - bw ? x : rc.right - bw;
                ::MoveWindow(csvBtn_, bx, rc.top, bw, btnH, TRUE);
            }
            if (listLog_) ::MoveWindow(listLog_, rc.left, rc.top + btnH, tw, listH, TRUE);
        }
    }
}

// ---------------------------------------------------------------- load

bool StdfPanel::Load(const std::wstring& filePath) {
    if (!hwnd_) return false;
    auto sf = std::make_unique<stdf::StdfFile>();
    std::wstring err;
    if (!sf->Open(filePath, &err)) {
        std::wstring msg = Tr(L"msg.stdfopenfail");
        if (!err.empty()) msg += L"\n" + err;
        ::MessageBoxW(hwnd_, msg.c_str(), L"xfsWinPad STDF",
                     MB_OK | MB_ICONINFORMATION);
        return false;
    }
    sf_ = std::move(sf);
    path_ = filePath;
    if (aiBtn_) ::EnableWindow(aiBtn_, TRUE);
    EnsureTabs();
    DWORD t0 = ::GetTickCount();
    BuildOverviewText();
    BuildDatalogColumns();
    DWORD t1 = ::GetTickCount();
    xfs::Logger::Debug("StdfPanel::Load cols=" +
                       std::to_string(sf_->TestColumnOrder().size()) +
                       " parts=" + std::to_string(sf_->Parts().size()) +
                       " buildCols=" + std::to_string(t1 - t0) + " ms");
    if (listTests_) {
        sortCol_ = -1;
        if (searchEdit_) ::SetWindowTextW(searchEdit_, L"");
        ApplyTestFilterSort();
    }
    if (listRecs_)
        ListView_SetItemCountEx(listRecs_,
                               static_cast<int>(sf_->Records().size()), LVSICF_NOINVALIDATEALL);
    if (statsCombo_) RebuildStatsCombo();
    if (listLog_) {
        rowMap_.clear();
        for (size_t i = 0; i < sf_->Parts().size(); ++i)
            rowMap_.push_back(static_cast<int32_t>(i));
        RebuildSiteBinCombos();
        if (countLabel_) {
            wchar_t cb[96];
            swprintf_s(cb, Tr(L"stdf.flt.count"),
                       static_cast<int>(sf_->Parts().size()),
                       static_cast<int>(sf_->Parts().size()));
            ::SetWindowTextW(countLabel_, cb);
        }
        ListView_SetItemCountEx(listLog_,
                               static_cast<int>(sf_->Parts().size() + 3),
                               LVSICF_NOINVALIDATEALL);   // +Unit/Low/High 三行
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    return true;
}

// Tab 1 概览文本（UTF-16）
void StdfPanel::BuildOverviewText() {
    if (!overview_ || !sf_) return;
    const auto& s = sf_->Summary();
    const char* base = sf_->Base();

    auto txt = [&](const stdf::StrSlice& sl) {
        return stdf::StdfFile::ToString(sl, base);
    };
    auto q = [&](const stdf::StrSlice& sl) {   // 空切片显示 “—”
        std::wstring v = txt(sl);
        return v.empty() ? L"—" : v;
    };
    auto num = [](unsigned long long v) {
        wchar_t b[32]; swprintf_s(b, L"%llu", v); return std::wstring(b);
    };
    auto S = [](const wchar_t* s) { return std::wstring(s); };

    std::wstring out;
    out += S(Tr(L"stdf.file")) + L": " + path_ + L"\r\n";
    out += S(Tr(L"stdf.cpu")) + L": " + num(s.cpuType) +
           L"    STDF v" + num(s.stdfVersion) +
           L"    " + S(Tr(L"stdf.recs")) + L": " + num(s.recCount) + L"\r\n\r\n";

    out += S(Tr(L"stdf.lot")) + L": " + q(s.lotId) + L"\r\n";
    out += S(Tr(L"stdf.prod")) + L": " + q(s.partTyp) + L"\r\n";
    out += S(Tr(L"stdf.node")) + L": " + q(s.nodeNam) + L"\r\n";
    out += S(Tr(L"stdf.job")) + L": " + q(s.jobNam) + L"\r\n";
    out += S(Tr(L"stdf.jobrev")) + L": " + q(s.jobRev) + L"\r\n";
    out += S(Tr(L"stdf.wafer")) + L": " + q(s.waferId) + L"\r\n";
    out += S(Tr(L"stdf.start")) + L": " +
           (s.startT.empty() ? std::wstring(L"—") : Utf8ToWide(s.startT)) + L"\r\n";
    out += S(Tr(L"stdf.finish")) + L": " +
           (s.finishT.empty() ? std::wstring(L"—") : Utf8ToWide(s.finishT)) + L"\r\n\r\n";

    out += S(Tr(L"stdf.parts")) + L": " + num(s.partCount) + L"    " +
           S(Tr(L"stdf.good")) + L": " + num(s.goodCount) +
           L" (" + num(s.partCount ? 100ull * s.goodCount / s.partCount : 0) +
           L"%)\r\n";

    // site 表
    if (!s.sites.empty()) {
        out += S(Tr(L"stdf.sites")) + L":";
        for (const auto& sc : s.sites)
            out += L"  " + num(sc.site) + L"→" + num(sc.partCount);
        out += L"\r\n";
    }
    // bin 表（HBIN / SBIN）
    if (!s.hbins.empty()) {
        out += L"\r\nHBIN:\r\n";
        for (const auto& b : s.hbins)
            out += L"  " + num(b.num) + L"  ×" + num(b.count) + L"  " + txt(b.name) + L"\r\n";
    }
    if (!s.sbins.empty()) {
        out += L"\r\nSBIN:\r\n";
        for (const auto& b : s.sbins)
            out += L"  " + num(b.num) + L"  ×" + num(b.count) + L"  " + txt(b.name) + L"\r\n";
    }
    // 记录类型统计
    out += L"\r\n" + S(Tr(L"stdf.reccounts")) + L":\r\n";
    out += L"  PTR ×" + num(s.ptrCount) + L"    MPR ×" + num(s.mprCount) +
           L"    FTR ×" + num(s.ftrCount) + L"\r\n";
    out += L"  PIR ×" + num(s.pirCount) + L"    PRR ×" + num(s.prrCount) +
           L"    WIR ×" + num(s.wirCount) + L"    WRR ×" + num(s.wrrCount) + L"\r\n";
    out += L"  GDR ×" + num(s.gdrCount) + L"    DTR ×" + num(s.dtrCount) + L"\r\n";
    if (!s.unknown.empty()) {
        out += L"  " + S(Tr(L"stdf.unknown")) + L":";
        for (const auto& kv : s.unknown)
            out += L"  " + num(kv.first >> 8) + L"-" + num(kv.first & 0xFF) +
                   L" ×" + num(kv.second);
        out += L"\r\n";
    }
    ::SetWindowTextW(overview_, out.c_str());
}


std::wstring StdfPanel::BuildAiStatsBlock() const {
    if (!sf_) return L"";
    return sf_->FormatAiStatsBlock(path_);
}

// ---------------------------------------------------------------- tests filter/sort

void StdfPanel::ApplyTestFilterSort() {
    if (!sf_ || !listTests_) return;
    const auto& tests = sf_->Tests();
    const char* base = sf_->Base();
    wchar_t q[128] = L"";
    if (searchEdit_) ::GetWindowTextW(searchEdit_, q, 128);
    // ANSI 样本名统一小写比较（避免每行 WideCharToMultiByte：q 同样 ASCII 化即可）
    auto lowerAscii = [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
    };
    wchar_t ql[128];
    int qlen = 0;
    for (; q[qlen] && qlen < 127; ++qlen) ql[qlen] = lowerAscii(q[qlen]);
    ql[qlen] = 0;

    std::vector<int32_t> rows;
    rows.reserve(tests.size());
    for (size_t i = 0; i < tests.size(); ++i) {
        if (qlen) {
            // testTxt 与 testNum 的十进制都参与匹配
            std::wstring name = stdf::StdfFile::ToString(tests[i].testTxt, base);
            bool hit = false;
            if (name.size() >= (size_t)qlen) {
                size_t n = name.size();
                for (size_t s = 0; s + (size_t)qlen <= n; ++s) {
                    size_t k = 0;
                    while (k < (size_t)qlen &&
                           lowerAscii(name[s + k]) == ql[k]) ++k;
                    if (k == (size_t)qlen) { hit = true; break; }
                }
            }
            if (!hit) {
                wchar_t nb[24];
                swprintf_s(nb, L"%lld",
                           static_cast<long long>(tests[i].testNum));
                for (wchar_t* p = nb; *p; ++p) *p = lowerAscii(*p);
                hit = wcsstr(nb, ql) != nullptr;
            }
            if (!hit) continue;
        }
        rows.push_back(static_cast<int32_t>(i));
    }

    if (sortCol_ >= 0) {
        const auto& T = tests;
        auto val = [&](int32_t i) -> std::pair<double, std::wstring> {
            const stdf::TestItem& t = T[i];
            switch (sortCol_) {
                case 0: return { (double)t.testNum, L"" };
                case 1: return { t.kind == 'M' ? 1.0 : 0.0, L"" };
                case 4: return { (double)t.count, L"" };
                case 5: return { (double)t.failCount, L"" };
                case 6: return { t.count ? t.minV : 0, L"" };
                case 7: return { t.count ? t.maxV : 0, L"" };
                case 8: return { t.count ? t.Mean() : 0, L"" };
                default: {
                    std::wstring s = stdf::StdfFile::ToString(t.testTxt, base);
                    return { 0, s };
                }
            }
        };
        std::sort(rows.begin(), rows.end(), [&](int32_t a, int32_t b) {
            if (sortCol_ == 2 || sortCol_ == 3) {
                std::wstring sa = val(a).second, sb2 = val(b).second;
                int c = sa.compare(sb2);
                return sortDesc_ ? c > 0 : c < 0;
            }
            double va = val(a).first, vb = val(b).first;
            return sortDesc_ ? va > vb : va < vb;
        });
    }

    testRows_.swap(rows);
    ListView_SetItemCountEx(listTests_, static_cast<int>(testRows_.size()),
                           LVSICF_NOINVALIDATEALL);
    UpdateSortArrows();
}

// 列头文本追加 ▲/▼ 指示当前排序列
void StdfPanel::UpdateSortArrows() {
    if (!listTests_) return;
    HWND hdr = ListView_GetHeader(listTests_);
    if (!hdr) return;
    wchar_t b[96];
    for (int i = 0; i < kTestColCount; ++i) {
        HDITEMW it{};
        it.mask = HDI_TEXT;
        it.pszText = b;
        it.cchTextMax = 96;
        if (!Header_GetItem(hdr, i, &it)) continue;
        // 去掉旧箭头
        size_t len = wcslen(b);
        if (len >= 2 && (b[len - 1] == 0x25B2 || b[len - 1] == 0x25BC))
            b[len - 2] = 0;
        if (i == sortCol_) {
            wcscat_s(b, sortDesc_ ? L" ▼" : L" ▲");
        }
        it.pszText = b;
        it.cchTextMax = (int)wcslen(b);
        Header_SetItem(hdr, i, &it);
    }
}

void StdfPanel::QueueTestSearch() {
    if (searchTimer_) ::KillTimer(hwnd_, searchTimer_);
    ::SetTimer(hwnd_, searchTimer_ = 3, 250, nullptr);
}

// ---------------------------------------------------------------- stats tab

void StdfPanel::RebuildStatsCombo() {
    if (!sf_ || !statsCombo_) return;
    if (statsTestIdx_ < 0) statsTestIdx_ = 0;   // 默认统计第一个测试项
    const auto& tests = sf_->Tests();
    const char* base = sf_->Base();
    SendMessageW(statsCombo_, WM_SETREDRAW, FALSE, 0);
    ComboBox_ResetContent(statsCombo_);
    wchar_t b[160];
    for (size_t i = 0; i < tests.size() && i < 20000; ++i) {
        std::wstring name = stdf::StdfFile::ToString(tests[i].testTxt, base);
        if (name.empty())
            swprintf_s(b, L"#%lld", (long long)tests[i].testNum);
        else if (name.size() > 150) {
            name.resize(150);
            name += L"…";
            wcscpy_s(b, name.c_str());
        } else {
            wcscpy_s(b, name.c_str());
        }
        int idx = ComboBox_AddString(statsCombo_, b);
        ComboBox_SetItemData(statsCombo_, idx, (LPARAM)i);
    }
    SendMessageW(statsCombo_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(statsCombo_, nullptr, TRUE);
    if (statsTestIdx_ >= 0 && (size_t)statsTestIdx_ < tests.size()) {
        // 尽量恢复之前的选择
        for (int i = 0; i < ComboBox_GetCount(statsCombo_); ++i) {
            if ((LPARAM)statsTestIdx_ ==
                ComboBox_GetItemData(statsCombo_, i)) {
                ComboBox_SetCurSel(statsCombo_, i);
                break;
            }
        }
    } else if (ComboBox_GetCount(statsCombo_) > 0) {
        ComboBox_SetCurSel(statsCombo_, 0);
    }
    OnStatsSelChange();
}

void StdfPanel::OnStatsSelChange() {
    if (!sf_ || !statsCombo_) return;
    int sel = ComboBox_GetCurSel(statsCombo_);
    if (sel < 0) { statsTestIdx_ = -1; return; }
    statsTestIdx_ = static_cast<int>(ComboBox_GetItemData(statsCombo_, sel));
    stats_ = stdf::StdfFile::ComputeStats(*sf_,
                                          static_cast<size_t>(statsTestIdx_));
    const auto& tests = sf_->Tests();
    const auto& t = tests[static_cast<size_t>(statsTestIdx_)];
    const char* base = sf_->Base();
    std::wstring name = stdf::StdfFile::ToString(t.testTxt, base);
    wchar_t b[96];
    std::wstring out;
    auto line = [&](const std::wstring& k, const std::wstring& v) {
        out += k + L": " + v + L"\r\n";
    };
    auto f6 = [&](double v) {
        std::wstring s;
        swprintf_s(b, L"%.6g", v);
        s = b;
        return s;
    };
    out += name + L"\r\n";
    line(Tr(L"stdf.stat.n"), std::to_wstring(stats_.n));
    if (stats_.n) {
        line(Tr(L"stdf.stat.mean"), f6(stats_.mean));
        line(Tr(L"stdf.stat.sigma"), f6(stats_.sigma));
        line(Tr(L"stdf.stat.min"), f6(stats_.minV));
        line(Tr(L"stdf.stat.max"), f6(stats_.maxV));
        if (stats_.hasLo) {
            std::wstring k = std::wstring(Tr(L"stdf.stat.lolim"));
            line(k, f6(stats_.lo));
        }
        if (stats_.hasHi) {
            std::wstring k = std::wstring(Tr(L"stdf.stat.hilim"));
            line(k, f6(stats_.hi));
        }
        if (stats_.hasLo || stats_.hasHi) {
            line(Tr(L"stdf.stat.over"),
                 std::to_wstring(stats_.overLo) + L" / " +
                 std::to_wstring(stats_.overHi));
        }
        if (stats_.Cpk != 0) line(L"Cpk", f6(stats_.Cpk));
        if (stats_.Cp != 0) line(L"Cp", f6(stats_.Cp));
        std::wstring u = stdf::StdfFile::ToString(t.units, base);
        if (!u.empty()) line(Tr(L"stdf.col.units"), u);
    }
    ::SetWindowTextW(statsText_, out.c_str());
    ::InvalidateRect(statsCanvas_, nullptr, TRUE);
}

void StdfPanel::DrawHistogram(HDC dc, const RECT& rc) {
    HBRUSH bg = ::CreateSolidBrush(colors_.bg);
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);
    if (!sf_ || statsTestIdx_ < 0 || !stats_.n) return;

    const auto& parts = sf_->Parts();
    size_t ti = static_cast<size_t>(statsTestIdx_);
    double lo = stats_.minV, hi = stats_.maxV;
    if (stats_.hasLo && stats_.hasHi) {
        // 视野扩到限值（若限值离分布不太远）
        double span = stats_.maxV - stats_.minV;
        if (stats_.lo > stats_.minV - span && stats_.lo < stats_.minV) lo = stats_.lo;
        if (stats_.hi < stats_.maxV + span && stats_.hi > stats_.maxV) hi = stats_.hi;
    }
    if (!(hi > lo)) return;

    int bins = kHistBins;
    std::vector<uint32_t> hist(bins, 0);
    uint32_t below = 0, above = 0;
    for (const auto& p : parts) {
        if (ti >= p.results.size()) continue;
        float f = p.results[ti];
        if (std::isnan(f)) continue;
        double v = f;
        if (v < lo) { ++below; continue; }
        if (v > hi) { ++above; continue; }
        int b = (int)((v - lo) / (hi - lo) * bins);
        if (b >= bins) b = bins - 1;
        ++hist[(size_t)b];
    }
    uint32_t maxCnt = 1;
    for (uint32_t c : hist) if (c > maxCnt) maxCnt = c;

    int dpi = ::GetDpiForWindow(hwnd_);
    int m = MulDiv(10, dpi, 96);
    int plotL = rc.left + m, plotR = rc.right - m - MulDiv(48, dpi, 96);
    int plotT = rc.top + m, plotB = rc.bottom - MulDiv(24, dpi, 96);
    if (plotR - plotL < 10 || plotB - plotT < 10) return;

    // 坐标轴
    HPEN axisPen = ::CreatePen(PS_SOLID, 1, colors_.fg);
    HGDIOBJ oldPen = ::SelectObject(dc, axisPen);
    ::MoveToEx(dc, plotL, plotB, nullptr);
    ::LineTo(dc, plotR, plotB);
    ::MoveToEx(dc, plotL, plotT, nullptr);
    ::LineTo(dc, plotL, plotB);

    double bw = (plotR - plotL) / (double)bins;
    HBRUSH bar = ::CreateSolidBrush(RGB(0x4C, 0x8B, 0xC7));
    HGDIOBJ oldBr = ::SelectObject(dc, bar);
    for (int i = 0; i < bins; ++i) {
        if (!hist[(size_t)i]) continue;
        int h = (int)((double)hist[(size_t)i] / maxCnt * (plotB - plotT - 4));
        RECT r{ plotL + (int)(i * bw), plotB - h, plotL + (int)((i + 1) * bw) - 1, plotB };
        ::FillRect(dc, &r, bar);
    }
    ::SelectObject(dc, oldBr);
    ::DeleteObject(bar);

    // 限值线（红）与均值线（橙）
    auto vline = [&](double v, COLORREF c, int style) {
        if (v < lo || v > hi) return;
        int x = plotL + (int)((v - lo) / (hi - lo) * (plotR - plotL));
        HPEN p = ::CreatePen(style, 1, c);
        ::SelectObject(dc, p);
        ::MoveToEx(dc, x, plotT, nullptr);
        ::LineTo(dc, x, plotB);
        ::SelectObject(dc, axisPen);
        ::DeleteObject(p);
    };
    if (stats_.hasLo) vline(stats_.lo, RGB(0xC0, 0x30, 0x30), PS_SOLID);
    if (stats_.hasHi) vline(stats_.hi, RGB(0xC0, 0x30, 0x30), PS_SOLID);
    vline(stats_.mean, RGB(0xE0, 0x90, 0x20), PS_DOT);

    // 轴标签：lo / mean / hi（彩色小字）
    ::SetTextColor(dc, colors_.fg);
    ::SetBkMode(dc, TRANSPARENT);
    wchar_t b[48];
    auto text = [&](const wchar_t* s, int x, int y) {
        ::TextOutW(dc, x, y, s, (int)wcslen(s));
    };
    swprintf_s(b, L"%.4g", lo); text(b, plotL, plotB + 2);
    swprintf_s(b, L"%.4g", hi); text(b, plotR - MulDiv(40, dpi, 96), plotB + 2);
    if (stats_.hasLo && stats_.lo >= lo && stats_.lo <= hi) {
        ::SetTextColor(dc, RGB(0xC0, 0x30, 0x30));
        swprintf_s(b, L"L=%.4g", stats_.lo);
        int x = plotL + (int)((stats_.lo - lo) / (hi - lo) * (plotR - plotL));
        text(b, x, plotT);
    }
    if (stats_.hasHi && stats_.hi >= lo && stats_.hi <= hi) {
        ::SetTextColor(dc, RGB(0xC0, 0x30, 0x30));
        swprintf_s(b, L"H=%.4g", stats_.hi);
        int x = plotL + (int)((stats_.hi - lo) / (hi - lo) * (plotR - plotL));
        if (x > plotR - MulDiv(50, dpi, 96)) x -= MulDiv(50, dpi, 96);
        text(b, x, plotT + MulDiv(14, dpi, 96));
    }
    ::SelectObject(dc, oldPen);
    ::DeleteObject(axisPen);
    ::SetTextColor(dc, colors_.fg);
}

void StdfPanel::ShowStatsForItem(int testIdx) {
    if (!sf_ || testIdx < 0) return;
    if (statsTestIdx_ != testIdx) statsTestIdx_ = testIdx;
    // 若组合框还没建，切到统计页时会重建
    SwitchTab(4);
    TabCtrl_SetCurFocus(tab_, 4);
    RebuildStatsCombo();
}

// ---------------------------------------------------------------- cells

std::wstring StdfPanel::TestRowText(int row, int col) const {
    if (!sf_) return L"";
    const auto& v = sf_->Tests();
    if (row < 0 || (size_t)row >= testRows_.size()) return L"";
    size_t r = static_cast<size_t>(testRows_[row]);
    if (r >= v.size()) return L"";
    const stdf::TestItem& t = v[r];
    const char* base = sf_->Base();
    wchar_t b[64];
    switch (col) {
        case 0:
            if (t.testNum == 0) return L"(txt)";
            swprintf_s(b, L"%lld", static_cast<long long>(t.testNum));
            return b;
        case 1:
            return t.kind == 'M' ? L"MPR" : L"PTR";
        case 2:
            return stdf::StdfFile::ToString(t.testTxt, base);
        case 3:
            return stdf::StdfFile::ToString(t.units, base);
        case 4:
            swprintf_s(b, L"%u", t.count); return b;
        case 5:
            swprintf_s(b, L"%u", t.failCount); return b;
        case 6:
            if (!t.count) return L"—";
            swprintf_s(b, L"%.6g", t.minV); return b;
        case 7:
            if (!t.count) return L"—";
            swprintf_s(b, L"%.6g", t.maxV); return b;
        case 8:
            if (!t.count) return L"—";
            swprintf_s(b, L"%.6g", t.Mean()); return b;
        case 9: {
            std::wstring out;
            if (t.hasLo) { swprintf_s(b, L"%.6g", t.lo); out += b; }
            if (t.hasLo || t.hasHi) out += L" ~ ";
            if (t.hasHi) { swprintf_s(b, L"%.6g", t.hi); out += b; }
            return out.empty() ? L"—" : out;
        }
    }
    return L"";
}

std::wstring StdfPanel::RecordRowText(int row, int col) const {
    if (!sf_) return L"";
    const auto& v = sf_->Records();
    if (row < 0 || static_cast<size_t>(row) >= v.size()) return L"";
    const stdf::RecordIndexEntry& e = v[row];
    wchar_t b[64];
    switch (col) {
        case 0:
            swprintf_s(b, L"%d", row + 1); return b;
        case 1: {
            const char* n = stdf::StdfFile::RecordName(e.typ, e.sub);
            wchar_t w[16];
            int i = 0;
            for (; n[i] && i < 15; ++i) w[i] = (wchar_t)(unsigned char)n[i];
            w[i] = 0;
            return w;
        }
        case 2:
            swprintf_s(b, L"%u.%u", e.typ, e.sub); return b;
        case 3:
            swprintf_s(b, L"0x%08X", e.offset); return b;
        case 4:
            swprintf_s(b, L"%u", e.len); return b;
    }
    return L"";
}

// ---------------------------------------------------------------- datalog

// Tab 4 列：7 个固定列（时间/序号/site/X/Y/HWbin/SWbin）+ Pass + 测试项（列序）
void StdfPanel::BuildDatalogColumns() {
    if (!listLog_ || !sf_) return;
    DWORD t0 = ::GetTickCount();
    int dpi = ::GetDpiForWindow(hwnd_);
    // 分批异步构建：3241 列同步插入实测 ~0.5s（每列 ~0.15ms，comctl 内部
    // header 布局），Load 期间整段冻结 UI。改为 SETREDRAW OFF + 定时器分批，
    // 每批之间让出消息循环，列渐进出现且不冻结。
    ::SendMessageW(listLog_, WM_SETREDRAW, FALSE, 0);
    HWND hdr = ListView_GetHeader(listLog_);
    if (hdr) ::ShowWindow(hdr, SW_HIDE);
    while (ListView_DeleteColumn(listLog_, 0)) {}
    AddCol(listLog_, 0, Tr(L"stdf.dl.time"), 110, dpi);
    AddCol(listLog_, 1, Tr(L"stdf.dl.no"), 55, dpi);
    AddCol(listLog_, 2, Tr(L"stdf.dl.site"), 45, dpi);
    AddCol(listLog_, 3, Tr(L"stdf.dl.x"), 55, dpi);
    AddCol(listLog_, 4, Tr(L"stdf.dl.y"), 55, dpi);
    AddCol(listLog_, 5, Tr(L"stdf.dl.hwbin"), 60, dpi);
    AddCol(listLog_, 6, Tr(L"stdf.dl.swbin"), 60, dpi);
    AddCol(listLog_, 7, Tr(L"stdf.dl.pass"), 45, dpi);
    buildCursor_ = 0;
    buildT0_ = t0;
    buildDpi_ = dpi;
    xfs::Logger::Debug("StdfPanel::delCols " +
                       std::to_string(::GetTickCount() - t0) + " ms");
    ::SetTimer(hwnd_, kBuildColTimer, 10, nullptr);
}

void StdfPanel::StepBuildDatalogColumns() {
    if (!listLog_ || !sf_) { ::KillTimer(hwnd_, kBuildColTimer); return; }
    const auto& order = sf_->TestColumnOrder();
    const auto& tests = sf_->Tests();
    const char* base = sf_->Base();
    // 收尾批：恢复重绘 + header
    bool last = buildCursor_ + kBuildColBatch >= order.size();
    size_t end = last ? order.size() : buildCursor_ + kBuildColBatch;
    for (size_t c = buildCursor_; c < end; ++c) {
        // 列头 = 测试项名（TEST_TXT；空则 #TEST_NUM）
        std::wstring name = stdf::StdfFile::ToString(tests[order[c]].testTxt, base);
        if (name.empty()) {
            wchar_t b[24];
            swprintf_s(b, L"#%lld", (long long)tests[order[c]].testNum);
            name = b;
        }
        AddCol(listLog_, 8 + (int)c, name.c_str(), 95, buildDpi_);
    }
    buildCursor_ = end;
    if (last) {
        ::KillTimer(hwnd_, kBuildColTimer);
        HWND hdr = ListView_GetHeader(listLog_);
        if (hdr) ::ShowWindow(hdr, SW_SHOW);
        ::SendMessageW(listLog_, WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(listLog_, nullptr, TRUE);
        xfs::Logger::Debug("StdfPanel::BuildDatalogColumns cols=" +
                           std::to_string(order.size() + 8) + " " +
                           std::to_string(::GetTickCount() - buildT0_) + " ms");
    }
}

std::wstring StdfPanel::DatalogRowText(int row, int col) const {
    // 诊断埋点：单格取词计时（问题定位后移除）
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER a, b;
    ::QueryPerformanceCounter(&a);
    std::wstring s = DatalogRowTextImpl(row, col);
    ::QueryPerformanceCounter(&b);
    unsigned long long us =
        (unsigned long long)((b.QuadPart - a.QuadPart) * 1000000 / freq.QuadPart);
    ++dbgCells_;
    dbgUs_ += us;
    if (us > dbgMaxUs_) { dbgMaxUs_ = us; dbgMaxRow_ = row; dbgMaxCol_ = col; }
    if (dbgUs_ >= 50000 || dbgMaxUs_ >= 5000) {
        xfs::Logger::Debug("STDF datalog cells=" + std::to_string(dbgCells_) +
                           " totalUs=" + std::to_string(dbgUs_) +
                           " maxUs=" + std::to_string(dbgMaxUs_) +
                           " maxAt=" + std::to_string(dbgMaxRow_) + "," +
                           std::to_string(dbgMaxCol_));
        dbgCells_ = dbgUs_ = dbgMaxUs_ = 0;
        dbgMaxRow_ = dbgMaxCol_ = -1;
    }
    return s;
}

std::wstring StdfPanel::DatalogRowTextImpl(int row, int col) const {
    if (!sf_) return L"";
    const auto& parts = sf_->Parts();
    const auto& order = sf_->TestColumnOrder();
    const auto& tests = sf_->Tests();
    const char* base = sf_->Base();
    wchar_t b[48];

    // 表头三行（与 ATE log CSV 一致）：0=Unit 1=Low Limit 2=High Limit；数据行从 3 起
    if (row < 3) {
        if (col < 8) {
            if (col == 0 && row == 0) return L"Unit";
            if (col == 0 && row == 1) return L"Low Limit";
            if (col == 0 && row == 2) return L"High Limit";
            return L"";
        }
        size_t ti = static_cast<size_t>(col) - 8;
        if (ti >= order.size()) return L"";
        const auto& t = tests[order[ti]];
        if (row == 0)
            return stdf::StdfFile::ToString(t.units, base);
        if (row == 1) {
            if (!t.hasLo) return L"";
            swprintf_s(b, L"%.6g", t.lo);
            return b;
        }
        if (!t.hasHi) return L"";
        swprintf_s(b, L"%.6g", t.hi);
        return b;
    }

    const size_t pi = rowMap_.empty()
                          ? static_cast<size_t>(row) - 3
                          : static_cast<size_t>(rowMap_[row - 3]);
    if (pi >= parts.size()) return L"";
    const stdf::PartRow& p = parts[pi];
    switch (col) {
        case 0:
            // PRR.TEST_T = 该颗测试耗时（ms）
            swprintf_s(b, L"%ums", p.testMs);
            return b;
        case 1:
            swprintf_s(b, L"%u", p.index);
            return b;
        case 2:
            swprintf_s(b, L"%u", p.site);
            return b;
        case 3:
            if (p.x == -32768) return L"—";
            swprintf_s(b, L"%d", p.x);
            return b;
        case 4:
            if (p.y == -32768) return L"—";
            swprintf_s(b, L"%d", p.y);
            return b;
        case 5:
            if (p.hbin == 0xFFFF) return L"—";
            swprintf_s(b, L"%u", p.hbin);
            return b;
        case 6:
            if (p.sbin == 0xFFFF) return L"—";
            swprintf_s(b, L"%u", p.sbin);
            return b;
        case 7:
            return p.pass ? L"Pass" : L"Fail";
        default: {
            size_t ti = static_cast<size_t>(col) - 8;
            if (ti >= order.size()) return L"";
            size_t idx = order[ti];
            if (idx >= p.results.size()) return L"";
            float v = p.results[idx];
            if (std::isnan(v)) return L"";
            swprintf_s(b, L"%.6g", v);
            return b;
        }
    }
}

void StdfPanel::RebuildRowMap() {
    if (!sf_) return;
    const auto& parts = sf_->Parts();
    int resSel = comboResult_ ? ComboBox_GetCurSel(comboResult_) : 0; // 0/1/2
    int siteSel = comboSite_ ? ComboBox_GetCurSel(comboSite_) : -1;   // -1/0..n
    int binSel = comboBin_ ? ComboBox_GetCurSel(comboBin_) : -1;
    std::vector<int32_t> next;
    next.reserve(parts.size());
    const size_t nSite = siteItems_.size(), nBin = binItems_.size();
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[i];
        if (resSel == 1 && !p.pass) continue;
        if (resSel == 2 && p.pass) continue;
        if (siteSel > 0 && (size_t)siteSel - 1 < nSite &&
            p.site != siteItems_[(size_t)siteSel - 1]) continue;
        if (binSel > 0 && (size_t)binSel - 1 < nBin &&
            p.hbin != binItems_[(size_t)binSel - 1]) continue;
        next.push_back(static_cast<int32_t>(i));
    }
    rowMap_.swap(next);
    int shown = listLog_ ? (int)(rowMap_.size() + 3) : 0;
    if (listLog_) ListView_SetItemCountEx(listLog_, shown, LVSICF_NOINVALIDATEALL);
    if (countLabel_) {
        wchar_t b[96];
        swprintf_s(b, Tr(L"stdf.flt.count"), (int)rowMap_.size(), (int)parts.size());
        ::SetWindowTextW(countLabel_, b);
    }
}

void StdfPanel::RebuildSiteBinCombos() {
    siteItems_.clear();
    binItems_.clear();
    if (!sf_) return;
    // 首见序收集 site / hbin（跳过哨兵）
    for (const auto& p : sf_->Parts()) {
        if (p.site != 0xFF &&
            std::find(siteItems_.begin(), siteItems_.end(), p.site) == siteItems_.end())
            siteItems_.push_back(p.site);
        if (p.hbin != 0xFFFF &&
            std::find(binItems_.begin(), binItems_.end(), p.hbin) == binItems_.end())
            binItems_.push_back(p.hbin);
        if (siteItems_.size() > 63 || binItems_.size() > 63) break;
    }
    wchar_t b[32];
    if (comboSite_) {
        SendMessageW(comboSite_, WM_SETREDRAW, FALSE, 0);
        ComboBox_ResetContent(comboSite_);
        ComboBox_AddString(comboSite_, Tr(L"stdf.flt.allsite"));
        for (uint8_t s : siteItems_) {
            swprintf_s(b, L"%u", s);
            ComboBox_AddString(comboSite_, b);
        }
        SendMessageW(comboSite_, WM_SETREDRAW, TRUE, 0);
        ComboBox_SetCurSel(comboSite_, 0);
        ::InvalidateRect(comboSite_, nullptr, TRUE);
    }
    if (comboBin_) {
        SendMessageW(comboBin_, WM_SETREDRAW, FALSE, 0);
        ComboBox_ResetContent(comboBin_);
        ComboBox_AddString(comboBin_, Tr(L"stdf.flt.allbin"));
        for (uint16_t hb : binItems_) {
            swprintf_s(b, L"%u", hb);
            ComboBox_AddString(comboBin_, b);
        }
        SendMessageW(comboBin_, WM_SETREDRAW, TRUE, 0);
        ComboBox_SetCurSel(comboBin_, 0);
        ::InvalidateRect(comboBin_, nullptr, TRUE);
    }
}

// 另存为 CSV：ATE log 格式（行1 名/行2 单位/行3 low/行4 high/行5+ 数据）
void StdfPanel::ExportDatalogCsv() {
    if (!sf_ || sf_->Parts().empty()) return;
    // 默认文件名 = STDF 文件名去扩展 + .csv；默认目录 = STDF 所在目录
    std::filesystem::path src(path_);
    std::wstring defName = src.stem().wstring() + L".csv";
    std::wstring initDir = src.parent_path().wstring();

    wchar_t file[MAX_PATH] = {};
    wcscpy_s(file, defName.c_str());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"CSV (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initDir.c_str();
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // 生成 CSV 文本（核心逻辑在 StdfFile::ToDatalogCsv，可单测）。
    // 视图过滤生效时仅导出可见行（rowMap_ 非空 = 未过滤等价全量）。
    DWORD tCsv = ::GetTickCount();
    const std::vector<int32_t>* rows =
        (rowMap_.size() == sf_->Parts().size()) ? nullptr : &rowMap_;
    std::string csv = stdf::StdfFile::ToDatalogCsv(*sf_, rows);
    Logger::Debug("StdfPanel::ToDatalogCsv " +
                  std::to_string(::GetTickCount() - tCsv) + " ms " +
                  std::to_string(csv.size()) + " bytes");

    HANDLE h = ::CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::MessageBoxW(hwnd_, Tr(L"stdf.csvfail"), L"xfsWinPad STDF",
                      MB_OK | MB_ICONERROR);
        return;
    }
    DWORD written = 0;
    ::WriteFile(h, csv.data(), (DWORD)csv.size(), &written, nullptr);
    ::CloseHandle(h);
    Logger::Info("StdfPanel: CSV exported to " + WideToUtf8(file) +
                 " (" + std::to_string(sf_->Parts().size()) + " parts, " +
                 std::to_string(sf_->TestColumnOrder().size()) + " tests, " +
                 std::to_string(::GetTickCount() - tCsv) + " ms total)");
    ::MessageBoxW(hwnd_, Tr(L"stdf.csvdone"), L"xfsWinPad STDF",
                  MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- theme/i18n

void StdfPanel::ApplyTheme(const ThemeDef& t) {
    colors_.bg = t.editorBg;
    colors_.fg = t.editorFg;
    if (overview_) {
        // EDIT 控件原色即可（系统主题）；仅背景贴主题底色
        ::InvalidateRect(overview_, nullptr, TRUE);
    }
    // ListView 行文本/背景走主题
    ListView_SetBkColor(listTests_, colors_.bg);
    ListView_SetTextBkColor(listTests_, colors_.bg);
    ListView_SetTextColor(listTests_, colors_.fg);
    ListView_SetBkColor(listRecs_, colors_.bg);
    ListView_SetTextBkColor(listRecs_, colors_.bg);
    ListView_SetTextColor(listRecs_, colors_.fg);
    ListView_SetBkColor(listLog_, colors_.bg);
    ListView_SetTextBkColor(listLog_, colors_.bg);
    ListView_SetTextColor(listLog_, colors_.fg);
}

void StdfPanel::Retranslate() {
    if (label_) ::SetWindowTextW(label_, Tr(L"panel.stdf"));
    if (closeBtn_) ::SetWindowTextW(closeBtn_, Tr(L"panel.stdf.close"));
    if (aiBtn_) ::SetWindowTextW(aiBtn_, Tr(L"panel.stdf.ai"));
    if (tab_) {
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<LPWSTR>(Tr(L"stdf.tab.overview"));
        TabCtrl_SetItem(tab_, 0, &it);
        if (TabCtrl_GetItemCount(tab_) > 1) {
            it.pszText = const_cast<LPWSTR>(Tr(L"stdf.tab.tests"));
            TabCtrl_SetItem(tab_, 1, &it);
            it.pszText = const_cast<LPWSTR>(Tr(L"stdf.tab.records"));
            TabCtrl_SetItem(tab_, 2, &it);
            it.pszText = const_cast<LPWSTR>(Tr(L"stdf.tab.datalog"));
            TabCtrl_SetItem(tab_, 3, &it);
            if (TabCtrl_GetItemCount(tab_) > 4) {
                it.pszText = const_cast<LPWSTR>(Tr(L"stdf.tab.stats"));
                TabCtrl_SetItem(tab_, 4, &it);
            }
        }
    }
    if (csvBtn_) ::SetWindowTextW(csvBtn_, Tr(L"stdf.savecsv"));
    // 列标题
    if (listTests_) {
        int dpi = ::GetDpiForWindow(hwnd_);
        struct { int i; const wchar_t* k; } cols[kTestColCount] = {
            { 0, L"stdf.col.testnum" }, { 1, L"stdf.col.kind" },
            { 2, L"stdf.col.testtxt" }, { 3, L"stdf.col.units" },
            { 4, L"stdf.col.n" },       { 5, L"stdf.col.fail" },
            { 6, L"stdf.col.min" },    { 7, L"stdf.col.max" },
            { 8, L"stdf.col.mean" },   { 9, L"stdf.col.limits" },
        };
        for (auto& c : cols) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT;
            col.pszText = const_cast<LPWSTR>(Tr(c.k));
            ListView_SetColumn(listTests_, c.i, &col);
            (void)dpi;
        }
        UpdateSortArrows();
    }
    if (listRecs_) {
        struct { int i; const wchar_t* k; } cols[kRecColCount] = {
            { 0, L"#" }, { 1, L"stdf.col.recname" }, { 2, L"stdf.col.typsub" },
            { 3, L"stdf.col.offset" }, { 4, L"stdf.col.length" },
        };
        for (auto& c : cols) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT;
            if (c.i == 0) col.pszText = const_cast<LPWSTR>(L"#");
            else col.pszText = const_cast<LPWSTR>(Tr(c.k));
            ListView_SetColumn(listRecs_, c.i, &col);
        }
    }
    if (sf_) BuildOverviewText();
}

// ---------------------------------------------------------------- wndproc

LRESULT StdfPanel::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kBuildColTimer) { StepBuildDatalogColumns(); return 0; }
            if (wp == searchTimer_) {
                ::KillTimer(hwnd_, searchTimer_);
                searchTimer_ = 0;
                ApplyTestFilterSort();
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            if (dis->CtlID == ID_STATSCANVAS && dis->CtlType == ODT_STATIC) {
                DrawHistogram(dis->hDC, dis->rcItem);
                return TRUE;
            }
            break;
        }
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
            if (nm->idFrom == ID_TAB && nm->code == TCN_SELCHANGE) {
                SwitchTab(TabCtrl_GetCurSel(tab_));
                // 让新页获得键盘焦点
                HWND target = curTab_ == 0 ? overview_
                            : curTab_ == 1 ? listTests_
                            : curTab_ == 2 ? listRecs_ : listLog_;
                if (target && ::IsWindowVisible(target)) ::SetFocus(target);
                return 0;
            }
            if (nm->code == LVN_GETDISPINFO) {
                NMLVDISPINFOW* di = (NMLVDISPINFOW*)lp;
                if (di->item.mask & LVIF_TEXT) {
                    int row = static_cast<int>(di->item.iItem);
                    int col = di->item.iSubItem;
                    std::wstring s;
                    if (di->hdr.idFrom == ID_LISTTESTS) s = TestRowText(row, col);
                    else if (di->hdr.idFrom == ID_LISTLOG) s = DatalogRowText(row, col);
                    else s = RecordRowText(row, col);
                    int n = static_cast<int>(s.size());
                    int cap = static_cast<int>(di->item.cchTextMax) - 1;
                    if (cap > 0) {
                        if (n > cap) n = cap;
                        memcpy(di->item.pszText, s.data(), n * sizeof(wchar_t));
                        di->item.pszText[n] = 0;
                    }
                }
                return 0;
            }
            if (nm->code == LVN_COLUMNCLICK && nm->idFrom == ID_LISTTESTS) {
                NMLISTVIEW* cl = (NMLISTVIEW*)lp;
                int c = static_cast<int>(cl->iSubItem);
                if (sortCol_ == c) sortDesc_ = !sortDesc_;
                else { sortCol_ = c; sortDesc_ = false; }
                ApplyTestFilterSort();
                return 0;
            }
            if (nm->code == NM_DBLCLK && nm->idFrom == ID_LISTTESTS) {
                NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lp;
                if (ia->iItem >= 0 && (size_t)ia->iItem < testRows_.size())
                    ShowStatsForItem(testRows_[ia->iItem]);
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wp) == ID_CLOSE && onClose) onClose();
            if (LOWORD(wp) == ID_AIBTN && onAiAnalyze) onAiAnalyze();
            if (LOWORD(wp) == ID_CSVBTN) ExportDatalogCsv();
            break;
        }
        case WM_CTLCOLORSTATIC:
            return (LRESULT)::GetSysColorBrush(COLOR_BTNFACE);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK StdfPanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (StdfPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->WndProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

} // namespace xfs
