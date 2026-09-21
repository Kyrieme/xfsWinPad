#include "PluginManager.h"
#include "npp/NppCompat.h"
#include "oop/OopHost.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../core/I18n.h"
#include "../workspace/Workspace.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Scintilla.h>

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <set>
#include <cstring>

namespace xfs {

namespace {
// handle -> PluginCommand* owning it (for remove + execute lookup).
std::unordered_map<xfs_plugin_command*, PluginCommand*> g_handleToCmd;

// The host-side manager whose callbacks are executing. The C ABI has no
// instance parameter, so the manager stays installed here for the whole time
// its host_ struct is handed out — NOT just during DLL registration (plugins
// call editor/document callbacks later, from their command handlers).
PluginManager* g_active = nullptr;

// Bounded UTF-8 copy: up to cap-1 bytes, always NUL-terminated.
// Returns true only when the whole string fit.
bool CopyBoundedUtf8(const std::string& s, char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    const uint32_t total = (uint32_t)s.size();
    const uint32_t n = (total > cap - 1) ? cap - 1 : total;
    if (n) memcpy(out, s.data(), n);
    out[n] = '\0';
    return n == total;
}

// Raw Scintilla messages go straight to the child control so plugin plugins
// never need Editor symbols linked into the host.
sptr_t Sci(HWND h, UINT msg, WPARAM wp = 0, LPARAM lp = 0) {
    return h ? ::SendMessageW(h, msg, wp, lp) : 0;
}

// Bounded copy of the target range [start,end): copies up to cap-1 bytes.
bool SciCopyTarget(HWND h, sptr_t start, sptr_t end, char* out, uint32_t cap) {
    if (!out || cap == 0 || !h || end <= start || start < 0) return false;
    const uint64_t span = (uint64_t)(end - start);
    const uint32_t want = (span > cap - 1) ? cap - 1 : (uint32_t)span;
    Sci(h, SCI_SETTARGETSTART, 0, start);
    Sci(h, SCI_SETTARGETEND, 0, (LPARAM)(start + want));
    const sptr_t got = Sci(h, SCI_GETTARGETTEXT, 0, (LPARAM)out);
    if (got != (sptr_t)want) return false;   // unexpected short copy
    out[want] = '\0';
    return want == span;                     // false = truncated
}
} // namespace

std::wstring PluginManager::PluginDir() {
    std::wstring base;
    wchar_t buf[MAX_PATH];
    if (::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) > 0)
        base = buf;
    return base + L"\\xfsWinPad\\plugins";
}

std::wstring PluginManager::ConfigDir() { return PluginDir() + L"\\config"; }

void PluginManager::Raise(uint32_t event, const char* utf8Arg) {
    if (hooks_.empty()) return;
    // snapshot: a hook may remove itself (or others) while being dispatched
    const std::vector<EventHook> snapshot = hooks_;
    for (const auto& h : snapshot)
        if ((h.mask & event) && h.cb) h.cb(event, utf8Arg, h.user);
}

uint32_t PluginManager::NextCmdId() {
    static uint32_t next = PluginCmdFirst;
    if (next > PluginCmdMax) return 0;   // pool exhausted
    return next++;
}

// --- C-ABI trampolines ------------------------------------------------------
namespace {

Document* ActiveDoc(PluginManager* m) {
    return (m && m->WorkspacePtr()) ? m->WorkspacePtr()->Active() : nullptr;
}

HWND ActiveSci(PluginManager* m) {
    Document* d = ActiveDoc(m);
    return d ? d->editor.Hwnd() : nullptr;
}

int TrampolineGetActiveFile(char* out, uint32_t cap) {
    Document* d = ActiveDoc(g_active);
    if (!d) return false;
    std::wstring path = d->path.wstring();   // empty for untitled
    return CopyBoundedUtf8(WideToUtf8(path), out, cap);
}

void TrampolineLog(const char* message) {
    if (message) Logger::Info(std::string("插件: ") + message);
}

xfs_plugin_command* TrampolineAddCommand(const char* label, const char* category,
                                         xfs_plugin_cmd_cb cb, void* user) {
    if (!g_active || !label) return nullptr;
    return g_active->HostAddCommand(label, category, cb, user);
}

void TrampolineRemoveCommand(xfs_plugin_command* cmd) {
    if (g_active) g_active->HostRemoveCommand(cmd);
}

int TrampolineGetDocumentCount(void) {
    return g_active && g_active->WorkspacePtr()
               ? (int)g_active->WorkspacePtr()->Count() : 0;
}

int TrampolineGetActiveDocumentIndex(void) {
    return g_active && g_active->WorkspacePtr()
               ? g_active->WorkspacePtr()->ActiveIndex() : -1;
}

int TrampolineGetDocumentPath(int index, char* out, uint32_t cap) {
    Workspace* ws = g_active ? g_active->WorkspacePtr() : nullptr;
    if (!ws || index < 0 || index >= (int)ws->Count()) return false;
    return CopyBoundedUtf8(WideToUtf8(ws->DocumentAt(index)->path.wstring()),
                           out, cap);
}

int TrampolineOpenFile(const char* utf8Path) {
    Workspace* ws = g_active ? g_active->WorkspacePtr() : nullptr;
    if (!ws || !utf8Path || !utf8Path[0]) return false;
    std::wstring wide = Utf8ToWide(utf8Path);
    std::error_code ec;
    if (!std::filesystem::exists(wide, ec)) return false;
    int before = (int)ws->Count();
    ws->OpenPath(wide);
    // OpenPath activates instead of duplicating when already open, and drops
    // read errors silently into LastError — judge success by state change or
    // active path match below.
    if ((int)ws->Count() > before) return true;
    Document* a = ws->Active();
    return a && a->path.wstring() == wide;
}

int TrampolineSaveActive(void) {
    return g_active && g_active->WorkspacePtr()
               ? g_active->WorkspacePtr()->Save() : false;
}

uint32_t TrampolineGetTextLength(void) {
    HWND h = ActiveSci(g_active);
    return h ? (uint32_t)Sci(h, SCI_GETLENGTH) : 0;
}

int TrampolineGetText(char* out, uint32_t cap) {
    HWND h = ActiveSci(g_active);
    if (!h) return false;
    const sptr_t len = Sci(h, SCI_GETLENGTH);
    const sptr_t end = (cap > 0 && (uint64_t)len > cap - 1) ? (sptr_t)(cap - 1) : len;
    return SciCopyTarget(h, 0, end, out, cap);
}

int TrampolineSetTextUndoable(const char* utf8Text) {
    HWND h = ActiveSci(g_active);
    if (!h || !utf8Text) return false;
    const size_t n = strlen(utf8Text);
    Sci(h, SCI_BEGINUNDOACTION);
    Sci(h, SCI_TARGETWHOLEDOCUMENT);
    Sci(h, SCI_REPLACETARGET, (WPARAM)n, (LPARAM)utf8Text);
    Sci(h, SCI_ENDUNDOACTION);
    return true;
}

int TrampolineInsertTextAtCaret(const char* utf8Text) {
    HWND h = ActiveSci(g_active);
    if (!h || !utf8Text) return false;
    Sci(h, SCI_REPLACESEL, 0, (LPARAM)utf8Text);   // undo-tracked
    return true;
}

int TrampolineGetSelectedText(char* out, uint32_t cap) {
    HWND h = ActiveSci(g_active);
    if (!h) return false;
    const sptr_t s = Sci(h, SCI_GETSELECTIONSTART);
    return SciCopyTarget(h, s, Sci(h, SCI_GETSELECTIONEND), out, cap);
}

int TrampolineGetCurrentLine(void) {
    HWND h = ActiveSci(g_active);
    if (!h) return -1;
    return (int)Sci(h, SCI_LINEFROMPOSITION, (WPARAM)Sci(h, SCI_GETCURRENTPOS)) + 1;
}

int TrampolineGetCurrentColumn(void) {
    HWND h = ActiveSci(g_active);
    if (!h) return -1;
    return (int)Sci(h, SCI_GETCOLUMN, (WPARAM)Sci(h, SCI_GETCURRENTPOS)) + 1;
}

int TrampolineGotoLine(int line1based) {
    HWND h = ActiveSci(g_active);
    if (!h || line1based < 1) return false;
    const sptr_t last = Sci(h, SCI_GETLINECOUNT);
    Sci(h, SCI_GOTOLINE, (WPARAM)((line1based > last) ? last - 1 : line1based - 1));
    Sci(h, SCI_SCROLLCARET);
    return true;
}

int TrampolineIsModified(void) {
    HWND h = ActiveSci(g_active);
    return h ? Sci(h, SCI_GETMODIFY) != 0 : false;
}

int TrampolineIsReadOnly(void) {
    HWND h = ActiveSci(g_active);
    return h ? Sci(h, SCI_GETREADONLY) != 0 : false;
}

int TrampolineAddEventHook(xfs_event_cb cb, void* user, uint32_t eventMask) {
    return g_active ? g_active->HostAddEventHook(cb, user, eventMask) : 0;
}

void TrampolineRemoveEventHook(int hookId) {
    if (g_active) g_active->HostRemoveEventHook(hookId);
}

int TrampolineGetConfigDir(char* out, uint32_t cap) {
    std::wstring dir = PluginManager::ConfigDir();
    // create on demand so plugins can write immediately
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return CopyBoundedUtf8(WideToUtf8(dir), out, cap);
}

} // namespace

