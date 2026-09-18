// test_npp_msgs.cpp - 4b: NPP 消息垫片契约单测。用可配置假文档集验证
// ForwardNppMessage 的参数解码、两段式字符串协议、buffer-id 编码与
// 动态命令 id 池。完全绕开真实 Workspace/Scintilla 控件。
#include "../src/plugin/PluginManager.h"
#include "../src/plugin/npp/NppMessages.h"
#include "../src/document/Document.h"
#include "../src/core/CommandIds.h"
#include "../src/core/Log.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// 可配置假文档集：生产路径由 PluginManager 内部 WorkspaceSink 提供，
// 这里注入假实现以脱离真实控件断言契约（插件系统设计笔记 §5.6）。
struct FakeDocs final : public NppDocSource {
    std::vector<std::unique_ptr<Document>> docs;
    int active = 0;

    int openNewCalls = 0;
    bool openNewOk = true;
    std::wstring lastOpen;
    int switchCalls = 0;
    bool switchOk = true;
    std::wstring lastSwitch;
    int saveCurrentCalls = 0;
    int saveAllCalls = 0;
    bool saveAllOk = true;
    bool saveAllAny = true;

    int DocCount() override { return (int)docs.size(); }
    Document* DocAt(int i) override {
        return (i >= 0 && i < (int)docs.size()) ? docs[(size_t)i].get() : nullptr;
    }
    Document* Current() override {
        return (active >= 0 && active < (int)docs.size()) ? docs[(size_t)active].get() : nullptr;
    }
    int CurrentIndex() override { return active; }
    bool OpenNew(const std::wstring& p) override { ++openNewCalls; lastOpen = p; return openNewOk; }
    bool SwitchTo(const std::wstring& p) override { ++switchCalls; lastSwitch = p; return switchOk; }
    bool SaveCurrent() override { ++saveCurrentCalls; return true; }
    bool SaveAllDocs(bool& anySaved) override { ++saveAllCalls; anySaved = saveAllAny; return saveAllOk; }

    static FakeDocs* MakeTwo() {
        auto* f = new FakeDocs();
        f->docs.emplace_back(new Document());
        f->docs[0]->path = L"D:\\proj\\src\\main.cpp";
        f->docs.emplace_back(new Document());
        f->docs[1]->path = L"D:\\proj\\data\\readme.md";
        f->active = 1;
        return f;
    }
};

// 假可停靠对话框宿主（4d）：记录 NPPM_DMM* 转发契约的每次调用与参数。
struct FakeDockHost final : public DockHost {
    int dockCalls = 0, showCalls = 0, hideCalls = 0, updateCalls = 0;
    int showByNameCalls = 0, findCalls = 0;
    npp::DockedWidgetData lastDock{};
    HWND lastShow = nullptr, lastHide = nullptr, lastUpdate = nullptr;
    std::wstring lastName;
    std::wstring lastFindWin, lastFindMod;
    bool lastFindWinSet = false, lastFindModSet = false;
    bool dockOk = true, showOk = true, hideOk = true, showByNameOk = true;
    HWND findResult = nullptr;

    bool DockWidget(const npp::DockedWidgetData& data) override {
        ++dockCalls; lastDock = data; return dockOk;
    }
    bool Show(HWND hDlg) override { ++showCalls; lastShow = hDlg; return showOk; }
    bool Hide(HWND hDlg) override { ++hideCalls; lastHide = hDlg; return hideOk; }
    void UpdateDisplayInfo(HWND hDlg) override { ++updateCalls; lastUpdate = hDlg; }
    bool ShowByName(const wchar_t* name) override {
        ++showByNameCalls; lastName = name ? name : L""; return showByNameOk;
    }
    HWND FindHwndByName(const wchar_t* windowName, const wchar_t* moduleName) override {
        ++findCalls;
        lastFindWinSet = windowName != nullptr;
        lastFindModSet = moduleName != nullptr;
        if (windowName) lastFindWin = windowName;
        if (moduleName) lastFindMod = moduleName;
        return findResult;
    }
};

// RUNCOMMAND/GETPLUGINSCONFIGDIR 两段式：首调查询长度（lp==NULL 返回 wchar_t
// 数，不含 NUL），二调按"长度+1"传入缓冲，成功 TRUE 失败 FALSE。
static bool QueryAndFetch(PluginManager& mgr, UINT msg, std::wstring& out,
                          WPARAM extra = 0) {
    bool h = false;
    LRESULT len = mgr.ForwardNppMessage(msg, extra, 0, h);   // 1st: query
    if (!h) return false;
    std::wstring buf((size_t)len + 1, L'\0');
    LRESULT ok = mgr.ForwardNppMessage(msg, (WPARAM)(len + 1), (LPARAM)buf.data(), h);
    if (!h) return false;
    out.assign(buf.c_str());
    return ok == TRUE;
}

