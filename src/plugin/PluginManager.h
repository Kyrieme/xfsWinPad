#pragma once
// xfsWinPad - PluginManager: loads plugins (.dll) from a fenced install dir and
// publishes their commands to the app.
//
// Design:
//   * Plugins live in %APPDATA%\xfsWinPad\plugins\*.dll. The host loads each
//     DLL, verifies its reported C ABI version, then calls registerPlugin().
//   * Each command a plugin adds is given a host-allocated command id in a high
//     range (>= PluginCmdFirst) so it never collides with built-in Cmd:: ids.
//   * The app asks the manager for the full list of plugin commands to populate
//     the Command Palette and a Plugins menu, and dispatches execution by id.
//   * Lifecycle: LoadAll() at startup, UnloadAll() at shutdown (calls
//     unregisterPlugin + FreeLibrary).

#include "../plugin/xfs_plugin_api.h"
#include "../plugin/npp/NppCompat.h"
#include "../plugin/npp/NppMessages.h"
#include "../plugin/npp/NppDocking.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <deque>
#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace xfs {

// 插件形态：原生 ABI 或 Notepad++ 兼容形态（docs/plugin-system.md §5）。
enum LoadedKind { kLoadedNative = 0, kLoadedNppCompat = 1 };

// 一次 LoadAll 中加载失败的 DLL 记录（插件管理「不兼容」页数据源）。
// reason 为稳定令牌：arch(架构不匹配)/load(无法加载)/abi(原生 ABI 不符)/
// export(缺导出)/ansi(ANSI NPP 插件被拒)——UI 层按令牌选翻译文案。
struct PluginLoadFailure {
    std::wstring path;
    std::wstring reason;
};

class Document;

// 进程外插件桥（oop/OopHost.{h,cpp}）：xfsWinPadPluginHost.exe 代理编排器。
class OopHost;

// NPP 消息垫片所需的文档集抽象（docs/plugin-system.md §5.6 测试方案）。
// 生产实现绑定 Workspace；单测注入假实现以脱离真实控件断言契约。
// 非拥有指针，虚拟析构。
struct NppDocSource {
    virtual ~NppDocSource() = default;
    virtual int DocCount() = 0;
    virtual Document* DocAt(int index) = 0;          // 越界返回 nullptr
    virtual Document* Current() = 0;                 // 无活动返回 nullptr
    virtual int CurrentIndex() = 0;                  // 无活动返回 -1
    virtual bool OpenNew(const std::wstring& path) = 0;        // NPPM_DOOPEN
    virtual bool SwitchTo(const std::wstring& path) = 0;       // 已开则激活
    virtual bool SaveCurrent() = 0;
    virtual bool SaveAllDocs(bool& anySaved) = 0;
};

// 可停靠对话框宿主抽象（docs/plugin-system.md §5.8 4d）。
// 生产实现绑定 MainWindow 的 DockManager；单测注入假实现断言转发契约。
// 非拥有指针，虚拟析构。所有方法只在 UI 线程调用。
struct DockHost {
    virtual ~DockHost() = default;
    // 注册一个插件对话框为可停靠面板（NPPM_DMMREGASDCKDLG）。data 是调用方
    // 拥有的瞬时缓冲，实现必须自行拷贝所需字段。失败返回 false。
    virtual bool DockWidget(const npp::DockedWidgetData& data) = 0;
    virtual bool Show(HWND hDlg) = 0;                     // NPPM_DMMSHOW
    virtual bool Hide(HWND hDlg) = 0;                     // NPPM_DMMHIDE
    virtual void UpdateDisplayInfo(HWND hDlg) = 0;        // NPPM_DMMUPDATEDISPINFO
    virtual bool ShowByName(const wchar_t* name) = 0;     // NPPM_DMMVIEWOTHERTAB
    // NPPM_DMMGETPLUGINHWNDBYNAME：windowName==NULL 时按 moduleName 取首个
    virtual HWND FindHwndByName(const wchar_t* windowName,
                                const wchar_t* moduleName) = 0;
};

class Workspace;

// plugin command ids start here (and grow). Kept below the TabBarId range.
constexpr unsigned int PluginCmdFirst = 10000u;
constexpr unsigned int PluginCmdMax = 10500u;

// One command a plugin published. The id is host-allocated (stable for the
// process lifetime, dispatches through MainWindow::ExecuteCommand).
struct PluginCommand {
    unsigned int id = 0;
    std::wstring label;       // palette/menu text (no group prefix)
    std::wstring category;    // plugin name = Plugins-menu subgroup title
    bool grouped = false;     // NPP-form plugins render under their own submenu

    // optional accelerator (transcribed from an NPP FuncItem's ShortcutKey)
    bool hasKey = false;
    BYTE fVirt = 0;           // FVIRTKEY | FCONTROL | FALT | FSHIFT
    UINT vk = 0;

    struct Impl {
        xfs_plugin_cmd_cb cb = nullptr;
        void* user = nullptr;
    };
    Impl* impl = nullptr;     // holds the plugin callback + user
};