// --- manager ----------------------------------------------------------------

// PE 头机器类型（不加载 DLL）：0x8664/0xAA64=64 位、0x14C=32 位、0=未知。
// 用于把 LoadLibrary 错误 193 细化成可读的「架构不匹配」原因。
static int PepMachineBits(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    IMAGE_DOS_HEADER dos{};
    if (!f.read((char*)&dos, sizeof(dos)) || dos.e_magic != 0x5A4D) return 0;
    f.seekg(dos.e_lfanew, std::ios::beg);
    DWORD sig = 0;
    if (!f.read((char*)&sig, sizeof(sig)) || sig != 0x00004550) return 0;
    WORD machine = 0;
    if (!f.read((char*)&machine, sizeof(machine))) return 0;
    if (machine == 0x8664 || machine == 0xAA64) return 64;
    if (machine == 0x14C) return 32;
    return 0;
}

// 失败原因令牌（PluginManager.h 的 PluginLoadFailure 契约）。
// LoadOne 各失败分支会调用；LoadLibrary 失败时按 GLE 与 PE 架构细分。
std::wstring PluginManager::DetectLoadFailureReason(const std::wstring& path) {
    if (lastLoadGle_ == 193) {
        const int bits = PepMachineBits(path);
        const int hostBits = (sizeof(void*) == 8) ? 64 : 32;
        if (bits != 0 && bits != hostBits) return L"arch";
    }
    return L"load";
}

int PluginManager::LoadAll() {
    return LoadAllFrom(PluginDir());
}

