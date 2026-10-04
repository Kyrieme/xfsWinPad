#pragma once
// OopHost.h — 进程外插件桥（编辑器侧编排器），v2（多插件共享代理）。
//
// 一个 xfsWinPadPluginHost.exe 代理进程可承载多个 NPP 兼容插件（上限
// kPluginsPerProxy）：
//   * 代理启动后发 WM_COPYDATA(OOPM_READY) 报到；本类对 slot0 控制窗发
//     OOPM_ADD(dllPath+cookie) 动态装载；插件 setInfo/getFuncsArray 完成后
//     经 WM_COPYDATA(OOPM_HANDSHAKE) 送回 FuncItem 表（含槽窗 HWND）；
//   * 本类把转录出的命令注册进 PluginManager 统一命令表（grouped=true，
//     快捷键转录与进程内路径一致，id 经 OOPM_CMDIDS 回填代理侧槽窗）；
//   * 命令执行 → 该插件槽窗 OOPM_EXEC（FuncItem 下标寻址）；
//     通知 → 各槽窗 OOPM_NOTIFY——路由与 v1 完全一致；
//   * 装载失败经 OOPM_REJECT 回执（坏 DLL/ANSI/缺导出，代理无恙、不连坐）；
//   * 代理进程死亡（插件 setInfo 里 exit()/卡死自杀/运行期崩）：
//     - 装载归因：Launch 期间死亡记在正被装载的插件头上，同车幸存者
//       撤销已注册命令后逐个重进新代理（每个死亡净淘汰一个嫌疑插件，
//       必然收敛；最坏退化为 v1 的一插件一代理）；
//     - 运行期死亡：同车全部插件记账 oop-died（不自动重启，与 v1 一致）。
//
// v2.1（批次 71）：messageProc 桥——OOPM_MSG 同步广播值类型窗口消息，
// 各槽插件的 LRESULT 经 OOPM_MSGREPLY 回带（首个非零为消费结果）。
// v2 范围：仍仅 NPP 形态；停靠族（NPPM_DMM*）在代理进程内无宿主，由
// DockManager 的跨进程 hClient 守卫拒绝（记日志）。
// ★ 该守卫的理由已按批次 117 的两进程实测改正（见 DockManager.cpp）：
//   "SetParent 跨进程做不到"是**假的**，真理由是**挂起风险**（dock 那三步会向
//   目标线程发消息）+ 通知链（DMN_*/DMM_*）跨进程走不通（`WM_NOTIFY` 被系统
//   拒绝，err=5，与指针无关）⇒ 桥接它必须先有**代理侧中继**。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "OopProtocol.h"

// 全局命名空间类型（xfs_plugin_api.h）；仅指针成员用，无需完整定义。
// 注意必须声明在 :: 作用域 —— 放进 namespace xfs 会派生出同名新类型。
struct xfs_plugin_command;

namespace xfs {

class PluginManager;
struct PluginCommand;
struct PluginLoadFailure;
struct OopPluginInfo {
    std::wstring dllPath;
    std::wstring name;
    int itemCount = 0;
};

class OopHost {
public:
    OopHost() = default;
    ~OopHost();
    OopHost(const OopHost&) = delete;
    OopHost& operator=(const OopHost&) = delete;

    // 把一个插件装进共享代理池并完成握手（UI 线程同步，超时 timeoutMs）。
    // 内部可能因死亡归因产生多轮 装载/重启/重加；最终仅当 dllPath 的
    // 插件存活时返回 true。失败原因进 failures（TakeFailures 由调用方并账）。
    bool Launch(PluginManager& mgr, const std::wstring& dllPath,
                HWND editorWnd, HWND sciMain, unsigned timeoutMs = 15000);

    // 广播一条 NPPN_* 通知给所有存活插件槽（1s SMTO_ABORTIFHUNG 防挂死）。
    void BroadcastNotify(int code, UINT_PTR idFrom);