int main() {
    namespace nn = xfs::npp;

    setvbuf(stdout, nullptr, _IONBF, 0);   // 管道下实时可见输出
    PluginManager mgr;

    // ---- 无文档集降级（既无注入源也无 workspace） ---------------------------
    {
        bool h = true;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETNBOPENFILES, 0, nn::kAllOpenFiles, h) == 0 && h);
        h = true;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETCURRENTBUFFERID, 0, 0, h) == 0 && h);
        h = true;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DOOPEN, 0, (LPARAM)L"C:\\x.txt", h) == FALSE && h);
        h = true;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SAVEALLFILES, 0, 0, h) == FALSE && h);
    }

    // ---- 区间外消息：handled=false，绝不拦截默认流程 ------------------------
    {
        bool h = true;
        mgr.ForwardNppMessage(nn::kRunCmdBase - 1, 0, 0, h);   // 两基值之间的空洞
        CHECK(h == false);
        h = true;
        mgr.ForwardNppMessage(WM_USER + 5000, 0, 0, h);
        CHECK(h == false);
        h = true;
        mgr.ForwardNppMessage(WM_COMMAND, 0, 0, h);        // 标准消息绝不拦截
        CHECK(h == false);
    }

    FakeDocs* fake = FakeDocs::MakeTwo();
    mgr.AttachNppDocSource(fake);

    // ---- 视图/文档计数 -------------------------------------------------------
    {
        bool h = false;
        int view = -1;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&view, h) == TRUE && h);
        CHECK(view == 0);   // 单视图模型 ⇒ Main View
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETNBOPENFILES, 0, nn::kAllOpenFiles, h) == 2 && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETNBOPENFILES, 0, nn::kPrimaryView, h) == 2 && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETNBOPENFILES, 0, nn::kSecondView, h) == 0 && h);
    }

    // ---- GETOPENFILENAMES 族 -------------------------------------------------
    {
        // 宿主契约：arr 是"每项 MAX_PATH 缓冲"的指针数组，测试必须提供真缓冲
        bool h = false;
        wchar_t buf0[260], buf1[260], buf2[260], buf3[260];
        wchar_t* arr[4] = { buf0, buf1, buf2, buf3 };
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETOPENFILENAMES_DEPRECATED,
                                    (WPARAM)arr, 4, h) == 2 && h);
        CHECK(arr[0] && arr[1]);
        CHECK(std::wstring(arr[0]) == L"D:\\proj\\src\\main.cpp");
        CHECK(std::wstring(arr[1]) == L"D:\\proj\\data\\readme.md");
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETOPENFILENAMESSECOND_DEPRECATED,
                                    (WPARAM)arr, 4, h) == 0 && h);   // 副视图恒空
    }

    // ---- buffer-id 族 ----------------------------------------------------------
    UINT_PTR curId = 0;
    {
        bool h = false;
        curId = (UINT_PTR)mgr.ForwardNppMessage(nn::NPPM_GETCURRENTBUFFERID, 0, 0, h);
        CHECK(h && curId == (UINT_PTR)fake->Current());
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETBUFFERIDFROMPOS, 1, nn::kMainViewPos, h) ==
              (LRESULT)fake->docs[1].get() && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETBUFFERIDFROMPOS, 1, nn::kSubViewPos, h) == 0 && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETPOSFROMBUFFERID, curId, 0, h) == 1 && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETPOSFROMBUFFERID, (UINT_PTR)fake->docs[0].get(), 0, h) == 0 && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETPOSFROMBUFFERID, (UINT_PTR)0xDEAD, 0, h) == -1 && h);
    }

    // ---- GETFULLPATHFROMBUFFERID：两段式 + 无效 id 返回 -1 ----------------------
    {
        bool h = false;
        LRESULT len = mgr.ForwardNppMessage(nn::NPPM_GETFULLPATHFROMBUFFERID, curId, 0, h);
        CHECK(h && len == 22);   // wcslen(L"D:\proj\data\readme.md")
        std::wstring buf((size_t)len + 1, L'\0');
        LRESULT ret = mgr.ForwardNppMessage(nn::NPPM_GETFULLPATHFROMBUFFERID,
                                            curId, (LPARAM)buf.data(), h);
        CHECK(h && ret == len && std::wstring(buf.c_str()) == L"D:\\proj\\data\\readme.md");
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETFULLPATHFROMBUFFERID, (UINT_PTR)0xDEAD, 0, h) == -1 && h);
    }

    // ---- RUNCOMMAND 字符串族两段式 ---------------------------------------------
    {
        std::wstring out;
        CHECK(QueryAndFetch(mgr, nn::NPPM_GETFULLCURRENTPATH, out));
        CHECK(out == L"D:\\proj\\data\\readme.md");
        CHECK(QueryAndFetch(mgr, nn::NPPM_GETCURRENTDIRECTORY, out));
        CHECK(out == L"D:\\proj\\data");
        CHECK(QueryAndFetch(mgr, nn::NPPM_GETFILENAME, out));
        CHECK(out == L"readme.md");
        CHECK(QueryAndFetch(mgr, nn::NPPM_GETNAMEPART, out));
        CHECK(out == L"readme");
        CHECK(QueryAndFetch(mgr, nn::NPPM_GETEXTPART, out));
        CHECK(out == L"md");   // 上游语义不含点
        // 缓冲不足 → FALSE
        bool h = false;
        wchar_t small[3] = { L'x', L'x', L'x' };
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETFULLCURRENTPATH, 3, (LPARAM)small, h) == FALSE && h);
    }

    // ---- SWITCHTOFILE / DOOPEN / SAVE 族 ------------------------------------------
    {
        bool h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SWITCHTOFILE, 0, (LPARAM)L"D:\\proj\\src\\main.cpp", h) == TRUE && h);
        CHECK(fake->switchCalls == 1 && fake->lastSwitch == L"D:\\proj\\src\\main.cpp");
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DOOPEN, 0, (LPARAM)L"D:\\other\\t.txt", h) == TRUE && h);
        CHECK(fake->openNewCalls == 1 && fake->lastOpen == L"D:\\other\\t.txt");
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SAVECURRENTFILE, 0, 0, h) == TRUE && h);
        CHECK(fake->saveCurrentCalls == 1);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SAVEALLFILES, 0, 0, h) == TRUE && h);
        CHECK(fake->saveAllCalls == 1);
        fake->saveAllAny = false;          // 无保存动作 → 契约返回 FALSE
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SAVEALLFILES, 0, 0, h) == FALSE && h);
        fake->saveAllAny = true;
        fake->switchOk = false;            // 未命中 → FALSE
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SWITCHTOFILE, 0, (LPARAM)L"no-such.txt", h) == FALSE && h);
        fake->switchOk = true;
        fake->openNewOk = false;           // 打开失败 → FALSE
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DOOPEN, 0, (LPARAM)L"x.txt", h) == FALSE && h);
        fake->openNewOk = true;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DOOPEN, 0, 0, h) == FALSE && h);   // 空路径
    }

    // ---- ALLOCATECMDID：动态池自 10501 顺序分配 -------------------------------------
    {
        bool h = false;
        int id = -1;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_ALLOCATECMDID, 4, (LPARAM)&id, h) == TRUE && h);
        CHECK(id == 10501);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_ALLOCATECMDID, 1, (LPARAM)&id, h) == TRUE && h);
        CHECK(id == 10505);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_ALLOCATECMDID, 9999, (LPARAM)&id, h) == FALSE && h);  // 超池
    }

    // ---- 菜单/命令句柄族：GETMENUHANDLE / GETMENUBAR / SETMENUITEMCHECK / GETSHORTCUTBYCMDID ----
    {
        // 未注入句柄：明确拒答（0）
        bool h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETMENUHANDLE, nn::NppPluginMenu, 0, h) == 0 && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETMENUBAR, 0, 0, h) == 0 && h);

        HMENU pluginMenu = ::CreatePopupMenu();
        HMENU mainMenu = ::CreateMenu();
        mgr.SetPluginMenus(pluginMenu, mainMenu);

        // GETMENUHANDLE：按 wParam 返回对应句柄；未知取值拒答
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETMENUHANDLE, nn::NppPluginMenu, 0, h) == (LRESULT)pluginMenu && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETMENUHANDLE, nn::NppMainMenu, 0, h) == (LRESULT)mainMenu && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETMENUBAR, 0, 0, h) == (LRESULT)mainMenu && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETMENUHANDLE, 7, 0, h) == 0 && h);

        // 注册一条插件命令（经公开 C-ABI 口），id 落在插件区间
        xfs_plugin_command* cmd = mgr.HostAddCommand("Toggle Console", "NppExec",
                                                     [](void*) {}, nullptr);
        CHECK(cmd != nullptr);
        const unsigned cmdId = mgr.CommandIdOfHandle(cmd);
        CHECK(cmdId >= PluginCmdFirst);
        // 把命令项真实挂进菜单，CheckMenuItem 才有命中对象（生产端 RebuildPluginMenu 等价）
        ::AppendMenuW(pluginMenu, MF_STRING, cmdId, L"Toggle Console");
        ::AppendMenuW(mainMenu, MF_STRING, Cmd::FileSave, L"Save");

        // SETMENUITEMCHECK：插件命令 → 插件菜单；内建命令 → 主菜单栏
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SETMENUITEMCHECK, cmdId, TRUE, h) == TRUE && h);
        CHECK(::GetMenuState(pluginMenu, cmdId, MF_BYCOMMAND) == MF_CHECKED);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SETMENUITEMCHECK, cmdId, 0, h) == TRUE && h);
        CHECK(::GetMenuState(pluginMenu, cmdId, MF_BYCOMMAND) == MF_UNCHECKED);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_SETMENUITEMCHECK, (UINT_PTR)Cmd::FileSave, TRUE, h) == TRUE && h);
        CHECK(::GetMenuState(mainMenu, Cmd::FileSave, MF_BYCOMMAND) == MF_CHECKED);

        // GETSHORTCUTBYCMDID：无快捷键 / 未知命令 / 空指针 → FALSE
        npp::ShortcutKey sk{};
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETSHORTCUTBYCMDID, cmdId, (LPARAM)&sk, h) == FALSE && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETSHORTCUTBYCMDID, (UINT_PTR)0xABCD, (LPARAM)&sk, h) == FALSE && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_GETSHORTCUTBYCMDID, cmdId, 0, h) == FALSE && h);

        mgr.SetPluginMenus(nullptr, nullptr);
        ::DestroyMenu(pluginMenu);
        ::DestroyMenu(mainMenu);
    }

    // ---- 4d: NPPM_DMM* 可停靠对话框族（DockHost 转发契约）-------------------
    {
        // 常量与结构布局契约（与官方 Docking.h / Notepad_plus_msgs.h 一致）
        CHECK(nn::kDmnFirst == 1050);
        CHECK(nn::DMN_CLOSE == 1051 && nn::DMN_DOCK == 1052 && nn::DMN_FLOAT == 1053);
        CHECK(nn::DMN_SWITCHIN == 1054 && nn::DMN_SWITCHOFF == 1055 && nn::DMN_FLOATDROPPED == 1056);
        CHECK(nn::kDmmMsg == 0x5000);
        CHECK(nn::DMM_CLOSE == 0x5001 && nn::DMM_DOCK == 0x5002 && nn::DMM_UPDATEDISPINFO == 0x5007);
        CHECK(nn::kDwsIconTab == 1u && nn::kDwsIconBar == 2u && nn::kDwsAddInfo == 4u);
        CHECK(nn::kDwsDfFloating == 0x80000000u);
        CHECK((nn::kDwsDfContBottom >> 28) == 3u);
        // DockedWidgetData 字段顺序/宽度是二进制契约（tTbData）
        CHECK(offsetof(npp::DockedWidgetData, hClient) == 0);
        CHECK(offsetof(npp::DockedWidgetData, pszName) == sizeof(void*));
        CHECK(offsetof(npp::DockedWidgetData, dlgID) == 2 * sizeof(void*));
        CHECK(offsetof(npp::DockedWidgetData, uMask) == 2 * sizeof(void*) + sizeof(int));
        CHECK(offsetof(npp::DockedWidgetData, rcFloat) ==
              4 * sizeof(void*) + 2 * sizeof(int));
        CHECK(offsetof(npp::DockedWidgetData, pszModuleName) ==
              5 * sizeof(void*) + 2 * sizeof(int) + sizeof(RECT));

        // 无宿主：NPPM_DMM* 明确拒答（handled=true 但返回 FALSE/0）
        bool h = false;
        npp::DockedWidgetData wd{};
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMREGASDCKDLG, 0, (LPARAM)&wd, h) == FALSE && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMSHOW, 0, (LPARAM)(HWND)1, h) == FALSE && h);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMGETPLUGINHWNDBYNAME,
                                    (WPARAM)L"W", (LPARAM)L"M", h) == 0 && h);

        FakeDockHost fake;
        mgr.SetDockHost(&fake);

        // 注册：结构整体转发（指针字段透传；宿主自行深拷贝）
        h = false;
        npp::DockedWidgetData reg{};
        HWND fakeDlg = (HWND)0x12345;
        reg.hClient = fakeDlg;
        reg.pszName = L"Probe Panel";
        reg.dlgID = 42;
        reg.uMask = nn::kDwsDfContBottom | nn::kDwsAddInfo;
        reg.pszModuleName = L"probe.dll";
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMREGASDCKDLG, 0, (LPARAM)&reg, h) == TRUE && h);
        CHECK(fake.dockCalls == 1);
        CHECK(fake.lastDock.hClient == fakeDlg);
        CHECK(fake.lastDock.dlgID == 42);
        CHECK(fake.lastDock.uMask == (nn::kDwsDfContBottom | nn::kDwsAddInfo));
        CHECK(std::wstring(fake.lastDock.pszName) == L"Probe Panel");
        CHECK(std::wstring(fake.lastDock.pszModuleName) == L"probe.dll");
        fake.dockOk = false;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMREGASDCKDLG, 0, (LPARAM)&reg, h) == FALSE && h);
        fake.dockOk = true;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMREGASDCKDLG, 0, 0, h) == FALSE && h);   // 空指针

        // 显示/隐藏/刷新：hwnd 直通
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMSHOW, 0, (LPARAM)fakeDlg, h) == TRUE && h);
        CHECK(fake.showCalls == 1 && fake.lastShow == fakeDlg);
        fake.showOk = false;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMSHOW, 0, (LPARAM)fakeDlg, h) == FALSE && h);
        fake.showOk = true;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMHIDE, 0, (LPARAM)fakeDlg, h) == TRUE && h);
        CHECK(fake.hideCalls == 1 && fake.lastHide == fakeDlg);
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMUPDATEDISPINFO, 0, (LPARAM)fakeDlg, h) == TRUE && h);
        CHECK(fake.updateCalls == 1 && fake.lastUpdate == fakeDlg);

        // 按名称切换：宽字符串指针透传
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMVIEWOTHERTAB, 0, (LPARAM)L"Probe Panel", h) == TRUE && h);
        CHECK(fake.showByNameCalls == 1 && fake.lastName == L"Probe Panel");
        fake.showByNameOk = false;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMVIEWOTHERTAB, 0, (LPARAM)L"X", h) == FALSE && h);
        fake.showByNameOk = true;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMVIEWOTHERTAB, 0, 0, h) == FALSE && h);

        // 按名称/模块查句柄：wParam=窗口名、lParam=模块名；任一可空
        fake.findResult = fakeDlg;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMGETPLUGINHWNDBYNAME,
                                    (WPARAM)L"Probe Panel", (LPARAM)L"probe.dll", h) ==
              (LRESULT)fakeDlg && h);
        CHECK(fake.findCalls == 1);
        CHECK(fake.lastFindWinSet && fake.lastFindWin == L"Probe Panel");
        CHECK(fake.lastFindModSet && fake.lastFindMod == L"probe.dll");
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMGETPLUGINHWNDBYNAME,
                                    0, (LPARAM)L"probe.dll", h) == (LRESULT)fakeDlg && h);
        CHECK(fake.findCalls == 2 && !fake.lastFindWinSet && fake.lastFindModSet);
        fake.findResult = nullptr;
        h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_DMMGETPLUGINHWNDBYNAME,
                                    (WPARAM)L"W", (LPARAM)L"M", h) == 0 && h);

        mgr.SetDockHost(nullptr);
    }

    // ---- 区间内未实现编号：明确拒答（0）但 handled=true ------------------------------
    printf("[10] unimplemented\n");
    {
        bool h = false;
        CHECK(mgr.ForwardNppMessage(nn::kNppMsgBase + 100, 0, 0, h) == 0 && h);
    }

    // ---- RELOADBUFFERID：测试注入源无后端 → FALSE -----------------------------------
    {
        bool h = false;
        CHECK(mgr.ForwardNppMessage(nn::NPPM_RELOADBUFFERID, curId, 0, h) == FALSE && h);
    }

    mgr.AttachNppDocSource(nullptr);
    delete fake;

    if (g_fail == 0) { printf("ALL NPP MSG TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
