#pragma once
// xfsWinPad - DockManager: 把 NPP 插件对话框宿主为底部停靠面板（4d）。
//
// 一个 NPP 插件用 CreateDialog 建好 modeless 对话框后，向宿主发
// NPPM_DMMREGASDCKDLG 注册（DockedWidgetData）。DockManager 为此对话框包一层
// wrapper：顶部是标题条（插件名 + 关闭钮），标题条下方把插件 hClient 重挂进来，
// 由宿主控制尺寸。整个 wrapper 是主窗口的子窗口，参与底部 dock 布局。
//
// 通知方向（与上游惯例一致，docs/plugin-system.md §5.8）：
//   * 用户点关闭 → 宿主向插件对话框发 WM_NOTIFY{code=DMN_CLOSE}；插件可把
//     消息结果置 TRUE 表示 veto（保持打开），否则面板隐藏（对话框不销毁，
//     插件可随时再 Show）。
//   * 注册成功 → DMN_DOCK；显示 → DMN_SWITCHIN。
//   * 宿主关闭（Destroy）→ 向插件对话框发 DMM_CLOSE 动作请求。
//
// 设计约束：只实现底部容器（与现有 LogPanel/TerminalPanel 同体系）；
// DWS_DF_FLOATING/左/右/上 一律按底部处理并记日志（兼容报告口径）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../plugin/PluginManager.h"

namespace xfs {

class DockManager : public DockHost {
public:
    // mainWnd = 主框架窗口（dock 面板的父窗口），inst = HINSTANCE。
    // 幂等：重复 Init 无害。
    void Init(HWND mainWnd, HINSTANCE inst);
    // 退出前：向每个插件对话框发 DMM_CLOSE，然后销毁全部 wrapper。
    void Destroy();
    // 构造/析构在 .cpp 定义：Panel 是不完整类型，vector 的展开需完整定义。
    DockManager();
    ~DockManager();

    // ---- DockHost（仅 UI 线程调用）------------------------------------------
    bool DockWidget(const npp::DockedWidgetData& data) override;
    bool Show(HWND hDlg) override;
    bool Hide(HWND hDlg) override;
    void UpdateDisplayInfo(HWND hDlg) override;
    bool ShowByName(const wchar_t* name) override;
    HWND FindHwndByName(const wchar_t* windowName,
                        const wchar_t* moduleName) override;

    // ---- MainWindow::LayoutChildren 集成 ------------------------------------
    // 所有可见面板的总高度（物理像素，按 dpi 折算）。
    int TotalHeight(int dpi) const;
    // 从 (x, y) 起逐个布置可见面板，宽 width；返回最后一个面板下方的 y。
    int Layout(int x, int y, int width, int dpi);

    size_t PanelCount() const;
    bool Empty() const;

    // MainWindow 注入：面板可见性变化后回调（一般 = LayoutChildren）。
    std::function<void()> onLayoutChanged;

private:
    struct Panel;
    Panel* FindByHwnd(HWND hDlg);
    Panel* FindByName(const wchar_t* name);

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, Panel* p);

    void AskClose(Panel* p);          // 用户点关闭 → DMN_CLOSE 协商
    void SetVisible(Panel* p, bool show);
    void NotifyLayout();

    HWND mainWnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    std::vector<std::unique_ptr<Panel>> panels_;
};

} // namespace xfs