// 扫描 + 加载的共享实现。skipLoaded=false 为启动全量加载；true 为会话中
// 加载新装插件（已加载路径跳过，避免重复注册命令）。
int PluginManager::ScanAndLoadFrom(const std::wstring& dir, bool skipLoaded) {
    g_active = this;
    host_.abiVersion = XFS_PLUGIN_ABI_VERSION;
    host_.addCommand = TrampolineAddCommand;
    host_.removeCommand = TrampolineRemoveCommand;
    host_.getActiveFile = TrampolineGetActiveFile;
    host_.log = TrampolineLog;
    host_.getDocumentCount = TrampolineGetDocumentCount;
    host_.getActiveDocumentIndex = TrampolineGetActiveDocumentIndex;
    host_.getDocumentPath = TrampolineGetDocumentPath;
    host_.openFile = TrampolineOpenFile;
    host_.saveActive = TrampolineSaveActive;
    host_.getTextLength = TrampolineGetTextLength;
    host_.getText = TrampolineGetText;
    host_.setTextUndoable = TrampolineSetTextUndoable;
    host_.insertTextAtCaret = TrampolineInsertTextAtCaret;
    host_.getSelectedText = TrampolineGetSelectedText;
    host_.getCurrentLine = TrampolineGetCurrentLine;
    host_.getCurrentColumn = TrampolineGetCurrentColumn;
    host_.gotoLine = TrampolineGotoLine;
    host_.isModified = TrampolineIsModified;
    host_.isReadOnly = TrampolineIsReadOnly;
    host_.addEventHook = TrampolineAddEventHook;
    host_.removeEventHook = TrampolineRemoveEventHook;
    host_.getConfigDir = TrampolineGetConfigDir;

    if (!std::filesystem::exists(dir)) {
        g_active = nullptr;
        return 0;
    }

    int loaded = 0;
    int loadedNew = 0;
    std::error_code ec;
    // 启动全量扫描重置账本；LoadNew 追加
    if (!skipLoaded) { failures_.clear(); isolated_.clear(); }

    // ---- 加载断路器（circuit breaker）-------------------------------------
    // 有失配插件会在 setInfo 里直接 exit() 杀死宿主（实测 ComparePlus），
    // 造成「程序打不开」死循环。约定：LoadOne 前把候选路径写进
    // loadattempt.txt、成功/拒绝后删除——若某次会话启动时该文件仍在，即
    // 上次加载中途死亡，将其加入 plugin_tombstones.txt 墓碑并永久跳过
    // （计入不兼容页，原因 fatal）。删除墓碑文件即可重试。
    const std::wstring attemptPath = dir + L"\\loadattempt.txt";
    const std::wstring tombPath = dir + L"\\plugin_tombstones.txt";
    // 路径归一化：小写 + 分隔符统一成 '\\'。
    // ★ 分隔符必须归一化。用户手写的 plugin_oop.txt 用的是 Windows 反斜杠，
    //   而扫描出来的路径可能带正斜杠（真机实测：APPDATA 带正斜杠时整条路径
    //   都是 `D:/...`）。只做小写会让两边**永远匹配不上**，而且**静默失败**
    //   —— 插件照样加载，只是加载在**进程内**，界面上完全看不出来。
    //   （这条是拿真实 GUI 跑出来的：单元测试两侧路径都来自 std::filesystem、
    //   都是反斜杠，所以对分隔符差异完全无感。）
    auto ToKey = [](const std::wstring& p) {
        std::wstring k = p;
        for (auto& c : k) {
            if (c == L'/') c = L'\\';
            c = (wchar_t)::towlower(c);
        }
        return k;
    };
    std::set<std::wstring> tomb;
    {
        std::ifstream tf(tombPath.c_str());
        std::string line;
        while (std::getline(tf, line)) {
            if (!line.empty()) tomb.insert(ToKey(Utf8ToWide(line)));
        }
    }
    {
        std::ifstream af(attemptPath.c_str());
        if (af) {
            std::string line;
            std::getline(af, line);
            if (!line.empty()) {
                tomb.insert(ToKey(Utf8ToWide(line)));
                // 必须落盘：内存集合只救本次会话，落盘才能真正阻断
                // 「死一次、开一次、再死一次」的循环。
                std::ofstream tf(tombPath.c_str(), std::ios::app);
                if (tf) tf << line << "\n";
                Logger::Error("PluginManager: previous session died while loading "
                              + line + " -> tombstoned in plugin_tombstones.txt");
            }
            ::DeleteFileW(attemptPath.c_str());
        }
    }

    auto IsLoaded = [this, &ToKey](const std::wstring& p) {
        const std::wstring k = ToKey(p);
        for (const auto& l : loaded_) if (ToKey(l.path) == k) return true;
        return false;
    };

    // ---- 主动进程外加载名单（plugin_oop.txt）-------------------------------
    // 与墓碑同一套文件约定：一行一个 DLL 路径，大小写不敏感。
    // 墓碑是**被动**触发（插件必须先杀死宿主一次才被隔离）；本名单是**主动**
    // 入口——已知有问题的插件可以提前关进代理进程，不必先挨一次崩溃。
    const std::wstring oopListPath = dir + L"\\plugin_oop.txt";
    std::set<std::wstring> forcedOop;
    {
        std::ifstream of(oopListPath.c_str());
        std::string line;
        while (std::getline(of, line)) {
            if (!line.empty()) forcedOop.insert(ToKey(Utf8ToWide(line)));
        }
    }

    auto tryLoad = [this, &loaded, &loadedNew, skipLoaded, &tomb, &forcedOop,
                    &ToKey, &IsLoaded, &attemptPath](const std::wstring& p) {
        if (skipLoaded && IsLoaded(p)) return;   // 会话中加载：跳过已注册 DLL
        const std::wstring key = ToKey(p);
        if (tomb.count(key)) {
            // 墓碑插件：进程外隔离尝试（OopHost 启用时代理进程承载，
            // exit()/崩溃只死代理）。未启用/失败 → 保持原「跳过 + fatal」。
            if (TryOopLoad(p, L"oop-auto")) {
                isolated_.push_back({p, L"oop-auto"});
                ++loaded;
                if (skipLoaded) ++loadedNew;
            } else {
                Logger::Info("PluginManager: skip tombstoned " + WideToUtf8(p));
                PluginLoadFailure f;
                f.path = p;
                f.reason = L"fatal";
                failures_.push_back(std::move(f));
            }
            return;
        }
        if (forcedOop.count(key)) {
            // 用户指定进程外加载。★ 失败时**不回落进程内** —— 指定进程外正是
            // 为了避开该插件在宿主进程内 exit()/崩溃，悄悄放回进程内等于把
            // 风险还给他。代理侧失败原因（oop-died 等）由 TryOopLoad 并入账本。
            const size_t before = failures_.size();
            if (TryOopLoad(p, L"oop-forced")) {
                isolated_.push_back({p, L"oop-forced"});
                ++loaded;
                if (skipLoaded) ++loadedNew;
            } else {
                if (failures_.size() == before) {   // 代理未给原因 → 兜底
                    PluginLoadFailure f;
                    f.path = p;
                    f.reason = L"oop-unavailable";
                    failures_.push_back(std::move(f));
                }
                Logger::Error("PluginManager: forced out-of-process load failed: "
                              + WideToUtf8(p));
            }
            return;
        }
        {   // 哨兵：记录正在加载的候选
            std::ofstream af(attemptPath.c_str(), std::ios::trunc);
            if (af) af << WideToUtf8(p) << "\n";
        }
        if (LoadOne(p)) {
            ++loaded;
            if (skipLoaded) ++loadedNew;
        } else {
            PluginLoadFailure f;
            f.path = p;
            f.reason = pendingFailReason_.empty()
                ? DetectLoadFailureReason(p) : pendingFailReason_;
            pendingFailReason_.clear();
            failures_.push_back(std::move(f));
        }
        ::DeleteFileW(attemptPath.c_str());
    };
    // 根目录散置 DLL（自有插件惯例）
    for (auto& ent : std::filesystem::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file(ec)) continue;
        std::wstring p = ent.path().wstring();
        if (std::filesystem::path(p).extension().wstring() != L".dll") continue;
        tryLoad(p);
    }
    // N++ 标准布局：plugins\<Name>\<Name>.dll（目录内其余 DLL 视为辅助库不加载）
    for (auto& sub : std::filesystem::directory_iterator(dir, ec)) {
        if (!sub.is_directory(ec)) continue;
        std::wstring name = sub.path().filename().wstring();
        std::wstring cand = sub.path().wstring() + L"\\" + name + L".dll";
        if (std::filesystem::exists(cand, ec)) tryLoad(cand);
    }
    Logger::Info("PluginManager: loaded " + std::to_string(loaded) +
                 " plugin(s) from " + WideToUtf8(dir) +
                 ", " + std::to_string(failures_.size()) + " refused");
    // g_active stays installed: plugins call the host_ callbacks at command
    // execution time, long after registration returned.
    if (loadedNew > 0 && onChanged_) onChanged_();   // 会话中新载入 → UI 重建菜单
    return loaded;
}

