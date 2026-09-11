#pragma once
// OopHost.h — 进程外插件桥（编辑器侧编排器），v1。
//
// 为 NPP 兼容插件启动 xfsWinPadPluginHost.exe 代理进程：
//   * 代理加载 DLL 完成 setInfo/getFuncsArray，FuncItem 表经
//     WM_COPYDATA(OOPM_HANDSHAKE) 送来；
//   * 本类把转录出的命令注册进 PluginManager 统一命令表（grouped=true，
//     快捷键转录与进程内路径一致，id 经 OOPM_CMDIDS 回填代理侧）；
//   * 命令执行 → OOPM_EXEC（FuncItem 下标寻址）；通知 → OOPM_NOTIFY；
//   * 插件 exit()/崩溃只死代理进程：看门狗线程观察到退出码后记账
//     （reason=oop-died，进不兼容页），编辑器无感。
//
// v1 范围：仅 NPP 形态；停靠族（NPPM_DMM*）在代理进程内无宿主，由
// DockManager 的跨进程 hClient 守卫拒绝（记日志），v2 桥接。

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

    // 启动一个插件代理进程并完成握手（UI 线程同步，超时 timeoutMs）。
    // 成功 = FuncItem 表已注册进 mgr、命令 id 已回填。失败返回 false
    // （原因进 failures，TakeFailures 由调用方并账）。
    bool Launch(PluginManager& mgr, const std::wstring& dllPath,
                HWND editorWnd, HWND sciMain, unsigned timeoutMs = 15000);

    // 广播一条 NPPN_* 通知给所有存活代理（1s SMTO_ABORTIFHUNG 防挂死）。
    void BroadcastNotify(int code, UINT_PTR idFrom);

    // 关停全部代理：先逐个撤销其命令（命令 impl 引用的 ctx 随代理销毁），
    // 再发 OOPM_SHUTDOWN、等 2s、超时强杀、join 看门狗。
    void ShutdownAll();

    // 存活代理清单（诊断/测试断言）。
    std::vector<OopPluginInfo> Alive() const;

    // 代理死亡记录（reason=oop-died + 退出码），Launch/看门狗路径产生。
    std::vector<PluginLoadFailure> TakeFailures();

private:
    // 每条命令的执行上下文（user 指针）：统一命令表回调 → 代理 EXEC。
    struct OopExecCtx {
        OopHost* host;
        HWND     hostWnd;   // 代理消息窗口（失效时 SendMessage 失败，无害）
        int      index;     // 代理侧 FuncItem 下标
    };

    struct Proxy {
        OopPluginInfo info;
        HWND   hostWnd = nullptr;
        HANDLE proc = nullptr;
        std::vector<int> cmdIds;                                  // 调试可读
        std::vector<xfs_plugin_command*> cmdHandles;              // 撤销用
        std::vector<std::unique_ptr<OopExecCtx>> ctxs;
        std::thread watchdog;
        bool dead = false;
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Handle(size_t index, DWORD exitCode);   // kDeadMsg（看门狗→UI 线程）
    static void ExecTrampoline(void* user);
    void SendExec(HWND hostWnd, int index);
    void StartWatchdog(Proxy& p);
    bool PumpOnce(HANDLE proc, unsigned sliceMs);   // 泵一次消息（握手期）

    HWND editorWnd_ = nullptr;
    HWND selfWnd_ = nullptr;
    PluginManager* mgr_ = nullptr;
    std::vector<std::unique_ptr<Proxy>> proxies_;
    std::unique_ptr<Proxy> pendingProxy_;          // 握手暂存（Launch 栈内）
    std::wstring pendingDllPath_;
    std::vector<PluginLoadFailure> failures_;
    UINT kOopDiedMsg = 0;                          // WM_APP+x：看门狗→UI 线程
};

} // namespace xfs
