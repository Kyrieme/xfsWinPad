#include "MainWindow.h"
#include "version.h"
#include "StatusBar.h"
#include "FindDialog.h"
#include "InputBox.h"
#include "ListPicker.h"
#include "ResultsPanel.h"
#include "WindowsListDialog.h"
#include "../core/CommandIds.h"
#include "../core/I18n.h"
#include "../core/JsonLite.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../document/Document.h"
#include "../editor/Editor.h"
#include "../language/LanguageMap.h"
#include "../language/XfsLexer.h"     // 批次 73：Chroma 族词法器名（模型来源标注）
#include "../resources/resource.h"
#include "../search/FindInFiles.h"
#include "../search/SearchAll.h"
#include "../search/SearchService.h"
#include "../session/Session.h"
#include "../shortcut/ShortcutTable.h"
#include "../theme/Styler.h"
#include <shlobj.h>
#include "../theme/Theme.h"
#include "../workspace/Workspace.h"
#include "../plugin/Workshop.h"
#include "../plugin/oop/OopHost.h"   // OOP messageProc 桥（v2.1，批次 71）
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <algorithm>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdiplus.lib")

namespace xfs {
namespace {

constexpr wchar_t kClassName[] = L"xfsWinPadMainWindow";
constexpr wchar_t kHostClassName[] = L"xfsWinPadEditorHost";
constexpr wchar_t kSplitterClassName[] = L"xfsWinPadSplitViewDivider";
constexpr UINT_PTR kPluginEvtTimerId = 2;   // coalesced plugin event flush
constexpr UINT_PTR kAiReloadTimer = 4;      // AI 改盘延迟对比（3s）

// Plain container for the tab strip + editor controls.
// Forwards child notifications up to the main window.
LRESULT CALLBACK HostWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NOTIFY:
        case WM_COMMAND:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
            return ::SendMessageW(::GetParent(h), msg, wp, lp);
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

// Draggable divider between the two split-view editors. The owning
// MainWindow* is stored in GWLP_USERDATA at WM_NCCREATE; drags update the
// left pane width proportion and relayout the two hosts. Hovering highlights
// the divider (same look as the bottom-panel splitters).
LRESULT CALLBACK SplitterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return TRUE;
    }
    MainWindow* self =
        reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_LBUTTONDOWN:
            self->SetSplitterHot(true);
            InvalidateRect(h, nullptr, FALSE);
            SetCapture(h);
            return 0;
        case WM_MOUSEMOVE: {
            if (self && (wp & MK_LBUTTON)) {
                POINT pt; GetCursorPos(&pt);
                MapWindowPoints(nullptr, self->Hwnd(), &pt, 1);
                self->MoveSplitter(pt.x);
                return 0;
            }
            // hover highlight (arm mouse-leave so it clears when the cursor leaves)
            if (self) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                if (!self->SplitterHot()) {
                    self->SetSplitterHot(true);
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self && self->SplitterHot()) {
                self->SetSplitterHot(false);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (self && self->SplitterHot()) {
                self->SetSplitterHot(false);
                InvalidateRect(h, nullptr, FALSE);
            }
            if (GetCapture() == h) ReleaseCapture();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            bool hot = self && self->SplitterHot();
            HBRUSH br = CreateSolidBrush(hot ? RGB(0xCC, 0xE3, 0xFF)
                                             : RGB(0xE9, 0xE9, 0xE9));
            FillRect(dc, &rc, br);
            DeleteObject(br);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

MainWindow::~MainWindow() {
    if (accel_) DestroyAcceleratorTable(accel_);
    for (HMENU pop : pluginPopups_)
        if (pop) DestroyMenu(pop);
    if (menu_) DestroyMenu(menu_);
}

bool MainWindow::RegisterWindowClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon   = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(1),
                                   IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW hc{};
    hc.cbSize = sizeof(wc);
    hc.lpfnWndProc = HostWndProc;
    hc.hInstance = inst_;
    hc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    hc.lpszClassName = kHostClassName;
    if (RegisterClassExW(&hc) == FALSE) return false;

    WNDCLASSEXW sc{};
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = SplitterProc;
    sc.hInstance = inst_;
    sc.hCursor = LoadCursorW(nullptr, IDC_SIZEWE);
    sc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    sc.lpszClassName = kSplitterClassName;
    return RegisterClassExW(&sc) != FALSE;
}

namespace { }

static std::vector<std::wstring> LoadRecentFolders();

StartupOptions ParseCommandLine(LPCWSTR cmd) {
    StartupOptions opts;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd, &argc);
    if (!argv) return opts;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--line" && i + 1 < argc) {
            opts.gotoLine = _wtoi(argv[++i]);
        } else if (a == L"--readonly") {
            opts.readOnly = true;
        } else if (a == L"--search" && i + 1 < argc) {
            opts.search = argv[++i];
        } else if (a == L"--log" && i + 1 < argc) {
            opts.logFile = argv[++i];
        } else if (a == L"--diff") {
            opts.autoDiff = true;
        } else if (a == L"--new") {
            // 绕过单实例转发：显式开新进程/新窗口
            opts.forceNew = true;
        } else if (a == L"--no-restore") {
            opts.noRestore = true;   // 空白窗口：不恢复会话（New Window 命令）
        } else if (a == L"--restore" && i + 1 < argc) {
            opts.restoreFile = argv[++i];   // 恢复指定槽位会话（多窗口扇出）
        } else if (!a.empty() && a[0] != L'-') {
            opts.files.push_back(a);
        }
    }
    LocalFree(argv);
    return opts;
}

bool MainWindow::Create(HINSTANCE hInst, const StartupOptions& opts) {
    inst_ = hInst;
    startup_ = opts;

    SettingsLoad(SettingsFilePath(), &settings_);
    // 界面语言：加载字典后调用者即可用 Tr()；先 Load 再注册回调，
    // 启动期不触发重建（此时窗口/菜单尚未就绪），仅响应后续实时切换。
    I18n::Instance().Load(settings_.uiLang);
    I18n::Instance().AddCallback([this](const std::wstring&) { ApplyLanguage(); });
    // 主题/语法样式的用户数据层先于主题解析加载（阶段 2a）：
    // themes\*.json 注册进主题表，stylers.json 的覆盖层在 SetLexerByName 生效
    theme::LoadUserThemes();
    GlobalStyler().Load(GlobalStyler().FilePath());
    theme_ = theme::Find(settings_.theme.c_str());

    if (!RegisterWindowClass()) {
        Logger::Error("RegisterClass failed");
        return false;
    }

    BuildMenus();
    // 快捷键单一数据源：先载用户覆盖（shortcuts.json），再由表构建加速器
    GlobalShortcuts().Load(ShortcutTable::FilePath());
    BuildAccelerators();

    // recent folders menu (project roots)
    RebuildRecentFolderMenu(LoadRecentFolders());

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = (std::min<int>)(1400, work.right - work.left - 80);
    int h = (std::min<int>)(900, work.bottom - work.top - 80);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (settings_.hasWindow &&
        settings_.winW >= 320 && settings_.winH >= 240) {
        // restore only when the saved rect still lies mostly on a monitor.
        // 仅"有交集"不够：窗口原先在扩展屏上、退出后拔掉显示器时，保存
        // 坐标可能与主屏只剩 1px 相交，MonitorFromRect 判定成功 → 窗口
        // 开到屏幕边上不可见（用户报告）。要求相交面积 >= 窗口面积 40%，
        // 不达标回退 CW_USEDEFAULT。
        RECT saved{settings_.winX, settings_.winY,
                   settings_.winX + settings_.winW,
                   settings_.winY + settings_.winH};
        HMONITOR mon = MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);
        if (mon) {
            MONITORINFO mi{ sizeof(MONITORINFO) };
            if (GetMonitorInfoW(mon, &mi)) {
                RECT isect;
                if (!::IntersectRect(&isect, &saved, &mi.rcMonitor) ||
                    (isect.right - isect.left) * (isect.bottom - isect.top) * 5 <
                        settings_.winW * settings_.winH * 2) {
                    Logger::Info("Window restore: saved rect mostly off-screen "
                                 "(isect " + std::to_string(isect.right - isect.left) +
                                 "x" + std::to_string(isect.bottom - isect.top) +
                                 "), falling back to default position");
                    mon = nullptr;   // 交集不足 40%：视为无效位置
                }
            } else {
                Logger::Info("Window restore: GetMonitorInfoW failed");
            }
        } else {
            Logger::Info("Window restore: saved rect on missing monitor, "
                         "falling back to default position");
        }
        if (mon) {
            x = settings_.winX; y = settings_.winY; w = settings_.winW; h = settings_.winH;
        }
    }

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"xfsWinPad",
        WS_OVERLAPPEDWINDOW, x, y, w, h,
        nullptr, menu_, inst_, this);
    if (!hwnd_) {
        Logger::Error("CreateWindowEx(main) failed, gle=" + std::to_string(::GetLastError())
                      + " class=" + WideToUtf8(kClassName));
        return false;
    }

    editorHost_ = CreateWindowExW(0, kHostClassName, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 100, 100, hwnd_, nullptr, inst_, nullptr);
    if (!editorHost_) {
        Logger::Error("CreateWindowEx(host) failed, gle=" + std::to_string(::GetLastError()));
        return false;
    }

    // right/other view host + split divider (hidden until a doc is moved across)
    editorHost1_ = CreateWindowExW(0, kHostClassName, nullptr,
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 100, 100, hwnd_, nullptr, inst_, nullptr);
    if (!editorHost1_) {
        Logger::Error("CreateWindowEx(host1) failed, gle=" + std::to_string(::GetLastError()));
        return false;
    }
    splitter_ = CreateWindowExW(0, kSplitterClassName, nullptr,
        WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 4, 100,
        hwnd_, nullptr, inst_, this);
    if (!splitter_) {
        Logger::Error("CreateWindowEx(splitter) failed, gle="
                      + std::to_string(::GetLastError()));
        return false;
    }

    status_ = std::make_unique<StatusBar>();
    status_->Create(hwnd_, inst_);

    workspace_ = std::make_unique<Workspace>(*this);
    // 初始空白文档延后创建：CLI 文件与会话恢复各自决定是否需要（否则
    // 会话恢复的文件会叠在一个多余的空白标签后面）。
    workspace_->Init(editorHost_, inst_, settings_, *theme_, false);
    workspace_->SetRightHost(editorHost1_);
    // hide the hex panel when its file's document is closed
    workspace_->onOversizeFile = [this](const std::wstring& path, unsigned long long) {
        ToggleBigFileView(path);
    };
    workspace_->onDocumentClosed = [this](const Document* d) {
        if (hex_ && hex_->Visible() && d &&
            hex_->FilePath() == d->path.wstring()) {
            hex_->Hide();
            CheckMenuItem(menu_, Cmd::ViewHexView, MF_UNCHECKED);
            LayoutChildren();
        }
        if (csv_ && csv_->Visible() && d &&
            csv_->FilePath() == d->path.wstring()) {
            csv_->Hide();
            CheckMenuItem(menu_, Cmd::ViewCsvView, MF_UNCHECKED);
            LayoutChildren();
        }
    };
    CreateToolbar();

    DragAcceptFiles(hwnd_, TRUE);

    // crash recovery 询问挪到 Create 尾部主窗显示后：若在窗口可见前弹
    // MessageBox，强杀后重启会表现为"无窗口假死"（对话框悬空等点击）。

    // restore dock panel heights (0 = default)
    if (settings_.hexPanelH >= 90) hexHLogical_ = settings_.hexPanelH;
    if (settings_.resultsPanelH >= 90) rpHLogical_ = settings_.resultsPanelH;
    if (settings_.logPanelH >= 90) logHLogical_ = settings_.logPanelH;
    if (settings_.terminalPanelH >= 90) termHLogical_ = settings_.terminalPanelH;
    if (settings_.aiPanelW >= 360) aiWLogical_ = settings_.aiPanelW;

    // load plugins (commands surface in palette + Plugins menu); hand them
    // editor access through the workspace before any DLL registers
    plugins_ = std::make_unique<PluginManager>();
    plugins_->SetWorkspace(workspace_.get());
    plugins_->SetHostWindow(hwnd_);
    // 菜单句柄：NppExec 等 NPP 插件通过 NPPM_GETMENUHANDLE/GETMENUBAR 拿主
    // 菜单栏句柄后自行 ModifyMenu/CheckMenuItem（BuildMenus 已先于本处完成）
    plugins_->SetPluginMenus(pluginMenu_, menu_);
    // 4d: 插件可停靠对话框宿主（NPPM_DMM* 的落地端）
    dockMgr_ = std::make_unique<DockManager>();
    dockMgr_->Init(hwnd_, inst_);
    dockMgr_->onLayoutChanged = [this]() { LayoutChildren(); };
    plugins_->SetDockHost(dockMgr_.get());
    // 进程外插件桥：墓碑（曾杀死宿主）的插件改由代理进程承载，exit()/崩溃
    // 只死代理不死编辑器（ComparePlus 案例）。正常插件仍走进程内快路径。
    plugins_->EnableOopHost();
    // 会话中热载新装插件 → 实时重建插件菜单（插件管理器安装完成路径）
    plugins_->SetOnChanged([this]() {
        RebuildPluginMenu();
        BuildAccelerators();
    });
    plugins_->LoadAll();
    RebuildPluginMenu();
    BuildAccelerators();   // 重建：并入插件命令快捷键

    // 4c: 宿主初始化完成 → NPPN_READY（hwndFrom=主窗口，idFrom=0）
    if (plugins_) plugins_->EmitNppNotification(npp::NPPN_READY, 0);
    git_.SetMainWnd(hwnd_);

    // plugin host event sources (v3 hooks) + NPP 通知合成（4c）
    // BufferID 沿用 4b 的 Document* 空间（见 docs/plugin-system.md §5.6）。
    workspace_->onDocumentOpened = [this](const Document* d) {
        if (d && d->HasPath()) git_.RequestForPath(d->path.wstring());
        if (plugins_ && d) {
            plugins_->Raise(XFS_EVT_DOC_OPENED,
                            WideToUtf8(d->path.wstring()).c_str());
            plugins_->EmitNppNotification(npp::NPPN_FILEOPENED, (UINT_PTR)d);
        }
    };
    workspace_->onDocumentActivated = [this](const Document* d) {
        UpdateUndoRedoState();   // 换文档：撤销/重做按钮跟随新文档状态
        if (d && d->HasPath()) git_.RequestForPath(d->path.wstring());
        if (plugins_ && d) {
            plugins_->Raise(XFS_EVT_DOC_ACTIVATED,
                            WideToUtf8(d->path.wstring()).c_str());
            plugins_->EmitNppNotification(npp::NPPN_BUFFERACTIVATED, (UINT_PTR)d);
        }
    };
    workspace_->onDocumentSaved = [this](const Document* d) {
        if (d && d->HasPath()) git_.RequestForPath(d->path.wstring());
        if (plugins_ && d) {
            plugins_->Raise(XFS_EVT_DOC_SAVED,
                            WideToUtf8(d->path.wstring()).c_str());
            plugins_->EmitNppNotification(npp::NPPN_FILESAVED, (UINT_PTR)d);
        }
    };
    // 关闭回调在 RemoveAt 之后触发：d 悬空，仅用作 BufferID（不 dereference）
    workspace_->onDocumentClosed = [this](const Document* d) {
        UpdateUndoRedoState();   // 关文档后可能无文档 → 双灰
        if (plugins_ && d)
            plugins_->EmitNppNotification(npp::NPPN_FILECLOSED, (UINT_PTR)d);
    };

    // restore folder workspace from settings (project mode)
    if (!settings_.projectRoot.empty() &&
        std::filesystem::exists(settings_.projectRoot))
        SetProjectRoot(settings_.projectRoot, false);

    // --log <file>: open the log analyzer panel on startup
    if (!startup_.logFile.empty()) {
        ToggleLogPanel();
        if (logPanel_) logPanel_->LoadFile(startup_.logFile);
    }

    // 启动会话分派（批次 67 多实例：CLI 文件 / --restore 槽位 / primary 恢复
    // + 扇出 / 空白窗口），见 StartupSession()。
    StartupSession();

    UpdateWindow(hwnd_);
    if (settings_.winMax) ShowWindow(hwnd_, SW_MAXIMIZE);

    // crash recovery: check for autosaved files from previous crashed session.
    // 必须在主窗显示后询问——启动途中弹模态框会让进程看起来"无窗卡死"。
    CheckAutoSaveRecovery();

    // 上次退出时 AI 面板可见 → 启动即恢复（连接异步，不阻塞开屏）
    if (settings_.aiPanelVisible) ToggleAiPanel();

    // taskbar/title icons from embedded app icon
    HICON hIconBig = LoadIconW(inst_, MAKEINTRESOURCEW(1));
    HICON hIconSmall = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (hIconBig) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    Logger::Info("MainWindow created");

    // crash-safe autosave timer（间隔/开关来自首选项，默认 30 秒）
    if (settings_.autosaveEnabled)
        ::SetTimer(hwnd_, 1, (UINT)(std::max(5, std::min(600,
                          settings_.autosaveSeconds))) * 1000, nullptr);

    // plugins may want to know the app finished coming up
    if (plugins_) plugins_->Raise(XFS_EVT_APP_READY, nullptr);

    if (!startup_.search.empty()) ShowFindDialog(startup_.search);
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // give the modeless search dialog proper dialog-key navigation (Tab/Esc)
        if (findDlg_ && findDlg_->IsVisible() &&
            IsDialogMessageW(findDlg_->Hwnd(), &msg))
            continue;
        if (!TranslateAcceleratorW(hwnd_, accel_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}

// --- menus ----------------------------------------------------------------------

void MainWindow::BuildMenus() {
    auto popup = [](HMENU parent, const wchar_t* text) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(parent, MF_POPUP, (UINT_PTR)m, text);
        return m;
    };
    // 菜单尾注（\tCtrl+N）从快捷键表生成：文本里的手写尾注被丢弃，
    // 改键后菜单文字自动跟随（阶段 3；Scintilla 自处理键在表内为仅展示项）。
    auto item = [](HMENU m, const wchar_t* text, unsigned int id) {
        std::wstring t = text;
        const size_t tab = t.find(L'\t');
        if (tab != std::wstring::npos) t.resize(tab);
        const ShortcutInfo info = GlobalShortcuts().Get(id);
        if (info.Valid()) {
            t += L"\t";
            t += info.ToString();
        }
        AppendMenuW(m, MF_STRING, id, t.c_str());
    };
    auto sep = [](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };

    menu_ = CreateMenu();

    HMENU file = popup(menu_, Tr(L"menu.file"));
    item(file, Tr(L"menu.file.new"), Cmd::FileNew);
    item(file, Tr(L"menu.file.open"), Cmd::FileOpen);
    item(file, Tr(L"menu.file.openfolder"), Cmd::FileOpenFolder);
    recentFolderMenu_ = popup(file, Tr(L"menu.file.openrecentfolder"));
    recentMenu_ = popup(file, Tr(L"menu.file.openrecent"));
    sep(file);
    item(file, Tr(L"menu.file.save"), Cmd::FileSave);
    item(file, Tr(L"menu.file.saveas"), Cmd::FileSaveAs);
    item(file, Tr(L"menu.file.saveall"), Cmd::FileSaveAll);
    sep(file);
    item(file, Tr(L"menu.file.print"), Cmd::FilePrint);
    sep(file);
    item(file, Tr(L"menu.file.reload"), Cmd::FileReload);
    item(file, Tr(L"menu.file.close"), Cmd::FileClose);
    item(file, Tr(L"menu.file.closefolder"), Cmd::FileCloseFolder);
    item(file, Tr(L"menu.file.closeall"), Cmd::FileCloseAll);
    sep(file);
    item(file, Tr(L"menu.file.reopenclosed"), Cmd::ReopenClosedTab);
    sep(file);
    item(file, Tr(L"menu.file.newwindow"), Cmd::FileNewWindow);
    sep(file);
    item(file, Tr(L"menu.file.exit"), Cmd::FileExit);

    HMENU edit = popup(menu_, Tr(L"menu.edit"));
    item(edit, Tr(L"menu.edit.undo"), Cmd::EditUndo);
    item(edit, Tr(L"menu.edit.redo"), Cmd::EditRedo);
    sep(edit);
    item(edit, Tr(L"menu.edit.cut"), Cmd::EditCut);
    item(edit, Tr(L"menu.edit.copy"), Cmd::EditCopy);
    item(edit, Tr(L"menu.edit.paste"), Cmd::EditPaste);
    item(edit, Tr(L"menu.edit.delete"), Cmd::EditDelete);
    sep(edit);
    item(edit, Tr(L"menu.edit.selectall"), Cmd::EditSelectAll);
    item(edit, Tr(L"menu.edit.duplicateline"), Cmd::EditDuplicateLine);
    HMENU lines = popup(edit, Tr(L"menu.edit.lineops"));
    item(lines, Tr(L"menu.edit.lineops.moveup"), Cmd::EditMoveLineUp);
    item(lines, Tr(L"menu.edit.lineops.movedown"), Cmd::EditMoveLineDown);
    item(lines, Tr(L"menu.edit.lineops.deleteline"), Cmd::EditDeleteLine);
    sep(lines);
    item(lines, Tr(L"menu.edit.lineops.sortasc"), Cmd::EditSortAsc);
    item(lines, Tr(L"menu.edit.lineops.sortdesc"), Cmd::EditSortDesc);
    sep(lines);
    item(lines, Tr(L"menu.edit.lineops.removedup"), Cmd::EditRemoveDupLines);
    item(lines, Tr(L"menu.edit.lineops.trim"), Cmd::EditTrimTrailingSpace);
    item(lines, Tr(L"menu.edit.lineops.removeempty"), Cmd::EditRemoveEmptyLines);
    item(lines, Tr(L"menu.edit.lineops.reverse"), Cmd::EditReverseLines);
    sep(lines);
    item(lines, Tr(L"menu.edit.lineops.join"), Cmd::EditJoinLines);
    item(lines, Tr(L"menu.edit.lineops.split"), Cmd::EditSplitLines);
    HMENU convert = popup(edit, Tr(L"menu.edit.convert"));
    item(convert, Tr(L"menu.edit.convert.upper"), Cmd::EditUpperCase);
    item(convert, Tr(L"menu.edit.convert.lower"), Cmd::EditLowerCase);
    item(convert, Tr(L"menu.edit.convert.comment"), Cmd::EditToggleComment);
    sep(edit);
    item(edit, Tr(L"menu.edit.insertdt"), Cmd::EditTimeDate);

    HMENU search = popup(menu_, Tr(L"menu.search"));
    item(search, Tr(L"menu.search.find"), Cmd::SearchFind);
    item(search, Tr(L"menu.search.findnext"), Cmd::SearchFindNext);
    item(search, Tr(L"menu.search.findprev"), Cmd::SearchFindPrev);
    item(search, Tr(L"menu.search.replace"), Cmd::SearchReplace);
    sep(search);
    HMENU marks = popup(search, Tr(L"menu.search.bookmark"));
    item(marks, Tr(L"menu.search.bookmark.toggle"), Cmd::BookmarkToggle);
    item(marks, Tr(L"menu.search.bookmark.next"), Cmd::BookmarkNext);
    item(marks, Tr(L"menu.search.bookmark.prev"), Cmd::BookmarkPrev);
    item(marks, Tr(L"menu.search.bookmark.clearall"), Cmd::BookmarkClearAll);
    sep(search);
    item(search, Tr(L"menu.search.gotoline"), Cmd::SearchGotoLine);
    sep(search);
    item(search, Tr(L"menu.search.compare"), Cmd::DiffCompare);
    item(search, Tr(L"menu.search.exitcompare"), Cmd::DiffExitCompare);

    HMENU view = popup(menu_, Tr(L"menu.view"));
    AppendMenuW(view, MF_STRING, Cmd::ViewWordWrap, Tr(L"menu.view.wrap"));
    sep(view);
    AppendMenuW(view, MF_STRING, Cmd::ViewExplorer,
                Tr(L"menu.view.explorer"));
    sep(view);
    item(view, Tr(L"menu.view.foldall"), Cmd::FoldAll);
    item(view, Tr(L"menu.view.unfoldall"), Cmd::UnfoldAll);
    sep(view);
    item(view, Tr(L"menu.view.hex"), Cmd::ViewHexView);
    item(view, Tr(L"menu.view.stdf"), Cmd::ViewStdfView);
    item(view, Tr(L"menu.view.csv"), Cmd::ViewCsvView);
    item(view, Tr(L"bigfile.menu"), Cmd::ViewBigFile);
    item(view, Tr(L"menu.view.log"), Cmd::ViewLogPanel);
    item(view, Tr(L"menu.view.terminal"), Cmd::ViewTerminal);
    sep(view);
    item(view, Tr(L"menu.view.zoomin"), Cmd::ViewZoomIn);
    item(view, Tr(L"menu.view.zoomout"), Cmd::ViewZoomOut);
    item(view, Tr(L"menu.view.zoomreset"), Cmd::ViewZoomReset);
    sep(view);
    item(view, Tr(L"menu.view.moveother"), Cmd::ViewMoveToOtherView);
    item(view, Tr(L"menu.view.movenew"), Cmd::ViewMoveToNewView);
    sep(view);
    HMENU themeMenu = popup(view, Tr(L"menu.view.theme"));
    item(themeMenu, Tr(L"menu.view.theme.light"), Cmd::ThemeLight);
    item(themeMenu, Tr(L"menu.view.theme.dark"), Cmd::ThemeDark);

    // AI 顶级菜单（用户要求的入口：菜单栏 AI > 打开/关闭 opencode）
    HMENU aiMenu = popup(menu_, Tr(L"menu.ai"));
    item(aiMenu, Tr(L"menu.ai.toggle"), Cmd::ViewAiPanel);
    item(aiMenu, Tr(L"menu.ai.newsession"), Cmd::AiNewSession);
    item(aiMenu, Tr(L"menu.ai.attach"), Cmd::AiToggleContext);

    // Insert the top-level "Language" menu after "View", before "Encoding" (same as Notepad++)
    languageMenu_ = popup(menu_, Tr(L"menu.language"));
    // enumerate the catalog and append one entry per menu item;
    // command id = Cmd::LangFirst + entry index
    int entryIdx = 0;
    for (const LanguageMenuItem* entry = LanguageMenuCatalog();
         entry->label != nullptr; ++entry, ++entryIdx) {
        item(languageMenu_, entry->label, Cmd::LangFirst + entryIdx);
        if (entryIdx == 0) sep(languageMenu_);   // separate "Normal Text"
    }

    HMENU enc = popup(menu_, Tr(L"menu.encoding"));
    HMENU conv = popup(enc, Tr(L"menu.encoding.convert"));
    item(conv, Tr(L"enc.utf8"),      Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::UTF8);
    item(conv, Tr(L"enc.utf8bom"),   Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::UTF8BOM);
    item(conv, Tr(L"enc.utf16le"),   Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::UTF16LE);
    item(conv, Tr(L"enc.utf16be"),   Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::UTF16BE);
    item(conv, Tr(L"enc.ansi"),      Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::ANSI);
    // batch 39: extended encodings (UTF-32 BOM auto-detects on load; the
    // legacy codepage ones are convert/reload-as only)
    // batch 65: menu labels are now Tr()-driven (single key set shared with
    // the reload-as submenu, so no &-mnemonics)
    item(conv, Tr(L"enc.utf32le"),   Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::UTF32LE);
    item(conv, Tr(L"enc.utf32be"),   Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::UTF32BE);
    item(conv, Tr(L"enc.big5"),      Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::Big5);
    item(conv, Tr(L"enc.shiftjis"),  Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::ShiftJIS);
    item(conv, Tr(L"enc.koi8r"),     Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::KOI8R);
    item(conv, Tr(L"enc.iso88591"),  Cmd::EncConvertFirst + (unsigned int)encoding::EncodingType::ISO88591);
    HMENU relas = popup(enc, Tr(L"menu.encoding.reload"));
    item(relas, Tr(L"enc.utf8"),     Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::UTF8);
    item(relas, Tr(L"enc.utf8bom"),  Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::UTF8BOM);
    item(relas, Tr(L"enc.utf16le"),  Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::UTF16LE);
    item(relas, Tr(L"enc.utf16be"),  Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::UTF16BE);
    item(relas, Tr(L"enc.ansi"),     Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::ANSI);
    item(relas, Tr(L"enc.utf32le"),  Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::UTF32LE);
    item(relas, Tr(L"enc.utf32be"),  Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::UTF32BE);
    item(relas, Tr(L"enc.big5"),     Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::Big5);
    item(relas, Tr(L"enc.shiftjis"), Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::ShiftJIS);
    item(relas, Tr(L"enc.koi8r"),    Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::KOI8R);
    item(relas, Tr(L"enc.iso88591"), Cmd::EncReloadAsFirst + (unsigned int)encoding::EncodingType::ISO88591);

    HMENU settingsMenu = popup(menu_, Tr(L"menu.settings"));
    item(settingsMenu, Tr(L"menu.settings.prefs"), Cmd::Preferences);
    item(settingsMenu, Tr(L"menu.settings.styler"), Cmd::StyleConfigurator);
    item(settingsMenu, Tr(L"menu.settings.shortcuts"), Cmd::ShortcutMapper);
    sep(settingsMenu);
    item(settingsMenu, Tr(L"menu.settings.themeexport"), Cmd::ThemeExport);
    item(settingsMenu, Tr(L"menu.settings.themeimport"), Cmd::ThemeImport);
    sep(settingsMenu);
    item(settingsMenu, Tr(L"menu.settings.cfgexport"), Cmd::ConfigExport);
    item(settingsMenu, Tr(L"menu.settings.cfgimport"), Cmd::ConfigImport);
    HMENU macroMenu = popup(menu_, Tr(L"menu.macro"));
    item(macroMenu, Tr(L"menu.macro.start"), Cmd::MacroStart);
    item(macroMenu, Tr(L"menu.macro.stop"), Cmd::MacroStop);
    item(macroMenu, Tr(L"menu.macro.playback"), Cmd::MacroPlayback);
    sep(macroMenu);
    item(macroMenu, Tr(L"menu.macro.save"), Cmd::MacroSave);
    item(macroMenu, Tr(L"menu.macro.load"), Cmd::MacroLoad);
    pluginMenu_ = popup(menu_, Tr(L"menu.plugin"));
    windowMenu_ = popup(menu_, Tr(L"menu.window"));
    item(windowMenu_, Tr(L"menu.window.windowsdlg"), Cmd::WindowList);
    AppendMenuW(windowMenu_, MF_SEPARATOR, 0, nullptr);
    RebuildWindowMenuItems();
    HMENU help = popup(menu_, Tr(L"menu.help"));
    item(help, Tr(L"menu.help.about"), Cmd::HelpAbout);
}

