#pragma once
// xfsWinPad - CsvPanel: CSV 表格视图底部停靠面板（批次 32/33）。
//
// - 仿 StdfPanel/HexPanel 的底部分割条/关闭按钮范式；
// - 虚拟 ListView（LVS_OWNERDATA）承接任意行数；列 = CSV 表头行；
// - 列头点击排序（数值感知，▲/▼ 指示），过滤框实时筛选（250ms 防抖）；
// - 数据源 = 当前文档同路径文件重新读取解码（不碰编辑器缓冲区）；
// - 批次 33：双击单元格就地编辑（EDIT 覆盖框）+ 保存回写（RFC 4180 引号
//   保真、编码/行尾/尾换行还原、外部修改冲突检测），保存后宿主静默重载文档。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../csv/CsvBigFilter.h"
#include "../csv/CsvParser.h"
#include "../encoding/Encoding.h"

namespace xfs {

struct ThemeDef;
namespace csv { class CsvBigModel; }

class CsvPanel {
public:
    CsvPanel();                        // 完整类型要求（big_ unique_ptr）
    bool Create(HWND parent, HINSTANCE hInst);
    ~CsvPanel();                       // 完整类型要求（big_ unique_ptr）
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    bool HasFocus() const;

    // 读取并解析一个 CSV/TSV 文件；成功后显示面板。失败返回 false（宿主弹窗）。
    // >64MB 走大文件只读虚拟模式（批次 36，LoadBig）。
    bool Load(const std::wstring& filePath);
    void Hide();

    const std::wstring& FilePath() const { return path_; }
    bool BigMode() const { return big_ != nullptr; }

    std::function<void()> onClose;
    std::function<void(int)> onHeightChange;
    // 保存回写成功后通知宿主（同步重载编辑器中的同路径文档）
    std::function<void(const std::wstring&)> onSaved;

    void ApplyTheme(const ThemeDef& t);
    void Retranslate();
    void Layout(int w, int h);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void BuildColumns();               // 表头 → ListView 列（含宽度启发）
    void RebuildView();                // 过滤 + 排序 → viewRows_ → 行数刷新
    void UpdateSortArrows();
    std::wstring CellText(int row, int col) const;
    void UpdateCountLabel();
    void UpdateSaveState();

    // 单元格就地编辑（批次 33）
    void BeginCellEdit(int viewRow, int col);
    void CommitCellEdit(bool commit);
    static LRESULT CALLBACK EditProcThunk(HWND, UINT, WPARAM, LPARAM,
                                          UINT_PTR, DWORD_PTR);
    LRESULT EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    // 行操作（批次 34）：右键菜单 + Ins/Del 键（列表子类化）
    void AddRowAt();
    void DeleteSelectedRow();
    void ShowRowMenu(int xScreen, int yScreen);

    // 批次 43：跳转显示行（右键菜单/Ctrl+G + InputBox）、导出选中行
    // （RFC 4180 引号保真，GetSaveFileNameW 选路径，UTF-8 无 BOM）
    void GoToRow();
    void ExportSelected();
    void PrintTable();
    static LRESULT CALLBACK ListProcThunk(HWND, UINT, WPARAM, LPARAM,
                                          UINT_PTR, DWORD_PTR);
    LRESULT ListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    // 列操作（批次 35）：插入列（选中列之前，无选中=末尾）/ 删除选中列
    void AddColAt();
    void DeleteColAt();
    void RebuildColumns();

    // 保存回写（序列化保真 + 冲突检测）
    bool DoSave();

    // 大文件只读模式（批次 36）：mmap 行索引 + 按需物化，无编辑/排序/保存
    bool LoadBig(const std::wstring& filePath);

    // 大文件后台过滤（批次 37）：防抖到期启动 / 计时器轮询刷新
    void StartBigFilter();
    void PollBigFilter();

    HWND hwnd_ = nullptr;
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HWND saveBtn_ = nullptr;
    HWND filterEdit_ = nullptr;
    HWND countLabel_ = nullptr;
    HWND list_ = nullptr;
    HWND cellEdit_ = nullptr;          // 单元格就地编辑框
    HFONT font_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HBRUSH bgBrush_ = nullptr;   // 主题底色画刷（ApplyTheme 重建，防泄漏）

    std::wstring path_;
    std::unique_ptr<csv::CsvData> data_;
    std::unique_ptr<csv::CsvBigModel> big_;   // 大文件模式（data_ 为空）
    csv::CsvBigFilter bigFilter_;             // 大文件后台过滤（批次 37）
    std::vector<uint32_t> viewRows_;
    int cols_ = 0;
    int sortCol_ = -1;
    bool sortDesc_ = false;
    UINT_PTR filterTimer_ = 0;

    // 编辑态：editViewRow_ = 显示行号，数据原始行号 = viewRows_[editViewRow_]
    int editViewRow_ = -1;
    int editCol_ = -1;
    int lastSubItem_ = -1;             // 最近点击的列（列操作锚点，批次 35）

    // 保存回写状态
    bool dirty_ = false;
    encoding::EncodingType enc_ = encoding::EncodingType::UTF8;
    std::string newline_ = "\r\n";
    bool trailingNewline_ = true;
    FILETIME loadTime_{};              // 外部修改冲突检测基线
    long long loadSize_ = 0;

    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;

    struct {
        COLORREF bg = RGB(0xFF, 0xFF, 0xFF), fg = RGB(0x20, 0x20, 0x20);
    } colors_;
};

} // namespace xfs
