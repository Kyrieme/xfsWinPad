#include "PluginAdminDialog.h"
#include "PluginCatalog.h"
#include "PluginRegistry.h"
#include "PluginInstaller.h"
#include "PluginManager.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wininet.h>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace xfs {

namespace {

constexpr unsigned IDC_TAB    = 3101;
constexpr unsigned IDC_SEARCH = 3102;
constexpr unsigned IDC_LIST   = 3103;
constexpr unsigned IDC_INFO   = 3104;
constexpr unsigned IDC_STATUS = 3105;
constexpr unsigned IDC_MAIN   = 3106;
constexpr unsigned IDC_CLOSE  = 3107;
constexpr unsigned IDC_LINK   = 3108;   // 插件清单仓库跳转 SysLink

// nppPluginList 仓库（插件清单 + 安装包）。Notepad++ 官方插件列表/分发仓。
constexpr wchar_t kCatalogRepoUrl[] =
    L"https://github.com/notepad-plus-plus/nppPluginList";

// 后台安装线程 → 对话框（数值载荷，避免跨线程指针/字符串生命周期问题）。
// WM_INSTALL_PROGRESS：wParam=percent|(jobIndex<<16)，lParam=InstallProgress::Phase。
// WM_INSTALL_DONE    ：wParam=成功数，lParam=总任务数。
constexpr UINT WM_INSTALL_PROGRESS = WM_APP + 1;
constexpr UINT WM_INSTALL_DONE     = WM_APP + 2;

// 四标签：可用 / 更新 / 已安装 / 不兼容（索引与 tab 顺序一致）。
enum Page : int { kAvailable = 0, kUpdates, kInstalled, kIncompatible };

// 当前页一行（逻辑行 = ListView 行，序一致）。
struct Row {
    std::wstring name;         // 显示名
    std::wstring versionText;  // 版本列文本
    std::wstring desc;         // 描述列
    std::wstring folder;       // 动作键（CatalogPlugin.folderName）
};

// 后台安装任务（UI 线程在启动线程前捕获，之后只读）。
struct Job {
    std::wstring folder;
    std::wstring version;
    std::wstring url;      // catalog.repository（可为空 → 仅用本地包）
    std::wstring name;     // 显示名（进度/报错文案）
};

// 后台安装结果。worker 写入后 PostMessage 再退出；对话框在收到
// WM_INSTALL_DONE 后读取（消息队列天然 happens-before）。
struct InstallResult {
    int total = 0;
    std::vector<int> okIndices;   // 成功安装的 job 下标
    bool failed = false;
    std::wstring failName;        // 首个失败插件显示名
    std::wstring failMessage;     // 首个失败原因
};

struct State {
    HINSTANCE inst = nullptr;
    HWND hTab = nullptr, hSearch = nullptr, hList = nullptr;
    HWND hInfo = nullptr, hStatus = nullptr, hMain = nullptr, hClose = nullptr;
    PluginManager* mgr = nullptr;   // 不兼容页读取加载失败清单（可空）

    PluginCatalog catalog;
    PluginRegistry registry;
    PluginInstaller installer;
    int page = kAvailable;
    std::vector<Row> rows;

    // 后台安装相关
    std::vector<Job> jobs;
    std::shared_ptr<InstallResult> result;
    std::thread work;
    bool working = false;

    explicit State(const std::wstring& pluginsDir)
        : registry(pluginsDir), installer(pluginsDir) {}
    ~State() { if (work.joinable()) work.join(); }