// 窗口菜单文档条目：全部清空后按 view0 → view1 顺序重建（N++ 式编号直达）。
// 固定保留 [0]=窗口列表…、[1]=分隔线；文档条目从位置 2 开始。
// BuildMenus 在 workspace_ 创建之前运行，此时只保留骨架，文档条目由
// 首次 OnWorkspaceChanged 补齐。
void MainWindow::RebuildWindowMenuItems() {
    if (!windowMenu_ || !workspace_) return;
    while (::DeleteMenu(windowMenu_, 1, MF_BYPOSITION)) {}
    ::AppendMenuW(windowMenu_, MF_SEPARATOR, 0, nullptr);
    int entry = 0;
    auto addDoc = [&](Document* d, int view, int index) {
        if (!d || entry >= 99) return;
        std::wstring label = d->TitleForTab();
        if (label.size() > 60) label = label.substr(0, 57) + L"...";
        label = std::to_wstring(entry + 1) + L" " + label;
        unsigned id = Cmd::WindowDocFirst + entry;
        ::AppendMenuW(windowMenu_, MF_STRING, id, label.c_str());
        if (d == workspace_->ActiveIn(view))
            ::CheckMenuItem(windowMenu_, id, MF_BYCOMMAND | MF_CHECKED);
        ++entry;
    };
    for (int i = 0; i < workspace_->Count(); ++i)
        addDoc(workspace_->DocumentAt(i), 0, i);
    for (int i = 0; i < workspace_->Count1(); ++i)
        addDoc(workspace_->FindByTabIndex1(i), 1, i);
    if (entry == 0)
        ::AppendMenuW(windowMenu_, MF_STRING | MF_GRAYED, 0,
                      Tr(L"menu.window.nodocs"));
}

// 语言切换：重建整个菜单树并重新挂到窗口（不重建加速器，快捷键与语言无关）。
void MainWindow::ApplyLanguage() {
    if (!hwnd_) return;   // 启动早期不重建（I18n 初始 Load 在建窗之前）
    if (menu_) {
        for (HMENU pop : pluginPopups_) if (pop) ::DestroyMenu(pop);
        pluginPopups_.clear();
        ::DestroyMenu(menu_);
        menu_ = nullptr;
    }
    BuildMenus();
    ::SetMenu(hwnd_, menu_);
    if (plugins_) {
        plugins_->SetPluginMenus(pluginMenu_, menu_);
        RebuildPluginMenu();
    }
    RebuildRecentFolderMenu(recentFolderItems_);
    RebuildRecentMenu(recentItems_);
    DrawMenuBar(hwnd_);
    UpdateStatusBar();
    if (findDlg_) findDlg_->Retranslate();
    if (hex_) hex_->Retranslate();
    if (stdf_) stdf_->Retranslate();
    if (csv_) csv_->Retranslate();
    if (bigfile_) bigfile_->Retranslate();
    if (logPanel_) logPanel_->Retranslate();
    if (terminal_) terminal_->Retranslate();
    if (ai_) ai_->Retranslate();
    if (results_) results_->Retranslate();
}

void MainWindow::BuildAccelerators() {
    // 快捷键单一数据源：GlobalShortcuts（默认表 + shortcuts.json 覆盖）。
    std::vector<ACCEL> v;
    for (const auto& [cmd, info] : GlobalShortcuts().All()) {
        ACCEL a{};
        a.fVirt = (BYTE)(FVIRTKEY |
                         (info.ctrl ? FCONTROL : 0u) |
                         (info.alt ? FALT : 0u) |
                         (info.shift ? FSHIFT : 0u));
        a.key = (WORD)info.vk;
        a.cmd = (WORD)cmd;
        v.push_back(a);
    }

    // 插件命令的快捷键（NPP FuncItem.ShortcutKey 转录）追加进同一张表；
    // 与内建项冲突时让位内建（先到先得），留日志便于插件作者调整。
    if (plugins_) {
        for (const auto& c : plugins_->Commands()) {
            if (!c.hasKey) continue;
            bool dup = false;
            for (const auto& e : v)
                if (e.fVirt == c.fVirt && e.key == c.vk) { dup = true; break; }
            if (dup) {
                Logger::Info("Plugin accelerator skipped (conflict): cmd=" +
                             std::to_string(c.id));
                continue;
            }
            ACCEL pa{};
            pa.fVirt = c.fVirt;
            pa.key = (WORD)c.vk;
            pa.cmd = (WORD)c.id;
            v.push_back(pa);
        }
    }

    HACCEL fresh = ::CreateAcceleratorTableW(v.data(), (int)v.size());
    if (accel_) ::DestroyAcceleratorTable(accel_);
    accel_ = fresh;
}

// --- layout / UI sync ---------------------------------------------------------------

namespace {

// Original hand-drawn 16px toolbar icons (GDI vector art, no external assets).
enum ToolIcon {
    ICON_NEW = 0, ICON_OPEN, ICON_SAVE, ICON_SAVEALL,
    ICON_CLOSE, ICON_FIND, ICON_REPLACE, ICON_WRAP, ICON_COUNT
};

void DrawSheet(HDC dc, int x, int y) {           // document sheet with fold
    RECT page{x, y, x + 11, y + 14};
    HBRUSH w = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &page, w); DeleteObject(w);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 90, 120));
    auto op = (HPEN)SelectObject(dc, pen);
    Rectangle(dc, x, y, x + 12, y + 15);
    MoveToEx(dc, x + 7, y, nullptr); LineTo(dc, x + 12, y + 5);
    MoveToEx(dc, x + 7, y, nullptr); LineTo(dc, x + 7, y + 5);
    LineTo(dc, x + 12, y + 5);
    // text lines
    for (int i = 0; i < 3; ++i) {
        MoveToEx(dc, x + 3, y + 7 + i * 2, nullptr);
        LineTo(dc, x + 9, y + 7 + i * 2);
    }
    SelectObject(dc, op); DeleteObject(pen);
}

HBITMAP MakeIconBitmap(int kind) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = 16; bi.bV5Height = 16; bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HBITMAP bm = CreateDIBSection(sdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, sdc);
    if (!bm) return nullptr;
    memset(bits, 0, 16 * 16 * 4);   // transparent

    HDC mdc = CreateCompatibleDC(sdc ? sdc : GetDC(nullptr));
    auto old = (HBITMAP)SelectObject(mdc, bm);

    const COLORREF bgKey = RGB(240, 240, 240);
    RECT bg{0, 0, 16, 16};
    HBRUSH bgb = CreateSolidBrush(bgKey);
    FillRect(mdc, &bg, bgb); DeleteObject(bgb);

    const COLORREF ink = RGB(60, 60, 68);
    switch (kind) {
        case ICON_NEW: DrawSheet(mdc, 2, 0);
            { HPEN p = CreatePen(PS_SOLID, 2, RGB(30, 130, 70)); auto o=(HPEN)SelectObject(mdc,p);
              MoveToEx(mdc, 10, 9, nullptr); LineTo(mdc, 14, 13);
              MoveToEx(mdc, 14, 9, nullptr); LineTo(mdc, 10, 13);
              SelectObject(mdc, o); DeleteObject(p); }
            break;
        case ICON_OPEN: {   // folder
            HPEN p = CreatePen(PS_SOLID, 1, ink); auto o=(HPEN)SelectObject(mdc,p);
            HBRUSH b = CreateSolidBrush(RGB(250, 205, 90)); auto ob=(HBRUSH)SelectObject(mdc,b);
            POINT pts[7] = {{1,4},{6,4},{8,6},{14,6},{14,13},{1,13},{1,4}};
            Polygon(mdc, pts, 7);
            POINT lid[5] = {{1,6},{5,6},{7,8},{13,8},{13,10}};
            (void)lid;
            SelectObject(mdc,ob); DeleteObject(b);
            SelectObject(mdc,o); DeleteObject(p); }
            break;
        case ICON_SAVE: case ICON_SAVEALL: {   // floppy
            RECT body{2,2,14,14};
            HBRUSH b = CreateSolidBrush(kind==ICON_SAVE?RGB(70,110,180):RGB(90,150,90));
            FillRect(mdc,&body,b); DeleteObject(b);
            HPEN p=CreatePen(PS_SOLID,1,ink); auto o=(HPEN)SelectObject(mdc,p);
            Rectangle(mdc,2,2,14,14);
            RECT shutter{6,2,11,7}; HBRUSH s=CreateSolidBrush(RGB(235,235,235));
            FillRect(mdc,&shutter,s); DeleteObject(s);
            RECT label{4,9,12,13}; FillRect(mdc,&label,s); DeleteObject(s);
            SelectObject(mdc,o); DeleteObject(p);
            if (kind == ICON_SAVEALL) {
                HPEN a=CreatePen(PS_SOLID,2,RGB(230,170,40)); auto oa=(HPEN)SelectObject(mdc,a);
                MoveToEx(mdc,1,1,nullptr); LineTo(mdc,15,1);
                SelectObject(mdc,oa); DeleteObject(a); }
            break; }
        case ICON_CLOSE: {
            HPEN p = CreatePen(PS_SOLID, 2, RGB(200, 40, 40));
            auto o=(HPEN)SelectObject(mdc,p);
            MoveToEx(mdc,3,3,nullptr); LineTo(mdc,13,13);
            MoveToEx(mdc,13,3,nullptr); LineTo(mdc,3,13);
            SelectObject(mdc,o); DeleteObject(p); }
            break;
        case ICON_FIND: {
            HPEN p = CreatePen(PS_SOLID, 2, ink); auto o=(HPEN)SelectObject(mdc,p);
            HBRUSH g = (HBRUSH)GetStockObject(NULL_BRUSH); auto og=(HBRUSH)SelectObject(mdc,g);
            Ellipse(mdc,3,3,10,10);
            SelectObject(mdc,o);
            MoveToEx(mdc,9,9,nullptr); LineTo(mdc,14,14);
            SelectObject(mdc,og); SelectObject(mdc,o); DeleteObject(p); }
            break;
        case ICON_REPLACE: {
            HPEN p = CreatePen(PS_SOLID, 2, RGB(40,120,200)); auto o=(HPEN)SelectObject(mdc,p);
            MoveToEx(mdc,2,5,nullptr); LineTo(mdc,11,5); LineTo(mdc,8,2);
            MoveToEx(mdc,11,5,nullptr); LineTo(mdc,8,8);
            SelectObject(mdc,o); DeleteObject(p);
            HPEN q = CreatePen(PS_SOLID, 2, RGB(220,120,30)); auto oq=(HPEN)SelectObject(mdc,q);
            MoveToEx(mdc,14,11,nullptr); LineTo(mdc,5,11); LineTo(mdc,8,8);
            MoveToEx(mdc,5,11,nullptr); LineTo(mdc,8,14);
            SelectObject(mdc,oq); DeleteObject(q); }
            break;
        case ICON_WRAP: {
            HPEN p = CreatePen(PS_SOLID, 2, RGB(80,140,80)); auto o=(HPEN)SelectObject(mdc,p);
            MoveToEx(mdc,2,4,nullptr); LineTo(mdc,12,4);
            MoveToEx(mdc,2,8,nullptr); LineTo(mdc,10,8);
            Arc(mdc,6,4,14,12,10,8,14,8);
            MoveToEx(mdc,11,5,nullptr); LineTo(mdc,14,8); LineTo(mdc,11,11);
            SelectObject(mdc,o); DeleteObject(p); }
            break;
    }

    SelectObject(mdc, old);
    DeleteDC(mdc);

    // GDI leaves the alpha channel at 0; synthesize transparency from the
    // background key so ImageList's 32bpp path doesn't show black squares.
    auto* px = (uint8_t*)bits;
    for (int i = 0; i < 16 * 16; ++i) {
        uint8_t b = px[i * 4], g = px[i * 4 + 1], r = px[i * 4 + 2];
        px[i * 4 + 3] = (r == 240 && g == 240 && b == 240) ? 0 : 255;
    }
    return bm;
}

HIMAGELIST CreateToolbarIcons() {
    HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR32, ICON_COUNT, 4);
    if (!il) return nullptr;
    for (int k = 0; k < ICON_COUNT; ++k) {
        HBITMAP bm = MakeIconBitmap(k);
        if (bm) { ImageList_Add(il, bm, nullptr); DeleteObject(bm); }
    }
    return il;
}

} // namespace

const wchar_t* CmdLabel(unsigned int cmd) {
    switch (cmd) {
        case Cmd::FileNew:       return Tr(L"cmd.new");
        case Cmd::FileOpen:      return Tr(L"cmd.open");
        case Cmd::FileSave:      return Tr(L"cmd.save");
        case Cmd::FileSaveAll:   return Tr(L"cmd.saveall");
        case Cmd::FileClose:     return Tr(L"cmd.close");
        case Cmd::EditUndo:      return Tr(L"cmd.undo");
        case Cmd::EditRedo:      return Tr(L"cmd.redo");
        case Cmd::EditCut:       return Tr(L"cmd.cut");
        case Cmd::EditCopy:      return Tr(L"cmd.copy");
        case Cmd::EditPaste:     return Tr(L"cmd.paste");
        case Cmd::SearchFind:    return Tr(L"cmd.find");
        case Cmd::SearchReplace: return Tr(L"cmd.replace");
        case Cmd::ViewZoomIn:    return Tr(L"cmd.zoomin");
        case Cmd::ViewZoomOut:   return Tr(L"cmd.zoomout");
        case Cmd::ViewWordWrap:  return Tr(L"cmd.wrap");
        case Cmd::ViewMoveToOtherView: return Tr(L"cmd.moveother");
        case Cmd::ViewMoveToNewView:   return Tr(L"cmd.movenew");
        case Cmd::ReopenClosedTab:     return Tr(L"menu.file.reopenclosed");
        case Cmd::FileNewWindow:      return Tr(L"menu.file.newwindow");
        default:                 return L"";
    }
}

const wchar_t* CmdShortcut(unsigned int cmd) {
    // Scintilla 自处理的标准编辑键不在 ACCEL 表内，保留静态提示。
    switch (cmd) {
        case Cmd::EditUndo:      return L" (Ctrl+Z)";
        case Cmd::EditRedo:      return L" (Ctrl+Y)";
        case Cmd::EditCut:       return L" (Ctrl+X)";
        case Cmd::EditCopy:      return L" (Ctrl+C)";
        case Cmd::EditPaste:     return L" (Ctrl+V)";
        default:                 break;
    }
    // 其余命令查快捷键表（默认 + shortcuts.json 覆盖）。
    const ShortcutInfo info = GlobalShortcuts().Get(cmd);
    if (!info.Valid()) return L"";
    static std::wstring tip;   // 仅 UI 线程使用（tooltip 路径）
    tip = L" (" + info.ToString() + L")";
    return tip.c_str();
}

// Toolbar PNG icons embedded as RCDATA (see resource.h), decoded with GDI+.
// Full-color icons are used as-is (no tinting); alpha is preserved.
HBITMAP PngResourceToBitmap(HINSTANCE inst, UINT resId, int px) {
    HRSRC hr = ::FindResourceW(inst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hr) return nullptr;
    HGLOBAL hg = ::LoadResource(inst, hr);
    if (!hg) return nullptr;
    const void* data = ::LockResource(hg);
    DWORD size = ::SizeofResource(inst, hr);
    if (!data || !size) return nullptr;

    IStream* stream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return nullptr;
    stream->Write(data, size, nullptr);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    HBITMAP hb = nullptr;
    Gdiplus::Bitmap png(stream, FALSE);
    if (png.GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Bitmap scaled(px, px, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&scaled);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.DrawImage(&png, 0, 0, px, px);
        // GetHBITMAP strips alpha; copy pixels into a top-down 32bpp DIB so
        // the image list's per-pixel alpha survives.
        Gdiplus::Rect rect2(0, 0, px, px);
        Gdiplus::BitmapData bd2{};
        if (scaled.LockBits(&rect2, Gdiplus::ImageLockModeRead,
                            PixelFormat32bppARGB, &bd2) == Gdiplus::Ok) {
            BITMAPV5HEADER bi{};
            bi.bV5Size = sizeof(BITMAPV5HEADER);
            bi.bV5Width = px;
            bi.bV5Height = -px;                 // top-down
            bi.bV5Planes = 1;
            bi.bV5BitCount = 32;
            bi.bV5Compression = BI_BITFIELDS;
            bi.bV5RedMask = 0x00FF0000;
            bi.bV5GreenMask = 0x0000FF00;
            bi.bV5BlueMask = 0x000000FF;
            void* bits = nullptr;
            HDC sdc = ::GetDC(nullptr);
            hb = ::CreateDIBSection(sdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                    &bits, nullptr, 0);
            ::ReleaseDC(nullptr, sdc);
            if (hb && bits) {
                for (int y = 0; y < px; ++y) {
                    auto* src = (BYTE*)bd2.Scan0 + y * bd2.Stride;
                    auto* dst = (BYTE*)bits + y * px * 4;
                    memcpy(dst, src, (size_t)px * 4);
                }
            }
            scaled.UnlockBits(&bd2);
        }
    }
    stream->Release();
    return hb;
}

void MainWindow::CreateToolbar() {
    toolbar_ = ::CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST |
        CCS_NODIVIDER | CCS_TOP,
        0, 0, 0, 0, hwnd_, (HMENU)(INT_PTR)1103, inst_, nullptr);
    if (!toolbar_) return;

    ::SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    ::SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0,
                   TBSTYLE_EX_HIDECLIPPEDBUTTONS);

    // our own tooltip over the strip; one RECT tool per button, static text
    toolbarTip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                    0, 0, 0, 0, toolbar_, nullptr, inst_, nullptr);

    // ---- buttons: icon-only (labels live in the hover tooltip) ----
    struct BtnDefX { const wchar_t* label; unsigned int cmd; UINT resId; };
    const BtnDefX defs[] = {
        {Tr(L"cmd.new"), Cmd::FileNew, IDR_TB_NEW},
        {Tr(L"cmd.open"), Cmd::FileOpen, IDR_TB_OPEN},
        {Tr(L"cmd.save"), Cmd::FileSave, IDR_TB_SAVE},
        {Tr(L"cmd.saveall"), Cmd::FileSaveAll, IDR_TB_SAVEALL},
        {nullptr, 0, 0},                                    // sep
        {Tr(L"cmd.close"), Cmd::FileClose, IDR_TB_CLOSE},
        {nullptr, 0, 0},                                    // sep
        {Tr(L"cmd.undo"), Cmd::EditUndo, IDR_TB_UNDO},
        {Tr(L"cmd.redo"), Cmd::EditRedo, IDR_TB_REDO},
        {nullptr, 0, 0},                                    // sep
        {Tr(L"cmd.cut"), Cmd::EditCut, IDR_TB_CUT},
        {Tr(L"cmd.copy"), Cmd::EditCopy, IDR_TB_COPY},
        {Tr(L"cmd.paste"), Cmd::EditPaste, IDR_TB_PASTE},
        {nullptr, 0, 0},                                    // sep
        {Tr(L"cmd.find"), Cmd::SearchFind, IDR_TB_FIND},
        {Tr(L"cmd.replace"), Cmd::SearchReplace, IDR_TB_REPLACE},
        {nullptr, 0, 0},                                    // sep
        {Tr(L"cmd.zoomin"), Cmd::ViewZoomIn, IDR_TB_ZOOMIN},
        {Tr(L"cmd.zoomout"), Cmd::ViewZoomOut, IDR_TB_ZOOMOUT},
        {nullptr, 0, 0},                                    // sep
        {Tr(L"cmd.wrap"), Cmd::ViewWordWrap, 0},             // caption-only
    };
    constexpr int N = sizeof(defs) / sizeof(defs[0]);

    int dpi = ::GetDpiForWindow(hwnd_);
    int iconPx = MulDiv(16, dpi, 96);
    HIMAGELIST il = ImageList_Create(iconPx, iconPx, ILC_COLOR32, N, 8);
    int added = 0;
    if (il) {
        for (int i = 0; i < N; ++i) {
            if (!defs[i].resId) continue;
            if (HBITMAP hb = PngResourceToBitmap(inst_, defs[i].resId, iconPx)) {
                ImageList_Add(il, hb, nullptr);
                ++added;
                DeleteObject(hb);
            } else {
                Logger::Error("Toolbar icon load FAILED resId=" +
                              std::to_string(defs[i].resId));
            }
        }
        ::SendMessageW(toolbar_, TB_SETIMAGELIST, 0, (LPARAM)il);
    } else {
        Logger::Error("ImageList_Create failed");
    }
    Logger::Info("Toolbar icons: dpi=" + std::to_string(dpi) +
                 " px=" + std::to_string(iconPx) +
                 " loaded=" + std::to_string(added) + "/" +
                 std::to_string(std::count_if(defs, defs + N,
                     [](const BtnDefX& d){ return d.resId != 0; })));

    // empty string pool entry shared by icon-only buttons (no caption)
    wchar_t emptyStr[2] = L"";
    int emptyIdx = (int)::SendMessageW(toolbar_, TB_ADDSTRINGW,
                                       0, (LPARAM)emptyStr);
    if (emptyIdx < 0) emptyIdx = 0;

    TBBUTTON tb[N] = {};
    int iconIdx = 0;
    for (int i = 0; i < N; ++i) {
        bool isSep = (defs[i].label == nullptr);
        tb[i].idCommand = defs[i].cmd;
        tb[i].fsState = TBSTATE_ENABLED;
        tb[i].fsStyle = isSep ? BTNS_SEP : (BTNS_BUTTON | BTNS_AUTOSIZE);
        if (isSep) {
            tb[i].iBitmap = 0;
            tb[i].iString = 0;
        } else if (defs[i].resId) {
            tb[i].iBitmap = iconIdx++;
            tb[i].iString = emptyIdx;
        } else {
            tb[i].iBitmap = I_IMAGENONE;          // word wrap: caption only
            std::wstring lbl = defs[i].label;
            int si = (int)::SendMessageW(toolbar_, TB_ADDSTRINGW,
                                         0, (LPARAM)lbl.c_str());
            tb[i].iString = si >= 0 ? si : 0;
        }
    }
    ::SendMessageW(toolbar_, TB_ADDBUTTONSW, N, (LPARAM)tb);
    ::SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);

    // ---- tooltips in TRACK mode with STATIC per-button text ----
    toolbarTipTexts_.clear();
    toolbarTipTools_.clear();
    toolbarTipTexts_.reserve(N);
    if (toolbarTip_) {
        for (int i = 0; i < N; ++i) {
            if (!defs[i].label) continue;
            toolbarTipTexts_.push_back(std::wstring(defs[i].label) +
                                       CmdShortcut(defs[i].cmd));
            RECT br{};
            ::SendMessageW(toolbar_, TB_GETITEMRECT, i, (LPARAM)&br);
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.hwnd = toolbar_;
            ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            ti.uId = (UINT_PTR)(i + 1);
            ti.rect = br;
            ti.lpszText = toolbarTipTexts_.back().data();   // stable: reserved
            ::SendMessageW(toolbarTip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            toolbarTipTools_.push_back(ti);
        }
    }
    BOOL subOk = ::SetWindowSubclass(toolbar_, ToolbarMouseProc, 2,
                                     (DWORD_PTR)this);
    Logger::Info(std::string("Toolbar mouse subclass installed=") +
                 (subOk ? "1" : "0"));

    // ---- self-render diagnostics: what does the toolbar itself draw? ----
    RECT trect{};
    ::GetClientRect(toolbar_, &trect);
    HDC wdc = ::GetDC(toolbar_);
    HDC mdc = ::CreateCompatibleDC(wdc);
    HBITMAP sbm = ::CreateCompatibleBitmap(wdc, trect.right, trect.bottom);
    auto sold = (HBITMAP)::SelectObject(mdc, sbm);
    ::PrintWindow(toolbar_, mdc, PW_CLIENTONLY);
    // sample the first icon button's image area
    RECT br{};
    if (::SendMessageW(toolbar_, TB_GETITEMRECT, 0, (LPARAM)&br)) {
        int cx0 = br.left + (br.right - br.left) / 2 - iconPx / 2;
        int cy0 = br.top + (br.bottom - br.top) / 2 - iconPx / 2;
        long sum = 0, dark = 0, colored = 0, n = 0;
        for (int y = cy0; y < cy0 + iconPx && y < trect.bottom; ++y)
            for (int x = cx0; x < cx0 + iconPx && x < trect.right; ++x) {
                COLORREF c = ::GetPixel(mdc, x, y);
                LONG lum = (GetRValue(c) + GetGValue(c) + GetBValue(c)) / 3;
                sum += lum; ++n;
                if (lum < 150) ++dark;
                BYTE rC = GetRValue(c), gC = GetGValue(c), bC = GetBValue(c);
                BYTE mx = rC > gC ? (rC > bC ? rC : bC) : (gC > bC ? gC : bC);
                BYTE mn = rC < gC ? (rC < bC ? rC : bC) : (gC < bC ? gC : bC);
                if (mx - mn > 40) ++colored;
            }
        Logger::Info("Toolbar self-render btn0: meanLum=" +
                     std::to_string(n ? sum / n : 0) +
                     " dark=" + std::to_string(dark) +
                     " colored=" + std::to_string(colored) + "/" +
                     std::to_string(n));
    } else {
        Logger::Warn("Toolbar self-render: TB_GETITEMRECT failed");
    }
    ::SelectObject(mdc, sold);
    ::DeleteObject(sbm);
    ::DeleteDC(mdc);
    ::ReleaseDC(toolbar_, wdc);
}