    // 同步广播一个值类型窗口消息到各存活槽的 messageProc（v2.1）。
    // 返回首个非零 LRESULT（NPP 广播消费语义）；handled（可空）= 是否有
    // 插件返回非零。仅限 wp/lp 都是整数/句柄的消息——指针参数禁止过桥。
    // 内部泵消息等待回包；槽窗送达失败或超时按未处理（0）计。
    LRESULT BroadcastMessage(UINT msg, WPARAM wp, LPARAM lp, bool* handled = nullptr,
                             unsigned timeoutMs = 2000);

    // 关停全部代理：先逐个撤销其命令（命令 impl 引用的 ctx 随代理销毁），
    // 再对每个进程发 OOPM_SHUTDOWN、等 2s、超时强杀、join 看门狗。
    void ShutdownAll();

    // 存活插件清单（诊断/测试断言）。
    std::vector<OopPluginInfo> Alive() const;

    // 存活代理进程数（含零插件的空闲池成员；测试断言进程收敛）。
    int ProcessCount() const;

    // 已处理的 NPPM_* 过桥请求数（v2.2，批次 111）。诊断/测试用：它涨了就
    // 说明中转窗那条路径真的被走过 —— 否则"插件拿到了数据"可能是别的路
    // 给的（本批的负控就是靠它把"过桥"与"没走桥"分开）。
    unsigned NppmRelayed() const { return nppmRelayed_; }

    // NPPM_* 过桥请求里**本侧拒答**的条数（v2.8，批次 117）：只有停靠族会拒
    // （载荷布局不过 / 双串越界）。加它的理由与 SCI 的 refused 计数同理：
    // "本侧拒答"与"宿主真的答了 0"在 result 上同形，而 DMM 的契约失败值恰好
    // 也是 0/FALSE ⇒ 不分开记账就无法判断一次 0 是拒答还是真答案。
    unsigned NppmRefused() const { return nppmRefused_; }

    // 已处理的 SCI_* **入参**过桥请求数（v2.4，批次 113）。与 NppmRelayed 同理：
    // 它涨了才说明"编辑器真的把串转给了 Scintilla"。注意它计数的是**到达**
    // 本函数，不等于成功 —— 成功与否看 SciRefused()（与 result 无关，因为
    // Scintilla 真返回 0 时 result 也是 0，两者同形）。
    unsigned SciRelayed() const { return sciRelayed_; }

    // 本侧拒答数（编号不在入参族 / 长度越界 / 没有目标 Scintilla）。
    unsigned SciRefused() const { return sciRefused_; }

    // 已处理的 SCI_* **出参**过桥请求数（v2.5，批次 114）。与 SciRelayed 同理：
    // 它涨了才说明"编辑器真的替插件向 Scintilla 取了出参"。
    unsigned SciOutRelayed() const { return sciOutRelayed_; }

    // 出参族本侧拒答数（不在出参族 / 拒答桶 / need 越界 / 哨兵校验不过）。
    unsigned SciOutRefused() const { return sciOutRefused_; }

    // 累计回带给插件的字节数（诊断/测试用：负控①下它会明显偏小）。
    unsigned long SciOutBytes() const { return sciOutBytes_; }

    // 已处理的 SCI_* **结构体指针**过桥请求数（v2.7，批次 116）。与 SciRelayed
    // 同理：它涨了才说明"编辑器真的替插件向 Scintilla 取了结构体参数/出缓冲"。
    unsigned SciStructRelayed() const { return sciStructRelayed_; }

    // 结构族本侧拒答数（表外 / 拒答桶 / wire 契约不过 / 容量算不出 / 写后校验不过）。
    unsigned SciStructRefused() const { return sciStructRefused_; }

    // 累计回带给插件的结构族出缓冲字节数（诊断/测试用）。
    unsigned long SciStructBytes() const { return sciStructBytes_; }

    // 成功回写 chrgText 的次数（v2.7）。**只有"真的找到"才会涨** —— 它是
    // "条件回写"这条规则的观测量：FindText 没找到时 Scintilla 一个字节都不写，
    // 这里也不该涨。
    unsigned SciStructChrgWrites() const { return sciStructChrgWrites_; }