    static State* Get(HWND h) {
        return (State*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    }
};

int Dpi(HWND h) { return ::GetDpiForWindow(h); }
int U(HWND h, int px) { const int d = Dpi(h); return ::MulDiv(px, d, 96); }

// ---------- ListView 帮助（直接发 LVM_*，不依赖 _WIN32_IE 宏） ----------
int ListViewCount(HWND lv) { return (int)::SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0); }
void InsertColumn(HWND lv, int idx, int width, const wchar_t* title) {
    LVCOLUMNW c{};
    c.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    c.cx = width; c.pszText = const_cast<wchar_t*>(title); c.iSubItem = idx;
    ::SendMessageW(lv, LVM_INSERTCOLUMNW, idx, (LPARAM)&c);
}
void SetItemText(HWND lv, int row, int col, const std::wstring& text) {
    LVITEMW it{};
    it.mask = LVIF_TEXT; it.iItem = row; it.iSubItem = col;
    it.pszText = const_cast<wchar_t*>(text.c_str());
    ::SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&it);
}
int AddItem(HWND lv, const std::wstring& name) {
    LVITEMW it{};
    it.mask = LVIF_TEXT; it.iItem = INT_MAX; it.iSubItem = 0;
    it.pszText = const_cast<wchar_t*>(name.c_str());
    return (int)::SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
}
void SetChecked(HWND lv, int row, bool checked) {
    LVITEMW it{};
    it.mask = LVIF_STATE; it.stateMask = LVIS_STATEIMAGEMASK;
    it.state = (checked ? 2 : 1) << 12;
    ::SendMessageW(lv, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&it);
}
bool GetChecked(HWND lv, int row) {
    const LRESULT s = ::SendMessageW(lv, LVM_GETITEMSTATE, (WPARAM)row,
                                     (LPARAM)LVIS_STATEIMAGEMASK);
    return (int)((s >> 12) - 1) == 1;
}
void SelectRow(HWND lv, int row) {
    LVITEMW it{};
    it.mask = LVIF_STATE; it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    ::SendMessageW(lv, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&it);
    ::SendMessageW(lv, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
}
int SelectedRow(HWND lv) {
    return (int)::SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, (LPARAM)LVNI_SELECTED);
}
void ClearAllRows(HWND lv) { ::SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0); }
void EnableRowChecks(HWND lv, bool on) {
    DWORD ex = LVS_EX_FULLROWSELECT;
    if (on) ex |= LVS_EX_CHECKBOXES;
    ::SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, on ? LVS_EX_CHECKBOXES : 0, (LPARAM)ex);
}

/* 标签页标题。程序化创建的 Tab 控件不处理 item 文本里的 "&" 助记符，
   只会原样渲染；若保留 "(&V)" 后缀，可见汉字会被推到偏左位置，无法居中。
   这里仅返回纯文本，使系统在固定宽度标签内按 DT_CENTER 居中显示。 */
const wchar_t* PageTabTitle(int page) {
    switch (page) {
        case kAvailable:    return Tr(L"plugadmin.tab.available");
        case kUpdates:      return Tr(L"plugadmin.tab.updates");
        case kInstalled:    return Tr(L"plugadmin.tab.installed");
        default:            return Tr(L"plugadmin.tab.incompat");
    }
}
const wchar_t* PageMainButton(int page) {
    switch (page) {
        case kAvailable:    return Tr(L"plugadmin.btn.install");
        case kUpdates:      return Tr(L"plugadmin.btn.update");
        case kInstalled:    return Tr(L"plugadmin.btn.remove");
        default:            return Tr(L"plugadmin.btn.start");
    }
}

// ---------- 按当前页构造逻辑行 ----------
// 失败原因令牌 → 本地化短文案（版本列位置展示）。
std::wstring IncompatReasonText(const std::wstring& token,
                                const std::wstring& path) {
    (void)path;
    if (token == L"arch")  return Tr(L"plugadmin.incompat.arch");
    if (token == L"abi")   return Tr(L"plugadmin.incompat.abi");
    if (token == L"export") return Tr(L"plugadmin.incompat.export");
    if (token == L"ansi")  return Tr(L"plugadmin.incompat.ansi");
    if (token == L"fatal") return Tr(L"plugadmin.incompat.fatal");
    return Tr(L"plugadmin.incompat.load");
}

