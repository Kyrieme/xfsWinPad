#pragma once
// xfsWinPad - 底部「静态检查」面板（批次 87）。把校验内核
// (src/language/Chroma3380Diagnostics.{h,cpp}) 的诊断列表变成看得见的东西：
// 级别 / 行 / 列 / 规则码 / 说明，双击跳转到编辑器里的那一行并选中被判错的片段。
//
// 【为什么这里要重申「非 CRAFT 编译结果」】
//   Chroma 没有公开错误码表（Operation Manual 全文 `error` 只出现 14 次），
//   本项目的每条规则都是**从语言手册正文逐条取证**得到、再放在手册自己的示例
//   与真实工程文件上回归过的。
//   它和 CRAFT 的编译输出不是一回事 —— 摘要行里必须写清楚，否则用户会把我们的
//   静态提示当成编译器结论，进而怀疑工具链。
//
// 【为什么"没有发现问题"也要显示一行】
//   面板空着不说话时，用户分不清"检查过了，没问题"和"根本没跑"。所以空结果
//   显示一句明确的话（本地化键 panel.diag.ok），只在面板被关闭/切换用途时才隐藏。
//
// 【与 ResultsPanel 的关系】
//   交互（顶部 6px 拖拽条、关闭按钮、双击激活行）刻意与搜索结果面板一致，
//   用户不必学两套。区别只有两处：列不同：以及空结果仍然可见（见上）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace xfs {

struct DiagItem {
    int          line    = 1;   // 1-based，给人看的
    int          column  = 1;   // 1-based（按字节列）
    int          start   = 0;   // 0-based 字节列（跳转选中用）
    int          length  = 0;   // 覆盖字节数
    bool         error   = true;
    std::wstring code;          // 稳定标识，如 C3380-PLN-010
    std::wstring message;       // 中文短文案
};

class DiagnosticsPanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ != nullptr && ::IsWindowVisible(hwnd_); }
    void Show();
    void Hide();
    void Layout(int w, int h);   // 与其它底部面板同签名（内部按客户区重排）

    // summary 显示在左上角（含「非 CRAFT 编译结果」声明）；items 为空也照常显示，
    // 换成"未发现确定的问题"（见文件头说明）。
    void Update(const std::wstring& summary, std::vector<DiagItem> items);
    void Clear();          // 清空并按 Retranslate 的规则重置标题
    void Retranslate();    // 语言切换后重刷文本

    // 双击回调用它取"是哪一条"（下标越界返回 nullptr）
    int ItemCount() const { return (int)items_.size(); }
    const DiagItem* ItemAt(int i) const {
        return (i >= 0 && i < (int)items_.size()) ? &items_[i] : nullptr;
    }

    // 双击/回车某一行：参数是 items 的下标（调用方负责校验范围）
    std::function<void(int)> onActivateRow;
    std::function<void()>    onClose;
    std::function<void(int)> onHeightChange;   // 拖顶部条；参数 = 期望高度 px

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void LayoutChildren();

    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<DiagItem> items_;
    bool hasRun_ = false;   // 是否已经跑过一次检查（决定标题是"未发现"还是"待检查"）

    // 顶部拖拽条状态（屏幕坐标：拖拽时面板跟着光标移动，客户端相对增量会自相抵消）
    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;
};

} // namespace xfs