    // ---- 停靠族（NPPM_DMM*）中继（v2.8，批次 117）-----------------------------
    // 正向（插件 → 编辑器：注册/显示/隐藏/刷新/切换/按名查句柄）走
    // OOPM_NPPMCALL，在 HandleNppmCall 里按形状处理 —— 计数复用 NppmRelayed()。
    // 下面三个只覆盖**反向**那一半（编辑器 → 插件）。

    // 已登记的"代理承载的插件对话框"个数（DMM 注册成功时登记）。诊断/测试用：
    // 它 > 0 才说明"跨进程 hClient 的信任判据"真的被走到过。
    unsigned DmmClients() const { return static_cast<unsigned>(dmmClients_.size()); }

    // 已**送达**插件对话框的停靠通知（DMN_*）条数（v2.8）。它涨了才说明通知
    // 真的绕过了"跨进程 WM_NOTIFY 被拒"——否则"插件收到了 DMN_DOCK"可能是
    // 别的路给的。
    unsigned DmmNotifies() const { return dmmNotifies_; }

    // 代理**没送到**的通知条数（归属验不过 / 代理已死 / 超时）。与 DmmNotifies
    // 分开记账的理由与 SCI 的 refused 同理：`DMN_CLOSE` 的"未 veto"与"没送到"
    // 在 result 上同形（都是 0）。
    unsigned DmmNotifyRefused() const { return dmmNotifyRefused_; }

    // 已**送达**的 DMM_* 动作请求条数（v2.8）。
    unsigned DmmActions() const { return dmmActions_; }

    // 代理**没送到**的 DMM_* 动作请求条数（v2.8）。
    unsigned DmmActionRefused() const { return dmmActionRefused_; }

    // hClient 是不是**本编辑器的某个代理承载**的插件对话框（DMM 注册时登记过）。
    // DockManager 用它把"跨进程 hClient"分成"受信任的代理承载"与"陌生窗口"：
    // 前者可以 dock（真理由见 DockManager.cpp 的守卫注释），后者仍拒绝。
    bool IsTrustedDmmClient(HWND hClient) const;

    // 请代理在**它自己的进程里**把 WM_NOTIFY{idFrom, code} 发给 hClient。
    // 返回插件对 WM_NOTIFY 的应答（`DMN_CLOSE` 的 **veto** 用它）；0 = 没送出去
    // / 超时 / 未登记（一律按"未 veto"处理）。
    // ★ 必须请求/应答式：`DockManager::AskClose` 靠这个返回值决定是否隐藏面板。
    LRESULT RelayDmmNotify(HWND hClient, UINT_PTR idFrom, int code,
                           unsigned timeoutMs = 2000);

    // 请代理在它自己的进程里给 hClient 发一条 DMM_* 动作请求（wp/lp 恒 0）。
    void RelayDmmAction(HWND hClient, UINT action, unsigned timeoutMs = 2000);

    // 代理死亡/拒绝记录（reason=oop-died 等 + 退出码），Launch/看门狗产生。
    std::vector<PluginLoadFailure> TakeFailures();

private:
    // 每条命令的执行上下文（user 指针）：统一命令表回调 → 槽窗 EXEC。
    struct OopExecCtx {
        OopHost* host;
        HWND     hostWnd;   // 插件槽窗（失效时 SendMessage 失败，无害）
        int      index;     // 代理侧 FuncItem 下标
    };

    // 一个代理进程（可承载多个插件）
    struct Process {
        HANDLE     proc = nullptr;
        HWND       addWnd = nullptr;   // slot0 控制窗（READY 后有效）
        bool       ready = false;
        bool       dead = false;
        std::thread watchdog;          // proc 死亡 → PostMessage(kDeadMsg)
    };