class PluginManager {
public:
    // 构造/析构均定义在 .cpp：oopOwned_（unique_ptr<OopHost>）需要 OopHost
    // 完整类型 —— MSVC 在实例化头文件内联的 = default 构造时也会实例化成员
    // 析构链，故两者都必须移出头文件。
    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Scan the plugin install dir and load every *.dll. Returns loaded count.
    int LoadAll();

    // Test seam: load every *.dll from an explicit directory (used by unit tests).
    int LoadAllFrom(const std::wstring& dir);

    // 会话中加载（插件管理器安装完成后调用）：扫描插件目录里尚未加载的
    // DLL（根目录 + N++ 子目录布局），走与启动一致的 tryLoad 管线（断路器/
    // 失败账本），已加载的路径自动跳过（不会重复注册命令）。每成功加载一个
    // 触发 onChanged 回调（MainWindow 用它重建插件菜单 = 菜单实时可见）。
    int LoadNew();
    // LoadNew 的显式目录版（Workshop 注入测试/定制目录用；语义同上）。
    int LoadNewFrom(const std::wstring& dir);
    void SetOnChanged(std::function<void()> cb) { onChanged_ = std::move(cb); }

    // App shutdown: unregister + free every plugin.
    // freeDlls=false 用于宿主退出（WM_DESTROY）：跳过 FreeLibrary——重插件
    // （NppExec 类）的内部线程可能仍在运行，卸载映射中的代码段会让线程踩空
    // 报 0xC0000005；进程随即退出，由 OS 回收一切（N++ 同款策略）。
    void UnloadAll(bool freeDlls = true);

    size_t CommandCount() const { return commands_.size(); }
    // deque：HostRemoveCommand 的 erase 只失效迭代器，g_handleToCmd 里存的
    // 元素指针保持有效（OOP v2 幸存者重加会在存活命令存在时删命令）。
    const std::deque<PluginCommand>& Commands() const { return commands_; }

    // 最近一次 LoadAll/LoadAllFrom 的加载失败清单（不兼容页数据源）
    const std::vector<PluginLoadFailure>& LoadFailures() const { return failures_; }

    // Dispatch: returns true + invokes the plugin callback if `id` is a plugin
    // command. Returns false for built-in ids. Runs on the UI thread.
    bool Execute(unsigned int id);

    static std::wstring PluginDir();

    // Editor/document access for host callbacks. Without a workspace (unit
    // tests, load-before-init) every editor callback degrades gracefully:
    // returns 0 / -1 / false instead of crashing.
    void SetWorkspace(Workspace* ws) { workspace_ = ws; }
    Workspace* WorkspacePtr() const { return workspace_; }
    // 主窗口句柄（NPP 形态插件的 setInfo 需要一个 npp 侧句柄）。
    void SetHostWindow(HWND h) { hostWnd_ = h; }
    // 菜单句柄（NPP 插件改写菜单用）。pluginMenu=Plugins 子菜单、
    // mainMenu=主菜单栏；MainWindow 在 BuildMenus() 之后、LoadAll() 之前注入。
    void SetPluginMenus(HMENU pluginMenu, HMENU mainMenu) {
        pluginMenu_ = pluginMenu;
        mainMenu_ = mainMenu;
    }

    // Dispatch a host event to every subscribed hook whose mask matches.
    // Public: the app (MainWindow/Workspace slots) raises real events through
    // this; unit tests drive it directly to verify hook semantics. Hook
    // callbacks run synchronously on the calling (UI) thread; hooks may remove
    // themselves mid-dispatch (the dispatch iterates a snapshot).
    void Raise(uint32_t event, const char* utf8Arg);

    size_t HookCount() const { return hooks_.size(); }

    // per-plugin scratch config root: %APPDATA%\xfsWinPad\plugins\config
    static std::wstring ConfigDir();

    // ---- NPP 消息垫片（4b）--------------------------------------------------
    // 处理发往 npp 句柄（主框架 HWND）的 NPPM_*/RUNCOMMAND 消息。
    // handled=false 表示编号不在支持子集内，调用方必须继续默认处理，
    // 绝不吞未知消息。契约细则：docs/plugin-system.md §5.6 + NppMessages.h。
    LRESULT ForwardNppMessage(UINT msg, WPARAM wp, LPARAM lp, bool& handled);
    // 测试注入口（非拥有，虚拟析构；attach 后优先于 Workspace 生效）
    void AttachNppDocSource(NppDocSource* src) { nppSrc_ = src; }
    NppDocSource* NppSourcePtr() const { return nppSrc_; }

    // 可停靠对话框宿主（4d）：NPPM_DMM* 消息的落地端。MainWindow 注入
    // DockManager；单测注入假实现。空指针时 NPPM_DMM* 明确拒答（FALSE）。
    void SetDockHost(DockHost* h) { dockHost_ = h; }
    DockHost* DockHostPtr() const { return dockHost_; }

