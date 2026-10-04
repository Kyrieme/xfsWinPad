// oop_nppm.cpp — 进程外桥 e2e 用插件：验证 NPPM_* 指针参数**过桥**（批次 111）。
//
// 与其它 oop_* 夹具同规则：不包含宿主头文件，手写公开契约布局。
//
// 与 oop_good 的关键差别：本插件把 NPPM_* 发给 g_data.npp，而 v2.2 起
// g_data.npp 是**代理进程里的中转窗**（不再是编辑器主窗口）。插件传的每个
// 指针都是本进程的合法地址 —— 这正是中转窗存在的意义；编辑器侧收到的是
// 封送后的载荷，它在自己的地址空间里重建缓冲，所以批次 110 那条"外来指针
// 判据"永远不会被触发。
//
// 观察出口：两种标记经 WM_COPYDATA 回传（中转窗转发给编辑器窗口）。
//   'NPVL' {tag, value}      —— 返回值/整数出参
//   'NPST' {tag, text[128]}  —— 字符串出参
#include <windows.h>

struct NPData { HWND npp; HWND sciMain; HWND sciSecond; };
struct SK { bool ctrl, alt, shift; unsigned char key; };
struct FI {
    wchar_t itemName[64];
    void (*func)(void);
    int cmdID;
    bool initCheck;
    SK* shortcut;
};
struct NotifyHdr { HWND hwndFrom; UINT_PTR idFrom; unsigned int code; };

// ---- 线上消息常量（与 src/plugin/npp/NppMessages.h 同源事实值）--------------
// NPPMSG = WM_USER + 1000；RUNCOMMAND_USER = WM_USER + 3000
#define NPPMSG  (WM_USER + 1000)
#define RUNCMD  (WM_USER + 3000)
#define NPPM_GETCURRENTSCINTILLA (NPPMSG + 4)
#define NPPM_GETNBOPENFILES      (NPPMSG + 7)
#define NPPM_GETNPPVERSION       (NPPMSG + 50)
#define NPPM_GETMENUHANDLE       (NPPMSG + 25)
#define NPPM_GETSHORTCUTBYCMDID  (NPPMSG + 76)
#define NPPM_SWITCHTOFILE        (NPPMSG + 37)
#define NPPM_ALLOCATECMDID       (NPPMSG + 81)
#define NPPM_GETFULLCURRENTPATH  (RUNCMD + 1)
#define NPPM_GETFILENAME         (RUNCMD + 3)

static NPData g_data{};
static FI items[1];
static SK skA = { true, true, false, 'K' };   // Ctrl+Alt+K（供 GETSHORTCUTBYCMDID 回读）

static constexpr UINT_PTR kValMagic = 0x4E50564C;   // 'NPVL'
static constexpr UINT_PTR kStrMagic = 0x4E505354;   // 'NPST'

static void SendMarker(UINT_PTR tag, INT_PTR value) {
    if (!g_data.npp) return;
    struct { UINT_PTR tag; INT_PTR value; } m{ tag, value };
    COPYDATASTRUCT cds{};
    cds.dwData = kValMagic;
    cds.cbData = sizeof(m);
    cds.lpData = &m;
    ::SendMessageW(g_data.npp, WM_COPYDATA, 0, (LPARAM)&cds);
}

static void SendStr(UINT_PTR tag, const wchar_t* text) {
    if (!g_data.npp) return;
    struct StrMarker { UINT_PTR tag; wchar_t text[128]; } m{};
    m.tag = tag;
    if (text) wcsncpy_s(m.text, text, _TRUNCATE);
    COPYDATASTRUCT cds{};
    cds.dwData = kStrMagic;
    cds.cbData = sizeof(m);
    cds.lpData = &m;
    ::SendMessageW(g_data.npp, WM_COPYDATA, 0, (LPARAM)&cds);
}

static LRESULT Npp(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_data.npp) return 0;
    return ::SendMessageW(g_data.npp, msg, wp, lp);
}