    // 一个插件（v1 的 Proxy 语义：命令表 + 槽窗），挂在 Process 下。
    struct Plugin {
        OopPluginInfo info;
        int    procIdx = -1;
        HWND   hostWnd = nullptr;                              // 槽窗
        std::vector<int> cmdIds;                               // 调试可读
        std::vector<xfs_plugin_command*> cmdHandles;           // 撤销用
        std::vector<std::unique_ptr<OopExecCtx>> ctxs;
        bool dead = false;
    };

    enum class AddResult { kAdded, kRejected, kDied };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    // NPPM_* 过桥（v2.2，批次 111）：按 NppmMarshal.h 的分类在**宿主本地**
    // 准备入/出参缓冲，走既有的 ForwardNppMessage，再把结果与出参经
    // OOPM_NPPMREPLY 送回中转窗。这里看到的每个指针都是宿主自己的。
    void HandleNppmCall(const oop::NppmCallWire& cw, DWORD cbData);
    // SCI_* **入参指针**过桥（v2.4，批次 113）：wire 里内联着插件给的字节，
    // 这里把它们还原成**宿主本地**指针再发给宿主真 Scintilla（sciMain_）。
    // 桥接本体是 oop::SciBridgeCall（独立自由函数，测试可直接调用 —— 否则
    // 夹具就得自己实现一遍编辑器侧，那条断言只能证明"夹具会搬指针"）。
    void HandleSciCall(const oop::SciCallWire& cw, DWORD cbData);
    // SCI_* **出参指针**过桥（v2.5，批次 114）：wire 里没有要搬过来的字节，
    // 只有"插件想要多少"这个问题。桥接本体是 oop::SciBridgeOutCall（同样是
    // 独立自由函数，理由见上）：探长度 → 在**宿主本地**缓冲里真调用 → 哨兵
    // 校验 → 把字节随 OOPM_SCIOUTREPLY 回带。
    void HandleSciOutCall(const oop::SciOutCallWire& cw, DWORD cbData);
    // SCI_* **结构体指针**过桥（v2.7，批次 116）：插件那个结构体已被代理侧
    // **展平**成 cpMin/cpMax（+ FindText 的 needle）。桥接本体是
    // oop::SciBridgeStructCall（同样是独立自由函数，理由见上）：按 full 标志
    // 重建宿主本地结构体 → 真调用 → 写后白名单 → 回带字节 / chrgText。
    void HandleSciStructCall(const oop::SciStructCallWire& cw, DWORD cbData);
    static void ExecTrampoline(void* user);
    void SendExec(HWND hostWnd, int index);
    void StartWatchdog(size_t procIdx);

    bool EnsureSelfWnd();
    size_t PickProcess();                       // 有空位的活代理，无则 -1
    bool SpawnProcess(unsigned deadline, std::wstring& err);  // 新建 + 泵到 READY
    AddResult AddOne(size_t procIdx, const std::wstring& dllPath,
                     unsigned deadline, UINT_PTR& rejectReason);  // 泵到 HANDSHAKE/REJECT/死亡
    void CommitPending(size_t procIdx);         // pendingPlugin_ 挂账
    // 代理死亡后的同车幸存者：撤销命令、标记 dead、回收 dllPath（不含元凶）。
    void CollectSurvivors(size_t procIdx, std::vector<std::wstring>& out);
    void HandleProcessDead(size_t procIdx, DWORD exitCode, bool recordFailures);
    bool PumpOnce(unsigned sliceMs);            // 泵一段时间（握手/等待期）

    static const int kPluginsPerProxy = 8;      // 共享上限（既定取舍的旋钮）