int PluginManager::LoadAllFrom(const std::wstring& dir) {
    return ScanAndLoadFrom(dir, /*skipLoaded=*/false);
}

int PluginManager::LoadNew() {
    return ScanAndLoadFrom(PluginDir(), /*skipLoaded=*/true);
}

int PluginManager::LoadNewFrom(const std::wstring& dir) {
    return ScanAndLoadFrom(dir, /*skipLoaded=*/true);
}

PluginManager::PluginManager() = default;
PluginManager::~PluginManager() { UnloadAll(); }

void PluginManager::EnableOopHost() {
    if (!oopOwned_) {
        oopOwned_ = std::make_unique<OopHost>();
        oopHost_ = oopOwned_.get();
        Logger::Info("PluginManager: out-of-process plugin host enabled");
    }
}

bool PluginManager::TryOopLoad(const std::wstring& path, const wchar_t* reason) {
    if (!oopHost_ || !hostWnd_) return false;
    Document* act = workspace_ ? workspace_->Active() : nullptr;
    HWND sci = act ? act->editor.Hwnd() : nullptr;
    if (!oopHost_->Launch(*this, path, hostWnd_, sci)) {
        // 代理失败账本（oop-died 等）并入本次失败记录
        for (auto& f : oopHost_->TakeFailures()) failures_.push_back(std::move(f));
        return false;
    }
    // 日志必须带上真实原因：墓碑自动隔离与用户指定强制隔离是两回事，
    // 一律写成 "tombstoned" 会让排查时看不到 plugin_oop.txt 是否生效。
    Logger::Info("PluginManager: plugin loaded out-of-process (" +
                 WideToUtf8(reason) + "): " + WideToUtf8(path));
    return true;
}

bool PluginManager::LoadOne(const std::wstring& path) {
    lastLoadGle_ = 0;
    // 损坏/非 PE 的 DLL 会让加载器弹「损坏的映像」硬错误框（0xc000012f）
    // 并阻塞线程直到用户点掉。SEM_FAILCRITICALERRORS 使 LoadLibrary 静默
    // 返回 NULL + GLE（走正常失败记录路径），宿主绝不为坏插件打断用户。
    UINT oldMode = 0;
    ::SetThreadErrorMode(SEM_FAILCRITICALERRORS, (LPDWORD)&oldMode);
    HMODULE dll = ::LoadLibraryW(path.c_str());
    ::SetThreadErrorMode(oldMode, nullptr);
    if (!dll) {
        lastLoadGle_ = (int)::GetLastError();
        Logger::Error("PluginManager: LoadLibrary failed for " + WideToUtf8(path)
                      + " gle=" + std::to_string(lastLoadGle_));
        return false;
    }

    // 分流探测：先原生（xfsPlugin_getInfo），缺位则尝试 NPP 兼容形态。
    auto getInfo = (const xfs_plugin_abi* (*)(void))::GetProcAddress(dll, "xfsPlugin_getInfo");
    bool ok = getInfo ? LoadNative(path, dll, getInfo)
                      : LoadNppStyle(path, dll);
    if (!ok) {
        ::FreeLibrary(dll);
    }
    return ok;
}

bool PluginManager::LoadNative(const std::wstring& path, HMODULE dll,
                               const xfs_plugin_abi* (*getInfo)(void)) {
    const xfs_plugin_abi* abi = getInfo();
    if (!abi || abi->abiVersion != XFS_PLUGIN_ABI_VERSION || !abi->registerPlugin) {
        Logger::Error("PluginManager: ABI mismatch in " + WideToUtf8(path));
        pendingFailReason_ = L"abi";
        return false;
    }

    Loaded l;
    l.dll = dll;
    l.path = path;
    l.kind = kLoadedNative;
    l.abi = *abi;

    // Track this plugin while it registers so its commands are linked to it.
    // g_active deliberately stays installed afterwards — see LoadAllFrom.
    g_active = this;
    l.abi.registerPlugin(&host_);

    loaded_.push_back(std::move(l));
    Logger::Info("PluginManager: loaded native '" + std::string(abi->name ? abi->name : "?") +
                 "' " + (abi->version ? abi->version : ""));
    return true;
}

// NPP ShortcutKey → 统一命令加速键字段（纯转录，不依赖任何协议数值）。
static void FillShortcut(const npp::ShortcutKey* sk, PluginCommand& pc) {
    if (!sk || !sk->key) return;
    pc.hasKey = true;
    pc.vk = sk->key;
    BYTE f = FVIRTKEY;
    if (sk->ctrl)  f |= FCONTROL;
    if (sk->alt)   f |= FALT;
    if (sk->shift) f |= FSHIFT;
    pc.fVirt = f;
}

