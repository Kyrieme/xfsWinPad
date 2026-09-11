#pragma once
// xfsWinPad - AiPanel: 右侧停靠 opencode 对话面板（AI 菜单 > 打开/关闭 opencode）。
//
// 架构（v1）：
//   * OcClient 走 HTTP 与 `opencode serve`（127.0.0.1:4096）通信；
//     serve 由 WMI 脱离启动，不随编辑器退出（EnsureServer 幂等）。
//   * transcript = 只读 Scintilla（同终端面板底座），append-only：
//     「你: 」「AI: 」段 + reasoning 灰显 + 「…思考中」占位。
//   * 流式刷新 = WM_TIMER(250ms) 轮询 /session/:id/message，直到
//     assistant.finish 非空；发送期间输入框置灰、Abort 可用。
//   * 布局：顶部 label+close（splitter 拖宽沿用左右方向），中部 transcript，
//     底部输入 EDIT（Enter 发送 / Shift+Enter 换行 / Esc 焦点回编辑器）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Scintilla.h>
#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "OcClient.h"
#include "OaiClient.h"

namespace xfs {

struct ThemeDef;

// 发送消息时附带的编辑器上下文（MainWindow::BuildAiContext 产出）。
// block = 发给模型的上文块（空 = 无活跃文档，不附带）；
// detail = transcript 回显用的简述（文件名 · 行/选中）。
struct AiContext {
    std::wstring block;
    std::wstring detail;
};

class AiPanel {
public:
    bool Create(HWND parent, HINSTANCE hInst);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && ::IsWindowVisible(hwnd_); }
    bool HasFocus() const;
    void Hide();

    std::function<void()> onClose;
    // 拖宽回调（px = 面板新宽度，屏幕像素；父窗口负责重排）
    std::function<void(int)> onWidthChange;
    // 用户切换模型（下拉；MainWindow 持久化 settings.aiModel）
    std::function<void(const std::wstring&, const std::wstring&)> onModelChanged;
    // openai 模式下用户切换本地模型（MainWindow 持久化 settings.aiLocalModel）
    std::function<void(const std::wstring&)> onOaiModelChanged;
    // 发送前/响应完成（MainWindow 用于磁盘快照对比 → 自动重载 AI 改动的文件）。
    // onResponseDone 携带本轮 AI 写类工具（write/edit/patch）的目标路径集合，
    // 供 MainWindow 提示「改动了未打开的文件」。
    std::function<void()> onBeforeSend;
    std::function<void(const std::vector<std::wstring>&)> onResponseDone;

    // AI 插件工场：用户在输入框发 /plugin <功能描述> 时触发。
    //AiPanel 只识别前缀并把手（任务描述）交给宿主——脚手架/提示词/安装
    //编排都在 MainWindow（它能访问 Workshop + PluginManager）。
    std::function<void(const std::wstring& taskDesc)> onPluginTask;

    // 上下文注入：发送时由 MainWindow 提供活跃文档快照（路径/光标/选区）。
    // attachContext_ 开且 block 与上次不同才附带（去重，省 token）。
    void SetContextProvider(std::function<AiContext()> p) {
        contextProvider_ = std::move(p);
    }
    void SetAttachContext(bool on) {
        attachContext_ = on;
        lastCtxSent_.clear();   // 切开关后让下一条消息立刻按新状态决定
    }
    // 权限请求自动放行开关（false = 不代答；serve 无 UI，任务会挂起等待，
    // 用户需在 opencode 终端/网页里手动放行——安全敏感场景用）
    void SetAutoApprove(bool on) { autoApprove_ = on; }

    // 后端选择（批次 13：本地/第三方直连）：
    //   "opencode"（默认）= agent 全功能（工具/diff/工场/权限）
    //   "openai"          = OpenAI-compatible 端点纯对话（Ollama /v1、
    //                       DeepSeek、Qwen…），离线可用，无工具调用
    void SetBackend(const std::wstring& backend, const std::wstring& endpoint,
                    const std::wstring& apiKey, const std::wstring& model) {
        if (backend_ != backend) {   // 切换后端 → 丢弃旧会话状态
            sessionId_.clear();
            serverOk_ = false;
            renderedMsgs_ = 0;
            streamingMsgIdx_ = -1;
            streamChars_ = 0;
            shownTools_.clear();
        }
        backend_ = backend;
        oaiEndpoint_ = endpoint;
        oaiApiKey_ = apiKey;
        oaiModel_ = model;
    }

    // 模型选择：连接线程按 preferred（settings 恢复）→ 服务端默认 择优；
    // 用户在下拉里换模型触发 onModelChanged（MainWindow 持久化）。
    void SetPreferredModel(const std::wstring& provider, const std::wstring& model) {
        prefProv_ = provider;
        prefModel_ = model;
    }
    // transcript 追加一条系统灰字（自动重载等提示）
    void SystemLine(const std::wstring& text);
    // transcript 追加 unified diff 文本（专用样式：+绿/-红，等宽字体）
    void DiffBlock(const std::wstring& diff);