void BuildRows(State& st) {
    st.rows.clear();
    switch (st.page) {
        case kAvailable: {
            for (const auto& p : st.catalog.plugins) {
                if (st.registry.IsInstalled(p.folderName)) continue;
                Row r;
                r.name = p.displayName.empty() ? p.folderName : p.displayName;
                r.versionText = p.version; r.desc = p.description; r.folder = p.folderName;
                st.rows.push_back(std::move(r));
            }
            break;
        }
        case kUpdates: {
            for (const auto& ip : st.registry.Installed()) {
                const CatalogPlugin* cat = nullptr;
                for (const auto& p : st.catalog.plugins)
                    if (p.folderName == ip.folder) { cat = &p; break; }
                if (!cat) continue;
                if (ComparePluginVersions(cat->version, ip.version) <= 0) continue;
                Row r;
                r.name = cat->displayName.empty() ? ip.folder : cat->displayName;
                r.versionText = ip.version + L"  →  " + cat->version;
                r.desc = cat->description; r.folder = ip.folder;
                st.rows.push_back(std::move(r));
            }
            break;
        }
        case kInstalled: {
            for (const auto& ip : st.registry.Installed()) {
                std::wstring name = ip.folder;
                std::wstring desc;
                for (const auto& p : st.catalog.plugins)
                    if (p.folderName == ip.folder) { if (!p.displayName.empty()) name = p.displayName; desc = p.description; break; }
                Row r;
                r.name = name; r.versionText = ip.version; r.desc = desc; r.folder = ip.folder;
                st.rows.push_back(std::move(r));
            }
            break;
        }
        case kIncompatible: {
            // 最近一次启动加载失败的 DLL：文件名 / 失败原因 / 完整路径。
            // 只读展示（无勾选、主按钮禁用）；空清单由状态行说明。
            if (!st.mgr) break;
            for (const auto& f : st.mgr->LoadFailures()) {
                Row r;
                r.folder = f.path;
                r.name = std::filesystem::path(f.path).filename().wstring();
                r.versionText = IncompatReasonText(f.reason, f.path);
                r.desc = f.path;
                st.rows.push_back(std::move(r));
            }
            break;
        }
        default:
            break;
    }
}

// ---------- 填充 ListView ----------
void RefillList(State& st) {
    ClearAllRows(st.hList);
    const bool withChecks = (st.page == kAvailable || st.page == kUpdates);
    EnableRowChecks(st.hList, withChecks);
    for (const Row& r : st.rows) {
        const int idx = AddItem(st.hList, r.name);
        SetItemText(st.hList, idx, 1, r.versionText);
        SetItemText(st.hList, idx, 2, r.desc);
    }
}

void SetStatus(State& st, const std::wstring& msg) {
    ::SetWindowTextW(st.hStatus, msg.c_str());
}

void SetInfo(State& st, bool usedDefaults) {
    // 行 1：明确告知离线安装包(ZIP)应放的真实目录。
    std::wstring txt = Tr(L"plugadmin.offline") + st.installer.DownloadDir();
    if (usedDefaults && st.catalog.listName.empty())
        txt += Tr(L"plugadmin.offline.defaults");
    ::SetWindowTextW(st.hInfo, txt.c_str());
}

// ---------- 后台安装：收集勾选 → 工作线程下载+解压+落盘 → 完成消息刷新 ----------
void StartInstallJob(State& st, HWND h) {
    if (st.working) return;
    const bool isUpdate = (st.page == kUpdates);
    const wchar_t* action = isUpdate ? Tr(L"plugadmin.action.update")
                                     : Tr(L"plugadmin.action.install");

    // 收集勾选行 → Job（folder/version/url/name）。
    st.jobs.clear();
    for (size_t i = 0; i < st.rows.size(); ++i) {
        if (!GetChecked(st.hList, (int)i)) continue;
        const Row& r = st.rows[i];
        Job job;
        job.folder = r.folder;
        job.version = r.versionText;   // 更新页此列为 "old → new"，下面重取
        job.name = r.name;
        for (const auto& p : st.catalog.plugins)
            if (p.folderName == r.folder) {
                job.version = p.version;
                job.url = p.repository;
                break;
            }
        st.jobs.push_back(std::move(job));
    }
    if (st.jobs.empty()) {
        SetStatus(st, I18n::Instance().Fmt(L"plugadmin.select.none", {action}));
        return;
    }

    st.result = std::make_shared<InstallResult>();
    st.result->total = (int)st.jobs.size();

    // 冻结 UI（工作期间禁止换页/改选/关闭）。
    st.working = true;
    ::EnableWindow(st.hMain, FALSE);
    ::EnableWindow(st.hClose, FALSE);
    ::EnableWindow(st.hTab, FALSE);
    ::EnableWindow(st.hSearch, FALSE);
    ::EnableWindow(st.hList, FALSE);
    SetStatus(st, I18n::Instance().Fmt(L"plugadmin.working",
                {action, std::to_wstring((int)st.jobs.size())}));

    const std::vector<Job> jobs = st.jobs;   // 线程内只读快照
    const HWND hwnd = h;
    const std::shared_ptr<InstallResult> result = st.result;
    PluginInstaller* installer = &st.installer;

    st.work = std::thread([hwnd, jobs, result, installer] {
        for (size_t i = 0; i < jobs.size(); ++i) {
            const Job& j = jobs[i];
            std::wstring err;
            auto progress = [hwnd, i](const InstallProgress& p) {
                const int packed = p.percent | ((int)i << 16);
                ::PostMessageW(hwnd, WM_INSTALL_PROGRESS,
                               (WPARAM)packed, (LPARAM)p.phase);
            };
            if (installer->Install(j.folder, j.version, j.url, progress, &err)) {
                result->okIndices.push_back((int)i);
            } else {
                result->failed = true;
                if (result->failName.empty()) {
                    result->failName = j.name;
                    result->failMessage = err;
                }
            }
        }
        ::PostMessageW(hwnd, WM_INSTALL_DONE,
                       (WPARAM)result->okIndices.size(), (LPARAM)result->total);
    });
}