bool PluginManager::LoadNppStyle(const std::wstring& path, HMODULE dll) {
    Logger::Info("LoadNppStyle begin: " + WideToUtf8(path));
    auto adapter = std::make_unique<npp::NppAdapter>();
    if (!adapter->Resolve(dll)) {
        // 令牌为 ASCII，窄→宽按字节拷贝安全
        const std::string& fr = adapter->FailReason();
        pendingFailReason_ = fr.empty() ? std::wstring(L"export")
                                        : std::wstring(fr.begin(), fr.end());
        return false;                       // reasons already logged
    }
    Logger::Info("LoadNppStyle resolve ok: " + WideToUtf8(path));

    // setInfo：npp 侧句柄给主窗口；Scintilla 主句柄给注册时刻的活动文档
    // （语义差与后续正解见 插件系统设计笔记 §5.1）。
    Document* act = workspace_ ? workspace_->Active() : nullptr;
    npp::NppData data{hostWnd_, act ? act->editor.Hwnd() : nullptr, nullptr};
    Logger::Info("LoadNppStyle pre-setInfo");
    adapter->CallSetInfo(data);
    Logger::Info("LoadNppStyle post-setInfo");

    int n = 0;
    npp::FuncItem* items = adapter->FetchItems(n);
    if (!items) {
        pendingFailReason_ = L"export";
        return false;
    }
    Logger::Info("LoadNppStyle fetch ok n=" + std::to_string(n));

    // 布局契约（现代 SDK）：内联 wchar_t[64] 名 + func + cmdID + initCheck +
    // ShortcutKey*，stride 152。NppExec 返回的数组偏移 0 即 UTF-16 文本；
    // 旧"指针名"布局会把文本字节误当地址导致 0xC0000005（已修复）。
    const std::wstring name = adapter->Name();   // 拷贝后再移交所有权

    Loaded l;
    l.dll = dll;
    l.path = path;
    l.kind = kLoadedNppCompat;
    l.npp = std::move(adapter);
    const auto& slots = l.npp->Slots();

    // 调用序列后半（公开契约）：宿主逐项回填 cmdID，并把 FuncItem 转录为
    // 统一命令（label 不再带前缀 —— 分组交由 Plugins 子菜单表现）。
    for (int i = 0; i < n; ++i) {
        const wchar_t* itemLabel = items[i].itemName;   // 内联 buffer，非指针
        Logger::Debug("LoadNppStyle item[" + std::to_string(i) +
                      "] name='" + WideToUtf8(itemLabel && itemLabel[0] ? itemLabel : Tr(L"plugcmd.unnamed")) +
                      "' funcPtr=0x" + std::to_string((uintptr_t)items[i].func) +
                      " shortcutPtr=0x" + std::to_string((uintptr_t)items[i].shortcut));
        wchar_t label[256];
        swprintf_s(label, L"%s",
                   itemLabel && itemLabel[0] ? itemLabel : Tr(L"plugcmd.unnamed"));
        xfs_plugin_command* h =
            HostAddCommand(WideToUtf8(label).c_str(), WideToUtf8(name).c_str(),
                           npp::NppAdapter::Thunk,
                           const_cast<npp::NppAdapter::FuncSlot*>(&slots[static_cast<size_t>(i)]));
        if (h) {
            if (PluginCommand* pc = CommandAt(h)) {
                pc->grouped = true;
                FillShortcut(items[i].shortcut, *pc);
            }
            items[i].cmdID = static_cast<int>(CommandIdOfHandle(h));
            l.cmds.push_back(h);
        }
    }

    loaded_.push_back(std::move(l));
    Logger::Info("PluginManager: loaded NPP-plugin '" + WideToUtf8(name) +
                 "' with " + std::to_string(n) + " FuncItem(s)");
    return true;
}

xfs_plugin_command* PluginManager::HostAddCommand(const char* label, const char* category,
                                                  xfs_plugin_cmd_cb cb, void* user) {
    if (!cb) return nullptr;
    uint32_t id = NextCmdId();
    if (id == 0) return nullptr;

    auto* handle = new xfs_plugin_command();

    PluginCommand pc;
    pc.id = id;
    pc.label = Utf8ToWide(label ? label : "");
    pc.category = Utf8ToWide(category ? category : "插件");
    pc.impl = new PluginCommand::Impl{cb, user};
    commands_.push_back(std::move(pc));

    // link handle -> this command (back of the vector)
    g_handleToCmd[handle] = &commands_.back();

    // remap handle ownership to the plugin being loaded (it must release on
    // unregister). We don't need per-plugin tracking for the skeleton since
    // UnloadAll clears everything, but record it so the handle stays valid.
    return handle;
}

int PluginManager::HostAddEventHook(xfs_event_cb cb, void* user, uint32_t eventMask) {
    if (!cb || eventMask == 0) return 0;
    hooks_.push_back({nextHookId_, cb, user, eventMask});
    return nextHookId_++;
}

void PluginManager::HostRemoveEventHook(int hookId) {
    for (size_t i = 0; i < hooks_.size(); ++i) {
        if (hooks_[i].id == hookId) {
            hooks_.erase(hooks_.begin() + i);
            return;
        }
    }
}

void PluginManager::HostRemoveCommand(xfs_plugin_command* cmd) {
    if (!cmd) return;
    auto it = g_handleToCmd.find(cmd);
    if (it == g_handleToCmd.end()) return;
    PluginCommand* target = it->second;

    // find and erase the command by pointer
    for (size_t i = 0; i < commands_.size(); ++i) {
        if (&commands_[i] == target) {
            delete commands_[i].impl;
            commands_.erase(commands_.begin() + i);
            break;
        }
    }
    g_handleToCmd.erase(it);
    delete cmd;
}

bool PluginManager::Execute(unsigned int id) {
    for (auto& c : commands_) {
        if (c.id == id) {
            if (c.impl && c.impl->cb) c.impl->cb(c.impl->user);
            return true;
        }
    }
    return false;
}

unsigned PluginManager::CommandIdOfHandle(xfs_plugin_command* h) const {
    if (!h) return 0;
    auto it = g_handleToCmd.find(h);
    return it != g_handleToCmd.end() ? it->second->id : 0u;
}

PluginCommand* PluginManager::CommandAt(xfs_plugin_command* h) {
    auto it = g_handleToCmd.find(h);
    return it != g_handleToCmd.end() ? it->second : nullptr;
}

void PluginManager::Unregister(Loaded& l) {
    if (l.abi.unregisterPlugin) l.abi.unregisterPlugin();
}

