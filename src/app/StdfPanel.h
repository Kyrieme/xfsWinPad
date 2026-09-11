#pragma once
// xfsWinPad - StdfPanel: STDF v4 查看器底部停靠面板（Phase 20）。
//
// - 仿 HexPanel 的底部分割条/关闭按钮范式；内容由四个 Tab 组成：
//     1. 概览   — 只读 EDIT（MIR 摘要 + 记录统计 + bin/site 表）；
//     2. 测试项 — ListView 虚拟模式（LVS_OWNERDATA，5.4M 条也能秒开）；
//     3. 记录   — ListView 虚拟模式（原始记录索引，#typ.sub/偏移/长度）；
//     4. Datalog — 每颗芯片一行 × 每测试项一列的结果矩阵（虚拟 ListView，
//        列 = Test_time/Test_no/Site/X/Y/HW_bin/SW_bin/Pass + 测试项按
//        TEST_NUM 升序）；「另存为 CSV」按钮按 ATE log 格式导出
//        （行1 测试项名/行2 单位/行3 low/行4 high/行5+ 数据）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../stdf/StdfFile.h"

namespace xfs {

struct ThemeDef;

class StdfPanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    bool HasFocus() const;

    // 打开并扫描一个 .std/.stdf 文件；成功后显示面板。失败弹 MessageBox。
    bool Load(const std::wstring& filePath);
    void Hide();

    const std::wstring& FilePath() const { return path_; }

    // 批次 28：生成给 AI 的 STDF 统计块（英文数据行，prompt 用）。
    // 实现在 StdfFile::FormatAiStatsBlock（可单测）；未加载返回空串。
    std::wstring BuildAiStatsBlock() const;

    std::function<void()> onClose;
    std::function<void()> onAiAnalyze;   // 「AI 分析」按钮（批次 28）
    std::function<void(int)> onHeightChange;

    void ApplyTheme(const ThemeDef& t);
    void Retranslate();
    void Layout(int w, int h);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TabProcThunk(HWND, UINT, WPARAM, LPARAM,
                                         UINT_PTR, DWORD_PTR);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void BuildOverviewText();          // Tab 1 内容
    void EnsureTabs();                 // 首次显示时建 Tab 页控件
    void SwitchTab(int idx);
    std::wstring TestRowText(int row, int col) const;     // Tab 2 单元格
    std::wstring RecordRowText(int row, int col) const;   // Tab 3 单元格
    std::wstring DatalogRowText(int row, int col) const;  // Tab 4 单元格（计时包装）
    std::wstring DatalogRowTextImpl(int row, int col) const;  // Tab 4 单元格实现
    void BuildDatalogColumns();        // Tab 4 列（首次 Load 时启动分批构建）
    void StepBuildDatalogColumns();    // Tab 4 列分批步进（WM_TIMER 驱动）
    void RebuildRowMap();              // Tab 4 行过滤（结果/site/bin 下拉）
    void RebuildSiteBinCombos();   // 载入后填充 site/bin 下拉项
    void ApplyTestFilterSort();        // Tab 2 搜索过滤 + 列排序（重建 testRows_）
    void UpdateSortArrows();           // Tab 2 列头 ▲/▼ 指示
    void QueueTestSearch();            // Tab 2 搜索防抖
    void RebuildStatsCombo();          // Tab 5 统计项下拉（Load 后填充）
    void OnStatsSelChange();           // Tab 5 下拉选择 → 重算+重绘
    void DrawHistogram(HDC dc, const RECT& rc);   // Tab 5 直方图绘制
    void ExportDatalogCsv();           // 另存为 CSV
    void ShowStatsForItem(int testIdx);// Tab 5 统计（直方图+Cpk）

    static constexpr UINT_PTR kBuildColTimer = 7;   // 列构建分批定时器 ID
    static constexpr size_t kBuildColBatch = 256;   // 每批插入列数
    size_t buildCursor_ = 0;       // 已插入测试列数
    DWORD buildT0_ = 0;            // 构建起始时间（日志）
    int buildDpi_ = 96;

    // Tab 4 行过滤：rowMap_[k] = Parts() 下标（可见 die 行）
    std::vector<int32_t> rowMap_;
    HWND comboResult_ = nullptr;   // All/Pass/Fail
    HWND comboSite_ = nullptr;     // All sites / 0..N
    HWND comboBin_ = nullptr;      // All bins / HW bin 列表
    HWND countLabel_ = nullptr;    // 「N / M 行」计数
    std::vector<uint8_t> siteItems_;   // site 下拉项（首见序）
    std::vector<uint16_t> binItems_;   // hbin 下拉项（首见序）

    // Tab 2 测试项：搜索 + 排序映射 testRows_[k] = Tests() 下标
    std::vector<int32_t> testRows_;
    HWND searchEdit_ = nullptr;    // 测试名搜索框（Tab 2 工具行）
    int sortCol_ = -1;             // 当前排序列（-1=文件序）
    bool sortDesc_ = false;
    UINT_PTR searchTimer_ = 0;     // 搜索防抖定时器

    // Tab 5 统计：下拉选测试项 → 直方图 + Cp/Cpk 文本
    static constexpr int kHistBins = 32;      // 直方图桶数
    HWND statsCombo_ = nullptr;
    HWND statsCanvas_ = nullptr;   // 自绘区域（WS_OWNERDRAW STATIC）
    HWND statsText_ = nullptr;     // 统计文本（只读 EDIT）
    int statsTestIdx_ = -1;        // 当前统计的 Tests() 下标
    stdf::StdfFile::ItemStats stats_;

    // 诊断埋点：datalog 单元格取词耗时累加（perf 诊断用，问题定位后移除）
    mutable unsigned long long dbgCells_ = 0;
    mutable unsigned long long dbgUs_ = 0;
    mutable unsigned long long dbgMaxUs_ = 0;
    mutable int dbgMaxRow_ = -1, dbgMaxCol_ = -1;

    HWND hwnd_ = nullptr;
    HWND tab_ = nullptr;        // WC_TABCONTROL
    HWND overview_ = nullptr;   // Tab 1: 只读 EDIT
    HWND listTests_ = nullptr;  // Tab 2: 测试项 ListView（虚拟）
    HWND listRecs_ = nullptr;   // Tab 3: 记录 ListView（虚拟）
    HWND listLog_ = nullptr;   // Tab 4: datalog 矩阵 ListView（虚拟）
    HWND csvBtn_ = nullptr;    // Tab 4 工具行「另存为 CSV」
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HWND aiBtn_ = nullptr;      // 「AI 分析」（批次 28）
    HFONT font_ = nullptr;
    HINSTANCE inst_ = nullptr;
    int curTab_ = 0;

    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;

    std::wstring path_;
    std::unique_ptr<stdf::StdfFile> sf_;   // nullptr = 未加载

    struct {
        COLORREF bg = RGB(0xFF, 0xFF, 0xFF), fg = RGB(0x20, 0x20, 0x20);
    } colors_;
};

} // namespace xfs