    HWND editorWnd_ = nullptr;
    HWND sciMain_ = nullptr;
    HWND selfWnd_ = nullptr;
    PluginManager* mgr_ = nullptr;
    std::vector<std::unique_ptr<Process>> procs_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<PluginLoadFailure> failures_;
    unsigned nppmRelayed_ = 0;                  // NPPM 过桥计数（v2.2）
    unsigned nppmRefused_ = 0;                  // NPPM 过桥**本侧拒答**数（v2.8）
    unsigned sciRelayed_ = 0;                   // SCI 入参过桥计数（v2.4）
    unsigned sciRefused_ = 0;                   // SCI 入参过桥**本侧拒答**数（v2.4）
    unsigned sciOutRelayed_ = 0;                // SCI 出参过桥计数（v2.5）
    unsigned sciOutRefused_ = 0;                // SCI 出参过桥**本侧拒答**数（v2.5）
    unsigned long sciOutBytes_ = 0;             // 累计回带字节数（v2.5，诊断）
    unsigned sciStructRelayed_ = 0;             // SCI 结构族过桥计数（v2.7）
    unsigned sciStructRefused_ = 0;             // 结构族**本侧拒答**数（v2.7）
    unsigned long sciStructBytes_ = 0;          // 累计回带字节数（v2.7，诊断）
    unsigned sciStructChrgWrites_ = 0;          // 成功回写 chrgText 次数（v2.7）
    // ---- 停靠族（v2.8，批次 117）--------------------------------------------
    // hClient → 承载它的代理的中转窗（= 注册请求的 replyTo）。这张表**就是**
    // "受信任的代理承载窗口"白名单：条目只在 DMM 注册成功时写入，而注册请求
    // 只能来自本编辑器自己启动的代理进程（且代理侧已验过 hClient 是它自己的
    // 窗口）⇒ 表里的 hwnd 一定是"我们的代理进程里的窗口"。
    // DockManager 的跨进程守卫按它放行（真理由见 DockManager.cpp 的守卫注释）。
    std::map<HWND, HWND> dmmClients_;
    unsigned dmmNotifies_ = 0;                  // 送达的 DMN_* 通知数（v2.8）
    unsigned dmmNotifyRefused_ = 0;             // 未送达的 DMN_* 通知数（v2.8）
    unsigned dmmActions_ = 0;                   // 送达的 DMM_* 动作数（v2.8）
    unsigned dmmActionRefused_ = 0;             // 未送达的 DMM_* 动作数（v2.8）

    // ---- DMMRELAY 等待栈（v2.8）---------------------------------------------
    // 与 BroadcastMessage 的 msgWaits_ 同构：编辑器 UI 线程阻塞在
    // SendMessageTimeout 里时，代理的回包经 WM_COPYDATA 投递到本窗并被
    // Handle 处理 ⇒ 这里按 reqId 把结果放进等待槽，发送侧返回后自取。
    // 用 vector 而非单槽：停靠通知可能嵌套（通知处理里又触发一次布局）。
    struct DmmWait {
        UINT_PTR reqId;
        bool got = false;
        bool delivered = false;
        LONG_PTR result = 0;
    };
    UINT_PTR dmmNotifySeq_ = 0;
    std::vector<DmmWait> dmmWaits_;

    // 停靠中继的公共实现（kind 判别；两个公开入口只是它的薄壳）。
    // 返回 true = 代理**送达**了插件对话框（outResult 才是插件的应答）。
    bool RelayDmm(UINT kind, HWND hClient, UINT_PTR idFrom, int code,
                  unsigned action, LONG_PTR& outResult, unsigned timeoutMs);

    // ---- Launch 栈内暂存（同步握手期间由 Handle 填充）----
    UINT_PTR cookieSeq_ = 0;
    UINT_PTR pendingCookie_ = 0;
    std::wstring pendingPath_;                  // 本次 ADD 的 DLL 路径
    std::unique_ptr<Plugin> pendingPlugin_;     // HANDSHAKE 完成
    bool pendingReject_ = false;                // REJECT 完成
    UINT_PTR pendingReason_ = 0;
    bool pendingReady_ = false;                 // READY 完成（新代理）
    HWND pendingReadyWnd_ = nullptr;            // READY 携带的 slot0 窗

    // ---- BroadcastMessage 等待栈（v2.1；嵌套泵时按 reqId 对账，可重入）----
    struct MsgWait {
        UINT_PTR reqId;
        std::vector<LONG_PTR> results;          // 已回收的各槽 LRESULT
    };
    UINT_PTR msgReqSeq_ = 0;
    std::vector<MsgWait> msgWaits_;
};

} // namespace xfs