void PluginManager::UnloadAll(bool freeDlls) {
    // 进程外代理先行关停：先发 SHUTDOWN（通知已由 WM_CLOSE 广播送达），
    // 再撤销其命令、join 看门狗 —— 必须先于命令表清空（impl→ctx 悬空）。
    if (oopHost_) oopHost_->ShutdownAll();
    Logger::Info("UnloadAll: begin, " + std::to_string(loaded_.size()) +
                 " plugin(s), freeDlls=" + (freeDlls ? "1" : "0"));
    for (auto& l : loaded_) {
        Logger::Debug("UnloadAll: unregister " + WideToUtf8(l.path));
        Unregister(l);   // native only; NPP plugins have no unload contract
        if (freeDlls) {
            Logger::Debug("UnloadAll: FreeLibrary " + WideToUtf8(l.path));
            if (l.dll) { ::FreeLibrary(l.dll); l.dll = nullptr; }
        }
        Logger::Debug("UnloadAll: reset adapter " + WideToUtf8(l.path));
        l.npp.reset();   // FuncSlot 生命周期随 adapter；命令表 impl 不解引用 user
    }
    Logger::Info("UnloadAll: clear commands");
    loaded_.clear();
    for (auto& c : commands_) delete c.impl;
    commands_.clear();
    hooks_.clear();
    nextHookId_ = 1;
    for (auto& [h, c] : g_handleToCmd) delete h;   // release the command handles
    g_handleToCmd.clear();
    g_active = nullptr;
    nppSrc_ = nullptr;   // 非拥有指针（生产适配器随进程消亡/测试自管）
    dockHost_ = nullptr; // 非拥有指针（MainWindow 的 DockManager 生命周期更长）

}

// ===================== NPP 消息垫片（4b）=====================================
// 契约数值来源：npp/NppMessages.h；行为决策记录：插件系统设计笔记 §5.6。
namespace {

// 生产环境的文档集视图：映射到真实 Workspace。
class WorkspaceSink final : public NppDocSource {
public:
    explicit WorkspaceSink(PluginManager* m) : m_(m) {}
    int DocCount() override {
        return m_->WorkspacePtr() ? (int)m_->WorkspacePtr()->Count() : 0;
    }
    Document* DocAt(int i) override {
        return (m_->WorkspacePtr() && i >= 0 && i < (int)m_->WorkspacePtr()->Count())
                   ? m_->WorkspacePtr()->DocumentAt(i) : nullptr;
    }
    Document* Current() override {
        return m_->WorkspacePtr() ? m_->WorkspacePtr()->Active() : nullptr;
    }
    int CurrentIndex() override {
        return m_->WorkspacePtr() ? m_->WorkspacePtr()->ActiveIndex() : -1;
    }
    bool OpenNew(const std::wstring& p) override {
        Workspace* ws = m_->WorkspacePtr();
        if (!ws) return false;
        std::error_code ec;
        std::wstring t = NormalizePath(p);
        if (!std::filesystem::exists(t, ec)) return false;
        int before = (int)ws->Count();
        ws->OpenPath(t);
        if ((int)ws->Count() > before) return true;
        Document* a = ws->Active();
        return a && a->path.wstring() == t;
    }
    bool SwitchTo(const std::wstring& p) override {
        Workspace* ws = m_->WorkspacePtr();
        if (!ws) return false;
        std::wstring t = NormalizePath(p);
        for (int i = 0; i < (int)ws->Count(); ++i)
            if (ws->DocumentAt(i)->path.wstring() == t) { ws->Activate(i); return true; }
        return false;
    }
    bool SaveCurrent() override {
        return m_->WorkspacePtr() ? m_->WorkspacePtr()->Save() : false;
    }
    bool SaveAllDocs(bool& anySaved) override {
        Workspace* ws = m_->WorkspacePtr();
        if (!ws) { anySaved = false; return false; }
        int dirty = 0;
        for (int i = 0; i < (int)ws->Count(); ++i)
            if (ws->DocumentAt(i)->editor.Modified()) ++dirty;
        anySaved = dirty > 0;
        return ws->SaveAll();
    }

private:
    PluginManager* m_;
};

Document* FindByBufferId(NppDocSource& src, UINT_PTR id) {
    for (int i = 0; i < src.DocCount(); ++i) {
        Document* d = src.DocAt(i);
        if (d && reinterpret_cast<UINT_PTR>(d) == id) return d;
    }
    return nullptr;
}

int IndexByBufferId(NppDocSource& src, UINT_PTR id) {
    for (int i = 0; i < src.DocCount(); ++i) {
        Document* d = src.DocAt(i);
        if (d && reinterpret_cast<UINT_PTR>(d) == id) return i;
    }
    return -1;
}

// 上游字符串族两段式回复：第一段 lp==NULL 返回所需 wchar_t 数（不含 NUL）；
// 第二段 wp=调用方按"返回值+1"分配的容量，拷入并返回 TRUE/FALSE(截断)。
LRESULT WideStrTwoCall(const std::wstring& s, WPARAM wp, LPARAM lp) {
    const unsigned need = (unsigned)s.size();
    wchar_t* buf = reinterpret_cast<wchar_t*>(lp);
    if (!buf) return (LRESULT)need;
    if (wp == 0) return FALSE;
    const unsigned cap = (unsigned)wp;
    const unsigned n = need < cap - 1 ? need : cap - 1;
    if (n) memcpy(buf, s.data(), n * sizeof(wchar_t));
    buf[n] = L'\0';
    return n == need ? (LRESULT)TRUE : (LRESULT)FALSE;
}

} // namespace