    // handle → 已发布命令的宿主 id（NPP 兼容通道回填 FuncItem.cmdID 用；
    // 未知/空句柄返回 0）。
    unsigned CommandIdOfHandle(xfs_plugin_command* h) const;

    // ---- 进程外插件桥（v1）--------------------------------------------------
    // EnableOopHost() 后，墓碑插件改走 OopHost 代理进程（插件 exit()/崩溃
    // 只死代理，编辑器无感）；未启用时保持原「永久跳过」策略。
    void EnableOopHost();
    class OopHost* OopHostPtr() const { return oopHost_; }

    // ---- NPP 通知桥（4c）---------------------------------------------------
    // 把宿主合成或编辑器转发的事件以 SCNotification 布局广播给所有 NPP 形态
    // 插件（beNotified）。scn 必须指向有效的 SCNotification（Scintilla.h 定义，
    // nmhdr 兼容头打头）；nullptr 安全。无 NPP 插件时为空操作。
    void BroadcastNppNotification(const void* scn);
    // 便捷合成器：构造一条 NPPN_* 通知（hwndFrom=主窗口、idFrom=bufferID，
    // 其余字段清零）并广播。code 取 NppNotify 枚举（NppMessages.h）。
    void EmitNppNotification(int code, UINT_PTR idFrom);

    // ---- called only by the C-ABI trampolines -----------------------------
    xfs_plugin_command* HostAddCommand(const char* label, const char* category,
                                       xfs_plugin_cmd_cb cb, void* user);
    void HostRemoveCommand(xfs_plugin_command* cmd);
    int HostAddEventHook(xfs_event_cb cb, void* user, uint32_t eventMask);
    void HostRemoveEventHook(int hookId);

    // 测试/桥接注入口：handle → 命令（可空）。OopHost 用它回填 grouped/
    // 快捷键字段，不需要触碰 commands_ 私有布局。
    PluginCommand* CommandPtrAt(xfs_plugin_command* h) { return CommandAt(h); }

private:
    // LoadAllFrom/LoadNew 的共享扫描管线（skipLoaded=true 时跳过已加载路径）。
    int ScanAndLoadFrom(const std::wstring& dir, bool skipLoaded);

    struct EventHook {
        int id;
        xfs_event_cb cb;
        void* user;
        uint32_t mask;
    };

    struct Loaded {
        HMODULE dll = nullptr;
        std::wstring path;
        LoadedKind kind = kLoadedNative;
        xfs_plugin_abi abi{};                       // native only
        std::unique_ptr<npp::NppAdapter> npp;       // NPP-compat only
        std::vector<xfs_plugin_command*> cmds;      // handles owned by this plugin
    };

    // 两条装载分支（docs/plugin-system.md §5.2）：按导出探测分流。
    bool LoadOne(const std::wstring& path);
    bool LoadNative(const std::wstring& path, HMODULE dll,
                    const xfs_plugin_abi* (*getInfo)(void));
    bool LoadNppStyle(const std::wstring& path, HMODULE dll);
    // LoadLibrary 失败且无上游原因令牌时的兜底分类（arch/load）
    std::wstring DetectLoadFailureReason(const std::wstring& path);
    void Unregister(Loaded& l);
    static uint32_t NextCmdId();
    PluginCommand* CommandAt(xfs_plugin_command* h);   // handle→命令（可空）

    HWND hostWnd_ = nullptr;
    HMENU pluginMenu_ = nullptr;    // "Plugins" 子菜单（NPPM_GETMENUHANDLE 0）
    HMENU mainMenu_ = nullptr;      // 主菜单栏（NPPM_GETMENUHANDLE 1 / GETMENUBAR）
    std::vector<Loaded> loaded_;
    std::deque<PluginCommand> commands_;
    std::vector<PluginLoadFailure> failures_;   // LoadAll 失败记录（每次扫描重置）
    std::function<void()> onChanged_;           // LoadNew 成功加载后通知 UI 重建菜单
    std::wstring pendingFailReason_;            // 分支内置的原因令牌（abi/export/ansi）
    int lastLoadGle_ = 0;                       // 最近一次 LoadLibrary 的 GLE
    std::vector<EventHook> hooks_;
    int nextHookId_ = 1;
    xfs_plugin_host host_{};
    Workspace* workspace_ = nullptr;
    NppDocSource* nppSrc_ = nullptr;            // 非拥有；UnloadAll 置空
    DockHost* dockHost_ = nullptr;              // 非拥有；MainWindow 生命周期长于本类
    unsigned nextAllocCmdId_ = 10501;           // ALLOCATECMDID 动态池起点
    std::unique_ptr<OopHost> oopOwned_;         // EnableOopHost() 创建
    OopHost* oopHost_ = nullptr;                // = oopOwned_.get()（空 = 未启用）

    // 墓碑插件的进程外加载尝试（OopHost 未启用时返回 false）
    bool TryOopLoad(const std::wstring& path);
};

} // namespace xfs
