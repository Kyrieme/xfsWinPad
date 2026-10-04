#pragma once
// xfsWinPad - DockManager: 把 NPP 插件对话框宿主为底部停靠面板（4d）。
//
// 一个 NPP 插件用 CreateDialog 建好 modeless 对话框后，向宿主发
// NPPM_DMMREGASDCKDLG 注册（DockedWidgetData）。DockManager 为此对话框包一层
// wrapper：顶部是标题条（插件名 + 关闭钮），标题条下方把插件 hClient 重挂进来，
// 由宿主控制尺寸。整个 wrapper 是主窗口的子窗口，参与底部 dock 布局。
//
// 通知方向（与上游惯例一致，插件系统设计笔记 §5.8）：
//   * 用户点关闭 → 宿主向插件对话框发 WM_NOTIFY{code=DMN_CLOSE}；插件可把
//     消息结果置 TRUE 表示 veto（保持打开），否则面板隐藏（对话框不销毁，
//     插件可随时再 Show）。
//   * 注册成功 → DMN_DOCK；显示 → DMN_SWITCHIN。
//   * 宿主关闭（Destroy）→ 向插件对话框发 DMM_CLOSE 动作请求。
//
// 设计约束（批次 122/123）：底部 / 右列 / 顶条 / 浮动 四种容器**全部真实存在**
// ⇒ 注册请求不再被降级（IsDockRequestCoerced 恒假，账本保留供兼容报告口径）。
// 停靠态与浮动态之间可互相转换：拖回宿主边缘 = FloatToDock（批次 126），
// 拖离原槽位 = FloatOut（批次 127）。
// 解码规则与标题口径在 npp/NppDockMask.h（纯函数、有单测）；本文件只负责调用。
// 注意位编码的固有性质：`kDwsDfContLeft == 0` 与"未指定"重合 ⇒ 显式请求左容器
// 在编码上不可表达（按"未指定 = 底部"处理）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "npp/NppDockMask.h"    // 批次 126：npp::DockRequest（FloatToDock 签名）
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
    // 注入进程外插件的停靠通道（v2.8，批次 117）。nullptr = 只支持同进程插件。
    void SetRemoteDock(DockRemote* remote) override { remote_ = remote; }

    // ---- MainWindow::LayoutChildren 集成 ------------------------------------
    // 底部容器：所有可见底部面板的总高度（物理像素，按 dpi 折算）。
    int TotalHeight(int dpi) const;
    // 从 (x, y) 起逐个布置可见底部面板，宽 width；返回最后一个面板下方的 y。
    int Layout(int x, int y, int width, int dpi);
    // ---- 右列 / 顶条（批次 122：真实侧容器）---------------------------------
    // 右列：所有可见右容器面板的列宽（无可见面板 = 0）。
    int TotalWidth(int dpi) const;
    // 从 (x, y) 起向下逐个布置可见右容器面板（列宽 = TotalWidth）；返回底部 y。
    int LayoutRight(int x, int y, int height, int dpi);
    // 顶条：所有可见顶容器面板的总高度（无可见面板 = 0）。
    int TopTotalHeight(int dpi) const;
    // 从 (x, y) 起向下逐个布置可见顶容器面板，宽 width；返回最后一个面板下方 y。
    int LayoutTop(int x, int y, int width, int dpi);

    // 批次 138b：DPI 变化后让每个 wrapper 按新 dpi 重排自己的标题条/图标/
    // 关闭钮。走的是 wrapper 既有的 WM_SIZE 重排路径 —— 尺寸没变时
    // MoveWindow 不会自发 WM_SIZE，而这里要的正是"尺寸没变、dpi 变了"。
    void OnDpiChanged(int dpi);

    size_t PanelCount() const;
    bool Empty() const;

    // 被降级到底部容器的面板数（当前注册集合的口径，**随降级状态变化**增减，
    // 不随注册次数累加）。批次 122 起右/上是真实容器 ⇒ 只有请求浮动的面板计入。
    // 给测试与兼容报告用：它是"插件要的容器 vs 实际给的容器"这处差异的账本。
    int CoercedContainers() const { return coercedContainers_; }

    // MainWindow 注入：面板可见性变化后回调（一般 = LayoutChildren）。
    std::function<void()> onLayoutChanged;

private:
    struct Panel;
    Panel* FindByHwnd(HWND hDlg);
    Panel* FindByName(const wchar_t* name);

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, Panel* p);

    // 批次 126：浮动面板标题条拖拽 + 拖回宿主重停靠
    static LRESULT CALLBACK LabelProc(HWND, UINT, WPARAM, LPARAM);
    void FloatToDock(Panel* p, npp::DockRequest target);
    // 批次 127：停靠面板拖出槽位 ⇒ 转浮动（FloatToDock 的逆操作）
    void FloatOut(Panel* p, POINT cursorScreen);

    void AskClose(Panel* p);          // 用户点关闭 → DMN_CLOSE 协商
    void SetVisible(Panel* p, bool show);
    void NotifyLayout();

    // 把 NMHDR{code} 送给插件对话框，返回插件应答（DMN_CLOSE 的 veto 用）。
    // 同进程 → 直接 SendMessageW；跨进程（代理承载）→ 交 remote_ 由代理转发。
    LRESULT SendNotifyToClient(HWND hClient, UINT_PTR idFrom, int code);
    // 把 DMM_* 动作请求送给插件对话框（wp/lp 恒 0）。
    void SendActionToClient(HWND hClient, UINT action);
    // hClient 是不是"可以接受"的客户端：本进程窗口，或**受信任的代理承载**。
    // 两条判据的差别与理由见 DockWidget 里的守卫注释。
    bool IsAcceptableClient(HWND hClient) const;

    HWND mainWnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    DockRemote* remote_ = nullptr;    // 进程外通道（不拥有；nullptr = 只支持同进程）
    std::vector<std::unique_ptr<Panel>> panels_;
    int coercedContainers_ = 0;       // 见 CoercedContainers()
};

} // namespace xfs