LRESULT PluginManager::ForwardNppMessage(UINT msg, WPARAM wp, LPARAM lp,
                                         bool& handled) {
    namespace nn = xfs::npp;

    // 只声明式拦截两个已知基值区间；区间外一律放行默认处理。
    const UINT loA = nn::kNppMsgBase, hiA = nn::kNppMsgBase + 128;
    const UINT loB = nn::kRunCmdBase, hiB = nn::kRunCmdBase + 64;
    if (!((msg >= loA && msg <= hiA) || (msg >= loB && msg <= hiB))) {
        handled = false;
        return 0;
    }

    handled = true;   // 区间内未列出的编号也视为已处理（明确拒答，不吞）

    WorkspaceSink sink(this);
    NppDocSource& src = nppSrc_ ? *nppSrc_
                                : static_cast<NppDocSource&>(sink);

    switch (msg) {
    case nn::NPPM_GETCURRENTSCINTILLA: {
        auto* out = reinterpret_cast<int*>(lp);
        if (out) *out = 0;                        // 单视图模型 ⇒ Main View
        return (LRESULT)TRUE;
    }
    case nn::NPPM_GETNBOPENFILES:
        if ((int)lp == nn::kSecondView) return 0;
        return (LRESULT)src.DocCount();

    case nn::NPPM_GETOPENFILENAMES_DEPRECATED:
    case nn::NPPM_GETOPENFILENAMESPRIMARY_DEPRECATED: {
        auto** arr = reinterpret_cast<wchar_t**>(wp);
        const int cap = (int)lp;
        int copied = 0;
        if (arr)
            for (int i = 0; i < src.DocCount() && copied < cap; ++i) {
                Document* d = src.DocAt(i);
                if (!d) continue;
                // 调用方惯例为每项 MAX_PATH 缓冲；超长按 _TRUNCATE 截断
                wcsncpy_s(arr[copied], 260, d->path.c_str(), _TRUNCATE);
                ++copied;
            }
        return (LRESULT)copied;
    }
    case nn::NPPM_GETOPENFILENAMESSECOND_DEPRECATED:
        return 0;                                 // 无副视图

    case nn::NPPM_SWITCHTOFILE: {
        if (!lp) return FALSE;
        return src.SwitchTo(reinterpret_cast<const wchar_t*>(lp)) ? TRUE : FALSE;
    }
    case nn::NPPM_DOOPEN: {
        if (!lp) return FALSE;
        return src.OpenNew(reinterpret_cast<const wchar_t*>(lp)) ? TRUE : FALSE;
    }
    case nn::NPPM_SAVECURRENTFILE:
        return src.SaveCurrent() ? TRUE : FALSE;
    case nn::NPPM_SAVEALLFILES: {
        bool anySaved = false;
        const BOOL ok = src.SaveAllDocs(anySaved) ? TRUE : FALSE;
        return anySaved ? ok : (LRESULT)FALSE;    // 契约：无保存动作返回 FALSE
    }

    case nn::NPPM_RELOADBUFFERID: {
        // 仅生产通道支持重载；测试注入源不提供后端
        if (nppSrc_ || !workspace_) return FALSE;
        const int index = IndexByBufferId(src, (UINT_PTR)wp);
        if (index < 0) return FALSE;
        return workspace_->ReloadDocument(index, lp != 0) ? TRUE : FALSE;
    }
    case nn::NPPM_RELOADFILE: {
        if (nppSrc_ || !lp) return FALSE;
        std::wstring target = NormalizePath(reinterpret_cast<const wchar_t*>(lp));
        for (int i = 0; i < src.DocCount(); ++i) {
            Document* d = src.DocAt(i);
            if (d && d->path.wstring() == target)
                return workspace_->ReloadDocument(i, wp != 0) ? TRUE : FALSE;
        }
        return FALSE;
    }

    case nn::NPPM_GETCURRENTBUFFERID: {
        Document* d = src.Current();
        return d ? reinterpret_cast<LRESULT>(d) : 0;
    }
    case nn::NPPM_GETBUFFERIDFROMPOS: {
        const int index = (int)wp;                 // wParam=index
        if ((int)lp != nn::kMainViewPos) return 0; // 无副视图
        Document* d = src.DocAt(index);
        return d ? reinterpret_cast<LRESULT>(d) : 0;
    }
    case nn::NPPM_GETPOSFROMBUFFERID: {
        // VIEW 占最高 2 位（单视图恒 MAIN_VIEW=0），INDEX 占低 30 位
        const int idx = IndexByBufferId(src, (UINT_PTR)wp);
        if (idx < 0) return -1;
        return (LRESULT)((nn::kMainViewPos << 30) | (unsigned)idx);
    }
    case nn::NPPM_GETFULLPATHFROMBUFFERID: {
        // 上游契约：wParam=bufferID、lParam=缓冲(NULL 为查询长度)。返回值为
        // 路径的 wchar_t 数（不含 NUL）；无效 bufferID 返回 -1。调用方按
        // 查询结果分配"≥返回值+1"，故宿主声明尺寸必须用 p.size()+1（真实
        // 容量）——若硬编码 260，Debug CRT 会按声明尺寸校验写并破坏堆。
        Document* d = FindByBufferId(src, (UINT_PTR)wp);
        if (!d) return -1;
        const std::wstring p = d->path.wstring();
        if (lp) {
            wcsncpy_s(reinterpret_cast<wchar_t*>(lp), p.size() + 1, p.c_str(), _TRUNCATE);
        }
        return (LRESULT)p.size();
    }

    case nn::NPPM_GETNPPVERSION:
        // 报告兼容版本（主版本<<16 | 次版本）；满足 NppExec ≥5.1 门槛
        Logger::Debug("NPPM_GETNPPVERSION answered 8.6");
        return (LRESULT)((8 << 16) | 6);

    case nn::NPPM_GETPLUGINSCONFIGDIR: {
        std::error_code ec;
        std::filesystem::create_directories(ConfigDir(), ec);
        return WideStrTwoCall(ConfigDir(), wp, lp);
    }

    // ---- 菜单/命令句柄族 -----------------------------------------------------
    // NppExec 拿主菜单栏句柄后自行 ModifyMenu/CheckMenuItem（MF_BYCOMMAND
    // 递归命中 Plugins 子菜单里的命令项）；菜单结构仍由宿主构建。
    case nn::NPPM_GETMENUHANDLE: {
        if (wp == nn::NppPluginMenu) return reinterpret_cast<LRESULT>(pluginMenu_);
        if (wp == nn::NppMainMenu)   return reinterpret_cast<LRESULT>(mainMenu_);
        return 0;                     // 未知 menuChoice：明确拒答
    }
    case nn::NPPM_GETMENUBAR:
        return reinterpret_cast<LRESULT>(mainMenu_);
    case nn::NPPM_SETMENUITEMCHECK: {
        const UINT cmd = (UINT)wp;
        HMENU target = (cmd >= PluginCmdFirst) ? pluginMenu_ : mainMenu_;
        if (!target) return FALSE;
        ::CheckMenuItem(target, cmd, MF_BYCOMMAND |
                        (lp ? MF_CHECKED : MF_UNCHECKED));
        return TRUE;
    }
    case nn::NPPM_GETSHORTCUTBYCMDID: {
        // 上游契约：wParam=cmdID、lParam=ShortcutKey*（布尔×3+UCHAR，布局
        // 见 NppCompat.h）。只在插件命令上应答；无快捷键/未知命令返回 FALSE。
        auto* sk = reinterpret_cast<npp::ShortcutKey*>(lp);
        if (!sk) return FALSE;
        const unsigned cmd = (unsigned)wp;
        for (const auto& c : commands_) {
            if (c.id != cmd) continue;
            if (!c.hasKey) return FALSE;
            sk->ctrl  = (c.fVirt & FCONTROL) != 0;
            sk->alt   = (c.fVirt & FALT) != 0;
            sk->shift = (c.fVirt & FSHIFT) != 0;
            sk->key   = (unsigned char)c.vk;
            return TRUE;
        }
        return FALSE;
    }

    // ---- RUNCOMMAND 字符串族（两段式同上）-----------------------------------
    case nn::NPPM_GETFULLCURRENTPATH:
    case nn::NPPM_GETCURRENTDIRECTORY:
    case nn::NPPM_GETFILENAME:
    case nn::NPPM_GETNAMEPART:
    case nn::NPPM_GETEXTPART: {
        Document* d = src.Current();
        if (!d) return WideStrTwoCall(L"", wp, lp);
        const std::filesystem::path& P = d->path;
        switch (msg) {
        case nn::NPPM_GETFULLCURRENTPATH:   return WideStrTwoCall(P.wstring(), wp, lp);
        case nn::NPPM_GETCURRENTDIRECTORY:  return WideStrTwoCall(P.parent_path().wstring(), wp, lp);
        case nn::NPPM_GETFILENAME:          return WideStrTwoCall(P.filename().wstring(), wp, lp);
        case nn::NPPM_GETNAMEPART:          return WideStrTwoCall(P.stem().wstring(), wp, lp);
        default: { // EXTPART：上游语义不含点
            std::wstring ext = P.extension().wstring();
            if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
            return WideStrTwoCall(ext, wp, lp);
        }
        }
    }
        case nn::NPPM_GETCURRENTWORD: {
            Document* d = src.Current();
            std::string sel;
            if (d && d->editor.Hwnd()) {
                const sptr_t bytes = Sci(d->editor.Hwnd(), SCI_GETSELTEXT, 0, 0);
                if (bytes > 0) {
                    // SCI_GETSELTEXT 返回字节数（不含 NUL），写入仍补结尾
                    // NUL，缓冲区多留 1；旧代码误按"含 NUL"再 -1 少最后一字符
                    sel.resize((size_t)bytes + 1);
                    Sci(d->editor.Hwnd(), SCI_GETSELTEXT, 0,
                        (LPARAM)sel.data());
                    sel.resize((size_t)bytes);
                }
            }
            return WideStrTwoCall(Utf8ToWide(sel), wp, lp);
        }

    case nn::NPPM_ALLOCATECMDID: {
        const int n = (int)wp;
        auto* out = reinterpret_cast<int*>(lp);
        if (n <= 0 || !out) return FALSE;
        // 动态命令 id 池：位于静态插件池上限之后、WORD 菜单 id 安全区内
        constexpr unsigned kPoolEnd = 16000u;
        if (nextAllocCmdId_ + (unsigned)n - 1 > kPoolEnd) return FALSE;
        *out = (int)nextAllocCmdId_;
        nextAllocCmdId_ += (unsigned)n;
        return TRUE;
    }

    // ---- 可停靠对话框族（4d）------------------------------------------------
    // 落地端 DockHost 由 MainWindow 注入（DockManager）；无宿主时明确拒答。
    case nn::NPPM_DMMREGASDCKDLG: {
        if (!dockHost_ || !lp) return FALSE;
        return dockHost_->DockWidget(
                   *reinterpret_cast<const npp::DockedWidgetData*>(lp))
                   ? TRUE : FALSE;
    }
    case nn::NPPM_DMMSHOW:
        return (dockHost_ && lp) && dockHost_->Show((HWND)lp) ? TRUE : FALSE;
    case nn::NPPM_DMMHIDE:
        return (dockHost_ && lp) && dockHost_->Hide((HWND)lp) ? TRUE : FALSE;
    case nn::NPPM_DMMUPDATEDISPINFO: {
        if (dockHost_ && lp) dockHost_->UpdateDisplayInfo((HWND)lp);
        return TRUE;                              // 官方契约：无失败路径
    }
    case nn::NPPM_DMMVIEWOTHERTAB: {
        if (!dockHost_ || !lp) return FALSE;
        return dockHost_->ShowByName(
                   reinterpret_cast<const wchar_t*>(lp)) ? TRUE : FALSE;
    }
    case nn::NPPM_DMMGETPLUGINHWNDBYNAME: {
        if (!dockHost_) return 0;
        const wchar_t* winName = wp ? reinterpret_cast<const wchar_t*>(wp) : nullptr;
        const wchar_t* modName = lp ? reinterpret_cast<const wchar_t*>(lp) : nullptr;
        return reinterpret_cast<LRESULT>(
            dockHost_->FindHwndByName(winName, modName));
    }

    default:
        return 0;   // 区间内未实现：明确拒答
    }
}