LRESULT CALLBACK MainWindow::ToolbarMouseProc(HWND h, UINT msg, WPARAM wp,
                                              LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = (MainWindow*)ref;
    if (!self) return DefSubclassProc(h, msg, wp, lp);
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE)
        Logger::Info(std::string("TbMouseProc msg=") +
                     (msg == WM_MOUSEMOVE ? "MOVE" : "LEAVE"));

    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int hit = -1;
            TBBUTTON tb{};
            int count = (int)::SendMessageW(h, TB_BUTTONCOUNT, 0, 0);
            for (int i = 0; i < count; ++i) {
                RECT rc{};
                if (::SendMessageW(h, TB_GETITEMRECT, i, (LPARAM)&rc) &&
                    pt.x >= rc.left && pt.x < rc.right &&
                    pt.y >= rc.top && pt.y < rc.bottom) { hit = i; break; }
            }
            if (hit == self->hotBtn_) break;
            self->hotBtn_ = hit;
            Logger::Info("Toolbar hover button=" + std::to_string(hit));

            // pick the matching TRACK tool and show its static text
            TOOLINFOW* tool = nullptr;
            for (auto& t : self->toolbarTipTools_)
                if ((int)t.uId == hit + 1) { tool = &t; break; }
            if (!tool) {   // gap/separator: hide
                TOOLINFO off{}; off.cbSize = sizeof(off); off.hwnd = h;
                ::SendMessageW(self->toolbarTip_, TTM_TRACKACTIVATE,
                               FALSE, (LPARAM)&off);
                break;
            }

            POINT sp{0, 0};
            ::ClientToScreen(h, &sp);
            int dpi = ::GetDpiForWindow(h);
            // standard TRACK sequence: deactivate -> position -> update text -> activate
            ::SendMessageW(self->toolbarTip_, TTM_TRACKACTIVATE,
                           FALSE, (LPARAM)tool);
            ::SendMessageW(self->toolbarTip_, TTM_TRACKPOSITION, 0,
                           MAKELPARAM(sp.x + pt.x + 12,
                                      sp.y + pt.y + MulDiv(26, dpi, 96)));
            ::SendMessageW(self->toolbarTip_, TTM_UPDATETIPTEXTW,
                           0, (LPARAM)tool);
            ::SendMessageW(self->toolbarTip_, TTM_TRACKACTIVATE,
                           TRUE, (LPARAM)tool);
            Logger::Info("Toolbar tip show btn=" + std::to_string(hit));
            break;
        }
        case WM_MOUSELEAVE:
            self->hotBtn_ = -1;
            { TOOLINFO off{}; off.cbSize = sizeof(off); off.hwnd = h;
              ::SendMessageW(self->toolbarTip_, TTM_TRACKACTIVATE,
                             FALSE, (LPARAM)&off); }
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void MainWindow::LayoutChildren() {
    RECT rc; GetClientRect(hwnd_, &rc);
    int dpi = GetDpiForWindow(hwnd_);
    int sbH = 0;
    if (status_ && settings_.showStatusBar) {
        ::ShowWindow(status_->Hwnd(), SW_SHOW);
        SendMessageW(status_->Hwnd(), WM_SIZE, 0, 0);
        RECT sbr; GetWindowRect(status_->Hwnd(), &sbr);
        MapWindowPoints(nullptr, hwnd_, (LPPOINT)&sbr, 2);
        sbH = sbr.bottom - sbr.top;
        status_->Layout(rc.right, dpi);
    } else if (status_) {
        ::ShowWindow(status_->Hwnd(), SW_HIDE);
    }
    int tbH = 0;
    if (toolbar_ && settings_.showToolbar) {
        ::ShowWindow(toolbar_, SW_SHOW);
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        RECT tbr; GetWindowRect(toolbar_, &tbr);
        MapWindowPoints(nullptr, hwnd_, (LPPOINT)&tbr, 2);
        tbH = tbr.bottom - tbr.top;
        MoveWindow(toolbar_, 0, 0, rc.right, tbH, TRUE);
    } else if (toolbar_) {
        ::ShowWindow(toolbar_, SW_HIDE);
    }
    int rpH = (results_ && results_->Visible()) ? MulDiv(rpHLogical_, dpi, 96) : 0;
    int hxH = (hex_ && hex_->Visible()) ? MulDiv(hexHLogical_, dpi, 96) : 0;
    int sdH = (stdf_ && stdf_->Visible()) ? MulDiv(stdfHLogical_, dpi, 96) : 0;
    int cvH = (csv_ && csv_->Visible()) ? MulDiv(csvHLogical_, dpi, 96) : 0;
    int bfH = (bigfile_ && bigfile_->Visible()) ? MulDiv(bigfileHLogical_, dpi, 96) : 0;
    int lgH = (logPanel_ && logPanel_->Visible()) ? MulDiv(logHLogical_, dpi, 96) : 0;
    int termH = (terminal_ && terminal_->Visible()) ? MulDiv(termHLogical_, dpi, 96) : 0;
    int aiW = (ai_ && ai_->Visible()) ? MulDiv(aiWLogical_, dpi, 96) : 0;
    int dockH = (dockMgr_ && !dockMgr_->Empty()) ? dockMgr_->TotalHeight(dpi) : 0;
    int feW = (explorer_ && explorer_->Visible()) ? MulDiv(260, dpi, 96) : 0;
    int top = tbH;
    // AI 右栏占据最右整列（编辑器同高），dock 链与编辑器都止步于它的左缘
    int rightEdge = rc.right - aiW;
    int hostH = rc.bottom - sbH - rpH - hxH - sdH - cvH - bfH - lgH - termH - dockH - top;
    int edW = rightEdge - feW;
    bool split = workspace_ && workspace_->SplitActive();
    int leftW = edW;   // left view width (full when not split)
    if (split) {
        int splitterW = MulDiv(6, dpi, 96);
        leftW = MulDiv((std::max)(10, (std::min)(90, splitterPosLogical_)),
                       edW - splitterW, 100);
        int rightW = (std::max)(0, edW - splitterW - leftW);
        MoveWindow(editorHost_, feW, top, leftW, hostH, TRUE);
        MoveWindow(splitter_, feW + leftW, top, splitterW, hostH, TRUE);
        ShowWindow(splitter_, SW_SHOW);
        MoveWindow(editorHost1_, feW + leftW + splitterW, top,
                   (std::max)(0, rightW), hostH, TRUE);
        ShowWindow(editorHost1_, SW_SHOW);
    } else {
        MoveWindow(editorHost_, feW, top, leftW, hostH, TRUE);
        ShowWindow(splitter_, SW_HIDE);
        ShowWindow(editorHost1_, SW_HIDE);
    }
    if (explorer_ && explorer_->Visible())
        MoveWindow(explorer_->Hwnd(), 0, top, feW, hostH, TRUE);
    int dockY = hostH + top;
    int dockW = rightEdge - feW;
    if (results_ && rpH > 0) {
        MoveWindow(results_->Hwnd(), feW, dockY, dockW, rpH, TRUE);
        dockY += rpH;
    }
    if (hex_ && hxH > 0) {
        MoveWindow(hex_->Hwnd(), feW, dockY, dockW, hxH, TRUE);
        hex_->Layout(dockW, hxH);
        dockY += hxH;
    }
    if (stdf_ && sdH > 0) {
        MoveWindow(stdf_->Hwnd(), feW, dockY, dockW, sdH, TRUE);
        stdf_->Layout(dockW, sdH);
        dockY += sdH;
    }
    if (csv_ && cvH > 0) {
        MoveWindow(csv_->Hwnd(), feW, dockY, dockW, cvH, TRUE);
        csv_->Layout(dockW, cvH);
        dockY += cvH;
    }
    if (bigfile_ && bfH > 0) {
        MoveWindow(bigfile_->Hwnd(), feW, dockY, dockW, bfH, TRUE);
        bigfile_->Layout(dockW, bfH);
        dockY += bfH;
    }
    if (logPanel_ && lgH > 0) {
        MoveWindow(logPanel_->Hwnd(), feW, dockY, dockW, lgH, TRUE);
        logPanel_->Layout(dockW, lgH);
        dockY += lgH;
    }
    if (terminal_ && termH > 0) {
        MoveWindow(terminal_->Hwnd(), feW, dockY, dockW, termH, TRUE);
        terminal_->Layout(dockW, termH);
        dockY += termH;
    }
    // 4d: 插件可停靠面板排在底部 dock 链最末（DockManager 自行逐块布置）
    if (dockMgr_ && dockH > 0)
        dockMgr_->Layout(feW, dockY, dockW, dpi);
    if (workspace_) {
        workspace_->Layout(0, 0, leftW, hostH);
        if (split)
            workspace_->LayoutRight(edW - leftW, hostH);
    }
    // AI 右栏：编辑器右侧整列（状态栏上方）
    if (ai_ && aiW > 0) {
        MoveWindow(ai_->Hwnd(), rightEdge, top, aiW, hostH, TRUE);
        ai_->Layout(aiW, hostH);
    }
}

void MainWindow::Relayout() { LayoutChildren(); }

void MainWindow::MoveSplitter(int xAbs) {
    RECT rc; GetClientRect(hwnd_, &rc);
    int dpi = GetDpiForWindow(hwnd_);
    int feW = (explorer_ && explorer_->Visible()) ? MulDiv(260, dpi, 96) : 0;
    int total = (std::max)(1, (int)(rc.right - feW));
    int pct = (xAbs - feW) * 100 / total;
    splitterPosLogical_ = (std::max)(10, (std::min)(90, pct));
    LayoutChildren();
}

void MainWindow::UpdateTitleBar() {
    std::wstring gitSuffix;
    if (!git_.Branch().empty()) {
        gitSuffix = L" [" + git_.Branch();
        if (git_.Ahead()) gitSuffix += L" \u2191" + std::to_wstring(git_.Ahead());
        if (git_.Behind()) gitSuffix += L" \u2193" + std::to_wstring(git_.Behind());
        if (git_.UpstreamGone()) gitSuffix += L" \u2298";  // upstream deleted
        gitSuffix += L"]";
    }
    Document* d = workspace_->Active();
    if (!d) { SetWindowTextW(hwnd_, (L"xfsWinPad" + gitSuffix).c_str()); return; }
    std::wstring title;
    if (macro_.Recording()) title += L"[REC] ";
    if (d->editor.Modified()) title += L"* ";
    if (settings_.fullPathTitle && d->HasPath())
        title += d->path.wstring() + L" - xfsWinPad";
    else
        title += d->DisplayName() + L" - xfsWinPad";
    SetWindowTextW(hwnd_, (title + gitSuffix).c_str());
}

void MainWindow::UpdateStatusBar() {
    Document* d = workspace_->Active();
    if (!d || !status_) return;
    EditorStatus st = d->editor.Status();
    status_->SetPosition(st.line, st.column, st.selection);
    status_->SetDocInfo(st.length, st.lines, d->editor.WordCount());
    status_->SetEol(Editor::EolDisplayName(d->editor.Eol()));
    status_->SetEncoding(encoding::DisplayName(d->encoding));
    status_->SetReadOnly(d->readOnly || d->editor.ReadOnly());
    // language name from extension
    std::wstring lang = d->HasPath() ? L"Text" : L"Text";
    const LanguageInfo* li = DetectLanguage(d->DisplayName());
    if (li && li->lexerName) {
        static const struct { const char* n; const wchar_t* label; } names[] = {
            {"cpp", L"C/C++"}, {"python", L"Python"}, {"hypertext", L"HTML"},
            {"xml", L"XML"}, {"json", L"JSON"}, {"yaml", L"YAML"}, {"sql", L"SQL"},
            {"bash", L"Shell"}, {"powershell", L"PowerShell"}, {"batch", L"Batch"},
            {"css", L"CSS"}, {"rust", L"Rust"}, {"ruby", L"Ruby"}, {"lua", L"Lua"},
            {"perl", L"Perl"}, {"pascal", L"Pascal"}, {"fortran", L"Fortran"},
            {"verilog", L"Verilog"}, {"vhdl", L"VHDL"}, {"matlab", L"MATLAB"},
            {"markdown", L"Markdown"}, {"props", L"INI"}, {"toml", L"TOML"},
            {"makefile", L"Makefile"}, {"cmake", L"CMake"}, {"diff", L"Diff"},
            {"asm", L"Assembler"},
            // 批次 73：ATE 族（批次 72 起就有词法器，但一直没给显示名，
            // 状态栏把 .pat/.stil/.log/.pln 全显示成 "Text"）
            {kLexAtePattern, L"ATE Pattern (.pat)"},
            {kLexStil,       L"STIL (.stil)"},
            {kLexAteLog,     L"ATE Log (.log)"},
            {kLexChromaDec,  L"Chroma Device (.dec)"},
            {kLexChromaPlan, L"Chroma Plan (.pln)"},
        };
        for (auto& e : names)
            if (strcmp(li->lexerName, e.n) == 0) { lang = e.label; break; }
    }
    status_->SetLanguage(lang);
    // 批次 73：Chroma 3380 族的语法高亮、签名提示（乃至以后的诊断）都建立在
    // 「从语言手册抽取并人工复核」的数据模型上，**不是 CRAFT 编译器的输出**
    // （Chroma 没有公开错误码表）。这件事必须让用户看得见，否则容易把我们的
    // 提示当成编译器给的结论。其它语言清空该段。
    {
        const char* lx = (li && li->lexerName) ? li->lexerName : "";
        const bool chroma = strcmp(lx, kLexChromaPlan) == 0 ||
                            strcmp(lx, kLexChromaDec) == 0 ||
                            strcmp(lx, kLexAtePattern) == 0;
        status_->SetModelNote(chroma ? Tr(L"sb.model.note") : L"");
    }
}

void MainWindow::OnWorkspaceChanged() {
    UpdateTitleBar();
    UpdateStatusBar();
    RebuildWindowMenuItems();
    // reflect active document encoding in the Convert-to group
    Document* d = workspace_->Active();
    for (unsigned int i = 0; i < 11; ++i) {
        CheckMenuItem(menu_, Cmd::EncConvertFirst + i,
                      (d && (unsigned int)d->encoding == i)
                          ? MF_BYCOMMAND | MF_CHECKED
                          : MF_BYCOMMAND | MF_UNCHECKED);
    }
    // theme radio state
    CheckMenuRadioItem(menu_, Cmd::ThemeLight, Cmd::ThemeDark,
                       (theme_ && wcscmp(theme_->name, L"dark") == 0)
                           ? Cmd::ThemeDark : Cmd::ThemeLight,
                       MF_BYCOMMAND);

    // reflect active document language in the Language menu
    int langIdx = 0;   // "Normal Text" fallback
    if (d) {
        if (d->langIndex >= 0) {
            langIdx = d->langIndex;
        } else {
            const LanguageInfo* li = DetectLanguage(d->DisplayName());
            if (li && li->lexerName) {
                const LanguageMenuItem* catalog = LanguageMenuCatalog();
                for (int i = 0; catalog[i].label; ++i)
                    if (catalog[i].lexerName &&
                        strcmp(catalog[i].lexerName, li->lexerName) == 0) {
                        langIdx = i; break;
                    }
            }
        }
    }
    int langCount = 0;
    for (const LanguageMenuItem* e = LanguageMenuCatalog(); e->label; ++e) ++langCount;
    CheckMenuRadioItem(menu_, Cmd::LangFirst, Cmd::LangFirst + langCount - 1,
                       Cmd::LangFirst + langIdx, MF_BYCOMMAND);
}

void MainWindow::RebuildRecentMenu(const std::vector<std::wstring>& items) {
    recentItems_ = items;
    while (DeleteMenu(recentMenu_, 0, MF_BYPOSITION)) {}
    for (size_t i = 0; i < items.size(); ++i) {
        AppendMenuW(recentMenu_, MF_STRING,
                    Cmd::FileRecentFirst + (unsigned int)i, items[i].c_str());
    }
    DrawMenuBar(hwnd_);
}

// --- recent folders -----------------------------------------------------------

static std::wstring RecentFolderFilePath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData;
        CoTaskMemFree(appData);
    }
    ::CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    return base + L"\\xfsWinPad\\recent_folders.txt";
}

void MainWindow::RebuildRecentFolderMenu(const std::vector<std::wstring>& items) {
    recentFolderItems_ = items;
    while (DeleteMenu(recentFolderMenu_, 0, MF_BYPOSITION)) {}
    for (size_t i = 0; i < items.size(); ++i) {
        AppendMenuW(recentFolderMenu_, MF_STRING,
                    Cmd::FileRecentFolderFirst + (unsigned int)i, items[i].c_str());
    }
    DrawMenuBar(hwnd_);
}

void MainWindow::AddRecentFolder(const std::wstring& dir) {
    std::vector<std::wstring> items;
    items.push_back(dir);
    std::string content;
    if (ReadFileBytes(RecentFolderFilePath(), content)) {
        size_t start = 0;
        while (start < content.size() && items.size() < 10) {
            size_t end = content.find('\n', start);
            if (end == std::wstring::npos) end = content.size();
            std::string line = content.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) {
                std::wstring w = Utf8ToWide(line);
                if (w != dir) items.push_back(w);
                if (items.size() >= 10) break;
            }
            start = end + 1;
        }
    }
    std::string out;
    for (auto& s : items) out += WideToUtf8(s) + "\r\n";
    WriteFileBytes(RecentFolderFilePath(), out.data(), out.size());
    RebuildRecentFolderMenu(items);
}

static std::vector<std::wstring> LoadRecentFolders() {
    std::vector<std::wstring> items;
    std::string content;
    if (!ReadFileBytes(RecentFolderFilePath(), content)) return items;
    size_t start = 0;
    while (start < content.size() && items.size() < 10) {
        size_t end = content.find('\n', start);
        if (end == std::wstring::npos) end = content.size();
        std::string line = content.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            std::wstring w = Utf8ToWide(line);
            std::error_code ec;
            if (std::filesystem::is_directory(w, ec)) items.push_back(w);
        }
        start = end + 1;
    }
    return items;
}

// --- commands ------------------------------------------------------------------------

void MainWindow::ShowFindDialog(const std::wstring& prefill, int pageIndex) {
    if (!findDlg_) {
        findDlg_ = std::make_unique<FindDialog>();
        findDlg_->BindState(&workspace_->Find());   // single shared search state
    }
    // 每次重新打开（对话框已关闭、hwnd_ 置空）时都从首选项重新播种搜索默认值。
    // 之前只在 findDlg_ 首次创建时播种，而它是跨会话存活的成员，之后改首选项
    // 再打开 Ctrl+F 不会生效（2026-08-30 用户报告）。
    if (!findDlg_->IsVisible()) {
        workspace_->Find().matchCase = settings_.searchMatchCase;
        workspace_->Find().wholeWord = settings_.searchWholeWord;
    }
    if (!prefill.empty()) findDlg_->SetSearchText(prefill);
    else if (Document* d = workspace_->Active()) {
        // prefill from selection. 本仓库 vendored Scintilla 的 SCI_GETSELTEXT
        // 返回值【不含】结尾 NUL（Editor.cxx: return selectedText.Length()），
        // 旧代码按"含 NUL"先 -1 再截断，prefill 永远少最后一个字符
        // （2026-09-15 报告：选中 PAD_IOVDD_4 只带入 PAD_IOVDD_）。
        sptr_t len = d->editor.Send(SCI_GETSELTEXT, 0, 0);
        if (len > 0 && len < 512) {
            // 但写入仍是 len+1 字节（补结尾 NUL），缓冲区照旧多留 1 字节
            std::string sel((size_t)len + 1, '\0');
            d->editor.Send(SCI_GETSELTEXT, 0, (LPARAM)sel.data());
            sel.resize((size_t)len);
            findDlg_->SetSearchText(Utf8ToWide(sel));
        }
    }
    findDlg_->Show(hwnd_, inst_, pageIndex);
}