// ---------- 底部主按钮动作 ----------
void OnMainAction(State& st, HWND parent) {
    if (st.page == kAvailable || st.page == kUpdates) {
        StartInstallJob(st, parent);
        return;
    }
    if (st.page == kInstalled) {
        const int sel = SelectedRow(st.hList);
        if (sel < 0 || sel >= (int)st.rows.size()) {
            SetStatus(st, Tr(L"plugadmin.remove.none"));
            return;
        }
        const Row& r = st.rows[sel];
        const bool clean = st.registry.TryUninstall(r.folder);
        BuildRows(st); RefillList(st);
        if (clean) SetStatus(st, I18n::Instance().Fmt(L"plugadmin.removed", {r.name}));
        else {
            std::wstring path;
            for (const auto& ip : st.registry.Installed())
                if (ip.folder == r.folder) { path = ip.path; break; }
            ::MessageBoxW(parent,
                I18n::Instance().Fmt(L"plugadmin.dll.body", {r.name, path}).c_str(),
                Tr(L"plugadmin.dll.title"), MB_OK | MB_ICONWARNING);
            SetStatus(st, Tr(L"plugadmin.dll.status"));
        }
        return;
    }
}

// ---------- 实时搜索：定位到当前页第一个名称/目录匹配项 ----------
void OnSearchChange(State& st) {
    wchar_t buf[256]{};
    ::GetWindowTextW(st.hSearch, buf, 256);
    std::wstring q = buf;
    if (q.empty()) {
        ClearAllRows(st.hList);
        RefillList(st);
        return;
    }
    for (size_t i = 0; i < st.rows.size(); ++i) {
        const Row& r = st.rows[i];
        const bool hit = q.size() <= r.name.size()
                ? (_wcsnicmp(r.name.c_str(), q.c_str(), q.size()) == 0 ||
                   _wcsnicmp(r.folder.c_str(), q.c_str(), q.size()) == 0)
                : false;
        if (hit) { SelectRow(st.hList, (int)i); return; }
    }
}