void PluginManager::BroadcastNppNotification(const void* scn) {
    if (!scn) return;
    // 只发给 NPP 形态插件（原生 ABI 插件没有 beNotified 概念）；
    // 快照语义：广播期间不增删 loaded_（UnloadAll 不在此调用路径）。
    for (auto& l : loaded_)
        if (l.kind == kLoadedNppCompat && l.npp)
            l.npp->Notify(scn);
    // 进程外代理：提取 code/idFrom 重发（ExecWire 通道重建同一 nmhdr 头）
    if (oopHost_) {
        auto* nm = static_cast<const SCNotification*>(scn);
        oopHost_->BroadcastNotify(static_cast<int>(nm->nmhdr.code),
                                  nm->nmhdr.idFrom);
    }
}

void PluginManager::EmitNppNotification(int code, UINT_PTR idFrom) {
    // SCNotification 布局取自我们自己的 Scintilla.h；nmhdr 兼容头打头，
    // 与 NPP 插件的 beNotified 收到的结构一致（接口事实，见 §5.1/§5.6）。
    SCNotification scn{};
    scn.nmhdr.hwndFrom = hostWnd_;   // 上游契约：NPPN_* 的 hwndFrom=主窗口
    scn.nmhdr.idFrom = idFrom;       // BufferID（Document* 空间；无则 0）
    scn.nmhdr.code = static_cast<unsigned int>(code);
    BroadcastNppNotification(&scn);
}

} // namespace xfs