void MainWindow::ShowSearchResults(std::vector<SearchHit> hits) {
    if (!results_) {
        results_ = std::make_unique<ResultsPanel>();
        results_->Create(hwnd_, inst_);
        results_->onActivateRow = [this](int row) { OnResultActivate(row); };
        results_->onClose = [this]() {
            results_->Clear();
            LayoutChildren();
            if (Document* d = workspace_->Active())
                ::SetFocus(d->editor.Hwnd());
        };
        results_->onHeightChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            rpHLogical_ = std::max(90, std::min(1200,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
    }
    int files = 0;
    {
        std::vector<std::wstring> seen;
        for (const SearchHit& h : hits) {
            bool dup = false;
            for (const auto& s : seen) if (s == h.file) { dup = true; break; }
            if (!dup) { seen.push_back(h.file); ++files; }
        }
    }
    wchar_t summary[160];
    swprintf_s(summary, L"%s",
        I18n::Instance().Fmt(L"sr.countfmt", {std::to_wstring(hits.size()),
                                              std::to_wstring(files)}).c_str());
    if (hex_ && hex_->Visible()) {
        hex_->Hide();   // bottom dock slot shared with hex panel
        CheckMenuItem(menu_, Cmd::ViewHexView, MF_UNCHECKED);
    }
    if (stdf_ && stdf_->Visible()) {
        stdf_->Hide();   // bottom dock slot shared with STDF panel
        CheckMenuItem(menu_, Cmd::ViewStdfView, MF_UNCHECKED);
    }
    if (csv_ && csv_->Visible()) {
        csv_->Hide();   // bottom dock slot shared with CSV table view
        CheckMenuItem(menu_, Cmd::ViewCsvView, MF_UNCHECKED);
    }
    results_->SetResults(std::move(hits), summary);
    LayoutChildren();
}

void MainWindow::OnResultActivate(int row) {
    if (!results_) return;
    const SearchHit* h = results_->HitAt(row);
    if (!h) { Logger::Warn("OnResultActivate: bad row"); return; }

    Logger::Info("OnResultActivate row=" + std::to_string(row) +
                 " docIndex=" + std::to_string(h->docIndex) +
                 " line=" + std::to_string(h->line) +
                 " span=[" + std::to_string(h->start) + "," + std::to_string(h->end) +
                 "] file='" + WideToUtf8(h->file) + "'");

    auto selectInDoc = [&](Document* doc) {
        if (!doc) { Logger::Warn("OnResultActivate: no document"); return; }
        sptr_t len = doc->editor.Send(SCI_GETLENGTH);
        sptr_t s = (std::min)(h->start, len);
        sptr_t e = (std::min)(h->end, len);
        if (e < s) e = s;
        doc->editor.SelectRange(s, e);
        Logger::Info("  selected in editor, clamped span=[" +
                     std::to_string(s) + "," + std::to_string(e) + "]");
    };

    // 双视图路由：docView 0=左（主）视图，1=右（其他）视图
    if (h->docIndex >= 0 &&
        (h->docView == 0 ? h->docIndex < workspace_->Count()
                         : h->docIndex < workspace_->Count1())) {
        if (h->docView == 0) {
            workspace_->Activate(h->docIndex);
            selectInDoc(workspace_->Active());
        } else {
            workspace_->ActivateView1(h->docIndex);
            selectInDoc(workspace_->Active1());
        }
        return;
    }

    // fallback for legacy hits without index: match by display name
    if (h->path.empty()) {
        int idx = -1, view = 0;
        for (int i = 0; i < workspace_->Count(); ++i)
            if (workspace_->DocumentAt(i)->DisplayName() == h->file) { idx = i; break; }
        if (idx < 0) {
            for (int i = 0; i < workspace_->Count1(); ++i)
                if (workspace_->FindByTabIndex1(i)->DisplayName() == h->file) {
                    idx = i; view = 1; break;
                }
        }
        if (idx >= 0) {
            if (view == 0) {
                workspace_->Activate(idx);
                selectInDoc(workspace_->Active());
            } else {
                workspace_->ActivateView1(idx);
                selectInDoc(workspace_->Active1());
            }
            Logger::Info("  routed via name fallback view=" + std::to_string(view) +
                         " idx=" + std::to_string(idx));
        } else {
            Logger::Warn("  no route: docIndex<0, no path, no name match");
        }
        return;
    }

    // disk hit (Find in Files/Projects): open or activate, then select.
    // Both views may hold the file; prefer the left view, then the right.
    int target = -1, targetView = 0;
    for (int i = 0; i < workspace_->Count(); ++i)
        if (workspace_->DocumentAt(i)->path == std::filesystem::path(h->path)) {
            target = i; break;
        }
    if (target < 0) {
        for (int i = 0; i < workspace_->Count1(); ++i)
            if (workspace_->FindByTabIndex1(i)->path == std::filesystem::path(h->path)) {
                target = i; targetView = 1; break;
            }
    }
    if (target >= 0) {
        if (targetView == 0) {
            workspace_->Activate(target);
            Logger::Info("  disk hit already open in view 0, activated idx=" +
                         std::to_string(target));
        } else {
            workspace_->ActivateView1(target);
            Logger::Info("  disk hit already open in view 1, activated idx=" +
                         std::to_string(target));
        }
    } else {
        workspace_->OpenPath(h->path, h->line);
        Logger::Info("  disk hit opened via OpenPath");
    }
    selectInDoc(workspace_->Active());
}

void MainWindow::HandleSearchAction(SearchAction act) {
    Document* d = workspace_->Active();
    if (!d) return;
    const FindState& st = workspace_->Find();
    Logger::Info("HandleSearchAction act=" + std::to_string((int)act) +
                 " text='" + WideToUtf8(st.text) + "'");

    switch (act) {
        case SearchAction::FindNext:
            FindInEditor(d->editor, st, FindDirection::Forward);
            break;
        case SearchAction::FindPrev:
            FindInEditor(d->editor, st, FindDirection::Backward);
            break;
        case SearchAction::Replace:
            ReplaceCurrentInEditor(d->editor, st);
            break;
        case SearchAction::ReplaceAll: {
            int n = ReplaceAllInEditor(d->editor, st);
            MessageBoxW(hwnd_, I18n::Instance().Fmt(L"sr.replacecur",
                        {std::to_wstring(n)}).c_str(),
                        L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case SearchAction::CountCurrent: {
            std::vector<SearchHit> hits;
            CollectHitsInEditor(d->editor, d->DisplayName(), st, &hits);
            MessageBoxW(hwnd_,
                        I18n::Instance().Fmt(L"sr.countcur",
                        {st.text, std::to_wstring(hits.size())}).c_str(),
                        L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case SearchAction::FindAllCurrent: {
            std::vector<SearchHit> hits;
            // 活动文档可能在任一视图：查清所在视图与索引再生成带路由的命中
            int idx = workspace_->IndexOf(d);
            int view = 0;
            if (idx < 0) { idx = workspace_->IndexOf1(d); view = 1; }
            CollectHitsInEditor(d->editor, d->DisplayName(), st, &hits, idx);
            if (view == 1)
                for (auto& h : hits) h.docView = 1;
            ShowSearchResults(std::move(hits));
            break;
        }
        case SearchAction::FindAllOpen: {
            std::vector<SearchHit> hits;
            for (int i = 0; i < workspace_->Count(); ++i) {
                Document* doc = workspace_->DocumentAt(i);
                CollectHitsInEditor(doc->editor, doc->DisplayName(), st, &hits, i);
            }
            // 双视图：右视图文档同样参与“所有打开文件”
            for (int i = 0; i < workspace_->Count1(); ++i) {
                Document* doc = workspace_->FindByTabIndex1(i);
                size_t before = hits.size();
                CollectHitsInEditor(doc->editor, doc->DisplayName(), st, &hits, i);
                for (size_t k = before; k < hits.size(); ++k) hits[k].docView = 1;
            }
            ShowSearchResults(std::move(hits));
            break;
        }
        case SearchAction::FindInFiles: {
            if (!findDlg_) break;
            FindInFilesOptions o;
            o.directory = findDlg_->LastFif().directory;
            o.filters   = findDlg_->LastFif().filters;
            o.recursive = findDlg_->LastFif().recursive;
            o.st        = st;
            std::vector<SearchHit> hits = RunFindInFiles(o);
            Logger::Info("FindInFiles dir='" + WideToUtf8(o.directory) +
                         "' filters='" + WideToUtf8(o.filters) +
                         "' hits=" + std::to_string(hits.size()));
            ShowSearchResults(std::move(hits));
            break;
        }
        case SearchAction::ReplaceInFiles: {
            if (!findDlg_) break;
            auto& fif = findDlg_->LastFif();
            int filesChanged = RunReplaceInFiles(
                {fif.directory, fif.filters, st, fif.recursive},
                findDlg_->LastFif().replaceWith);
            MessageBoxW(hwnd_,
                I18n::Instance().Fmt(L"sr.fifdone",
                {std::to_wstring(filesChanged)}).c_str(),
                Tr(L"sr.fiftitle"), MB_OK | MB_ICONINFORMATION);
            break;
        }
        case SearchAction::FindInProjects: {
            const FindInProjectsUi& pui = findDlg_ ? findDlg_->LastProj()
                                                   : FindInProjectsUi{};
            std::vector<SearchHit> hits;

            // v2: an open folder is THE project scope
            if (explorer_ && !explorer_->Root().empty()) {
                FindInFilesOptions o;
                o.directory = explorer_->Root();
                o.filters   = pui.filters;
                o.recursive = true;
                o.st        = st;
                Logger::Info("FindInProjects v2: root='" + WideToUtf8(o.directory) + "'");
                hits = RunFindInFiles(o);
            } else {
                // v1 fallback: every folder that owns an open document (recursive,
                // both views)
                std::vector<std::wstring> dirs;
                auto addDirOf = [&](const Document* doc) {
                    auto dir = doc->path.parent_path().wstring();
                    if (dir.empty()) return;
                    for (const auto& d2 : dirs) if (d2 == dir) return;
                    dirs.push_back(dir);
                };
                for (int i = 0; i < workspace_->Count(); ++i)
                    addDirOf(workspace_->DocumentAt(i));
                for (int i = 0; i < workspace_->Count1(); ++i)
                    addDirOf(workspace_->FindByTabIndex1(i));
                for (const auto& dir : dirs) {
                    FindInFilesOptions o;
                    o.directory = dir;
                    o.filters   = pui.filters;
                    o.recursive = true;
                    o.st        = st;
                    auto part = RunFindInFiles(o);
                    hits.insert(hits.end(), part.begin(), part.end());
                }
            }
            ShowSearchResults(std::move(hits));
            break;
        }
        case SearchAction::ReplaceAllOpen: {
            if (st.text.empty()) break;
            int total = 0, files = 0;
            for (int i = 0; i < workspace_->Count(); ++i) {
                Document* doc = workspace_->DocumentAt(i);
                int n = ReplaceAllInEditor(doc->editor, st);
                if (n > 0) { total += n; ++files; }
            }
            // 双视图：右视图文档同样参与“所有打开文件替换”
            for (int i = 0; i < workspace_->Count1(); ++i) {
                Document* doc = workspace_->FindByTabIndex1(i);
                int n = ReplaceAllInEditor(doc->editor, st);
                if (n > 0) { total += n; ++files; }
            }
            workspace_->SyncTabTitles();
            UpdateTitleBar();
    MessageBoxW(hwnd_,
        I18n::Instance().Fmt(L"sr.projdone",
        {std::to_wstring(files), std::to_wstring(total)}).c_str(),
        L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case SearchAction::MarkAll: {
            const bool allOpen = findDlg_ && findDlg_->MarkAllOpenScope();
            const bool bookmark = findDlg_ && findDlg_->MarkBookmarkLines();
            int marked = 0;
            auto markDoc = [&](Document* doc) {
                std::vector<SearchHit> hits;
                CollectHitsInEditor(doc->editor, doc->DisplayName(), st, &hits);
                for (const SearchHit& h : hits)
                    if (doc->editor.MarkLine(h.line)) ++marked;
            };
            if (allOpen) {
                for (int i = 0; i < workspace_->Count(); ++i)
                    markDoc(workspace_->DocumentAt(i));
                for (int i = 0; i < workspace_->Count1(); ++i)
                    markDoc(workspace_->FindByTabIndex1(i));
            } else if (d) {
                markDoc(d);
            }
            Logger::Info("MarkAll allOpen=" + std::to_string(allOpen ? 1 : 0) +
                         " marked=" + std::to_string(marked));
    MessageBoxW(hwnd_,
        I18n::Instance().Fmt(L"sr.marked",
        {std::to_wstring(marked)}).c_str(),
        L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case SearchAction::MarkClear: {
            const bool allOpen = findDlg_ && findDlg_->MarkAllOpenScope();
            if (allOpen) {
                for (int i = 0; i < workspace_->Count(); ++i)
                    workspace_->DocumentAt(i)->editor.ClearLineMarks();
                for (int i = 0; i < workspace_->Count1(); ++i)
                    workspace_->FindByTabIndex1(i)->editor.ClearLineMarks();
            } else if (d) {
                d->editor.ClearLineMarks();
            }
            break;
        }
    }
}

void MainWindow::DoGotoLine() {
    Document* d = workspace_->Active();
    if (!d) return;
    int line = GotoDialog::Run(hwnd_, inst_, (int)d->editor.Send(SCI_GETLINECOUNT));
    if (line > 0) d->editor.GotoLine(line);
}

void MainWindow::InsertDateTime() {
    Document* d = workspace_->Active();
    if (!d) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    d->editor.InsertTextAtCaret(WideToUtf8(buf));
}

void MainWindow::ToggleWordWrap() {
    Document* d = workspace_->Active();
    if (!d) return;
    bool on = !d->editor.WordWrap();
    d->editor.SetWordWrap(on);
    CheckMenuItem(menu_, Cmd::ViewWordWrap,
                  on ? MF_CHECKED : MF_UNCHECKED);
}

void MainWindow::PrintDocument() {
    Document* d = workspace_->Active();
    if (!d) return;

    // get printer DC via PrintDlg
    PRINTDLGW pd{};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = hwnd_;
    pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE;
    pd.nMinPage = 1; pd.nMaxPage = 1; pd.nFromPage = 1; pd.nToPage = 1;
    if (!PrintDlgW(&pd) || !pd.hDC) return;

    HDC pdc = pd.hDC;
    int pxPerInchX = GetDeviceCaps(pdc, LOGPIXELSX);
    int pxPerInchY = GetDeviceCaps(pdc, LOGPIXELSY);
    int pageW = GetDeviceCaps(pdc, HORZRES);
    int pageH = GetDeviceCaps(pdc, VERTRES);
    int margin = pxPerInchX / 2;   // ~0.5 inch margins

    DOCINFOW di{};
    di.cbSize = sizeof(di);
    di.lpszDocName = d->DisplayName().c_str();
    if (::StartDocW(pdc, &di) <= 0) { DeleteDC(pdc); return; }

    HFONT printFont = CreateFontW(-MulDiv(10, pxPerInchY, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    auto oldF = (HFONT)SelectObject(pdc, printFont);

    TEXTMETRICW tm;
    GetTextMetricsW(pdc, &tm);
    int lineH = tm.tmHeight + tm.tmExternalLeading;
    int linesPerPage = (pageH - 2 * margin) / lineH;

    sptr_t totalLines = d->editor.Send(SCI_GETLINECOUNT);
    sptr_t curLine = 0;
    int pageNum = 0;

    while (curLine < totalLines) {
        if (::StartPage(pdc) <= 0) break;
        ++pageNum;

        wchar_t hdr[128];
        swprintf_s(hdr, L"%s - Page %d", d->DisplayName().c_str(), pageNum);
        TextOutW(pdc, margin, margin / 2, hdr, (int)wcslen(hdr));

        for (int row = 0; row < linesPerPage && curLine < totalLines; ++row, ++curLine) {
            char buf[4096];
            sptr_t len = d->editor.Send(SCI_GETLINE, curLine, (LPARAM)buf);
            if (len <= 0) continue;
            buf[len] = '\0';
            // strip CR/LF
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';

            int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)len, nullptr, 0);
            if (wlen <= 0) continue;
            std::wstring wline((size_t)wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, buf, (int)len, wline.data(), wlen);

            int y = margin + MulDiv(20, pxPerInchY, 72) + row * lineH;
            if (y + lineH > pageH - margin) { --curLine; break; }
            TextOutW(pdc, margin, y, wline.c_str(), (int)wline.size());
        }
        ::EndPage(pdc);
    }

    ::EndDoc(pdc);
    SelectObject(pdc, oldF);
    DeleteObject(printFont);
    DeleteDC(pdc);
}

void MainWindow::OpenFolderDialog() {
    IFileDialog* fd = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IFileDialog, (void**)&fd);
    if (FAILED(hr) || !fd) {
        Logger::Error("OpenFolderDialog: CoCreateInstance failed");
        return;
    }
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    fd->SetTitle(Tr(L"msg.pickfolder"));
    bool picked = false;
    if (SUCCEEDED(fd->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(fd->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                settings_.explorerVisible = true;   // opening a folder shows the panel
                SetProjectRoot(path, true);
                AddRecentFolder(path);
                ::CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }
    fd->Release();
    Logger::Info(std::string("OpenFolderDialog: ") + (picked ? "picked" : "cancelled"));
}

void MainWindow::SetProjectRoot(const std::wstring& dir, bool persist) {
    if (dir.empty()) return;
    if (!explorer_) {
        explorer_ = std::make_unique<FileExplorer>();
        if (!explorer_->Create(hwnd_, inst_)) { explorer_.reset(); return; }
        explorer_->onOpenFile = [this](const std::wstring& path) {
            OpenUserFile(path);
        };
        WireExplorerGit();
    }
    explorer_->SetRoot(dir);
    git_.RequestForPath(dir);
    // panel visibility follows the persisted toggle state, not the root
    if (settings_.explorerVisible) {
        explorer_->Show();
        CheckMenuItem(menu_, Cmd::ViewExplorer, MF_CHECKED);
    }
    if (persist) {
        settings_.projectRoot = dir;
        SettingsSave(SettingsFilePath(), settings_);
    }
    LayoutChildren();
    Logger::Info("Project root: " + WideToUtf8(dir));
}

void MainWindow::CloseFolder() {
    settings_.projectRoot.clear();
    settings_.explorerVisible = false;
    SettingsSave(SettingsFilePath(), settings_);
    if (explorer_) {
        explorer_->CloseFolder();
        explorer_->Hide();
    }
    git_.ClearNow();
    UpdateTitleBar();
    CheckMenuItem(menu_, Cmd::ViewExplorer, MF_UNCHECKED);
    LayoutChildren();
    Logger::Info("Project folder closed");
}

void MainWindow::ToggleExplorer() {
    if (!explorer_) {
        explorer_ = std::make_unique<FileExplorer>();
        if (!explorer_->Create(hwnd_, inst_)) { explorer_.reset(); return; }
        explorer_->onOpenFile = [this](const std::wstring& path) {
            OpenUserFile(path);
        };
        WireExplorerGit();
    }
    bool show = !explorer_->Visible();
    if (show && explorer_->Root().empty()) {
        std::wstring dir;
        if (!settings_.projectRoot.empty())
            dir = settings_.projectRoot;             // remembered project folder
        else {
            Document* d = workspace_->Active();
            if (d && d->HasPath())
                dir = d->path.parent_path().wstring();
            else {
                wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
                dir = cwd;
            }
        }
        explorer_->SetRoot(dir);
        git_.RequestForPath(dir);
    }
    if (show) explorer_->Show(); else explorer_->Hide();
    CheckMenuItem(menu_, Cmd::ViewExplorer,
                  show ? MF_CHECKED : MF_UNCHECKED);
    settings_.explorerVisible = show;   // remember for next launch
    SettingsSave(SettingsFilePath(), settings_);
    LayoutChildren();
}

// --- crash-safe autosave ---------------------------------------------------------

static std::wstring AutoSaveDir() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        base = appData; ::CoTaskMemFree(appData);
    }
    auto dir = base + L"\\xfsWinPad\\autosave";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void MainWindow::DoAutoSave() {
    if (!workspace_) return;
    auto dir = AutoSaveDir();
    int saved = 0;
    for (int i = 0; i < workspace_->Count(); ++i) {
        Document* doc = workspace_->DocumentAt(i);
        if (!doc || !doc->editor.Modified()) continue;
        sptr_t len = doc->editor.Send(SCI_GETLENGTH);
        // huge buffers: streaming a 500MB+ copy to disk every 30s is worse
        // than losing the undo state of one crashed session
        if (len < 0 || (unsigned long long)len > 200ULL * 1024 * 1024) {
            Logger::Info("AutoSave: skip " + std::to_string(i) + " (" +
                         std::to_string((long long)len) + " bytes, too large)");
            continue;
        }
        std::wstring asFile = dir + L"\\" + std::to_wstring(i) + L".asb";
        HANDLE h = ::CreateFileW(asFile.c_str(), GENERIC_WRITE, 0,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        // stream the buffer out in chunks (no full-size copy)
        static const size_t kChunk = 8u << 20;
        bool ok = true;
        std::string chunk;
        for (sptr_t pos = 0; ok && pos < len; ) {
            doc->editor.GetTextRangeUtf8(pos, kChunk, chunk);
            if (chunk.empty()) break;
            DWORD w = 0;
            if (!::WriteFile(h, chunk.data(), (DWORD)chunk.size(), &w, nullptr) ||
                w != chunk.size())
                ok = false;
            pos += (sptr_t)chunk.size();
        }
        ::CloseHandle(h);
        if (ok) ++saved;
    }
    if (saved > 0)
        Logger::Info("AutoSave: " + std::to_string(saved) + " buffer(s) saved");
}

void MainWindow::CheckAutoSaveRecovery() {
    auto dir = AutoSaveDir();
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW((dir + L"\\*.asb").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(fd.cFileName);
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);

    if (files.empty()) return;

    // oversized snapshots are dropped, not restored: a 500MB synchronous
    // restore would block startup (matches DoAutoSave's save cap)
    const unsigned long long kMaxRecover = 200ULL * 1024 * 1024;
    std::vector<std::wstring> recover;
    for (auto& f : files) {
        auto fullPath = dir + L"\\" + f;
        std::error_code ec;
        auto sz = std::filesystem::file_size(fullPath, ec);
        if (!ec && sz <= kMaxRecover)
            recover.push_back(f);
        else
            Logger::Info("Crash recovery: dropping oversize snapshot " +
                         WideToUtf8(f) + " (" + std::to_string((long long)sz) + " bytes)");
        DeleteFileW(fullPath.c_str());
    }
    if (recover.empty()) {
        Logger::Info("Crash recovery: dropped all " +
                     std::to_string(files.size()) + " snapshot(s) (oversize)");
        return;
    }

    // offer recovery. 弹框前确保主窗可见：Create 期间窗口尚未 ShowWindow
    //（正常显示在 main.cpp Create 返回后），直接弹会让对话框凭空悬停、
    // 主窗不可见，看起来像启动卡死。
    if (!::IsWindowVisible(hwnd_)) ::ShowWindow(hwnd_, SW_SHOW);
    wchar_t msg[256];
    swprintf_s(msg, L"%s",
        I18n::Instance().Fmt(L"msg.crash.found",
        {std::to_wstring(recover.size())}).c_str());
    int r = MessageBoxW(hwnd_, msg, Tr(L"msg.crash.title"),
                        MB_YESNO | MB_ICONQUESTION);
    for (auto& f : recover) {
        auto fullPath = dir + L"\\" + f;
        if (r == IDYES) {
            std::string raw;
            ReadFileBytes(fullPath, raw);
            workspace_->NewDocument();
            Document* doc = workspace_->Active();
            if (doc) doc->editor.SetTextUtf8(raw);
        }
        DeleteFileW(fullPath.c_str());
    }
    Logger::Info("Crash recovery: " + std::to_string(files.size()) + " file(s)");
}


void MainWindow::ToggleLogPanel() {
    if (!logPanel_) {
        logPanel_ = std::make_unique<LogPanel>();
        if (!logPanel_->Create(hwnd_, inst_)) { logPanel_.reset(); return; }
        logPanel_->onClose = [this]() {
            logPanel_->Hide();
            CheckMenuItem(menu_, Cmd::ViewLogPanel, MF_UNCHECKED);
            LayoutChildren();
        };
        logPanel_->onHeightChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            logHLogical_ = std::max(90, std::min(1400,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
        if (theme_) logPanel_->ApplyTheme(*theme_);
    }
    bool show = !logPanel_->Visible();
    if (show) {
        // docks stack; user can close others as needed
        ::ShowWindow(logPanel_->Hwnd(), SW_SHOW);
    } else {
        logPanel_->Hide();
    }
    CheckMenuItem(menu_, Cmd::ViewLogPanel, show ? MF_CHECKED : MF_UNCHECKED);
    LayoutChildren();
}

void MainWindow::ToggleTerminal() {
    if (!terminal_) {
        terminal_ = std::make_unique<TerminalPanel>();
        if (!terminal_->Create(hwnd_, inst_)) { terminal_.reset(); return; }
        terminal_->onClose = [this]() {
            terminal_->Hide();
            // clear the menu state before StopShell, which may block briefly
            // waiting on the ConPTY reader thread
            CheckMenuItem(menu_, Cmd::ViewTerminal, MF_UNCHECKED);
            terminal_->StopShell();   // kill the shell so reopening is clean
            LayoutChildren();
        };
        terminal_->onHeightChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            termHLogical_ = std::max(120, std::min(1400,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
        // apply persisted terminal font before first use
        terminal_->SetFont(settings_.termFontName, settings_.termFontSize);
        if (theme_) terminal_->ApplyTheme(*theme_);
    }
    bool show = !terminal_->Visible();
    if (show) {
        ::ShowWindow(terminal_->Hwnd(), SW_SHOW);
        if (!terminal_->StartShell())
            Logger::Error("Terminal: ConPTY shell failed to start");
    } else {
        terminal_->Hide();
        terminal_->StopShell();   // stop PTY so reopening starts a fresh session
    }
    CheckMenuItem(menu_, Cmd::ViewTerminal, show ? MF_CHECKED : MF_UNCHECKED);
    LayoutChildren();
}

void MainWindow::ToggleAiPanel() {
    if (!ai_) {
        ai_ = std::make_unique<AiPanel>();
        if (!ai_->Create(hwnd_, inst_)) { ai_.reset(); return; }
        ai_->onClose = [this]() {
            ai_->Hide();
            CheckMenuItem(menu_, Cmd::ViewAiPanel, MF_UNCHECKED);
            LayoutChildren();
        };
        ai_->onWidthChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            // 最小 360 逻辑px：顶部 label+历史+模型下拉+关闭 不重叠的下限
            aiWLogical_ = std::max(360, std::min(1200,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
        // 模型切换 → settings 持久化（"provider/model"）
        ai_->onModelChanged = [this](const std::wstring& p, const std::wstring& m) {
            settings_.aiModel = p + L"/" + m;
            SettingsSave(SettingsFilePath(), settings_);
        };
        // openai 直连模式切换本地模型 → aiLocalModel 持久化
        ai_->onOaiModelChanged = [this](const std::wstring& m) {
            settings_.aiLocalModel = m;
            SettingsSave(SettingsFilePath(), settings_);
        };
        // 发送前快照打开文档的磁盘状态；响应完成后对比 → 有变化且文档干净
        // 则静默重载（AI 改盘后编辑器自动显示新内容）
        ai_->onBeforeSend = [this]() { SnapDiskState(); };
        ai_->onResponseDone = [this](const std::vector<std::wstring>& touched) {
            AutoReloadChanged();
            // AI 写过、但当前没有任何标签页打开的文件 → transcript 提示
            //（自动重载只覆盖已打开文档；未打开的改动需要用户自己去看）
            for (const auto& f : touched) {
                if (!workspace_->FindByPath(f)) {
                    if (ai_)
                        ai_->SystemLine(I18n::Instance().Fmt(
                            L"panel.ai.touchedunopened", {f}));
                    Logger::Info("AutoReloadChanged: AI touched unopened file: " +
                                 WideToUtf8(f));
                }
            }
            // AI 工场：本轮若有进行中的插件项目 → 尝试安装构建产物。
            // 失败时自动回喂修复提示（一轮自救）；连续失败不过三（防死循环）
            if (!workshopPending_.empty()) {
                std::wstring proj = workshopPending_;
                workshopPending_.clear();
                std::wstring err;
                std::wstring name = workshop_->InstallBuilt(proj, &err);
                if (!name.empty()) {
                    if (ai_) ai_->SystemLine(I18n::Instance().Fmt(
                        L"panel.ai.plugininstalled", {name}));
                    workshopRetries_ = 0;
                } else {
                    if (ai_) ai_->SystemLine(I18n::Instance().Fmt(
                        L"panel.ai.pluginfailed", {err}));
                    Logger::Warn("Workshop: install failed - " + WideToUtf8(err));
                    if (ai_ && workshopRetries_ < 3) {
                        ++workshopRetries_;
                        workshopPending_ = proj;   // 下一轮响应再试安装
                        std::wstring fix;
                        fix += L"The plugin you built failed to load: " + err +
                               L"\n";
                        fix += L"Project: " + proj + L"\n";
                        fix += L"Fix it: re-read " + workshop_->SdkDir() +
                               L"\\PLUGIN_DEV_GUIDE.md, check that the three "
                               L"exports are plain C (no /TP, no C++ name "
                               L"mangling - use the template's pattern), "
                               L"rebuild, and reply DONE.";
                        ai_->SendUserText(fix);
                    }
                }
            }
        };
        // 上下文注入：发送时取活跃文档快照（路径/光标/选区）
        ai_->SetContextProvider([this]() { return BuildAiContext(); });
        // /plugin <描述> —— AI 插件工场：脚手架 → 标准提示词 → 发送
        ai_->onPluginTask = [this](const std::wstring& task) {
            RunPluginWorkshop(task);
        };
        ai_->SetAttachContext(settings_.aiAttachContext);
        ai_->SetAutoApprove(settings_.aiAutoApprove);
        ai_->SetBackend(settings_.aiBackend.empty() ? L"opencode"
                                                    : settings_.aiBackend,
                        settings_.aiEndpoint, settings_.aiApiKey,
                        settings_.aiLocalModel);
        if (!settings_.aiModel.empty()) {
            const std::wstring& s = settings_.aiModel;
            size_t slash = s.find(L'/');
            if (slash != std::wstring::npos) {
                ai_->SetPreferredModel(s.substr(0, slash), s.substr(slash + 1));
            }
        }
        if (theme_) ai_->ApplyTheme(*theme_);
    }
    bool show = !ai_->Visible();
    if (show) {
        ::ShowWindow(ai_->Hwnd(), SW_SHOW);
        // 连接 serve（幂等；工作目录 = 当前项目根或文档目录，供 opencode 定位 worktree）
        std::wstring wd = settings_.projectRoot;
        if (wd.empty()) {
            Document* d = workspace_ ? workspace_->Active() : nullptr;
            if (d && !d->path.empty()) wd = d->path.parent_path().wstring();
        }
        ai_->EnsureReady(wd);
    } else {
        ai_->Hide();
    }
    CheckMenuItem(menu_, Cmd::ViewAiPanel, show ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu_, Cmd::AiToggleContext, MF_BYCOMMAND |
        (settings_.aiAttachContext ? MF_CHECKED : MF_UNCHECKED));
    LayoutChildren();
}

// StdfPanel「AI 分析」（批次 28）：当前 STDF 的统计块（yield/bin/失败测试项
// top N）交给 AiPanel。面板未开时先打开（EnsureReady 连接期间外部请求自动
// 排队，就绪后补发）；busy 时也排队（本轮回复完成补发）。
void MainWindow::AiAskStdf() {
    if (!stdf_ || !stdf_->Visible()) return;
    std::wstring stats = stdf_->BuildAiStatsBlock();
    if (stats.empty()) return;
    if (!ai_ || !ai_->Visible()) ToggleAiPanel();
    if (ai_) {
        ai_->SendExternal(
            I18n::Instance().Fmt(L"panel.ai.stdfblock",
                                 {stdf_->FilePath()}),
            stats);
        ::SetFocus(ai_->Hwnd());
    }
}

// AI 插件工场：/plugin <任务描述>
//   1. 首次惰性建 Workshop（含 SDK 释放）
//   2. Scaffold 脚手架（模板已把任务名/描述/命令名替换）
//   3. BuildPrompt 包装标准规范 + 脚手架路径 → 作为普通消息发给 AI
//   4. onResponseDone 时（见上方 lambda）InstallBuilt 热加载
void MainWindow::RunPluginWorkshop(const std::wstring& taskDesc) {
    if (!ai_) return;
    if (!workshop_) {
        workshop_ = std::make_unique<Workshop>(plugins_.get(),
                                               PluginManager::PluginDir());
        std::wstring err;
        if (!workshop_->EnsureSdk(&err)) {
            ai_->SystemLine(I18n::Instance().Fmt(
                L"panel.ai.pluginfailed", {L"SDK init: " + err}));
            return;
        }
    }
    // 命令名：任务前 6 个 ASCII 词组成（不足回退目录名）
    std::wstring words;
    int n = 0;
    for (wchar_t c : taskDesc) {
        if (c == L' ' || c == L'\t' || c == L',') {
            if (!words.empty() && words.back() != L' ') { words += L' '; ++n; }
            if (n >= 6) break;
        } else if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                   (c >= L'0' && c <= L'9')) {
            words += c;
        }
    }
    while (!words.empty() && (words.back() == L' ')) words.pop_back();
    std::wstring folder = Workshop::SanitizeFolderName(
        words.empty() ? taskDesc : words);
    std::wstring proj = workshop_->Scaffold(taskDesc, folder,
                                            folder, taskDesc);
    if (proj.empty()) {
        ai_->SystemLine(I18n::Instance().Fmt(
            L"panel.ai.pluginfailed", {L"scaffold failed"}));
        return;
    }
    workshopPending_ = proj;

    // 工场消息直接走 SendUserText 的普通路径（提示词就是「正文」），
    // 附带上下文照旧——AI 可能需要看当前文档来理解任务。
    std::wstring prompt = workshop_->BuildPrompt(proj, taskDesc);
    ai_->SendUserText(prompt);

    // SendUserText 会把 workshopPending_ 的安装时机挂到本轮响应完成；
    // 但 prompt 里已包含脚手架路径，AI 用 bash 跑 build.cmd 的产物落
    // build\output.dll，onResponseDone 收尾安装 + LoadNew 热加载。
    Logger::Info("Workshop: task started, project=" + WideToUtf8(proj));
}

// 发送 AI 消息时附带的编辑器上下文快照：
//   块格式（给模型）：
//     [Editor context]
//     File: <abs path or "(untitled)">
//     Lines: <总行数>
//     Cursor: L<line>,C<col>          （1 基）
//     Selection: L<a>C<b>-L<c>C<d>    （有选区时）
//     <selection text, ≤8000 字符截断>
//   detail（transcript 回显）：文件名 · 选中 N 行 / 光标 L,C
AiContext MainWindow::BuildAiContext() {
    AiContext c;
    if (!workspace_) return c;
    Document* d = workspace_->Active();
    if (!d) return c;

    const long long selStart = d->editor.Send(SCI_GETSELECTIONSTART);
    const long long selEnd = d->editor.Send(SCI_GETSELECTIONEND);
    const bool hasSel = selEnd > selStart;
    const long long curPos = d->editor.Send(SCI_GETCURRENTPOS);
    const int curLine = (int)d->editor.Send(SCI_LINEFROMPOSITION, curPos);
    const int curCol = (int)(curPos - d->editor.Send(SCI_POSITIONFROMLINE, curLine));

    std::wstring path;
    std::wstring note;
    if (d->HasPath()) {
        path = d->path.wstring();
    } else {
        // 无路径文档：镜像文件路径（AI 的 write/edit 工具的真实操作目标）。
        // 镜像由 SnapDiskState 在发送前刷新；此处仅在已生成时引用。
        auto mit = aiMirrorByDoc_.find(d->DisplayName());
        if (mit != aiMirrorByDoc_.end()) {
            path = mit->second;
            note =
                L"(Note: this document has no file on disk yet. The path "
                L"below is a temporary mirror the AI can write to; "
                L"changes to it appear back in the editor automatically. "
                L"Use it instead of asking the user to save.)\n";
        } else {
            path = L"(untitled, no mirror)";
        }
    }
    const std::wstring name = d->DisplayName();
    std::wstring blk = L"[Editor context]\n" + note + L"File: " + path;

    // 选区文本（UTF-8 → 宽字符；8000 字符上限防 prompt 爆量）
    // 注意 SCI_GETSELTEXT 返回值不含结尾 NUL，但写入仍是 长度+1 字节——
    // 缓冲区必须多留 1 字节，否则 1 字节堆越界 → 延迟堆损坏
    // （2026-09-04 真机 AV@memcpy 现挂现修；2026-09-15 又因"返回含 NUL"
    // 的误判先 -1 导致选区末字符被截，一并修正）。
    std::wstring sel;
    int selLines = 0;
    if (hasSel) {
        long long len = d->editor.Send(SCI_GETSELTEXT, 0, 0);
        if (len > 0) {
            if (len > 8000) len = 8000;
            std::string raw((size_t)len + 1, '\0');
            d->editor.Send(SCI_GETSELTEXT, 0, (LPARAM)raw.data());
            raw.resize((size_t)len);
            sel = Utf8ToWide(raw);
            const int selStartLine =
                (int)d->editor.Send(SCI_LINEFROMPOSITION, selStart);
            const int selEndLine =
                (int)d->editor.Send(SCI_LINEFROMPOSITION, selEnd);
            selLines = selEndLine - selStartLine + 1;
        }
    }

    // 行数直接给模型（SCI_GETLINECOUNT O(1)）——否则模型会自己调 read 工具
    // 去数行数，551MB 大文件会把 tool 卡死 90+ 分钟（2026-09-04 真机复现）。
    blk += L"\nLines: " + std::to_wstring(
        (long long)d->editor.Send(SCI_GETLINECOUNT));
    blk += L"\nCursor: L" + std::to_wstring(curLine + 1) +
           L",C" + std::to_wstring(curCol);
    if (hasSel && !sel.empty()) {
        blk += L"\nSelection: L" + std::to_wstring(
            (int)d->editor.Send(SCI_LINEFROMPOSITION, selStart) + 1) +
            L"C" + std::to_wstring((int)(selStart -
            d->editor.Send(SCI_POSITIONFROMLINE,
                d->editor.Send(SCI_LINEFROMPOSITION, selStart)))) +
            L"-L" + std::to_wstring(
            (int)d->editor.Send(SCI_LINEFROMPOSITION, selEnd) + 1) +
            L"C" + std::to_wstring((int)(selEnd -
            d->editor.Send(SCI_POSITIONFROMLINE,
                d->editor.Send(SCI_LINEFROMPOSITION, selEnd))));
        blk += L"\n--- selected text ---\n" + sel;
        if (sel.size() >= 8000) blk += L"\n... (truncated)";
        blk += L"\n--- end selection ---";
    }

    // 全部已打开标签页清单（两个视图去重）：模型由此得知其它文件的
    // 绝对路径，可直接用 read/write 工具按路径操作；未保存文档标注
    // 无路径（其镜像仅在成为活跃文档发送时生成）。
    if (workspace_) {
        Document* active = workspace_->Active();
        std::wstring tabs = L"\nOpen tabs:";
        auto listTabs = [&](Document* t) {
            if (!t) return;
            tabs += L"\n";
            tabs += (t == active) ? L"* " : L"  ";
            if (t->HasPath())
                tabs += t->path.wstring();
            else
                tabs += t->DisplayName() + L" (unsaved - no disk path)";
            if (t == active) tabs += L" [current]";
        };
        for (int i = 0; i < workspace_->Count(); ++i)
            listTabs(workspace_->DocumentAt(i));
        for (int i = 0; i < workspace_->Count1(); ++i)
            listTabs(workspace_->FindByTabIndex1(i));
        if (workspace_->Count() + workspace_->Count1() > 0)
            blk += tabs;
    }

    c.block = std::move(blk);
    c.detail = name + L" · " +
        (hasSel && selLines > 0
            ? I18n::Instance().Fmt(L"panel.ai.ctxsel", {std::to_wstring(selLines)})
            : I18n::Instance().Fmt(L"panel.ai.ctxcursor",
                {std::to_wstring(curLine + 1), std::to_wstring(curCol)}));
    return c;
}

// 未保存文档镜像：全文写到 %TEMP%\xfsWinPad\<name>.txt，给 AI 一个可被
// write/edit 工具操作的真实路径（缓冲区本身无路径）。5MB 上限防意外大
// 缓冲区拖垮发送。失败返回 false（上下文里标注不可镜像）。
bool MainWindow::RefreshUntitledMirror(Document* d) {
    if (!d || d->HasPath()) return false;
    const unsigned long long kMirrorMax = 5ull * 1024 * 1024;
    const long long len = d->editor.Send(SCI_GETLENGTH);
    if (len < 0 || (unsigned long long)len > kMirrorMax) return false;

    wchar_t tempRoot[MAX_PATH];
    if (::GetTempPathW(MAX_PATH, tempRoot) == 0) return false;
    std::wstring dir = std::wstring(tempRoot) + L"xfsWinPad";
    ::CreateDirectoryW(dir.c_str(), nullptr);   // 已存在则忽略

    std::wstring mirror = dir + L"\\" + d->DisplayName() + L".txt";
    std::string utf8 = d->editor.GetTextUtf8();
    if (!WriteFileBytes(mirror, utf8.data(), utf8.size())) return false;
    aiMirrorByDoc_[d->DisplayName()] = mirror;
    Logger::Info("Untitled mirror: " + WideToUtf8(d->DisplayName()) + " -> " +
                 WideToUtf8(mirror) + " (" + std::to_string(utf8.size()) + "B)");
    return true;
}

// AI 发送前：快照所有已打开文档的磁盘 mtime/size（view 0 + view 1）。
// 无路径文档同步刷新镜像文件并快照其 mtime/size（key=镜像路径）。
void MainWindow::SnapDiskState() {
    aiDiskSnap_.clear();
    aiMirrorByDoc_.clear();
    if (!workspace_) return;
    auto snap = [this](Document* d) {
        if (!d) return;
        std::wstring key;
        if (d->HasPath()) {
            key = d->path.wstring();
        } else {
            // 无路径：镜像写入即快照（mtime 刚生成，直接取一次）
            if (!RefreshUntitledMirror(d)) return;
            key = aiMirrorByDoc_[d->DisplayName()];
        }
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (::GetFileAttributesExW(key.c_str(),
                                   GetFileExInfoStandard, &fa)) {
            FILETIME wt = fa.ftLastWriteTime;
            unsigned long long mtime =
                ((unsigned long long)wt.dwHighDateTime << 32) |
                wt.dwLowDateTime;
            aiDiskSnap_[key] =
                std::make_pair(mtime, (unsigned long long)fa.nFileSizeLow |
                    ((unsigned long long)fa.nFileSizeHigh << 32));
        }
    };
    for (int i = 0; i < workspace_->Count(); ++i)
        snap(workspace_->DocumentAt(i));
    for (int i = 0; i < workspace_->Count1(); ++i) {
        Document* d = workspace_->FindByTabIndex1(i);
        snap(d);
    }
}

// AI 响应完成后：对比快照 → 有文档在磁盘上变了且编辑器无未保存修改 →
// 静默重载该文档（AI 改盘内容即时上屏）。
// 注意：模型"回复完成"与其 write 工具真正落盘之间有 3~5 秒收尾延迟
//（实测 00:36:47 finish → 00:36:50 mtime、00:41:51 → 00:41:55），
// 立即对比会误判 unchanged。因此轮询对比：2s 一次，最多 8 次（16s）。
void MainWindow::AutoReloadChanged() {
    aiReloadTries_ = 0;
    ::SetTimer(hwnd_, kAiReloadTimer, 2000, nullptr);
}

void MainWindow::AutoReloadChangedNow() {
    if (!workspace_ || aiDiskSnap_.empty()) return;
    bool anyChanged = false;
    auto check = [&](Document* d, int view, int index) {
        if (!d) return;
        // 无路径文档：检查镜像文件变化 → 读回填充缓冲区（保持 untitled）
        if (!d->HasPath()) {
            auto mit = aiMirrorByDoc_.find(d->DisplayName());
            if (mit == aiMirrorByDoc_.end()) return;
            auto it = aiDiskSnap_.find(mit->second);
            if (it == aiDiskSnap_.end()) return;
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!::GetFileAttributesExW(mit->second.c_str(),
                                        GetFileExInfoStandard, &fa))
                return;
            FILETIME wt = fa.ftLastWriteTime;
            unsigned long long mtime =
                ((unsigned long long)wt.dwHighDateTime << 32) | wt.dwLowDateTime;
            unsigned long long size =
                (unsigned long long)fa.nFileSizeLow |
                ((unsigned long long)fa.nFileSizeHigh << 32);
            if (mtime == it->second.first && size == it->second.second)
                return;
            anyChanged = true;
            std::string raw;
            if (!ReadFileBytes(mit->second, raw)) {
                Logger::Warn("AutoReloadChanged: mirror read failed: " +
                             WideToUtf8(mit->second));
                return;
            }
            DecodedText dec = encoding::DecodeToUtf8(raw);
            // AI 刚写的内容就是权威状态——即便用户随后手改缓冲区，镜像快照
            // 对比的是文件而非缓冲区，dirty 不阻挡（与磁盘文档语义一致：
            // AI 写镜像=用户按了保存到镜像）。
            d->editor.SetEol(DetectEol(dec.utf8));
            d->editor.SetTextUtf8(dec.utf8);
            d->editor.SetSavePoint();   // 无路径文档无“保存”概念，避免全红盘
            Logger::Info("AutoReloadChanged: untitled backfilled: " +
                         WideToUtf8(d->DisplayName()) + " <- " +
                         WideToUtf8(mit->second) + " (" +
                         std::to_string(dec.utf8.size()) + "B)");
            if (ai_)
                ai_->SystemLine(I18n::Instance().Fmt(
                    L"panel.ai.autoreloadmirror", {d->DisplayName()}));
            return;
        }
        auto it = aiDiskSnap_.find(d->path.wstring());
        if (it == aiDiskSnap_.end()) return;   // 未跟踪（快照后才打开的）
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!::GetFileAttributesExW(d->path.c_str(),
                                    GetFileExInfoStandard, &fa))
            return;
        FILETIME wt = fa.ftLastWriteTime;
        unsigned long long mtime =
            ((unsigned long long)wt.dwHighDateTime << 32) | wt.dwLowDateTime;
        unsigned long long size =
            (unsigned long long)fa.nFileSizeLow |
            ((unsigned long long)fa.nFileSizeHigh << 32);
        if (mtime == it->second.first && size == it->second.second)
            return;   // 磁盘未变（本轮）
        anyChanged = true;
        Logger::Info("AutoReloadChanged: disk changed for " +
                     WideToUtf8(d->path.wstring()) + " mtime " +
                     std::to_string(it->second.first) + "->" +
                     std::to_string(mtime) + " size " +
                     std::to_string(it->second.second) + "->" +
                     std::to_string(size));
        if (d->editor.Modified()) {
            Logger::Info("AutoReloadChanged: AI changed disk but doc dirty, "
                         "skip reload: " + WideToUtf8(d->path.wstring()));
            if (ai_)
                ai_->SystemLine(I18n::Instance().Fmt(L"panel.ai.autoreloadskip",
                    {d->DisplayName()}));
            return;
        }
        // 两个视图统一走 ReloadDocumentDirect（不激活、不切 tab、不抢焦点），
        // 修掉旧路径 view 1 只记日志的缺口（ReloadDocument 会 Activate 抢焦点）
        if (workspace_->ReloadDocumentDirect(d)) {
            Logger::Info("AutoReloadChanged: reloaded after AI change: " +
                         WideToUtf8(d->path.wstring()));
            if (ai_)
                ai_->SystemLine(I18n::Instance().Fmt(L"panel.ai.autoreload",
                    {d->DisplayName()}));
        }
    };
    for (int i = 0; i < workspace_->Count(); ++i)
        check(workspace_->DocumentAt(i), 0, i);
    for (int i = 0; i < workspace_->Count1(); ++i)
        check(workspace_->FindByTabIndex1(i), 1, i);
    // 收尾延迟轮询：本轮没检测到变化且还没到 16s → 2s 后再查
    if (!anyChanged && ++aiReloadTries_ < 8)
        ::SetTimer(hwnd_, kAiReloadTimer, 2000, nullptr);
    else if (!anyChanged)
        Logger::Info("AutoReloadChanged: no disk change within 16s, done");
}

void MainWindow::RebuildPluginMenu() {
    if (!pluginMenu_ || !plugins_) return;
    // 弹窗子菜单先销毁再重建，避免每次刷新泄漏 HMENU
    for (HMENU pop : pluginPopups_) {
        if (pop) ::DestroyMenu(pop);
    }
    pluginPopups_.clear();
    while (::GetMenuItemCount(pluginMenu_) > 0)
        ::RemoveMenu(pluginMenu_, 0, MF_BYPOSITION);
    // 固定入口：插件管理（始终可见，即便没有任何插件命令）
    ::AppendMenuW(pluginMenu_, MF_STRING, Cmd::PluginAdmin, Tr(L"menu.plugin.admin"));
    ::AppendMenuW(pluginMenu_, MF_SEPARATOR, 0, nullptr);
    const auto& cmds = plugins_->Commands();
    if (cmds.empty()) {
        ::AppendMenuW(pluginMenu_, MF_STRING | MF_GRAYED, 0, Tr(L"menu.plugin.empty"));
        return;
    }

    // NPP 形态命令按「category=插件名」聚成各自子菜单（N++ 视觉惯例）；
    // 原生命令保持顶层平铺。分组顺序 = 命令表首次出现顺序。
    int flatIdx = 0;
    const int total = (int)cmds.size();
    std::vector<bool> grouped(total, false);
    for (int i = 0; i < total; ++i) {
        if (grouped[i] || !cmds[i].grouped) continue;
        HMENU pop = ::CreatePopupMenu();
        if (!pop) continue;
        pluginPopups_.push_back(pop);
        for (int j = i; j < total; ++j) {
            if (!cmds[j].grouped || cmds[j].category != cmds[i].category)
                continue;
            ::AppendMenuW(pop, MF_STRING, cmds[j].id, cmds[j].label.c_str());
            grouped[j] = true;
        }
        ::AppendMenuW(pluginMenu_, MF_POPUP, (UINT_PTR)pop,
                      cmds[i].category.c_str());
    }
    for (; flatIdx < total; ++flatIdx) {
        if (grouped[flatIdx]) continue;
        ::AppendMenuW(pluginMenu_, MF_STRING, cmds[flatIdx].id,
                      cmds[flatIdx].label.c_str());
    }
}

void MainWindow::RunPluginAdmin() {
    PluginAdminDialog::Run(hwnd_, inst_, plugins_.get());
}

void MainWindow::RunPreferences() {
    // 模态；对话框内改动经 ApplyAll 实时生效，「确定」后此处落盘。
    PreferencesDialog::Run(hwnd_, inst_, &settings_, this);
    SettingsSave(SettingsFilePath(), settings_);
    Logger::Info("Preferences closed (saved)");
}

// IStyleApplier：全部编辑器重刷样式（ReapplyLexer 会带上 stylers.json 覆盖层）
void MainWindow::RestyleAll() {
    if (theme_ && workspace_) workspace_->ApplyThemeToAll(*theme_);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

bool MainWindow::SwitchThemeByName(const wchar_t* name) {
    const ThemeDef* t = theme::Find(name);
    if (!t) return false;
    SwitchTheme(t);
    return true;
}

const wchar_t* MainWindow::CurrentThemeName() {
    return theme_ ? theme_->name : L"light";
}

void MainWindow::RunStyleConfigurator() {
    StyleConfiguratorDialog::Run(hwnd_, inst_, this);
    Logger::Info("Style Configurator closed");
}

// IShortcutChange：快捷键变更后重建加速器表并落盘 shortcuts.json
void MainWindow::OnShortcutChanged() {
    BuildAccelerators();
    GlobalShortcuts().Save(ShortcutTable::FilePath());
}

void MainWindow::RunWindowList() {
    const bool changed = WindowsListDialog::Run(hwnd_, inst_, *this);
    if (changed) OnWorkspaceChanged();
}

// 设置 > 导出主题 JSON…：把当前主题（内建或用户）写为用户选择的文件
void MainWindow::RunThemeExport() {
    const ThemeDef* t = theme::Find(settings_.theme.c_str());
    if (!t) t = theme::Find(L"light");
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH] = L"";
    _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%s.json", t->name);
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Theme JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    if (theme::SaveThemeJsonTo(t, ofn.lpstrFile)) {
        Logger::Info("Theme exported: " + WideToUtf8(ofn.lpstrFile));
        MessageBoxW(hwnd_, I18n::Instance().Fmt(L"theme.exported",
                     {ofn.lpstrFile}).c_str(), Tr(L"theme.title"),
                    MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd_, Tr(L"theme.exportfail"), Tr(L"theme.title"),
                    MB_OK | MB_ICONERROR);
    }
}

// 设置 > 导入主题 JSON…：校验后复制进 themes\ 注册目录并重载用户主题表
void MainWindow::RunThemeImport() {
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Theme JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    RunThemeImportFrom(ofn.lpstrFile);
}

// 从任意路径导入主题（文件对话框 + CLI 双击直达共用）
void MainWindow::RunThemeImportFrom(const std::wstring& path) {
    // 校验是可解析的 JSON（具体字段缺失由加载器的 light 兜底负责）
    std::string raw;
    if (!ReadFileBytes(path, raw)) {
        MessageBoxW(hwnd_, Tr(L"theme.importfail"), Tr(L"theme.title"),
                    MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring j = Utf8ToWide(raw);
    if (!j.empty() && j[0] == 0xFEFF) j.erase(0, 1);
    json::Value root;
    bool ok = json::Parse(j, &root);
    if (!ok) {
        MessageBoxW(hwnd_, Tr(L"theme.importfail"), Tr(L"theme.title"),
                    MB_OK | MB_ICONERROR);
        return;
    }

    const std::wstring dst = theme::ThemesDir() + L"\\" +
        std::filesystem::path(path).filename().wstring();
    std::error_code ec;
    std::filesystem::create_directories(theme::ThemesDir(), ec);
    std::filesystem::copy_file(path, dst,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        MessageBoxW(hwnd_, Tr(L"theme.importfail"), Tr(L"theme.title"),
                    MB_OK | MB_ICONERROR);
        return;
    }
    theme::LoadUserThemes();
    Logger::Info("Theme imported: " + WideToUtf8(dst));
    MessageBoxW(hwnd_, I18n::Instance().Fmt(L"theme.imported",
                 {std::filesystem::path(path).stem().wstring()}).c_str(),
                Tr(L"theme.title"), MB_OK | MB_ICONINFORMATION);
}

// 设置 > 导出配置 profile…：把 settings/shortcuts/stylers/session/themes
// 打包成一个 .xfprofile（换机迁移/备份）
void MainWindow::RunConfigExport() {
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH] = L"xfsWinPad-profile.xfprofile";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter =
        L"xfsWinPad Profile (*.xfprofile)\0*.xfprofile\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"xfprofile";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    std::wstring err;
    if (ProfileExport(ofn.lpstrFile, &err)) {
        Logger::Info("Config profile exported: " + WideToUtf8(ofn.lpstrFile));
        MessageBoxW(hwnd_, I18n::Instance().Fmt(L"profile.exported",
                     {ofn.lpstrFile}).c_str(), Tr(L"profile.title"),
                    MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd_, (std::wstring(Tr(L"profile.exportfail")) + err).c_str(),
                    Tr(L"profile.title"), MB_OK | MB_ICONERROR);
    }
}

// 设置 > 导入配置 profile…：备份现有配置后写回（重启编辑器生效）
void MainWindow::RunConfigImport() {
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter =
        L"xfsWinPad Profile (*.xfprofile)\0*.xfprofile\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::wstring err;
    if (!ProfileImport(ofn.lpstrFile, &err)) {
        MessageBoxW(hwnd_, (std::wstring(Tr(L"profile.importfail")) + err).c_str(),
                    Tr(L"profile.title"), MB_OK | MB_ICONERROR);
        return;
    }
    Logger::Info("Config profile imported: " + WideToUtf8(ofn.lpstrFile));
    if (MessageBoxW(hwnd_, std::wstring(Tr(L"profile.imported.restart")).c_str(),
                    Tr(L"profile.title"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
        // 重启：退出（会话已随 profile 覆盖，重启后按导入的会话恢复）
        PostQuitMessage(0);
    }
}

void MainWindow::RunShortcutMapper() {
    std::vector<ShortcutEntry> entries;
    auto addName = [&](unsigned id) -> std::wstring {
        const wchar_t* label = CmdLabel(id);
        if (label && label[0]) return label;   // CmdLabel returns "" for unmapped ids
        if (id == 6001) return Tr(L"cmd.nexttab");
        if (id == 6002) return Tr(L"cmd.prevtab");
        if (id == Cmd::MacroStart || id == Cmd::MacroStop) return Tr(L"cmd.macrorec");
        if (id == Cmd::MacroPlayback) return Tr(L"cmd.macroplay");
        if (id == Cmd::PaletteShow) return Tr(L"cmd.palette");
        if (id == Cmd::EditTimeDate) return Tr(L"cmd.datetime");
        if (id == Cmd::ViewZoomReset) return Tr(L"cmd.zoomreset");
        if (id == Cmd::ViewHexView) return Tr(L"cmd.hexview");
        if (id == Cmd::ViewStdfView) return Tr(L"cmd.stdfview");
        if (id == Cmd::ViewCsvView) return Tr(L"cmd.csvview");
        if (id == Cmd::ViewBigFile) return Tr(L"bigfile.menu");
        if (id == Cmd::ViewLogPanel) return Tr(L"cmd.logpanel");
        if (id == Cmd::ViewTerminal) return Tr(L"cmd.terminal");
    if (id == Cmd::ViewAiPanel) return Tr(L"menu.ai.toggle");
    if (id == Cmd::AiToggleContext) return Tr(L"menu.ai.attach");
        if (id == Cmd::SearchFindNext) return Tr(L"cmd.findnext");
        if (id == Cmd::SearchFindPrev) return Tr(L"cmd.findprev");
        if (id == Cmd::BookmarkToggle) return Tr(L"cmd.bookmarktoggle");
        if (id == Cmd::BookmarkNext) return Tr(L"cmd.bookmarknext");
        if (id == Cmd::BookmarkPrev) return Tr(L"cmd.bookmarkprev");
        if (id == Cmd::FoldAll) return Tr(L"cmd.foldall");
        if (id == Cmd::UnfoldAll) return Tr(L"cmd.unfoldall");
        if (id == Cmd::ViewExplorer) return Tr(L"cmd.explorer");
        if (id == Cmd::FilePrint) return Tr(L"cmd.print");
        if (id == Cmd::FileCloseAll) return Tr(L"cmd.closeall");
        if (id == Cmd::FileReload) return Tr(L"cmd.reload");
        if (id == Cmd::ReopenClosedTab) return Tr(L"menu.file.reopenclosed");
        return I18n::Instance().Fmt(L"cmd.unknown", {std::to_wstring(id)});
    };
    for (const auto& kv : GlobalShortcuts().Everything()) {
        ShortcutEntry e;
        e.id = kv.first;
        e.name = addName(kv.first);
        if (kv.first == 6001 || kv.first == 6002) e.group = 1;   // 内部命令
        else e.group = 0;
        entries.push_back(std::move(e));
    }
    if (plugins_) {
        for (const auto& c : plugins_->Commands()) {
            ShortcutEntry e;
            e.id = c.id;
            e.name = c.category + L" :: " + c.label;
            e.group = 2;
            entries.push_back(std::move(e));
        }
    }
    ShortcutMapperDialog::Run(hwnd_, inst_, entries, this);
    Logger::Info("Shortcut Mapper closed (saved)");
}

// IPrefsApplier：把设置灌进全部编辑器 + 主题 + 自动保存定时器（实时预览）。
void MainWindow::ApplyAll(const AppSettings& s) {
    const bool themeChanged = (s.theme != settings_.theme);
    const bool langChanged = (s.uiLang != settings_.uiLang);
    const bool visChanged = (s.showStatusBar != settings_.showStatusBar ||
                             s.showTabBar != settings_.showTabBar ||
                             s.showToolbar != settings_.showToolbar);
    settings_ = s;
    if (langChanged) I18n::Instance().Load(settings_.uiLang);   // 触发 ApplyLanguage 回调
    if (themeChanged) {
        if (const ThemeDef* t = theme::Find(s.theme.c_str()))
            SwitchTheme(t);   // 内部会再落盘一次（同数据，无害）
    }
    if (workspace_) workspace_->ApplyPrefsToAll(settings_);
    if (ai_) {
        ai_->SetAttachContext(settings_.aiAttachContext);
        ai_->SetAutoApprove(settings_.aiAutoApprove);
        ai_->SetBackend(settings_.aiBackend.empty() ? L"opencode"
                                                    : settings_.aiBackend,
                        settings_.aiEndpoint, settings_.aiApiKey,
                        settings_.aiLocalModel);
    }
    CheckMenuItem(menu_, Cmd::AiToggleContext, MF_BYCOMMAND |
        (settings_.aiAttachContext ? MF_CHECKED : MF_UNCHECKED));
    ::KillTimer(hwnd_, 1);
    if (settings_.autosaveEnabled)
        ::SetTimer(hwnd_, 1, (UINT)(std::max(5, std::min(600,
                          settings_.autosaveSeconds))) * 1000, nullptr);
    if (visChanged) Relayout();   // 状态栏/标签栏/工具栏显隐改变需要重排
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// 批次 67：多实例会话分派。
//   1) CLI 文件优先，完全绕过会话；
//   2) --restore <slot>：父进程扇出的子窗口，恢复指定槽位后认领（删除旧槽，
//      退出时以自身 pid 写新槽）；
//   3) --no-restore / 非 primary 无参：空白窗口；
//   4) primary 无参：恢复 session.json，并把其余存活槽位逐个 spawn 成独立
//      窗口（孤儿槽由 SessionSlots 的 30 天 GC 兜底回收）。
void MainWindow::StartupSession() {
    if (!startup_.files.empty()) { OpenCliFiles(startup_); return; }
    if (!startup_.restoreFile.empty()) {
        SessionState ss;
        if (SessionLoad(startup_.restoreFile, &ss)) RestoreSession(ss);
        std::error_code ec;
        std::filesystem::remove(startup_.restoreFile, ec);   // claim the slot
        if (workspace_->Count() == 0 && workspace_->Count1() == 0)
            workspace_->NewDocument();
        return;
    }
    if (startup_.noRestore || !startup_.firstInstance) {
        workspace_->NewDocument();
        return;
    }
    SessionState ss;
    if (SessionLoad(SessionFilePath(), &ss)) RestoreSession(ss);
    for (const auto& slot : SessionSlots(SessionDir(),
                                         (unsigned long)::GetCurrentProcessId()))
        SpawnRestoreWindow(slot);
    // 会话缺失/损坏/全部失效 → 兜底空白文档（仅在两视图都为空时）
    if (workspace_->Count() == 0 && workspace_->Count1() == 0)
        workspace_->NewDocument();
}

void MainWindow::RestoreSession(const SessionState& ss) {
    int restored = 0;
    // 恢复语言菜单手动选择（批次 66 后补：langIndex 入 session.json）。
    // lang 越界（旧目录版本/手改文件）当作未选，走扩展名探测。
    auto applyLang = [&](Document* doc, int lang) {
        if (!doc || lang < 0) return;
        const LanguageMenuItem* cat = LanguageMenuCatalog();
        int n = 0;
        for (; cat[n].label; ++n) {}
        if (lang >= n) return;
        doc->langIndex = lang;
        const char* kw[2] = { cat[lang].keywords[0], cat[lang].keywords[1] };
        doc->editor.SetLexerByName(cat[lang].lexerName, kw, theme_);
    };
    for (auto& e : ss.entries) {
        if (!e.path.empty() && std::filesystem::exists(e.path)) {
            workspace_->OpenPath(e.path);
            if (workspace_->Count() > 0) {
                Document* doc = workspace_->DocumentAt(workspace_->Count()-1);
                if (doc && (e.line > 1 || e.col > 1))
                    doc->editor.GotoPosition(e.line, e.col);
                if (doc && e.locked) {   // 批次 38：锁定状态入 session
                    doc->locked = true;
                    doc->editor.SetReadOnly(true);
                }
                applyLang(doc, e.lang);
                ++restored;
            }
        } else if (!e.text.empty()) {
            // untitled snapshot: recreate the scratch tab with its text
            workspace_->NewDocument();
            Document* doc = workspace_->Active();
            if (doc) {
                if (!e.name.empty()) doc->SetUntitledName(e.name);
                doc->editor.SetTextUtf8(WideToUtf8(e.text));
                if (e.line > 1 || e.col > 1) doc->editor.GotoPosition(e.line, e.col);
                if (e.locked) {
                    doc->locked = true;
                    doc->editor.SetReadOnly(true);
                }
                applyLang(doc, e.lang);
                ++restored;
            }
        }
    }

    // right/other split view: open each file then move it across so a
    // split session survives the restart.
    int restored1 = 0;
    for (auto& e : ss.entries1) {
        if (!e.path.empty() && std::filesystem::exists(e.path)) {
            workspace_->OpenPath(e.path);              // lands in left view
            workspace_->MoveActiveToOtherView();       // now in right view
            if ((e.line > 1 || e.col > 1) && workspace_->Count1() > 0) {
                Document* d = workspace_->Active1();
                if (d) d->editor.GotoPosition(e.line, e.col);
            }
            if (e.locked && workspace_->Count1() > 0) {
                Document* d = workspace_->Active1();
                if (d) { d->locked = true; d->editor.SetReadOnly(true); }
            }
            if (e.lang >= 0 && workspace_->Count1() > 0)
                applyLang(workspace_->Active1(), e.lang);
            ++restored1;
        } else if (!e.text.empty()) {
            // untitled snapshot in the right view
            workspace_->NewDocument();
            Document* doc = workspace_->Active();
            if (doc) {
                if (!e.name.empty()) doc->SetUntitledName(e.name);
                doc->editor.SetTextUtf8(WideToUtf8(e.text));
                if (e.line > 1 || e.col > 1) doc->editor.GotoPosition(e.line, e.col);
                if (e.locked) {
                    doc->locked = true;
                    doc->editor.SetReadOnly(true);
                }
                applyLang(doc, e.lang);
                workspace_->MoveActiveToOtherView();
                if ((e.line > 1 || e.col > 1) && workspace_->Count1() > 0) {
                    Document* d = workspace_->Active1();
                    if (d) d->editor.GotoPosition(e.line, e.col);
                }
                ++restored1;
            }
        }
    }

    // 两视图的选中 tab 各自独立恢复；退出时处于激活态的视图最后处理，
    // 以决定最终 currentView_ 与焦点（否则非激活侧的 activeIndex/activeIndex1 会被丢弃）。
    const int cnt1r = workspace_->Count1();
    const bool leftActive = (ss.activeView == 0);
    auto restoreLeft = [&]() {
        if (ss.activeIndex >= 0 && ss.activeIndex < workspace_->Count())
            workspace_->Activate(ss.activeIndex);
    };
    auto restoreRight = [&]() {
        if (cnt1r > 0) {
            int idx = ss.activeIndex1;
            if (idx < 0) idx = 0;
            if (idx >= cnt1r) idx = cnt1r - 1;
            workspace_->ActivateView1(idx);
        }
    };
    if (leftActive) { restoreRight(); restoreLeft(); }
    else            { restoreLeft(); restoreRight(); }
    // 跳行延迟到消息循环：此刻编辑器可能尚未挂进可见窗口树，立即 SCI_GOTOLINE
    // 存在丢失风险，统一攒到 WM_APP_RESTOREJUMP 后应用（覆盖左右两视图）。
    pendingJumps_.clear();
    for (const auto& e : ss.entries)
        if (e.line > 1 || e.col > 1) pendingJumps_.push_back({e.path, e.name, e.line, e.col});
    for (const auto& e : ss.entries1)
        if (e.line > 1 || e.col > 1) pendingJumps_.push_back({e.path, e.name, e.line, e.col});
    if (!pendingJumps_.empty())
        ::PostMessageW(hwnd_, WM_APP_RESTOREJUMP, 0, 0);
    Logger::Info("Session restored: " + std::to_string(restored) +
                 " + " + std::to_string(restored1) + " file(s)");
}

void MainWindow::ApplyRestoreJumps() {
    auto jumps = std::move(pendingJumps_);
    pendingJumps_.clear();
    for (const auto& j : jumps) {
        Document* doc = nullptr;
        if (!j.path.empty()) {
            doc = workspace_->FindByPath(j.path);
        } else if (!j.name.empty()) {
            int n = workspace_->Count();
            for (int i = 0; i < n; ++i) {
                Document* d = workspace_->FindByTabIndex(i);
                if (d && !d->HasPath() && d->DisplayName() == j.name) { doc = d; break; }
            }
            int n1 = workspace_->Count1();
            for (int i = 0; i < n1 && !doc; ++i) {
                Document* d = workspace_->FindByTabIndex1(i);
                if (d && !d->HasPath() && d->DisplayName() == j.name) { doc = d; break; }
            }
        }
        if (doc) doc->editor.GotoPosition(j.line, j.col);
    }
}

void MainWindow::MoveCurrentToNewWindow() {
    Document* d = workspace_->Active();
    if (!d) return;
    std::wstring path = d->path.wstring();   // empty when the doc has no path
    if (!path.empty()) {
        // Close()/Close1() prompt-save dirty buffers; on cancel we abort so the
        // file on disk (read by the new window) always matches what the user saw.
        bool ok = (d->view == 1)
            ? workspace_->Close1(workspace_->ActiveIndex1())
            : workspace_->Close(workspace_->ActiveIndex());
        if (!ok) return;
    }
    SpawnWindow(path);   // a pathless doc opens a fresh blank window, tab kept
}

void MainWindow::SpawnWindow(const std::wstring& file) {
    std::wstring args = L"--new";
    if (!file.empty())
        args += L" \"" + file + L"\"";
    SpawnWithArgs(args);
}

// 批次 67：primary 扇出——把幸存的窗口槽位交给独立子进程恢复
void MainWindow::SpawnRestoreWindow(const std::wstring& slotFile) {
    SpawnWithArgs(L"--new --no-restore --restore \"" + slotFile + L"\"");
}

// 批次 67：File > New Window (Ctrl+Shift+N)——独立空窗口，不重复恢复会话
void MainWindow::NewWindowProcess() {
    SpawnWithArgs(L"--new --no-restore");
}

void MainWindow::SpawnWithArgs(const std::wstring& args) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(GetModuleHandleW(nullptr), exe, MAX_PATH);
    // --new：绕过单实例转发，真的开一个新进程窗口
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + args;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    // second xfsWinPad window in a fresh process keeps this one alive
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Logger::Error("SpawnWithArgs CreateProcessW failed gle=" +
                      std::to_string(::GetLastError()));
    }
}

// CLI/双击直达分派：.xfm → 宏加载；主题 JSON（含 ThemeDef 特征键）→ 主题导入；
// 其余返回 false 走普通文档打开。Explore 双击 .xfm / 主题 json 即直达对应管理器。
bool MainWindow::OpenSpecialFile(const std::wstring& f) {
    std::filesystem::path p(f);
    const std::wstring ext = [&] {
        std::wstring e = p.extension().wstring();
        for (auto& c : e) c = towlower(c);
        return e;
    }();
    if (ext == L".xfm") {
        DoMacroLoadFrom(f);
        return true;
    }
    if (ext == L".json") {
        // 主题 json 特征：root 含 "editorBg" 键（ThemeDef 首字段；普通 json 不会撞）
        std::string raw;
        if (!ReadFileBytes(f, raw)) return false;
        std::wstring j = Utf8ToWide(raw);
        if (!j.empty() && j[0] == 0xFEFF) j.erase(0, 1);
        json::Value root;
        if (!json::Parse(j, &root)) return false;
        if (json::Find(root, L"editorBg")) {
            RunThemeImportFrom(f);
            return true;
        }
        return false;
    }
    return false;
}

// 资源管理器面板双击 / 最近文件菜单 / 拖放进窗 的统一打开入口（与 CLI
// OpenCliFiles 同语义）：目录=作为项目文件夹打开；.xfm/主题 json=直达对应
// 管理器；其余=普通文档。CLI 路径不走这里（需透传 gotoLine/readOnly）。
void MainWindow::OpenUserFile(const std::wstring& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        settings_.explorerVisible = true;   // opening a folder shows the panel
        SetProjectRoot(path, true);
        AddRecentFolder(path);
        return;
    }
    if (OpenSpecialFile(path)) return;
    workspace_->OpenPath(path);
}

void MainWindow::OpenCliFiles(const StartupOptions& opts) {
    for (auto& f : opts.files) {
        // a directory argument = open it as the project/folder workspace
        std::error_code ec;
        if (std::filesystem::is_directory(f, ec)) {
            settings_.explorerVisible = true;   // opening a folder shows the panel
            SetProjectRoot(f, true);
            AddRecentFolder(f);
            continue;
        }
        if (OpenSpecialFile(f)) continue;   // .xfm / theme json 直达
        workspace_->OpenPath(f, opts.gotoLine, opts.readOnly);
    }
    if (workspace_->Count() == 0)
        workspace_->NewDocument();
    // --diff: compare the first two opened files directly (no menu)
    if (opts.autoDiff && workspace_->Count() >= 2) {
        RunDiffCompare(workspace_->DocumentAt(0), workspace_->DocumentAt(1));
    }
}

void MainWindow::ToggleHexView() {
    Document* d = workspace_->Active();
    if (!d) return;

    // toggle off when the panel already shows this document
    if (hex_ && hex_->Visible() && hex_->FilePath() == d->path.wstring()) {
        hex_->Hide();
        CheckMenuItem(menu_, Cmd::ViewHexView, MF_UNCHECKED);
        LayoutChildren();
        return;
    }
    if (!d->HasPath()) {
        MessageBoxW(hwnd_, Tr(L"msg.hexneedsave"),
                    L"xfsWinPad Hex", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!hex_) {
        hex_ = std::make_unique<HexPanel>();
        if (!hex_->Create(hwnd_, inst_)) { hex_.reset(); return; }
        hex_->onClose = [this]() {
            hex_->Hide();
            CheckMenuItem(menu_, Cmd::ViewHexView, MF_UNCHECKED);
            LayoutChildren();
        };
        hex_->onHeightChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            hexHLogical_ = std::max(90, std::min(1400,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
        if (theme_) hex_->ApplyTheme(*theme_);
    }
    if (results_) results_->Clear();   // bottom dock slot is shared with search results
    if (hex_->Load(d->path.wstring())) {
        CheckMenuItem(menu_, Cmd::ViewHexView, MF_CHECKED);
        LayoutChildren();
    }
}

void MainWindow::ToggleStdfView() {
    Document* d = workspace_->Active();
    if (!d) return;

    // toggle off when the panel already shows this document
    if (bigfile_ && bigfile_->Visible() && bigfile_->Path() == d->path.wstring()) {
        bigfile_->Hide();
        CheckMenuItem(menu_, Cmd::ViewBigFile, MF_UNCHECKED);
    }    if (stdf_ && stdf_->Visible() && stdf_->FilePath() == d->path.wstring()) {
        stdf_->Hide();
        CheckMenuItem(menu_, Cmd::ViewStdfView, MF_UNCHECKED);
        LayoutChildren();
        return;
    }
    if (!d->HasPath()) {
        MessageBoxW(hwnd_, Tr(L"msg.hexneedsave"),
                    L"xfsWinPad STDF", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!stdf_) {
        stdf_ = std::make_unique<StdfPanel>();
        if (!stdf_->Create(hwnd_, inst_)) { stdf_.reset(); return; }
        stdf_->onClose = [this]() {
            stdf_->Hide();
            CheckMenuItem(menu_, Cmd::ViewStdfView, MF_UNCHECKED);
            LayoutChildren();
        };
        stdf_->onHeightChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            stdfHLogical_ = std::max(90, std::min(1400,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
        // 「AI 分析」（批次 28）：STDF 统计块 → AiPanel SendExternal，
        // 面板未开时先打开（EnsureReady 幂等，连接期间自动排队）。
        stdf_->onAiAnalyze = [this]() { AiAskStdf(); };
        if (theme_) stdf_->ApplyTheme(*theme_);
    }
    if (results_) results_->Clear();   // bottom dock slot is shared with search results
    if (stdf_->Load(d->path.wstring())) {
        CheckMenuItem(menu_, Cmd::ViewStdfView, MF_CHECKED);
        LayoutChildren();
    }
}

// --- CSV 表格视图（批次 32）------------------------------------------------
void MainWindow::ToggleCsvView() {
    Document* d = workspace_->Active();
    if (!d) return;

    // toggle off when the panel already shows this document
    if (csv_ && csv_->Visible() && csv_->FilePath() == d->path.wstring()) {
        csv_->Hide();
        CheckMenuItem(menu_, Cmd::ViewCsvView, MF_UNCHECKED);
        LayoutChildren();
        return;
    }
    if (!d->HasPath()) {
        MessageBoxW(hwnd_, Tr(L"msg.csvneedsave"),
                    L"xfsWinPad CSV", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!csv_) {
        csv_ = std::make_unique<CsvPanel>();
        if (!csv_->Create(hwnd_, inst_)) { csv_.reset(); return; }
        csv_->onClose = [this]() {
            csv_->Hide();
            CheckMenuItem(menu_, Cmd::ViewCsvView, MF_UNCHECKED);
            LayoutChildren();
        };
        csv_->onHeightChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            csvHLogical_ = std::max(120, std::min(1400,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
        csv_->onSaved = [this](const std::wstring& p) {
            // CSV 面板回写文件后同步编辑器文档（保持光标行、不弹确认）
            Document* d = workspace_->FindByPath(p);
            if (d) workspace_->ReloadDocumentDirect(d);
        };
        if (theme_) csv_->ApplyTheme(*theme_);
        csv_->Retranslate();
    }
    if (results_) results_->Clear();   // bottom dock slot is shared with search results
    if (csv_->Load(d->path.wstring())) {
        CheckMenuItem(menu_, Cmd::ViewCsvView, MF_CHECKED);
        LayoutChildren();
    } else {
        MessageBoxW(hwnd_, Tr(L"msg.csvloadfail"),
                    L"xfsWinPad CSV", MB_OK | MB_ICONWARNING);
    }
}

// --- big-file viewer (>2GB read-only, paged mmap) ------------------------------
void MainWindow::ToggleBigFileView(const std::wstring& forcedPath) {
    // toggle off when no explicit path (menu action) and panel visible
    if (forcedPath.empty() && bigfile_ && bigfile_->Visible()) {
        bigfile_->Hide();
        CheckMenuItem(menu_, Cmd::ViewBigFile, MF_UNCHECKED);
        LayoutChildren();
        return;
    }
    std::wstring path = forcedPath;
    if (path.empty()) {
        Document* d = workspace_->Active();
        if (!d || !d->HasPath()) {
            MessageBoxW(hwnd_, Tr(L"msg.hexneedsave"),
                        L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
            return;
        }
        path = d->path.wstring();
    }
    if (!bigfile_) {
        bigfile_ = std::make_unique<BigFileView>();
        if (!bigfile_->Create(hwnd_, inst_)) { bigfile_.reset(); return; }
        bigfile_->onClose = [this]() {
            bigfile_->Hide();
            CheckMenuItem(menu_, Cmd::ViewBigFile, MF_UNCHECKED);
            LayoutChildren();
        };
        bigfile_->onHeightChange = [this](int px) {
            int dpi = ::GetDpiForWindow(hwnd_);
            bigfileHLogical_ = std::max(120, std::min(1400,
                MulDiv(px, 96, std::max(96, dpi))));
            LayoutChildren();
        };
        if (theme_) bigfile_->ApplyTheme(*theme_);
    }
    if (results_) results_->Clear();   // bottom dock slot is shared with search results
    if (bigfile_->LoadFile(path)) {
        CheckMenuItem(menu_, Cmd::ViewBigFile, MF_CHECKED);
        LayoutChildren();
    } else {
        MessageBoxW(hwnd_, Tr(L"bigfile.openfail"),
                    L"xfsWinPad", MB_OK | MB_ICONWARNING);
    }
}

// --- macro record / playback ---------------------------------------------------
void MainWindow::AttachMacroHook(Editor* ed) {

    if (!ed) return;

    ed->SetKeyHook(this, [](void* ctx, UINT msg, WPARAM wp) {
        auto* self = static_cast<MainWindow*>(ctx);
        if (msg == WM_CHAR) self->macro_.RecordChar((wchar_t)wp);
        else self->macro_.RecordKey((unsigned int)wp);
    });

    // 编辑器右键 → 宿主上下文菜单（编辑操作 + AI 快捷组）
    ed->SetContextMenu([this, ed](int sx, int sy) {
        ShowEditorContextMenu(ed, sx, sy);
    });

}

// 编辑器右键菜单：基础编辑 + 「AI」快捷组（v3）。菜单命令直接发斜杠命令。
void MainWindow::ShowEditorContextMenu(Editor* ed, int sx, int sy) {
    if (!ed) return;
    enum : UINT { kUndo = 1, kRedo, kCut, kCopy, kPaste, kDel, kSelAll,
                  kAiExplain = 30, kAiFix, kAiRefactor, kAiTranslate, kAiSummary };
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    const bool hasSel = ed->Send(SCI_GETSELECTIONEND) > ed->Send(SCI_GETSELECTIONSTART);
    ::AppendMenuW(menu, MF_STRING, kUndo, Tr(L"menu.edit.undo"));
    ::AppendMenuW(menu, MF_STRING, kRedo, Tr(L"menu.edit.redo"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), kCut, Tr(L"menu.edit.cut"));
    ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), kCopy, Tr(L"menu.edit.copy"));
    ::AppendMenuW(menu, MF_STRING, kPaste, Tr(L"menu.edit.paste"));
    ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED), kDel, Tr(L"menu.edit.delete"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kSelAll, Tr(L"menu.edit.selectall"));
    if (ai_) {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED),
                      kAiExplain, Tr(L"menu.ai.explain"));
        ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED),
                      kAiFix, Tr(L"menu.ai.fix"));
        ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED),
                      kAiRefactor, Tr(L"menu.ai.refactor"));
        ::AppendMenuW(menu, MF_STRING | (hasSel ? 0 : MF_GRAYED),
                      kAiTranslate, Tr(L"menu.ai.translate"));
        ::AppendMenuW(menu, MF_STRING, kAiSummary, Tr(L"menu.ai.summary"));
    }
    UINT cmd = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                  sx, sy, hwnd_, nullptr);
    ::DestroyMenu(menu);
    if (cmd == 0) return;
    switch (cmd) {
        case kUndo:    ed->Send(SCI_UNDO); break;
        case kRedo:    ed->Send(SCI_REDO); break;
        case kCut:     ed->Send(SCI_CUT); break;
        case kCopy:    ed->Send(SCI_COPY); break;
        case kPaste:   ed->Send(SCI_PASTE); break;
        case kDel:     ed->Send(SCI_CLEAR); break;
        case kSelAll:  ed->Send(SCI_SELECTALL); break;
        case kAiExplain:   AiQuickCommand(L"/explain"); break;
        case kAiFix:       AiQuickCommand(L"/fix"); break;
        case kAiRefactor:  AiQuickCommand(L"/refactor"); break;
        case kAiTranslate: AiQuickCommand(L"/translate"); break;
        case kAiSummary:   AiQuickCommand(L"/summary"); break;
    }
}

// AI 快捷命令：确保面板打开 → 发对应斜杠命令（AiPanel 展开成完整 prompt）。
// 选区要求类命令在无选区时 AiPanel 自动降级为「当前文档」文案。
void MainWindow::AiQuickCommand(const std::wstring& kind) {
    if (!ai_) return;
    if (!ai_->Visible()) ToggleAiPanel();   // EnsureReady 在 Toggle 内
    if (ai_->Busy()) {
        ai_->SystemLine(Tr(L"panel.ai.busydrop"));
        return;
    }
    ai_->SendUserText(kind);
}
void MainWindow::ToggleMacroRecord() {

    if (macro_.Recording()) {

        macro_.Stop();

        UpdateTitleBar();

        return;

    }

    if (!workspace_ || !workspace_->Active()) {

        MessageBoxW(hwnd_, Tr(L"msg.macro.nodoc"), L"xfsWinPad Macro",

                    MB_OK | MB_ICONINFORMATION);

        return;

    }

    macro_.Start();

    UpdateTitleBar();

}


void MainWindow::DoMacroPlayback() {

    if (macro_.Recording()) {

        MessageBoxW(hwnd_, Tr(L"msg.macro.recording"), L"xfsWinPad Macro",

                    MB_OK | MB_ICONWARNING);

        return;

    }

    Document* d = workspace_->Active();

    if (!d || macro_.Empty()) {

        MessageBoxW(hwnd_, Tr(L"msg.macro.empty"),

                    L"xfsWinPad Macro", MB_OK | MB_ICONINFORMATION);

        return;

    }

    if (d->editor.ReadOnly()) return;

    d->editor.Send(SCI_BEGINUNDOACTION);

    int played = macro_.Playback(

        [this](unsigned int id) -> bool {

            // replay only safe editing commands; guard against recursion

            switch (id) {

                case Cmd::EditUndo: case Cmd::EditRedo:

                case Cmd::MacroStart: case Cmd::MacroStop:

                case Cmd::MacroPlayback: case Cmd::MacroSave: case Cmd::MacroLoad:

                case Cmd::FileSave: case Cmd::FileSaveAs: case Cmd::FileExit:

                    return false;

            }

            ExecuteCommand(id);

            return true;

        },

        d->editor.Hwnd());

    d->editor.Send(SCI_ENDUNDOACTION);

    wchar_t msg[128];

    swprintf_s(msg, L"%s",
        I18n::Instance().Fmt(L"msg.macro.played",
        {std::to_wstring(played)}).c_str());

    if (status_) status_->SetPart(5, msg);

    Logger::Info("Macro playback done: " + std::to_string(played));

}


std::wstring MainWindow::MacroDir() const {
    wchar_t* appData = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        return L".";
    std::wstring dir = std::wstring(appData) + L"\\xfsWinPad\\macros";
    ::CoTaskMemFree(appData);
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void MainWindow::DoMacroSave() {
    if (macro_.Empty()) {
        MessageBoxW(hwnd_, Tr(L"msg.macro.nosave"), L"xfsWinPad Macro",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH] = L"macro.xfm";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Macro Files (*.xfm)\0*.xfm\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = MacroDir().c_str();
    ofn.lpstrDefExt = L"xfm";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    std::wstring text = macro_.Serialize();
    HANDLE h = ::CreateFileW(ofn.lpstrFile, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    static const BYTE bom[] = {0xEF, 0xBB, 0xBF};
    ::WriteFile(h, bom, 3, &w, nullptr);
    std::string utf8 = WideToUtf8(text);
    ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    ::CloseHandle(h);
    Logger::Info("Macro saved: " + WideToUtf8(ofn.lpstrFile));
}

void MainWindow::DoMacroLoad() {
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Macro Files (*.xfm)\0*.xfm\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = MacroDir().c_str();
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    DoMacroLoadFrom(ofn.lpstrFile);
}

// 从任意路径加载宏（DoMacroLoad 文件对话框路径 + CLI .xfm 双击直达共用）
void MainWindow::DoMacroLoadFrom(const std::wstring& path) {
    std::string raw;
    if (!ReadFileBytes(path, raw)) {
        MessageBoxW(hwnd_, Tr(L"msg.macro.loadfail"), L"xfsWinPad Macro", MB_OK | MB_ICONERROR);
        return;
    }
    if (raw.size() >= 3 && (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF)
        raw.erase(0, 3);
    if (macro_.LoadSerialized(Utf8ToWide(raw))) {
        Logger::Info("Macro loaded: " + WideToUtf8(path) +
                     " events=" + std::to_string(macro_.Count()));
    } else {
        MessageBoxW(hwnd_, Tr(L"msg.macro.badformat"), L"xfsWinPad Macro", MB_OK | MB_ICONERROR);
    }
}
// --- tab strip painting -------------------------------------------------------
// Notepad++-style close button: real X icon at the tab's right edge, larger
// DPI-scaled hit area (TabBar::CloseRect), highlighted on hover, dim otherwise.
// The tabs are NOT owner-drawn: TabBar keeps the stock control in auto-layout
// mode so each tab width tracks its own title (short file -> narrow tab, long
// file -> wide tab). TabBar intercepts WM_PAINT and calls back here for every
// tab, so we render the full themed look while the control only handles layout,
// hit-testing, tooltips and drag reordering.

void MainWindow::DrawTabItem(HDC hdc, TabBar& tabs, int itemID, const RECT& rc,
                             bool selected) {
    const ThemeDef* t = theme_ ? theme_ : theme::Find(L"Light");

    // background
    COLORREF bg = selected ? t->tabActiveBg : t->tabInactiveBg;
    RECT fill = rc;
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(hdc, &fill, b);
    DeleteObject(b);

    // accent bar on the active tab + right edge separator
    if (selected) {
        RECT accent{rc.left, rc.top, rc.right, rc.top + 3};
        HBRUSH ab = CreateSolidBrush(t->tabAccent);
        FillRect(hdc, &accent, ab);
        DeleteObject(ab);
    }
    RECT edge{rc.right - 1, rc.top, rc.right, rc.bottom};
    HBRUSH eb = CreateSolidBrush(t->tabEdge);
    FillRect(hdc, &edge, eb);
    DeleteObject(eb);

    // close button geometry (identical to hit-testing)
    RECT closeRc = tabs.CloseRect(rc);
    bool closeHot = tabs.IsCloseHovered(itemID);

    // title text (may carry "* " dirty prefix). The stored text has trailing
    // padding spaces (see TabBar::PaddedTitle) that reserve the close-button
    // area in the auto-layout; strip them before painting.
    wchar_t title[512] = L"";
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = title;
    ti.cchTextMax = 512;
    SendMessageW(tabs.Hwnd(), TCM_GETITEMW, itemID, (LPARAM)&ti);
    size_t len = wcslen(title);
    while (len > 0 && title[len - 1] == L' ') title[--len] = L'\0';

    SetBkMode(hdc, TRANSPARENT);
    HFONT font = (HFONT)SendMessageW(tabs.Hwnd(), WM_GETFONT, 0, 0);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, selected ? t->tabActiveText : t->tabInactiveText);

    RECT textRc = rc;
    textRc.left += 10;
    textRc.right = closeRc.left - 2;
    DrawTextW(hdc, title, -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // dirty indicator: red dot before the text
    Document* d = (&tabs == &workspace_->Tabs1())
                      ? workspace_->FindByTabIndex1(itemID)
                      : workspace_->FindByTabIndex(itemID);
    if (d && d->editor.Modified()) {
        int hgt = rc.bottom - rc.top;
        int r = hgt / 9;
        HBRUSH dot = CreateSolidBrush(RGB(0xE8, 0x11, 0x23));
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, dot);
        HPEN oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        int cy = rc.top + hgt / 2;
        Ellipse(hdc, textRc.left, cy - r, textRc.left + 2 * r, cy + r);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(dot);
    }

    // close X icon — locked tabs draw an amber padlock badge instead
    Document* tabDoc = (&tabs == &workspace_->Tabs1())
                      ? workspace_->FindByTabIndex1(itemID)
                      : workspace_->FindByTabIndex(itemID);
    int size = closeRc.bottom - closeRc.top;
    if (tabDoc && tabDoc->locked) {
        const COLORREF bodyC = RGB(0xE8, 0xA3, 0x3D);   // 琥珀锁体
        const COLORREF lineC = RGB(0x8A, 0x5A, 0x10);   // 深棕锁环
        const int bw = size * 6 / 10, bh = size * 5 / 10;
        const int bx = closeRc.left + (size - bw) / 2;
        const int by = closeRc.top + size / 2;
        HBRUSH bb = CreateSolidBrush(bodyC);
        HPEN bp = CreatePen(PS_SOLID, 1, lineC);
        HBRUSH obb = (HBRUSH)SelectObject(hdc, bb);
        HPEN obp = (HPEN)SelectObject(hdc, bp);
        Rectangle(hdc, bx, by, bx + bw, by + bh);                       // 锁体
        Arc(hdc, closeRc.left + size / 4, closeRc.top + size / 8,
                 closeRc.right - size / 4, closeRc.top + size * 3 / 5,
                 closeRc.left + size / 4, by,
                 closeRc.right - size / 4, by);                         // 锁环
        SelectObject(hdc, obp);
        SelectObject(hdc, obb);
        DeleteObject(bp);
        DeleteObject(bb);
    } else {
    if (closeHot) {
        // highlight pill behind the X (Notepad++/Chrome style)
        RECT pill = closeRc;
        HBRUSH pb = CreateSolidBrush(RGB(0xE8, 0x11, 0x23));
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, pb);
        HPEN oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, pill.left, pill.top, pill.right, pill.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(pb);
    }
    int inset = size * 3 / 10;
    int x1 = closeRc.left + inset, y1 = closeRc.top + inset;
    int x2 = closeRc.right - inset, y2 = closeRc.bottom - inset;
    HPEN pen = CreatePen(PS_SOLID, closeHot ? 2 : 1,
                         closeHot ? RGB(0xFF, 0xFF, 0xFF) : t->tabCloseGlyph);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    MoveToEx(hdc, x2 - 1, y1, nullptr);
    LineTo(hdc, x1 - 1, y2);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    }

    SelectObject(hdc, oldFont);
    SetTextColor(hdc, RGB(0, 0, 0));
}

void MainWindow::DoDiffCompare() {
    int count = workspace_->Count();
    if (count < 2) {
    MessageBoxW(hwnd_, Tr(L"msg.diff.needtwo"),
                L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
        return;
    }
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    for (int i = 0; i < count; ++i) {
        Document* d = workspace_->DocumentAt(i);
        if (!d) continue;
        UINT flags = MF_STRING;
        if (d == workspace_->Active()) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, 2001 + i, d->DisplayName().c_str());
    }
    POINT pt; GetCursorPos(&pt);
    UINT sel = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(menu);
    if (sel == 0) return;
    int otherIdx = (int)sel - 2001;
    Document* a = workspace_->Active();
    Document* b = workspace_->DocumentAt(otherIdx);
    if (!a || !b || a == b) return;
    RunDiffCompare(a, b);
}

// a = left pane, b = right pane
void MainWindow::RunDiffCompare(Document* a, Document* b) {
    if (!a || !b || a == b) return;

    // collect all lines from both docs
    auto getLines = [](Editor& ed) -> std::vector<std::string> {
        std::vector<std::string> v;
        sptr_t n = ed.Send(SCI_GETLINECOUNT);
        char buf[4096];
        for (sptr_t i = 0; i < n; ++i) {
            sptr_t len = ed.Send(SCI_GETLINE, i, (LPARAM)buf);
            buf[len > 0 ? len : 0] = '\0';
            std::string s(buf);
            while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
            v.push_back(s);
        }
        return v;
    };
    auto LA = getLines(a->editor);
    auto LB = getLines(b->editor);
    int N = (int)LA.size(), M = (int)LB.size();

    // LCS dynamic programming table (N+1 x M+1)
    std::vector<std::vector<int>> dp(N + 1, std::vector<int>(M + 1, 0));
    for (int i = N - 1; i >= 0; --i)
        for (int j = M - 1; j >= 0; --j)
            dp[i][j] = (LA[i] == LB[j])
                ? dp[i+1][j+1] + 1
                : (std::max)(dp[i+1][j], dp[i][j+1]);

    // backtrack: mark differing lines in both docs and build the line mapping
    // used for diff-aligned scroll sync in side-by-side mode.
    // Consecutive remove/add runs are paired up as "changed" (orange) on both sides;
    // unpaired surplus lines are pure insert/delete (blue).
    a->editor.ClearDiffMarks();
    b->editor.ClearDiffMarks();
    std::vector<int> mapA(N, -1), mapB(M, -1);
    int diffCount = 0;
    int i = 0, j = 0;
    while (i < N && j < M) {
        if (LA[i] == LB[j]) {
            mapA[i] = j; mapB[j] = i;
            ++i; ++j; continue;
        }

        int i0 = i, j0 = j;
        while (i < N && j < M && LA[i] != LB[j])
            (dp[i+1][j] >= dp[i][j+1]) ? ++i : ++j;

        int remA = i - i0, addB = j - j0;
        int pairs = (std::min)(remA, addB);
        for (int k = 0; k < pairs; ++k) {           // changed in place -> orange
            a->editor.MarkDiffLine(i0 + k, false);
            b->editor.MarkDiffLine(j0 + k, false);
            mapA[i0 + k] = j0 + k;                  // aligned pair: scroll together
            mapB[j0 + k] = i0 + k;
            diffCount += 2;
        }
        for (int k = pairs; k < remA; ++k) {        // only in A -> blue
            a->editor.MarkDiffLine(i0 + k, true);
            ++diffCount;
        }
        for (int k = pairs; k < addB; ++k) {        // only in B -> blue
            b->editor.MarkDiffLine(j0 + k, true);
            ++diffCount;
        }
    }
    for (; i < N; ++i) { a->editor.MarkDiffLine(i, true); ++diffCount; }
    for (; j < M; ++j) { b->editor.MarkDiffLine(j, true); ++diffCount; }

    Logger::Info("LCS Diff: " + std::to_string(diffCount) +
                 " diffs between '" + WideToUtf8(a->DisplayName()) +
                 "' and '" + WideToUtf8(b->DisplayName()) + "'");

    workspace_->EnterCompareMode(a, b, std::move(mapA), std::move(mapB));
    LayoutChildren();

    wchar_t msg[128];
    swprintf_s(msg, L"%s",
        I18n::Instance().Fmt(L"msg.diff.done",
        {std::to_wstring(diffCount)}).c_str());
    MessageBoxW(hwnd_, msg, L"xfsWinPad Diff", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::SyncGitUi() {
    if (explorer_) explorer_->SetGitStates(git_.States());
    UpdateTitleBar();
}

void MainWindow::OnGitDone(GitSnapshot* snap) {
    git_.OnDone(snap);
    SyncGitUi();
}

void MainWindow::GitCompareWithHead(const std::wstring& absPath) {
    if (!git_.HasRoot()) return;
    wchar_t base[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, base)) return;
    std::wstring dir = std::wstring(base) + L"xfsWinPad\\git\\";
    ::SHCreateDirectoryExW(hwnd_, dir.c_str(), nullptr);
    std::wstring name = std::filesystem::path(absPath).filename().wstring();
    size_t h = std::hash<std::wstring>{}(absPath);
    std::wstring temp = dir + name + L".head-" + std::to_wstring(h) + L".txt";
    if (!git_.FetchHeadBlob(absPath, temp)) {
        MessageBoxW(hwnd_, Tr(L"git.noblob"), L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Logger::Info("git: compare HEAD started for " + WideToUtf8(absPath));
}

void MainWindow::GitShowRevision(const std::wstring& absPath,
                                 const std::wstring& rev) {
    if (!git_.HasRoot() || rev.empty()) return;
    wchar_t base[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, base)) return;
    std::wstring dir = std::wstring(base) + L"xfsWinPad\\git\\";
    ::SHCreateDirectoryExW(hwnd_, dir.c_str(), nullptr);
    std::wstring name = std::filesystem::path(absPath).filename().wstring();
    size_t h = std::hash<std::wstring>{}(absPath + rev);
    std::wstring temp = dir + name + L"." + rev + L"-" + std::to_wstring(h) + L".txt";
    if (!git_.FetchBlobRev(rev, absPath, temp)) {
        MessageBoxW(hwnd_, Tr(L"git.noblob"), L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Logger::Info("git: history blob fetch started for " + WideToUtf8(absPath));
}

void MainWindow::OnGitBlob(GitBlobResult* res) {
    if (!res) return;
    std::unique_ptr<GitBlobResult> guard(res);
    if (!res->ok) {
        MessageBoxW(hwnd_, Tr(L"git.noblob"), L"xfsWinPad", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Document* orig = nullptr;
    for (int i = 0; i < workspace_->Count(); ++i) {
        Document* d = workspace_->DocumentAt(i);
        if (d && d->HasPath() &&
            _wcsicmp(d->path.wstring().c_str(), res->absPath.c_str()) == 0)
            { orig = d; break; }
    }
    workspace_->OpenPath(res->tempPath, -1, true, false);
    Document* blob = nullptr;
    for (int i = 0; i < workspace_->Count(); ++i) {
        Document* d = workspace_->DocumentAt(i);
        if (d && d->HasPath() &&
            _wcsicmp(d->path.wstring().c_str(), res->tempPath.c_str()) == 0)
            { blob = d; break; }
    }
    if (orig && blob && orig != blob) RunDiffCompare(orig, blob);
    else Logger::Warn("git: compare HEAD skipped (document not found)");
}

void MainWindow::WireExplorerGit() {
    if (!explorer_) return;
    explorer_->onGitCompare = [this](const std::wstring& path) {
        GitCompareWithHead(path);
    };
    explorer_->onGitHistory = [this](const std::wstring& path) {
        if (!git_.FileHistory(path))
            Logger::Warn("git: file history could not start");
    };
    explorer_->onGitStage = [this](const std::wstring& path) {
        GitStagePath(path, false);
    };
    explorer_->onGitUnstage = [this](const std::wstring& path) {
        GitStagePath(path, true);
    };
    explorer_->onGitCommit = [this]() {
        GitCommitDialog();
    };
    explorer_->onGitBranch = [this]() {
        if (!git_.HasRoot()) return;
        gitPick_ = GitPick::Switch;   // OnGitOp(Fetch) chains into the picker
        if (!git_.Fetch()) {
            gitPick_ = GitPick::None;
            Logger::Warn("git: pre-switch fetch could not start");
        }
    };
    explorer_->onGitMerge = [this]() {
        if (!git_.HasRoot()) return;
        gitPick_ = GitPick::Merge;
        if (!git_.ListBranches()) {
            gitPick_ = GitPick::None;
            Logger::Warn("git: branch list could not start");
        }
    };
    explorer_->onGitBranchDel = [this]() {
        if (!git_.HasRoot()) return;
        gitPick_ = GitPick::DeleteBranch;
        if (!git_.ListBranches()) {
            gitPick_ = GitPick::None;
            Logger::Warn("git: branch list could not start");
        }
    };
    explorer_->onGitBranchDelRemote = [this]() {
        if (!git_.HasRoot()) return;
        gitPick_ = GitPick::DeleteRemoteBranch;
        if (!git_.ListBranches()) {
            gitPick_ = GitPick::None;
            Logger::Warn("git: branch list could not start");
        }
    };
    explorer_->onGitBranchNew = [this]() {
        if (!git_.HasRoot()) return;
        std::wstring name;
        if (!InputBox(hwnd_, inst_, Tr(L"git.branch.new"),
                      Tr(L"git.branch.new.prompt"), name))
            return;
        while (!name.empty() && name.back() == L' ') name.pop_back();
        while (!name.empty() && name.front() == L' ') name.erase(name.begin());
        if (name.empty()) return;
        if (!git::BranchNameOk(name)) {
            MessageBoxW(hwnd_, Tr(L"git.branch.bad"), L"xfsWinPad",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        if (!git_.CreateBranch(name))
            Logger::Warn("git: branch create could not start");
    };
    explorer_->onGitBranchRen = [this]() {
        if (!git_.HasRoot()) return;
        const std::wstring cur = git_.Branch();
        if (cur.empty() || cur == L"HEAD") {
            // detached: git would refuse anyway; say so without spawning
            MessageBoxW(hwnd_, Tr(L"git.branch.detached"), L"xfsWinPad",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        std::wstring name = cur;   // prefill so the user edits instead of retyping
        if (!InputBox(hwnd_, inst_, Tr(L"git.branch.ren"),
                      Tr(L"git.branch.ren.prompt"), name))
            return;
        while (!name.empty() && name.back() == L' ') name.pop_back();
        while (!name.empty() && name.front() == L' ') name.erase(name.begin());
        if (name.empty() || name == cur) return;
        if (!git::BranchNameOk(name)) {
            MessageBoxW(hwnd_, Tr(L"git.branch.bad"), L"xfsWinPad",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        if (!git_.RenameBranch(name))
            Logger::Warn("git: branch rename could not start");
    };
    explorer_->onGitFetch = [this]() {
        if (!git_.Fetch()) Logger::Warn("git: fetch could not start");
    };
    explorer_->onGitPull = [this]() {
        if (!git_.Pull()) Logger::Warn("git: pull could not start");
    };
    explorer_->onGitStash = [this]() {
        if (!git_.Stash()) Logger::Warn("git: stash could not start");
    };
    explorer_->onGitUnstash = [this]() {
        if (!git_.Unstash()) Logger::Warn("git: unstash could not start");
    };
    explorer_->onGitMergeAbort = [this]() {
        if (!git_.HasRoot()) return;
        if (!git_.MergeAbort()) Logger::Warn("git: merge abort could not start");
    };
    explorer_->onGitRevert = [this](const std::wstring& path) {
        if (!git_.HasRoot()) return;
        std::wstring msg = I18n::Instance().Fmt(Tr(L"git.revert.confirm"),
                                                {path});
        if (::MessageBoxW(hwnd_, msg.c_str(), L"xfsWinPad",
                           MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
            Logger::Info("git: revert cancelled " + WideToUtf8(path));
            return;
        }
        if (!git_.RevertFile(path))
            Logger::Warn("git: revert could not start");
    };
    explorer_->onGitPush = [this]() {
        if (!git_.Push()) Logger::Warn("git: push could not start");
    };
}

void MainWindow::GitStagePath(const std::wstring& absPath, bool unstage) {
    if (!git_.HasRoot()) return;
    if (unstage) git_.Unstage(absPath);
    else git_.Stage(absPath);
}

void MainWindow::GitCommitDialog() {
    if (!git_.HasRoot()) return;
    std::wstring msg;
    if (!InputBox(hwnd_, inst_, Tr(L"git.commit"), Tr(L"git.commit.msg"), msg, true))
        return;
    std::replace(msg.begin(), msg.end(), L'\r', L'\n');
    while (!msg.empty() && msg.back() == L'\n') msg.pop_back();
    if (msg.empty()) {
        Logger::Info("git: commit cancelled (empty message)");
        return;
    }
    if (!git_.Commit(msg))
        Logger::Warn("git: commit could not start");
}

void MainWindow::OnGitOp(GitOpResult* res) {
    if (!res) return;
    std::unique_ptr<GitOpResult> guard(res);
    GitPick pick = gitPick_;
    gitPick_ = GitPick::None;   // consumed once, whatever the outcome
    if (res->kind == GitOpKind::Fetch && pick == GitPick::Switch) {
        // pre-switch fetch: a dead network must only degrade the picker to
        // local refs, so failures are logged, never dialogged.
        if (!res->ok)
            Logger::Warn("git: pre-switch fetch failed: " + res->output);
        else
            Logger::Info("git: pre-switch fetch ok");
        git_.RequestForPath(git_.Root());   // refresh ahead/behind from new refs
        if (!git_.ListBranches())
            Logger::Warn("git: branch list could not start");
        return;
    }
    if (res->kind == GitOpKind::History && res->ok) {
        // `git log --oneline` rows: "<hash> <subject>"; the hash feeds the
        // blob fetch, the whole line is what the user reads.
        std::vector<std::wstring> rows;
        std::vector<std::wstring> revs;
        size_t pos = 0;
        const std::string& out = res->output;
        while (pos < out.size()) {
            size_t nl = out.find('\n', pos);
            std::string line = out.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? out.size() : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t sp = line.find(' ');
            if (line.empty() || sp == std::string::npos) continue;
            revs.push_back(Utf8ToWide(line.substr(0, sp)));
            rows.push_back(Utf8ToWide(line));
        }
        if (rows.empty()) {
            Logger::Info("git: file history empty");
            return;
        }
        int selH = -1;
        if (!ListPicker(hwnd_, inst_, Tr(L"git.log"), Tr(L"git.log"),
                        rows, -1, selH))
            return;
        GitShowRevision(res->arg, revs[selH]);
        return;
    }
    if (res->kind == GitOpKind::ListBranches && res->ok) {
        auto branches = git::ParseBranchList(res->output);
        if (branches.empty()) {
            Logger::Warn("git: branch list empty");
            return;
        }
        if (pick == GitPick::DeleteBranch) {
            // local branches only, minus the checked-out one (git refuses it
            // anyway — hiding it keeps the list honest)
            std::vector<std::wstring> locals;
            for (const auto& b : branches)
                if (!b.remote && !b.current) locals.push_back(b.name);
            if (locals.empty()) {
                Logger::Info("git: no other local branch to delete");
                return;
            }
            int selD = -1;
            if (!ListPicker(hwnd_, inst_, Tr(L"git.branch.del"),
                             Tr(L"git.branch.del.pick"), locals, -1, selD))
                return;
            if (!git_.DeleteBranch(locals[selD]))
                Logger::Warn("git: branch delete could not start");
            return;
        }
        if (pick == GitPick::DeleteRemoteBranch) {
            // refs/remotes entries already have HEAD pruned by ParseBranchList
            std::vector<std::wstring> rems;
            for (const auto& b : branches)
                if (b.remote) rems.push_back(b.name);  // "or52/head2"
            if (rems.empty()) {
                Logger::Info("git: no remote branch to delete");
                return;
            }
            int selR = -1;
            if (!ListPicker(hwnd_, inst_, Tr(L"git.branch.delremote"),
                             Tr(L"git.branch.delremote.pick"), rems, -1, selR))
                return;
            // remote name stops at the first '/', the branch may contain more
            const std::wstring& full = rems[selR];
            size_t slash = full.find(L'/');
            if (slash == std::wstring::npos) {
                Logger::Warn("git: malformed remote branch " + WideToUtf8(full));
                return;
            }
            std::wstring remote = full.substr(0, slash);
            std::wstring branch = full.substr(slash + 1);
            std::wstring msg = std::wstring(Tr(L"git.branch.delremote.confirm")) +
                               L"\n" + full;
            if (MessageBoxW(hwnd_, msg.c_str(), L"xfsWinPad",
                            MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
                Logger::Info("git: remote branch delete cancelled " +
                             WideToUtf8(full));
                return;
            }
            if (!git_.DeleteRemoteBranch(remote, branch))
                Logger::Warn("git: remote branch delete could not start");
            return;
        }
        std::vector<std::wstring> names;
        std::vector<std::wstring> checkoutName;
        int cur = -1;
        for (size_t i = 0; i < branches.size(); ++i) {
            names.push_back(branches[i].name);
            if (branches[i].remote) {
                // "origin/main" -> "main": git DWIM creates the tracking branch
                auto slash = branches[i].name.find(L'/');
                checkoutName.push_back(slash == std::wstring::npos
                                           ? branches[i].name
                                           : branches[i].name.substr(slash + 1));
            } else {
                checkoutName.push_back(branches[i].name);
            }
            if (branches[i].current) cur = (int)i;
        }
        int sel = -1;
        if (!ListPicker(hwnd_, inst_, Tr(L"git.branch"), Tr(L"git.branch.pick"),
                        names, cur, sel))
            return;
        const std::wstring& target = checkoutName[sel];
        if (pick == GitPick::Merge) {
            // git itself refuses merges that would clobber uncommitted
            // changes, so no extra dirty-guard here.
            if (!git_.Merge(target))
                Logger::Warn("git: merge could not start");
            return;
        }
        bool dirty = false;
        if (auto st = git_.States())
            for (auto& [path, state] : *st)
                if (state != git::FileState::Untracked) { dirty = true; break; }
        if (dirty) {
            int r = MessageBoxW(hwnd_, Tr(L"git.checkout.dirty"), L"xfsWinPad",
                                MB_OKCANCEL | MB_ICONWARNING);
            if (r != IDOK) {
                Logger::Info("git: checkout cancelled (dirty) " + WideToUtf8(target));
                return;
            }
        }
        if (branches[sel].remote) {
            // A remote row whose short name also sits on another remote is
            // DWIM-ambiguous; with no local counterpart git would refuse it
            // opaquely, so --track the exact ref the user clicked instead.
            bool otherRemote = false, localSame = false;
            for (size_t i = 0; i < branches.size(); ++i) {
                if (i == (size_t)sel || checkoutName[i] != target) continue;
                if (branches[i].remote) otherRemote = true;
                else localSame = true;
            }
            if (otherRemote && !localSame) {
                if (!git_.CheckoutTrack(branches[sel].name))
                    Logger::Warn("git: track checkout could not start");
                return;
            }
        }
        if (!git_.Checkout(target))
            Logger::Warn("git: checkout could not start");
        return;
    }
    const wchar_t* name =
        res->kind == GitOpKind::Commit ? Tr(L"git.commit") :
        res->kind == GitOpKind::Unstage ? Tr(L"git.unstage") :
        res->kind == GitOpKind::Checkout ? Tr(L"git.branch") :
        res->kind == GitOpKind::CheckoutTrack ? Tr(L"git.branch") :
        res->kind == GitOpKind::CreateBranch ? Tr(L"git.branch.new") :
        res->kind == GitOpKind::Push ? Tr(L"git.push") :
        res->kind == GitOpKind::Fetch ? Tr(L"git.fetch") :
        res->kind == GitOpKind::Pull ? Tr(L"git.pull") :
        res->kind == GitOpKind::Merge ? Tr(L"git.merge") :
        res->kind == GitOpKind::MergeAbort ? Tr(L"git.merge.abort") :
        res->kind == GitOpKind::DeleteBranch ? Tr(L"git.branch.del") :
        res->kind == GitOpKind::DeleteRemoteBranch ?
            Tr(L"git.branch.delremote") :
        res->kind == GitOpKind::RenameBranch ? Tr(L"git.branch.ren") :
        res->kind == GitOpKind::Stash ? Tr(L"git.stash") :
        res->kind == GitOpKind::Unstash ? Tr(L"git.unstash") :
        res->kind == GitOpKind::Revert ? Tr(L"git.revert") :
        res->kind == GitOpKind::History ? Tr(L"git.log") :
        res->kind == GitOpKind::ListBranches ? Tr(L"git.branch") :
        Tr(L"git.stage");
    if (!res->ok) {
        std::wstring text = I18n::Instance().Fmt(Tr(L"git.op.fail"),
                                                 {Utf8ToWide(res->output)});
        MessageBoxW(hwnd_, (std::wstring(name) + L"\n" + text).c_str(),
                    L"xfsWinPad", MB_OK | MB_ICONWARNING);
    } else {
        Logger::Info("git: op ok (kind=" + std::to_string((int)res->kind) + ")");
    }
    git_.RequestForPath(git_.Root());   // refresh status colors + branch
}

void MainWindow::SwitchTheme(const ThemeDef* t) {
    if (!t || t == theme_) return;
    theme_ = t;
    settings_.theme = t->name;
    workspace_->ApplyThemeToAll(*t);
    if (hex_) hex_->ApplyTheme(*t);
    if (stdf_) stdf_->ApplyTheme(*t);
    if (csv_) csv_->ApplyTheme(*t);
    if (logPanel_) logPanel_->ApplyTheme(*t);
    if (terminal_) terminal_->ApplyTheme(*t);
    if (ai_) ai_->ApplyTheme(*t);
    InvalidateRect(hwnd_, nullptr, TRUE);
    SettingsSave(SettingsFilePath(), settings_);
    Logger::Info(std::string("Theme switched: ") +
                 xfs::WideToUtf8(t->name));
}

void MainWindow::ExecuteCommand(unsigned int id) {
    // window-menu doc entries: switch directly, never macro-recorded
    if (id >= Cmd::WindowDocFirst && id < Cmd::WindowDocFirst + 100) {
        const int flat = (int)(id - Cmd::WindowDocFirst);
        const int n0 = workspace_->Count();
        if (flat < n0) workspace_->Activate(flat);
        else           workspace_->ActivateView1(flat - n0);
        return;
    }
    if (macro_.Recording()) macro_.RecordCommand(id);
    // plugin commands live in a separate id range and dispatch to the plugin
    if (plugins_ && plugins_->Execute(id)) return;
    switch (id) {
        case Cmd::FileNew:     workspace_->NewDocument(); break;
        case Cmd::FileOpen:    workspace_->OpenFileDialog(); break;
        case Cmd::FileOpenFolder:  OpenFolderDialog(); break;
        case Cmd::FileCloseFolder: CloseFolder(); break;
        case Cmd::FileSave:
            if (hex_ && hex_->HasFocus()) {
                hex_->Save();   // Ctrl+S in hex panel writes the binary overlay
            } else {
                // hex edits pending for the active document? flush them too,
                // otherwise Ctrl+S from the editor silently skips hex changes
                if (hex_ && hex_->Modified() && workspace_->Active() &&
                    hex_->FilePath() == workspace_->Active()->path.wstring())
                    hex_->Save();
                workspace_->Save();
            }
            break;
        case Cmd::FileSaveAs:  workspace_->SaveAs(); break;
        case Cmd::FileSaveAll: workspace_->SaveAll(); break;
        case Cmd::FileClose:   workspace_->CloseActive(); break;
        case Cmd::ReopenClosedTab: workspace_->ReopenClosedTab(); break;
        case Cmd::FileNewWindow: NewWindowProcess(); break;
        case Cmd::FileReload:  workspace_->ReloadActive(); break;
        case Cmd::FileCloseAll:
            if (workspace_->CloseAll()) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case Cmd::FilePrint:
            if (Document* d = workspace_->Active()) PrintDocument();
            break;
        case Cmd::FileExit:    PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;

        case Cmd::EditUndo:
            if (hex_ && hex_->HasFocus()) hex_->Undo();
            else EditCmd(Cmd::EditUndo);
            break;
        case Cmd::EditRedo:
            if (hex_ && hex_->HasFocus()) hex_->Redo();
            else EditCmd(Cmd::EditRedo);
            break;
        case Cmd::EditCut: EditCmd(Cmd::EditCut); break;
        case Cmd::EditCopy: EditCmd(Cmd::EditCopy); break;
        case Cmd::EditPaste: EditCmd(Cmd::EditPaste); break;
        case Cmd::EditDelete: EditCmd(Cmd::EditDelete); break;
        case Cmd::EditSelectAll: EditCmd(Cmd::EditSelectAll); break;
        case Cmd::EditTimeDate: InsertDateTime(); break;
        case Cmd::EditDuplicateLine: EditCmd(Cmd::EditDuplicateLine); break;
        case Cmd::EditMoveLineUp: EditCmd(Cmd::EditMoveLineUp); break;
        case Cmd::EditMoveLineDown: EditCmd(Cmd::EditMoveLineDown); break;
        case Cmd::EditDeleteLine: EditCmd(Cmd::EditDeleteLine); break;
        case Cmd::EditUpperCase: EditCmd(Cmd::EditUpperCase); break;
        case Cmd::EditLowerCase: EditCmd(Cmd::EditLowerCase); break;
        case Cmd::EditSortAsc: EditCmd(Cmd::EditSortAsc); break;
        case Cmd::EditSortDesc: EditCmd(Cmd::EditSortDesc); break;
        case Cmd::EditRemoveDupLines: EditCmd(Cmd::EditRemoveDupLines); break;
        case Cmd::EditTrimTrailingSpace: EditCmd(Cmd::EditTrimTrailingSpace); break;
        case Cmd::EditToggleComment: EditCmd(Cmd::EditToggleComment); break;
        case Cmd::EditJoinLines: EditCmd(Cmd::EditJoinLines); break;
        case Cmd::EditSplitLines: EditCmd(Cmd::EditSplitLines); break;
        case Cmd::EditRemoveEmptyLines: EditCmd(Cmd::EditRemoveEmptyLines); break;
        case Cmd::EditReverseLines: EditCmd(Cmd::EditReverseLines); break;

        case Cmd::SearchFind:      ShowFindDialog(L"", 0); break;
        case Cmd::SearchReplace:   ShowFindDialog(L"", 1); break;
        case Cmd::SearchFindNext:
            if (Document* d = workspace_->Active())
                FindInEditor(d->editor, workspace_->Find(), FindDirection::Forward);
            break;
        case Cmd::SearchFindPrev:
            if (Document* d = workspace_->Active())
                FindInEditor(d->editor, workspace_->Find(), FindDirection::Backward);
            break;
        case Cmd::SearchGotoLine: DoGotoLine(); break;

        case Cmd::ViewWordWrap: ToggleWordWrap(); break;
        case Cmd::ViewExplorer: ToggleExplorer(); break;
        case Cmd::ViewHexView: ToggleHexView(); break;
        case Cmd::ViewStdfView: ToggleStdfView(); break;
        case Cmd::ViewCsvView: ToggleCsvView(); break;
        case Cmd::ViewBigFile: ToggleBigFileView(); break;
        case Cmd::ViewLogPanel: ToggleLogPanel(); break;
        case Cmd::ViewTerminal: ToggleTerminal(); break;
    case Cmd::ViewAiPanel:  ToggleAiPanel(); break;
    case Cmd::AiNewSession:
        if (ai_) ai_->NewSession(); break;
    case Cmd::AiToggleContext:
        settings_.aiAttachContext = !settings_.aiAttachContext;
        ::CheckMenuItem(menu_, Cmd::AiToggleContext, MF_BYCOMMAND |
            (settings_.aiAttachContext ? MF_CHECKED : MF_UNCHECKED));
        if (ai_) ai_->SetAttachContext(settings_.aiAttachContext);
        SettingsSave(SettingsFilePath(), settings_);
        break;
        case Cmd::WindowList:   RunWindowList(); break;
        case Cmd::PluginAdmin: RunPluginAdmin(); break;
        case Cmd::Preferences: RunPreferences(); break;
        case Cmd::StyleConfigurator: RunStyleConfigurator(); break;
        case Cmd::ShortcutMapper: RunShortcutMapper(); break;
        case Cmd::ThemeExport: RunThemeExport(); break;
        case Cmd::ThemeImport: RunThemeImport(); break;
        case Cmd::ConfigExport: RunConfigExport(); break;
        case Cmd::ConfigImport: RunConfigImport(); break;
        case Cmd::DiffCompare: DoDiffCompare(); break;
        case Cmd::DiffExitCompare:
            workspace_->ExitCompareMode();
            LayoutChildren();
            break;
        case Cmd::ViewMoveToOtherView:
            workspace_->MoveActiveToOtherView();
            LayoutChildren();
            break;
        case Cmd::ViewMoveToNewView:
            MoveCurrentToNewWindow();
            break;
        case Cmd::FoldAll:
            if (Document* d = workspace_->Active())
                d->editor.Send(SCI_FOLDALL);
            break;
        case Cmd::UnfoldAll:
            if (Document* d = workspace_->Active())
                d->editor.UnfoldAll();
            break;
        case Cmd::PaletteShow:
            if (!palette_) {
                palette_ = std::make_unique<CommandPalette>();
                palette_->onExecute = [this](unsigned id) { ExecuteCommand(id); };
            }
            if (plugins_ && plugins_->CommandCount() > 0) {
                std::vector<std::wstring> labels;
                std::vector<CommandPalette::Item> extra;
                labels.reserve(plugins_->CommandCount());
                extra.reserve(plugins_->CommandCount());
                for (const auto& c : plugins_->Commands()) {
                    labels.push_back(c.category + L": " + c.label);
                    extra.push_back({ labels.back().c_str(), c.id });
                }
                palette_->SetExtraItems(extra);
            } else {
                palette_->SetExtraItems({});
            }
            palette_->Show(hwnd_, inst_);
            break;
        case Cmd::ThemeLight:   SwitchTheme(theme::Find(L"light")); break;
        case Cmd::ThemeDark:    SwitchTheme(theme::Find(L"dark"));  break;
        case Cmd::ViewZoomIn:  if (workspace_->Active()) workspace_->Active()->editor.ZoomIn(); break;
        case Cmd::ViewZoomOut: if (workspace_->Active()) workspace_->Active()->editor.ZoomOut(); break;
        case Cmd::ViewZoomReset: if (workspace_->Active()) workspace_->Active()->editor.ZoomReset(); break;

        case Cmd::MacroStart:   ToggleMacroRecord(); break;
        case Cmd::MacroStop:    ToggleMacroRecord(); break;
        case Cmd::MacroPlayback: DoMacroPlayback(); break;
        case Cmd::MacroSave:    DoMacroSave(); break;
        case Cmd::MacroLoad:    DoMacroLoad(); break;

        case Cmd::HelpAbout: {
            const wchar_t* ver =
    #define XFS_STR2(x) L##x
    #define XFS_WSTR(x) XFS_STR2(x)
                XFS_WSTR(XFS_VERSION_STRING);
            MessageBoxW(hwnd_,
                (std::wstring(L"xfsWinPad v") + ver +
                 L"\nProfessional Windows Text Editor\n\n"
                 L"Editor core: Scintilla 5.6.6 + Lexilla 5.5.3").c_str(),
                L"About", MB_OK | MB_ICONINFORMATION);
            break;
        }

        default:
            if (id >= Cmd::LangFirst) {
                const LanguageMenuItem* catalog = LanguageMenuCatalog();
                size_t idx = (size_t)(id - Cmd::LangFirst);
                Document* d = workspace_->Active();
                // catalog ends at the first entry with label == nullptr
                if (catalog[idx].label != nullptr && d) {
                    d->langIndex = (int)idx;
                    const char* kw[2] = { catalog[idx].keywords[0],
                                          catalog[idx].keywords[1] };
                    d->editor.SetLexerByName(
                        catalog[idx].lexerName, kw,
                        theme_);
                    OnWorkspaceChanged();   // refresh menu check + status bar
                }
                return;
            }
            if (id >= Cmd::EncConvertFirst &&
                id < Cmd::EncConvertFirst + 11) {
                workspace_->SetActiveEncoding(
                    (encoding::EncodingType)(id - Cmd::EncConvertFirst));
                return;
            }
            if (id >= Cmd::EncReloadAsFirst &&
                id < Cmd::EncReloadAsFirst + 11) {
                workspace_->ReloadActiveAs(
                    (encoding::EncodingType)(id - Cmd::EncReloadAsFirst));
                return;
            }
            switch (id) {
                case Cmd::BookmarkToggle:
                    if (Document* d = workspace_->Active())
                        d->editor.ToggleBookmark();
                    return;
                case Cmd::BookmarkNext:
                    if (Document* d = workspace_->Active())
                        d->editor.NextBookmark();
                    return;
                case Cmd::BookmarkPrev:
                    if (Document* d = workspace_->Active())
                        d->editor.PreviousBookmark();
                    return;
                case Cmd::BookmarkClearAll:
                    if (Document* d = workspace_->Active())
                        d->editor.ClearBookmarks();
                    return;
            }
            if (id >= Cmd::FileRecentFirst && id < Cmd::FileRecentFirst + recentItems_.size()) {
                OpenUserFile(recentItems_[id - Cmd::FileRecentFirst]);
            } else if (id >= Cmd::FileRecentFolderFirst &&
                       id < Cmd::FileRecentFolderFirst + recentFolderItems_.size()) {
                std::wstring dir = recentFolderItems_[id - Cmd::FileRecentFolderFirst];
                if (std::filesystem::is_directory(dir)) {
                    settings_.explorerVisible = true;
                    SetProjectRoot(dir, true);
                    AddRecentFolder(dir);
                }
            }
            break;
    }
}

void MainWindow::EditCmd(unsigned int cmd) {
    Document* d = workspace_->Active();
    if (!d) return;
    Editor& ed = d->editor;
    switch (cmd) {
        case Cmd::EditUndo: ed.Undo(); break;
        case Cmd::EditRedo: ed.Redo(); break;
        case Cmd::EditCut: ed.Cut(); break;
        case Cmd::EditCopy: ed.Copy(); break;
        case Cmd::EditPaste: ed.Paste(); break;
        case Cmd::EditDelete: ed.ClearSelection(); break;
        case Cmd::EditSelectAll: ed.SelectAll(); break;
        case Cmd::EditDuplicateLine: ed.DuplicateLine(); break;
        case Cmd::EditMoveLineUp: ed.MoveLineUp(); break;
        case Cmd::EditMoveLineDown: ed.MoveLineDown(); break;
        case Cmd::EditDeleteLine: ed.DeleteLine(); break;
        case Cmd::EditUpperCase: ed.UpperCase(); break;
        case Cmd::EditLowerCase: ed.LowerCase(); break;
        case Cmd::EditSortAsc:
            Logger::Info("SortAsc triggered");
            ed.SortSelectedLines(true); break;
        case Cmd::EditSortDesc:
            Logger::Info("SortDesc triggered");
            ed.SortSelectedLines(false); break;
        case Cmd::EditRemoveDupLines:
            Logger::Info("RemoveDup triggered");
            ed.RemoveDuplicateLines(); break;
        case Cmd::EditTrimTrailingSpace:
            Logger::Info("TrimTrailing triggered");
            ed.TrimTrailingSpace(); break;
        case Cmd::EditToggleComment: ed.ToggleLineComment(); break;
        case Cmd::EditJoinLines: ed.JoinSelectedLines(); break;
        case Cmd::EditSplitLines: ed.SplitLineAtCaret(); break;
        case Cmd::EditRemoveEmptyLines: ed.RemoveEmptyLines(); break;
        case Cmd::EditReverseLines: ed.ReverseLines(); break;
    }
    UpdateUndoRedoState();
}

// 工具栏撤销/重做禁用态：跟随活动文档的 SCI_CANUNDO/SCI_CANREDO。
// 无文档双灰；到底（或无可撤销）单灰。只动这两个按钮，其余保持 TBSTATE_ENABLED。
void MainWindow::UpdateUndoRedoState() {
    if (!toolbar_) return;
    Document* d = workspace_ ? workspace_->Active() : nullptr;
    bool canUndo = d && d->editor.Send(SCI_CANUNDO) != 0;
    bool canRedo = d && d->editor.Send(SCI_CANREDO) != 0;
    Logger::Debug(std::string("UndoRedoState: canUndo=") +
                  (canUndo ? "1" : "0") + " canRedo=" + (canRedo ? "1" : "0"));
    struct UR { unsigned int cmd; bool enabled; } items[] = {
        {Cmd::EditUndo, canUndo},
        {Cmd::EditRedo, canRedo},
    };
    for (const auto& it : items) {
        // TB_ENABLEBUTTON（WM_USER+42）：lParam 低字非 0 = enable。
        // 注意不是 TB_SETSTATE（0x0413）——之前误用 0x042A 当 SETSTATE，
        // 实际一直是 ENABLEBUTTON，语义恰好兼容但取值混乱。
        BOOL want = it.enabled ? TRUE : FALSE;
        BOOL cur = (BOOL)::SendMessageW(toolbar_, TB_ISBUTTONENABLED,
                                        it.cmd, 0);
        if (cur != want)
            ::SendMessageW(toolbar_, TB_ENABLEBUTTON, it.cmd,
                           MAKELPARAM(want, 0));
    }
}

// --- window proc ---------------------------------------------------------------------

LRESULT CALLBACK MainWindow::WndProcThunk(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = h;   // make member valid for messages during creation
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = (MainWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->Handle(msg, wp, lp)
                : DefWindowProcW(h, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    // NPP 插件消息垫片（4b）：NPPM_*/RUNCOMMAND 子集先于默认处理拦截转发。
    // ForwardNppMessage 内部做区间检查，区间外置 handled=false 继续默认流程，
    // 绝不吞未知消息（docs/plugin-system.md §5.6）。
    if (plugins_) {
        bool handled = false;
        LRESULT res = plugins_->ForwardNppMessage(msg, wp, lp, handled);
        if (handled) return res;
    }

    // OOP messageProc 桥（v2.1，批次 71）：白名单 OS 广播按值过桥到进程外
    // 插件槽。红线：仅限 wp/lp 均为整数/句柄的消息——带指针参数的
    // （WM_SETTINGCHANGE 的 LPWSTR、WM_POWERBROADCAST 的 setting 块）绝不过桥。
    // 插件返回值仅诊断记录（OOPM_MSGREPLY 回带链路），不改变编辑器默认处理。
    if (plugins_ && plugins_->OopHostPtr()) {
        bool oopValued = false;
        switch (msg) {
            case WM_SYSCOLORCHANGE:
            case WM_TIMECHANGE:
            case WM_DISPLAYCHANGE:
            case WM_DWMCOLORIZATIONCOLORCHANGED:
            case WM_WTSSESSION_CHANGE:
                oopValued = true;
                break;
            default:
                break;
        }
        if (oopValued) {
            bool pluginHandled = false;
            plugins_->OopHostPtr()->BroadcastMessage(msg, wp, lp, &pluginHandled);
            if (pluginHandled) {
                wchar_t buf[96];
                swprintf_s(buf, L"OOP: a plugin handled broadcast msg 0x%x", msg);
                Logger::Info(WideToUtf8(buf));
            }
        }
    }

    switch (msg) {
        case WM_SIZE:
            LayoutChildren();
            return 0;

        case WM_DISPLAYCHANGE:
            // 显示器拓扑变化（拔插屏/改分辨率）：若窗口已不在任何显示器上
            // （或只剩边缘相交），把它拉回最近显示器的可视工作区，避免
            // "窗口在虚无里看不见也拖不到"。
            if (!IsIconic(hwnd_) && !IsZoomed(hwnd_)) {
                RECT wr;
                if (GetWindowRect(hwnd_, &wr)) {
                    HMONITOR mon = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{ sizeof(MONITORINFO) };
                    if (mon && GetMonitorInfoW(mon, &mi)) {
                        RECT isect;
                        long vis = 0, area = 0;
                        if (::IntersectRect(&isect, &wr, &mi.rcMonitor))
                            vis = (isect.right - isect.left) * (isect.bottom - isect.top);
                        area = (wr.right - wr.left) * (wr.bottom - wr.top);
                        if (area > 0 && vis * 5 < area * 2) {   // 可视 < 40%
                            int nx = (int)wr.left, ny = (int)wr.top;
                            const RECT& rc = mi.rcWork;
                            if (wr.right < rc.left)   nx = (int)rc.left;
                            if (wr.bottom < rc.top)   ny = (int)rc.top;
                            if (wr.left > rc.right)   nx = (int)rc.right - (int)(wr.right - wr.left);
                            if (wr.top > rc.bottom)   ny = (int)rc.bottom - (int)(wr.bottom - wr.top);
                            nx = (std::max)((int)rc.left, (std::min)(nx, (int)rc.right - 160));
                            ny = (std::max)((int)rc.top, (std::min)(ny, (int)rc.bottom - 120));
                            SetWindowPos(hwnd_, nullptr, nx, ny, 0, 0,
                                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                            Logger::Info("DisplayChange: window off-screen, "
                                         "moved to nearest monitor work area");
                        }
                    }
                }
            }
            break;

        case WM_COPYDATA: {
            // 单实例转发：第二个进程把完整命令行送来，在本进程内开新标签
            auto* cds = (COPYDATASTRUCT*)lp;
            if (!cds || cds->dwData != 0x58465750 /* 'XFWP' */ ||
                !cds->lpData || cds->cbData < sizeof(wchar_t) ||
                cds->cbData % sizeof(wchar_t) != 0)
                break;
            auto* str = (const wchar_t*)cds->lpData;
            size_t n = cds->cbData / sizeof(wchar_t);
            for (size_t i = 0; i < n; ++i)          // 强制 NUL 结尾防越界
                if (str[i] == L'\0') { n = i; break; }
            StartupOptions fwd = ParseCommandLine(std::wstring(str, n).c_str());
            Logger::Info("Single-instance forward: " +
                         std::to_string(fwd.files.size()) + " file(s)");
            if (!fwd.files.empty()) OpenCliFiles(fwd);
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            else ShowWindow(hwnd_, SW_SHOW);
            BringWindowToTop(hwnd_);
            SetForegroundWindow(hwnd_);
            return TRUE;
        }

        case WM_SETFOCUS:
            if (Document* d = workspace_->Active())
                SetFocus(d->editor.Hwnd());
            return 0;

        case WM_COMMAND: {
            unsigned int id = LOWORD(wp);
            if (id == 6001 || id == 6002) { // Ctrl+Tab cycling (active view)
                int dir = (id == 6001) ? 1 : -1;
                if (workspace_->CurrentView() == 1) {
                    int n = workspace_->Count1();
                    if (n > 1) {
                        int cur = workspace_->Tabs1().Current();
                        workspace_->ActivateView1(((cur + dir) % n + n) % n);
                    }
                } else {
                    int n = workspace_->Count();
                    if (n > 1) {
                        int cur = workspace_->Tabs().Current();
                        workspace_->Activate(((cur + dir) % n + n) % n);
                    }
                }
                return 0;
            }
            ExecuteCommand(id);
            return 0;
        }

        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;

            // toolbar tooltip text (our own tooltip control over the strip)
            if (nm->code == TTN_GETDISPINFOW &&
                nm->idFrom == (UINT_PTR)toolbar_) {
                NMTTDISPINFOW* di = (NMTTDISPINFOW*)lp;
                int hot = (int)SendMessageW(toolbar_, TB_GETHOTITEM, 0, 0);
                Logger::Info("Toolbar GETDISPINFO hot=" + std::to_string(hot));
                wchar_t tip[128] = L"";
                if (hot >= 0) {
                    TBBUTTON tb{};
                    if (SendMessageW(toolbar_, TB_GETBUTTON, hot, (LPARAM)&tb)) {
                        unsigned cmd = tb.idCommand;
                        lstrcpynW(tip, CmdLabel(cmd), 64);
                        lstrcatW(tip, CmdShortcut(cmd));
                    }
                }
                if (tip[0]) {
                    static wchar_t tipBuf[128];
                    lstrcpynW(tipBuf, tip, 128);
                    di->lpszText = tipBuf;
                } else {
                    di->szText[0] = L'\0';
                    di->lpszText = di->szText;
                }
                SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, 0);
                return TRUE;
            }

            // toolbar tooltip text (one RECT tool per button)
            if (toolbarTip_ && nm->hwndFrom == toolbarTip_ &&
                nm->code == TTN_GETDISPINFOW) {
                NMTTDISPINFOW* di = (NMTTDISPINFOW*)lp;
                int btn = (int)nm->idFrom - 1;
                di->szText[0] = L'\0';
                di->lpszText = di->szText;
                TBBUTTON tb{};
                if (btn >= 0 &&
                    SendMessageW(toolbar_, TB_GETBUTTON, btn, (LPARAM)&tb)) {
                    wchar_t tip[128];
                    lstrcpynW(tip, CmdLabel(tb.idCommand), 64);
                    lstrcatW(tip, CmdShortcut(tb.idCommand));
                    static wchar_t tipBuf[128];
                    lstrcpynW(tipBuf, tip, 128);
                    di->lpszText = tipBuf;
                    Logger::Info("Toolbar tip shown: " + WideToUtf8(tip));
                }
                SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, 0);
                return TRUE;
            }

            // ---- tab bar custom drawing removed: native rendering shows labels + × ----

            if (nm->idFrom == Cmd::TabBarId) {
                if (nm->code == TCN_SELCHANGE) {
                    int i = (int)SendMessageW(nm->hwndFrom, TCM_GETCURSEL, 0, 0);
                    if (workspace_->Tabs().Current() != i)
                        workspace_->Activate(i);
                } else if (nm->code == TCN_SELCHANGING) {
                    return FALSE;
                }
                return 0;
            }
            if (nm->idFrom == Cmd::TabBarId2) {
                if (nm->code == TCN_SELCHANGE) {
                    int i = (int)SendMessageW(nm->hwndFrom, TCM_GETCURSEL, 0, 0);
                    if (workspace_->Tabs1().Current() != i)
                        workspace_->ActivateView1(i);
                } else if (nm->code == TCN_SELCHANGING) {
                    return FALSE;
                }
                return 0;
            }
            SCNotification* sn = (SCNotification*)lp;
            if (Document* d = workspace_->FindByEditor(nm->hwndFrom)) {
                Editor& ed = d->editor;
                switch (sn->nmhdr.code) {
                    case SCN_CHARADDED:
                        ed.HandleCharAdded(sn);
                        break;
                    case SCN_AUTOCCOMPLETED:
                        // 批次 78：补全项**已经**写进文档之后才发的通知
                        // （ScintillaBase::AutoCompleteCompleted 末尾）。
                        // 语句名补全靠它做续动作：补 `(` + 出签名提示。
                        ed.HandleAutocCompleted(sn);
                        break;
                    case SCN_DOUBLECLICK:
                        ed.HighlightOccurrences();
                        break;
                    case SCN_UPDATEUI: {
                        // focus in the other split view? track it for title/status
                        if (workspace_->CurrentView() != d->view) {
                            workspace_->SetActiveDoc(d);
                            UpdateTitleBar();
                            // refresh the Language menu / status bar for the
                            // newly-focused document in the other view
                            OnWorkspaceChanged();
                        }
                        // 双击词高亮：点击别处 → 选区塌缩 → 清除（编辑路径已在
                        // SCN_MODIFIED 清，这里覆盖纯光标移动/单击/方向键场景）
                        if (ed.HasOccurrenceHighlight()) {
                            const sptr_t selStart = ed.Send(SCI_GETSELECTIONSTART);
                            const sptr_t selEnd = ed.Send(SCI_GETSELECTIONEND);
                            if (selStart == selEnd) ed.ClearOccurrenceHighlight();
                        }
                        UpdateStatusBar();
                        // plugin host: caret/scroll activity → cursor event
                        pluginEvtPending_ |= XFS_EVT_CURSOR_MOVED;
                        ::SetTimer(hwnd_, kPluginEvtTimerId, 250, nullptr);
                        // side-by-side diff: keep the other pane scroll-aligned
                        if (workspace_->Diff().Active())
                            workspace_->Diff().SyncScroll(nm->hwndFrom);
                        // brace matching via INDICATOR_CONTAINER (style-independent)
                        if (!settings_.braceMatch) {
                            ed.Send(SCI_SETINDICATORCURRENT, 8);
                            ed.Send(SCI_INDICATORCLEARRANGE, 0,
                                    ed.Send(SCI_GETLENGTH));
                        } else {
                            sptr_t pos = ed.Send(SCI_GETCURRENTPOS);
                            int c = (int)ed.Send(SCI_GETCHARAT, pos, 0);
                            if (!strchr("()[]{}", c)) {
                                --pos;
                                c = (int)ed.Send(SCI_GETCHARAT, pos, 0);
                            }
                            if (strchr("()[]{}", c)) {
                                sptr_t match = ed.Send(SCI_BRACEMATCH, pos, 0);
                                if (match >= 0) {
                                    // highlight using indicator 8
                                    ed.Send(SCI_SETINDICATORCURRENT, 8);
                                    ed.Send(SCI_INDICATORCLEARRANGE, 0,
                                            ed.Send(SCI_GETLENGTH));
                                    ed.Send(SCI_INDICATORFILLRANGE,
                                            (pos < match) ? pos : match, 1);
                                    ed.Send(SCI_INDICATORFILLRANGE,
                                            (pos < match) ? match : pos, 1);
                                }
                            } else {
                                ed.Send(SCI_SETINDICATORCURRENT, 8);
                                ed.Send(SCI_INDICATORCLEARRANGE, 0,
                                        ed.Send(SCI_GETLENGTH));
                            }
                        }
                        break;
                    }
                    case SCN_MODIFIED:
                        // real buffer change (SC_MOD_INSERTTEXT/DELETETEXT): distinguishable from
                        // indicator/style/marker changes which also fire SCN_MODIFIED.
                        // Only clear word highlight on actual text edits, not on indicator fills
                        // (which synchronously trigger SCN_MODIFIED and would erase the highlight
                        // we just applied).
                        if (sn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
                            ed.ClearOccurrenceHighlight();   // 编辑后词高亮位置失效
                            pluginEvtPending_ |= XFS_EVT_TEXT_MODIFIED;
                            ::SetTimer(hwnd_, kPluginEvtTimerId, 250, nullptr);
                            UpdateUndoRedoState();
                        } else if (sn->modificationType & (SC_MOD_CONTAINER |
                                    SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) {
                            // undo/redo 以 container actions 回放文本时也走这里
                            UpdateUndoRedoState();
                        }
                        break;
                    case SCN_SAVEPOINTLEFT:
                    case SCN_SAVEPOINTREACHED:
                        workspace_->SyncTabTitles();
                        UpdateTitleBar();
                        UpdateUndoRedoState();
                        break;
                    case SCN_MARGINCLICK:
                        if (sn->margin == 2 /* fold margin */) {
                            int line = (int)ed.Send(SCI_LINEFROMPOSITION, sn->position);
                            ed.Send(SCI_TOGGLEFOLD, line);
                        }
                        break;
                }
            }
            return 0;
        }

        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, path, MAX_PATH * 4))
                    OpenUserFile(path);
            }
            DragFinish(drop);
            return 0;
        }

        case WM_APP_SEARCHACT:
            HandleSearchAction((SearchAction)(int)lp);
            return 0;

        case WM_APP_RESTOREJUMP:
            ApplyRestoreJumps();
            return 0;

        case WM_APP_GOTOHIT:
            OnResultActivate((int)wp);
            return 0;

        case WM_APP_GIT_DONE:
            OnGitDone((GitSnapshot*)lp);
            return 0;

        case WM_APP_GIT_BLOB:
            OnGitBlob((GitBlobResult*)lp);
            return 0;

        case WM_APP_GIT_OP:
            OnGitOp((GitOpResult*)lp);
            return 0;

        case WM_TIMER: {
            if (wp == 1) DoAutoSave();
            else if (wp == kAiReloadTimer) {
                ::KillTimer(hwnd_, kAiReloadTimer);
                AutoReloadChangedNow();
            }
            else if (wp == kPluginEvtTimerId && pluginEvtPending_) {
                ::KillTimer(hwnd_, kPluginEvtTimerId);
                const uint32_t bits = pluginEvtPending_;
                pluginEvtPending_ = 0;
                if (plugins_ && (bits & XFS_EVT_TEXT_MODIFIED))
                    plugins_->Raise(XFS_EVT_TEXT_MODIFIED, nullptr);
                if (plugins_ && (bits & XFS_EVT_CURSOR_MOVED)) {
                    char pos[32] = "";
                    Document* d = workspace_ ? workspace_->Active() : nullptr;
                    if (d) {
                        EditorStatus st = d->editor.Status();
                        snprintf(pos, sizeof(pos), "%d,%d", st.line, st.column);
                    }
                    plugins_->Raise(XFS_EVT_CURSOR_MOVED, pos);
                }
            }
            return 0;
        }

        case WM_CLOSE: {
            // 4c: 宿主即将退出 → NPPN_SHUTDOWN（先于文档关闭，插件可做清理）
            if (plugins_) plugins_->EmitNppNotification(npp::NPPN_SHUTDOWN, 0);

            // save session before closing
            SessionState ss;
            constexpr size_t kUntitledSnapCap = 2 * 1024 * 1024;  // 2MB text cap
            auto collect = [&](int view) {
                int n = view == 0 ? workspace_->Count() : workspace_->Count1();
                std::vector<SessionEntry>* dst =
                    view == 0 ? &ss.entries : &ss.entries1;
                for (int i = 0; i < n; ++i) {
                    Document* doc = view == 0 ? workspace_->DocumentAt(i)
                                              : workspace_->FindByTabIndex1(i);
                    if (!doc) continue;
                    SessionEntry e;
                    EditorStatus st = doc->editor.Status();
                    e.line = st.line;
                    e.col = st.column;
                    e.locked = doc->locked;
                    e.lang = doc->langIndex;
                    if (doc->HasPath()) {
                        e.path = doc->path.wstring();
                    } else {
                        // untitled tab: snapshot label + text so it survives
                        e.name = doc->DisplayName();
                        auto utf8 = doc->editor.GetTextUtf8();
                        if (utf8.size() <= kUntitledSnapCap)
                            e.text = Utf8ToWide(utf8);
                        // over-cap scratch tabs degrade to pathless skip
                        else
                            continue;
                    }
                    dst->push_back(std::move(e));
                }
            };
            collect(0);
            collect(1);
            ss.activeIndex = workspace_->ActiveIndex();
            ss.activeIndex1 = workspace_->ActiveIndex1();
            ss.activeView = workspace_->CurrentView();
            // 批次 67：primary 写 legacy session.json，额外窗口各写自己的
            // session-<pid>.json 槽位，多进程并发退出互不覆盖。
            SessionSave(SessionSlotPath(startup_.firstInstance), ss);

            if (workspace_->CloseAll(/*keepOneDoc=*/false)) {
                // delete autosave files (clean exit)
                auto asDir = AutoSaveDir();
                WIN32_FIND_DATAW fd{};
                HANDLE hFind = ::FindFirstFileW((asDir + L"\\*.asb").c_str(), &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do { ::DeleteFileW((asDir + L"\\" + fd.cFileName).c_str()); }
                    while (::FindNextFileW(hFind, &fd));
                    ::FindClose(hFind);
                }

                // persist window geometry + editor prefs.
                // 最大化时保存还原矩形（GetWindowPlacement）：GetWindowRect 在
                // 最大化时会取当前显示器的全工作区坐标，拔掉扩展屏后这些
                // 坐标可能落在虚无区域，恢复时窗口就开到屏幕边上（用户
                // 报告：扩展屏上退出 → 拔屏 → 重开窗口只露一条边）。
                WINDOWPLACEMENT wp{ sizeof(WINDOWPLACEMENT) };
                if (GetWindowPlacement(hwnd_, &wp)) {
                    RECT nr = wp.rcNormalPosition;
                    // rcNormalPosition 是工作区坐标，转回屏幕坐标（MonInfo 里的
                    // rcWork 已是屏幕坐标，直接偏移）
                    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{ sizeof(MONITORINFO) };
                    if (mon && GetMonitorInfoW(mon, &mi)) {
                        ::OffsetRect(&nr, mi.rcWork.left, mi.rcWork.top);
                    }
                    settings_.hasWindow = true;
                    settings_.winX = nr.left;  settings_.winY = nr.top;
                    settings_.winW = nr.right - nr.left;
                    settings_.winH = nr.bottom - nr.top;
                }
                settings_.winMax = IsZoomed(hwnd_) != FALSE;
                // persist current dock panel heights
                settings_.hexPanelH = hexHLogical_;
                settings_.resultsPanelH = rpHLogical_;
                settings_.logPanelH = logHLogical_;
                settings_.terminalPanelH = termHLogical_;
                settings_.aiPanelW = aiWLogical_;
                settings_.aiPanelVisible = ai_ && ai_->Visible();
                SettingsSave(SettingsFilePath(), settings_);
                GlobalShortcuts().Save(ShortcutTable::FilePath());
                DestroyWindow(hwnd_);
            }
            return 0;
        }

        case WM_DESTROY:
            Logger::Info("Shutdown: destroy terminal");
            if (terminal_) terminal_->Destroy();
            // 4d: 先销毁插件 dock 面板（DMM_CLOSE 通知各对话框），再卸载 DLL
            Logger::Info("Shutdown: destroy dock panels");
            if (dockMgr_) dockMgr_->Destroy();
            Logger::Info("Shutdown: unload plugins");
            if (plugins_) plugins_->UnloadAll(/*freeDlls=*/false);  // 进程随即退出，避免卸载仍有线程的重插件
            Logger::Info("Shutdown: post quit");
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace xfs