// 标记 tag 分配（测试侧按同一编号断言）：
//   20 两段式查询返回的长度      21 两段式写入的返回值     +NPST 20 = 取到的路径
//   22 GETCURRENTSCINTILLA 返回  23 它的 int 出参
//   24 GETNPPVERSION（值类型）
//   25 GETMENUHANDLE（明确不支持 ⇒ 0）
//   26 ALLOCATECMDID 返回        27 它的 int 出参是否 > 0
//   28 SWITCHTOFILE 返回（入参宽串过桥）
//   29 GETFILENAME 返回          +NPST 21 = 取到的文件名
//   30 GETSHORTCUTBYCMDID 返回   31 它的 key 字节
//   99 全部完成
static void probe(void) {
    // A. 两段式宽串：先问长度，再按"长度+1"取内容
    {
        const int need = (int)Npp(NPPM_GETFULLCURRENTPATH, 0, 0);
        SendMarker(20, need);
        wchar_t buf[512] = {};
        const LRESULT ok = Npp(NPPM_GETFULLCURRENTPATH,
                               (WPARAM)(need + 1), (LPARAM)buf);
        SendMarker(21, (INT_PTR)ok);
        SendStr(20, buf);
    }
    // B. int 出参
    {
        int view = -1;
        const LRESULT r = Npp(NPPM_GETCURRENTSCINTILLA, 0, (LPARAM)&view);
        SendMarker(22, (INT_PTR)r);
        SendMarker(23, view);
    }
    // C. 纯值类型（wp/lp 都不含指针）
    SendMarker(24, (INT_PTR)Npp(NPPM_GETNPPVERSION, 0, 0));
    // D. 明确不支持：HMENU 不可跨进程 ⇒ 中转窗直接拒答（返回 0）
    SendMarker(25, (INT_PTR)Npp(NPPM_GETMENUHANDLE, 0, 0));
    // E. int 出参 + 宿主侧有副作用（扣命令 id 池）
    {
        int start = -1;
        const LRESULT r = Npp(NPPM_ALLOCATECMDID, 1, (LPARAM)&start);
        SendMarker(26, (INT_PTR)r);
        SendMarker(27, start > 0 ? 1 : 0);
    }
    // F. 入参宽串：编辑器侧会记下它收到的路径（测试据此断言串原样过桥）
    SendMarker(28, (INT_PTR)Npp(NPPM_SWITCHTOFILE, 0,
                                (LPARAM)L"D:\\proj\\oop\\relay.txt"));
    // G. 另一条两段式（同一 family 的第二个成员）
    {
        wchar_t buf[128] = {};
        const LRESULT ok = Npp(NPPM_GETFILENAME, 128, (LPARAM)buf);
        SendMarker(29, (INT_PTR)ok);
        SendStr(21, buf);
    }
    // H. 4 字节出参结构（ShortcutKey 布局）
    {
        SK sk = {};
        const LRESULT r = Npp(NPPM_GETSHORTCUTBYCMDID,
                              (WPARAM)items[0].cmdID, (LPARAM)&sk);
        SendMarker(30, (INT_PTR)r);
        SendMarker(31, (INT_PTR)(unsigned char)sk.key);
    }
    // I. ★ kValue 的 lp 必须真的过桥（批次 117 修的**静默**缺陷）。
    //    NppmCallWire 原来只有 wParam 一格 ⇒ 宿主永远看到 lp == 0。
    //    选 GETNBOPENFILES 是因为它的**答案取决于 lp**（契约见 NppMessages.h：
    //    `int (0, int iViewType)`，0=全部 / 1=主视图 / 2=副视图）：
    //    副视图查询必须返回 0，主视图查询返回文档数。缺陷下前者会等于后者
    //    ⇒ 两条一起红。夹具里恰好有 1 个文档，所以 0 与 1 可分。
    SendMarker(32, (INT_PTR)Npp(NPPM_GETNBOPENFILES, 0, 2));   // lp = 副视图
    SendMarker(33, (INT_PTR)Npp(NPPM_GETNBOPENFILES, 0, 0));   // lp = 全部
    SendMarker(99, 1);
}

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-nppm"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"NPPM Bridge Probe");
    items[0].func = probe;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