void SwitchPage(State& st, int page) {
    st.page = page;
    const bool incompatible = (page == kIncompatible);
    // 列头随页语义切换：常规页 = 名称/版本/描述；不兼容页 = 文件/原因/路径。
    const wchar_t* cols[3] = {
        incompatible ? Tr(L"plugadmin.incompat.col.file")
                     : Tr(L"plugadmin.col.name"),
        incompatible ? Tr(L"plugadmin.incompat.col.reason")
                     : Tr(L"plugadmin.col.version"),
        incompatible ? Tr(L"plugadmin.incompat.col.path")
                     : Tr(L"plugadmin.col.desc"),
    };
    LVCOLUMNW cv{};
    cv.mask = LVCF_TEXT;
    for (int i = 0; i < 3; ++i) {
        cv.pszText = const_cast<wchar_t*>(cols[i]);
        cv.iSubItem = i;
        ::SendMessageW(st.hList, LVM_SETCOLUMNW, (WPARAM)i, (LPARAM)&cv);
    }
    ::SetWindowTextW(st.hMain, PageMainButton(page));
    ::EnableWindow(st.hMain, !incompatible);
    ::ShowWindow(st.hList, SW_SHOW);   // 不兼容页也显示列表（可能为空）
    BuildRows(st); RefillList(st);
    if (incompatible)
        SetStatus(st, st.rows.empty() ? Tr(L"plugadmin.incompat.empty")
                                      : Tr(L"plugadmin.incompat.note"));
    else
        SetStatus(st, L"");
}

/* 仓库链接子类化处理（替代 WC_LINK）：悬停显示手型光标，单击打开浏览器。
   原 WPProc 存于 GWLP_USERDATA，控件销毁时还原，保证可正常回收。 */
LRESULT CALLBACK LinkSubProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC orig = (WNDPROC)::GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_SETCURSOR:
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_LBUTTONUP: {
            HWND owner = ::GetWindow(::GetParent(h), GW_OWNER);
            ::ShellExecuteW(owner, L"open", kCatalogRepoUrl, nullptr, nullptr,
                            SW_SHOWNORMAL);
            return 0;
        }
        case WM_NCDESTROY:
            if (orig) ::SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)orig);
            break;
    }
    return ::CallWindowProcW(orig, h, msg, wp, lp);
}

