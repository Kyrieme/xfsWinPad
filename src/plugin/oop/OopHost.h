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
// DockManager 的跨进程 hClient 守卫拒绝（记日志），批次 72 桥接。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