    void ApplyTheme(const ThemeDef& t);
    void Retranslate();
    void Layout(int w, int h);

    // 打开面板时调用：确保 serve 在线 + 建/复用会话 + 恢复历史。
    // workDir 作为 opencode 的项目根（当前编辑器工作区）。
    bool EnsureReady(const std::wstring& workDir);

    // 新会话：异步建 session 成功后清空 transcript 重新开始。
    void NewSession();

    // 发送一行用户输入（UI 线程）。busy=true 时忽略。
    void SendUserText(const std::wstring& text);

    // 外部入口（批次 28）：宿主模块（StdfPanel）直接把「指令 + 数据块」
    // 发给 AI，绕过输入框。busy=true 或未连接时排队 extCtx_，连接完成/
    // 回复完成后 flush。label 仅回显用；body 是发给模型的完整内容。
    void SendExternal(const std::wstring& label, const std::wstring& body);

    // 当前是否在等模型回复（发送按钮/Abort 状态）
    bool Busy() const { return busy_.load(); }

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProcThunk(HWND, UINT, WPARAM, LPARAM,
                                           UINT_PTR, DWORD_PTR);
    LRESULT InputProc(HWND, UINT, WPARAM, LPARAM);

    // ---- 后台任务（std::thread；结果 PostMessage 回 UI）---------------------
    // EnsureServer（探活/启动可能 ~12s）+ CreateSession，threadStarted_ 防重入
    void StartConnectThread(const std::wstring& workDir);
    // 单次消息轮询（快，<100ms，直接在 timer 里发同步请求）
    void PollMessages();
    // openai 模式：SSE 流式 Chat（工作线程），delta 即时 kPmsgOaiDelta
    // 回 UI 逐字上屏，收尾 kPmsgOaiReply 落历史
    void StartOaiChat(std::vector<ai::OaiMsg> msgs);

    // ---- transcript 渲染 -----------------------------------------------------
    void AppendLine(const std::wstring& text, int styleBase);
    void AppendTextStyled(const std::wstring& text, int styleBase);
    void RenderMessages(const std::vector<ai::OcMessage>& msgs, bool finalPass);
    void UpdateLabel();
    // 会话累计 cost/tokens（批次 27）：GET /session/:id → label 追加段。
    // 挂点：历史切换 + 生成完成（PollMessages 每轮拉太贵）。失败静默。
    void RefreshSessionTokens();
    void UpdateBusyUi();
    // 会话历史：拉 /session → UI 弹列表 → 选中切换（同步加载该会话历史）
    void ShowHistoryPicker();

    sptr_t Send(HWND h, UINT m, uptr_t wp = 0, LPARAM lp = 0) {
        return ::SendMessageW(h, m, (WPARAM)wp, lp);
    }

    HWND hwnd_ = nullptr;
    HWND sci_ = nullptr;       // transcript（只读）
    HWND input_ = nullptr;     // 多行输入（ES_MULTILINE）
    HWND sendBtn_ = nullptr;
    HWND label_ = nullptr;
    HWND closeBtn_ = nullptr;
    HWND histBtn_ = nullptr;
    HWND modelCombo_ = nullptr;  // 模型选择（连接后填充 models_）
    HFONT font_ = nullptr;
    HFONT mono_ = nullptr;
    HINSTANCE inst_ = nullptr;

    // opencode 状态
    std::wstring workDir_;
    std::wstring sessionId_;
    std::wstring serverVersion_;
    std::wstring modelProvider_;   // 发送时带的 model（连接时按 preferred →
    std::wstring modelName_;       //  服务端默认择优；空 = 不带 model 走默认）
    std::vector<ai::OcModelEntry> models_;   // /config/providers 目录（连接后）
    std::wstring prefProv_, prefModel_;      // settings.aiModel 恢复的偏好

    // 会话累计 cost/tokens（批次 27，RefreshSessionTokens 刷新）
    double sessCost_ = 0;          // 美元
    long long sessTokIn_ = 0, sessTokOut_ = 0, sessTokReason_ = 0,
              sessTokCacheRead_ = 0;

    // openai 直连状态（backend_ == "openai" 时生效）
    std::wstring backend_ = L"opencode";
    std::wstring oaiEndpoint_, oaiApiKey_, oaiModel_;
    std::vector<ai::OaiMsg> oaiHistory_;   // 本地对话记忆（内存，新会话清空）
    std::atomic<bool> busy_{false};
    // openai Abort 旗标：堆上 atomic（shared_ptr），UI 置位 + 工作线程
    // ChatStream 轮询共用；面板销毁后线程仍安全读（atomic 独立存活）。
    std::shared_ptr<std::atomic<bool>> oaiAbort_{
        std::make_shared<std::atomic<bool>>(false)};
    DWORD busyStart_ = 0;        // 本轮生成开始时刻（看门狗用）
    bool busyStalled_ = false;   // 本轮已无进展（看门狗提示后终止）
    std::wstring lastProgressSig_;  // 进展指纹（消息数:末条 parts 数）
    std::set<std::wstring> shownMsgErrs_;   // 已上屏的消息级 error（去重）
    std::atomic<bool> connecting_{false};
    bool serverOk_ = false;
    bool notInstalledShown_ = false;
    bool poisonAuthShown_ = false;

