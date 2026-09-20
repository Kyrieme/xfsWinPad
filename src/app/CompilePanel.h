#pragma once
// xfsWinPad - 底部「编译输出」面板（批次 96，方向 D 第三刀）
//
// 把 CraftRunner 跑出来的结果变成看得见的东西：每一步的命令行、退出码、耗时，
// 以及编译器逐行的输出；带 (文件, 行) 的输出行双击可跳到源码。
//
// 【为什么这个面板必须和「静态检查」面板分开】
//   诊断面板（DiagnosticsPanel）的摘要行里明确写着「非 CRAFT 编译结果」—— 那是
//   **我们自己**从语言手册取证出来的规则。而这个面板显示的是 **CRAFT 编译器自己的
//   结论**。两者混在一起，用户会把我们的静态提示当成编译器判决，反过来也会把
//   编译器的报错当成我们的规则 —— 出了问题都不知道该找谁。所以：独立面板、
//   独立菜单项、独立摘要文案。
//
// 【没有 CRAFT 也必须能用（硬约束）】
//   找不到 plncmp / patcmp 时，本面板显示一段**说明**（探测过哪些来源、当前设置是
//   什么、怎么指定工具目录），**不弹错误框**。编辑器主体功能不许因缺工具链而退化。
//   本机（开发机）实测自动探测必然失败，所以这条不是假想需求。
//
// 【解析不出位置就退化为纯文本（硬约束）】
//   批次 99 起手上**有**失败样本了（六份真机输出，逐字节钉在 test_craftproj.cpp
//   里；批次 97 之前那句"没有失败样本可验证"已经不成立）。样本证明 CRAFT 用了
//   至少三套消息语法，而且同一条消息里位置与级别可能分居两行。所以
//   ParseCompilerOutput 仍然刻意保守：**认得出才认，认不出就当普通文本**。
//   结果是：每一行都会变成一行可见文本（`header` 为假的普通行），认不出位置的
//   行**照样显示、只是不能双击跳转** —— 绝不丢行。面板里任何一行都不做"猜测性归属"。
//
// 【双击跳转的端到端验证】
//   上面这些是"行怎么来的"。行**到了面板之后**双击能不能真的跳到源码，靠
//   scripts/craft-e2e.ps1 验证：它用桩工具链（tests/craft_stub.cpp）让 GUI 走完
//   真实编译路径，再逐行双击核对落点。改 JumpToCompileLocation 之后跑它。
//
// 【与 DiagnosticsPanel / ResultsPanel 的关系】
//   交互（顶部 6px 拖拽条、关闭按钮、双击激活行）刻意与它们一致，用户不必学两套。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace xfs {

// 一行编译输出。`header == true` 表示这是**我们自己插入的步骤分隔行**
// （"1/4 plncmp …"），不是编译器的输出，不可跳转。
struct CompileRow {
    bool         header  = false;
    std::wstring step;              // 步骤标签，如 "1/4"（只在 header 行填）
    std::wstring file;              // 编译器给出的源文件（空 = 无法归属）
    int          line    = 0;       // 0 = 未知
    int          column  = 0;       // 0 = 未知
    int          kind    = 0;       // 0 普通 / 1 警告 / 2 错误
    std::wstring text;              // 该行原文（未加工）

    // 能不能双击跳转：有文件**且有行号**才行。刻意不接受"只有行号没有文件"
    // —— 那会跳到当前文档里一个毫无关系的行上，比不跳更糟。
    bool Jumpable() const { return !header && !file.empty() && line > 0; }
};

class CompilePanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ != nullptr && ::IsWindowVisible(hwnd_); }
    void Show();
    void Hide();
    void Layout(int w, int h);   // 与其它底部面板同签名（内部按客户区重排）

    // summary 显示在左上角（含「CRAFT 编译输出」字样，见文件头）
    void Update(const std::wstring& summary, std::vector<CompileRow> rows);
    void ShowBusy(const std::wstring& text);   // 清列表 + 只换摘要（编译进行中）
    void Clear();          // 清空并按 Retranslate 的规则重置标题
    void Retranslate();    // 语言切换后重刷文本

    int RowCount() const { return (int)rows_.size(); }
    const CompileRow* RowAt(int i) const {
        return (i >= 0 && i < (int)rows_.size()) ? &rows_[i] : nullptr;
    }

    // 双击/回车某一行：参数是 rows 的下标（调用方负责校验范围与 Jumpable）
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
    std::vector<CompileRow> rows_;
    bool hasRun_ = false;   // 是否已经跑过一次编译（决定标题是"未编译"还是结果）

    // 顶部拖拽条状态（屏幕坐标：拖拽时面板跟着光标移动，客户端相对增量会自相抵消）
    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenY_ = 0;
    int dragStartH_ = 0;
};

} // namespace xfs