LRESULT CALLBACK AdminProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = State::Get(h);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_COMMAND:
            if (!st) return 0;
            switch (LOWORD(wp)) {
                case IDC_MAIN:   OnMainAction(*st, h); return 0;
                case IDC_CLOSE:
                case IDCANCEL:
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
                    return 0;
                case IDC_SEARCH:
                    switch (HIWORD(wp)) {
                        case EN_CHANGE: OnSearchChange(*st); return 0;
                    }
                    break;
            }
            break;
        case WM_NOTIFY: {
            const NMHDR* hdr = (const NMHDR*)lp;
            if (!st || hdr->code != TCN_SELCHANGE) break;
            const int sel = (int)::SendMessageW(st->hTab, TCM_GETCURSEL, 0, 0);
            SwitchPage(*st, sel);
            return 0;
        }
        case WM_INSTALL_PROGRESS: {
            if (!st) return 0;
            const int idx = (int)(wp >> 16);
            const int percent = (int)(wp & 0xFFFF);
            const int phase = (int)lp;
            std::wstring name = L"...";
            if (idx >= 0 && idx < (int)st->jobs.size()) name = st->jobs[idx].name;
            std::wstring msg;
            switch (phase) {
                case InstallProgress::kDownloading:
                    msg = I18n::Instance().Fmt(L"plugadmin.prog.download",
                          {name, std::to_wstring(percent)});
                    break;
                case InstallProgress::kExtracting:
                    msg = I18n::Instance().Fmt(L"plugadmin.prog.extract", {name});
                    break;
                case InstallProgress::kInstalling:
                    msg = I18n::Instance().Fmt(L"plugadmin.prog.write", {name});
                    break;
                default:
                    msg = I18n::Instance().Fmt(L"plugadmin.prog.generic", {name});
            }
            SetStatus(*st, msg);
            return 0;
        }
        case WM_INSTALL_DONE: {
            if (!st || !st->result) return 0;
            const int okCount = (int)wp;
            const int total = (int)lp;
            st->working = false;
            if (st->work.joinable()) st->work.join();   // 回收已结束的 worker，允许再次安装

            // 成功项登记进本地记录（真实文件已由 worker 落盘）。
            for (int idx : st->result->okIndices)
                if (idx >= 0 && idx < (int)st->jobs.size())
                    st->registry.MarkInstalled(st->jobs[idx].folder,
                                               st->jobs[idx].version);

            // 即时加载（N++ 需重启才生效；我们直接热载）：扫描新落盘的 DLL，
            // 走启动同款管线；成功后经 onChanged 回调实时重建插件菜单。
            if (st->mgr && !st->result->okIndices.empty())
                st->mgr->LoadNew();

            // 恢复 UI。
            const bool enabled = (st->page != kIncompatible);
            ::EnableWindow(st->hMain, enabled);
            ::EnableWindow(st->hClose, TRUE);
            ::EnableWindow(st->hTab, TRUE);
            ::EnableWindow(st->hSearch, TRUE);
            ::EnableWindow(st->hList, TRUE);

            BuildRows(*st); RefillList(*st);

            if (st->result->failed) {
                std::wstring msg = I18n::Instance().Fmt(L"plugadmin.fail.fmt",
                                   {st->result->failName, st->result->failMessage});
                SetStatus(*st, msg);
                ::MessageBoxW(h, msg.c_str(), Tr(L"plugadmin.dll.title"),
                              MB_OK | MB_ICONWARNING);
            } else {
                const wchar_t* action = (st->page == kUpdates)
                    ? Tr(L"plugadmin.action.update") : Tr(L"plugadmin.action.install");
                SetStatus(*st, I18n::Instance().Fmt(L"plugadmin.done.fmt",
                          {std::to_wstring(okCount), std::to_wstring(total), action}));
            }
            return 0;
        }
        case WM_CLOSE:
            if (st && st->working) return 0;   // 安装进行中禁止关闭
            ::EnableWindow(::GetWindow(h, GW_OWNER), TRUE);
            ::SetForegroundWindow(::GetWindow(h, GW_OWNER));
            ::DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            delete st;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void PluginAdminDialog::Run(HWND parent, HINSTANCE hInst, PluginManager* mgr) {
    // 窗口类只注册一次
    static const wchar_t kClass[] = L"xfsWinPadPluginAdmin";
    static bool regDone = false;
    if (!regDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AdminProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        regDone = true;
    }

    const int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return ::MulDiv(px, dpi, 96); };

    // 构建数据（清单 + 已安装注册表）
    State* st = new State(PluginManager::PluginDir());
    st->inst = hInst;
    st->mgr = mgr;
    bool usedDefaults = false;
    LoadPluginCatalog(PluginManager::PluginDir(), st->catalog, usedDefaults);
    st->registry.Refresh();

    HFONT font = ::CreateFontW(-u(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const int W = u(700), H = u(560);
    RECT rc{0, 0, W, H};
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr{}; ::GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    const int y = pr.top + u(30);

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, Tr(L"plugadmin.title"),
                                 gstyle, x, y, rc.right - rc.left,
                                 rc.bottom - rc.top, parent, nullptr, hInst, st);
    if (!dlg) { delete st; ::DeleteObject(font); return; }

    const int w = W - u(16);            // 内宽
    st->hTab = ::CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH | WS_TABSTOP,
        u(8), u(8), w, u(26), dlg, (HMENU)(UINT_PTR)IDC_TAB, hInst, nullptr);
    for (int i = 0; i < 4; ++i) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        std::wstring t = PageTabTitle(i);
        ti.pszText = const_cast<wchar_t*>(t.c_str());
        ::SendMessageW(st->hTab, TCM_INSERTITEMW, i, (LPARAM)&ti);
    }
    // 四页等宽（TCS_FIXEDWIDTH 下由宿主设置项宽）
    ::SendMessageW(st->hTab, TCM_SETITEMSIZE, 0,
                   MAKELPARAM((w - u(8)) / 4, u(26)));
    ::SendMessageW(st->hTab, WM_SETFONT, (WPARAM)font, TRUE);

    // 搜索行：搜索框与其前导 label 用同高同顶对齐，label 加 SS_CENTERIMAGE
    // 使其文本在 22px 高内垂直居中，与编辑框内容基线对齐（任意 DPI 下都居中）。
    constexpr int kSearchX0 = 8, kSearchH = 22, kSearchY = 40, kFontW = 44;
    HWND lbl = ::CreateWindowExW(0, L"STATIC", Tr(L"plugadmin.search"),
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        u(kSearchX0), u(kSearchY), u(kFontW), u(kSearchH),
        dlg, nullptr, hInst, nullptr);
    ::SendMessageW(lbl, WM_SETFONT, (WPARAM)font, TRUE);
    st->hSearch = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
        u(kSearchX0 + kFontW), u(kSearchY), w - u(kSearchX0 + kFontW), u(kSearchH),
        dlg, (HMENU)(UINT_PTR)IDC_SEARCH, hInst, nullptr);
    ::SendMessageW(st->hSearch, WM_SETFONT, (WPARAM)font, TRUE);

    // 列表
    st->hList = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        u(8), u(66), w, H - u(66) - u(176), dlg,
        (HMENU)(UINT_PTR)IDC_LIST, hInst, nullptr);
    ::SendMessageW(st->hList, WM_SETFONT, (WPARAM)font, TRUE);
    InsertColumn(st->hList, 0, u(250), Tr(L"plugadmin.col.name"));
    InsertColumn(st->hList, 1, u(124), Tr(L"plugadmin.col.version"));
    InsertColumn(st->hList, 2, u(300), Tr(L"plugadmin.col.desc"));

    // 底部三行：安装包位置提示 / 插件仓库来源 / 状态，右侧按钮。
    // 行 1：离线安装包应该放哪（真实 downloads 目录）。y 需低于列表底(H-176)，避免被列表重绘覆盖。
    st->hInfo = ::CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE, u(8), H - u(170), w, u(16),
        dlg, (HMENU)(UINT_PTR)IDC_INFO, hInst, nullptr);
    ::SendMessageW(st->hInfo, WM_SETFONT, (WPARAM)font, TRUE);
    // 行 2：插件清单 / 安装包来源 → nppPluginList 仓库（可点击跳转）。
    HWND lblSrc = ::CreateWindowExW(0, L"STATIC", Tr(L"plugadmin.src"),
        WS_CHILD | WS_VISIBLE, u(8), H - u(146), u(96), u(17),
        dlg, nullptr, hInst, nullptr);
    ::SendMessageW(lblSrc, WM_SETFONT, (WPARAM)font, TRUE);
    // 用子类化 STATIC 替代 WC_LINK：点击路径完全可控，悬停显示手型光标。
    HWND hLink = ::CreateWindowExW(0, L"STATIC", L"notepad-plus-plus/nppPluginList",
        WS_CHILD | WS_VISIBLE,
        u(8 + 96), H - u(146), w - u(8 + 96), u(20),
        dlg, (HMENU)(UINT_PTR)IDC_LINK, hInst, nullptr);
    ::SendMessageW(hLink, WM_SETFONT, (WPARAM)font, TRUE);
    ::SetWindowLongPtrW(hLink, GWLP_USERDATA,
        (LONG_PTR)::SetWindowLongPtrW(hLink, GWLP_WNDPROC, (LONG_PTR)LinkSubProc));
    // 行 3：状态（安装进度 / 提示）。
    st->hStatus = ::CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE, u(8), H - u(118), w - u(240), u(16),
        dlg, (HMENU)(UINT_PTR)IDC_STATUS, hInst, nullptr);
    ::SendMessageW(st->hStatus, WM_SETFONT, (WPARAM)font, TRUE);
    // 按钮：右下
    st->hClose = ::CreateWindowExW(0, L"BUTTON", Tr(L"sc.close"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        w - u(92), H - u(46), u(76), u(28), dlg,
        (HMENU)(UINT_PTR)IDC_CLOSE, hInst, nullptr);
    ::SendMessageW(st->hClose, WM_SETFONT, (WPARAM)font, TRUE);
    st->hMain = ::CreateWindowExW(0, L"BUTTON", Tr(L"plugadmin.btn.install"),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        w - u(188), H - u(46), u(90), u(28), dlg,
        (HMENU)(UINT_PTR)IDC_MAIN, hInst, nullptr);
    ::SendMessageW(st->hMain, WM_SETFONT, (WPARAM)font, TRUE);

    SetInfo(*st, usedDefaults);
    SwitchPage(*st, kAvailable);
    ::ShowWindow(dlg, SW_SHOW);

    // 模态消息循环
    MSG m;
    while (::IsWindow(dlg) && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &m)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    ::EnableWindow(parent, TRUE);
    ::DeleteObject(font);
}

} // namespace xfs