    // 上下文注入
    std::function<AiContext()> contextProvider_;   // MainWindow 供活跃文档快照
    bool attachContext_ = true;
    std::wstring lastCtxSent_;   // 上一条附带的上文块（去重）；新会话清空
    bool autoApprove_ = true;    // 权限请求自动放行（settings.aiAutoApprove）
    bool permNoticeShown_ = false;   // 「等待权限确认」提示一次标记

    // 外部请求（批次 28 StdfPanel「AI 分析」）：body 非空 = 待发送。
    // 发送时 body 作为本轮上下文直接附加（优先于文档快照），label 回显。
    std::wstring extLabel_, extBody_;
    void TryFlushExternalAsk();   // 就绪时补发排队的外部请求

    // 渲染状态：轮询时只 append 增量（上次渲染完成的消息数 + 流内偏移）
    int renderedMsgs_ = 0;      // 已完整渲染的消息数
    int streamingMsgIdx_ = -1;  // 正在流式渲染的消息下标
    int streamChars_ = 0;       // 流式消息已渲染的 UTF-16 字符数
    DWORD lastPermCheck_ = 0;   // permission 自动放行检查节流（2s）
    bool pendingReplay_ = false;  // 连接成功后首轮拉取 → 全量重绘（含历史 user 消息）
    // tool 调用上屏去重（callId:state → 已显示），防每 250ms 轮询重复打印
    std::set<std::wstring> shownTools_;

    // splitter（左缘拖宽）
    bool resizing_ = false;
    bool splitHot_ = false;
    int dragStartScreenX_ = 0;
    int dragStartW_ = 0;

    static constexpr UINT_PTR kPollTimer = 9;
    std::vector<int> msgFinishCache_;   // 上次轮询各消息 finish 是否已有值

    constexpr static UINT kPmsgNewSession = WM_APP + 73;   // lp = new std::wstring(sid)
    constexpr static UINT kPmsgOaiReply = WM_APP + 74;     // lp = new OaiReply
    constexpr static UINT kPmsgOaiConnected = WM_APP + 75; // wp=1/0, lp = new vector<wstring>
    constexpr static UINT kPmsgPromptFail = WM_APP + 76;   // lp = new OaiReply(err)
    constexpr static UINT kPmsgOaiDelta = WM_APP + 77;     // lp = new std::wstring(delta)
    constexpr static UINT kPmsgOcEvent = WM_APP + 78;      // lp = new OcEvent{type,sid}
    struct OaiReply { std::wstring text, err; };
    // openai 流式状态（仅 UI 线程访问）：
    bool oaiStreaming_ = false;    // 本轮已开始 delta 上屏（AI: 标签已打）
    // opencode /event SSE 订阅线程（批次 26）：事件到达 → PostMessage
    // kPmsgOcEvent → UI 立即 PollMessages()。250ms timer 保留为兜底。
    // 生命周期：连接成功后启动一次；面板销毁只置取消旗标 + detach
    //（线程阻塞在 ReadData ≤10s 超时自退，join 会卡退出；shared_ptr
    // 保证旗标存活到线程退出，PostMessage 对已死 hwnd 静默失败+堆包自删）。
    std::thread evtThread_;
    std::shared_ptr<std::atomic<bool>> evtCancel_{
        std::make_shared<std::atomic<bool>>(false)};
    bool evtThreadUp_ = false;
    struct OcEvent { std::wstring type, sid; };
    void StartEventThread();
    // 连接线程 → UI 的结果包（堆分配，UI 消费后 delete）
    struct ConnectResult {
        bool ok = false;
        bool notInstalled = false;
        std::wstring sid, prov, model;
        std::vector<ai::OcModelEntry> models;   // 连接成功时填充下拉
    };

    struct {
        COLORREF bg = RGB(0xFF, 0xFF, 0xFF), fg = RGB(0x20, 0x20, 0x20);
        COLORREF userTag = RGB(0x00, 0x55, 0xCC);
        COLORREF aiTag = RGB(0x00, 0x77, 0x33);
        COLORREF dim = RGB(0x88, 0x88, 0x88);
        // diff 着色（ApplyTheme 从主题语法色板取）
        COLORREF diffAdd = RGB(0x0A, 0x7D, 0x22);
        COLORREF diffDel = RGB(0xB0, 0x1F, 0x1F);
    } colors_;
};

} // namespace xfs
