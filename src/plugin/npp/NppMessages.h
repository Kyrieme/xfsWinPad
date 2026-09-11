#pragma once
// NppMessages.h — Notepad++ 线上消息常量契约（xfsWinPad 原创声明）。
//
// 数值是从公开事实源人工核对后抄录的【接口数值】（接口数值属于事实，
// 不构成表达）；实现均为原创。来源与抓取日期：
//   上游: github.com/notepad-plus-plus/notepad-plus
//         PowerEditor/src/MISC/PluginsManager/Notepad_plus_msgs.h
//   实际取数通道(2026-08-27): gitee.com/mirrors/notepad-plus-plus @ master
//   （raw.githubusercontent 直连在当时的网络环境不可达）
//
// 若上游未来调整基值/偏移：改这里即可，分发器逻辑不动。
// ⚠️ 字段顺序无关本文件；每个值的语义要点见对应注释块，分歧以上游原文为准。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace xfs {
namespace npp {

// ---- 基值 ------------------------------------------------------------------
constexpr UINT kNppMsgBase       = WM_USER + 1000; // 上游宏 NPPMSG
constexpr UINT kRunCmdBase       = WM_USER + 3000; // 上游宏 RUNCOMMAND_USER

enum NppMsg : UINT {
    // ---- NPPMSG 族 ---------------------------------------------------------
    NPPM_GETCURRENTSCINTILLA      = kNppMsgBase + 4,
    // BOOL (0, int* iScintillaView) → *out ∈ {0=Main,1=Sub}；我们恒为单视图
    NPPM_GETNBOPENFILES           = kNppMsgBase + 7,
    // int (0, int iViewType)：0=全部 / 1=主视图 / 2=副视图
    NPPM_GETOPENFILENAMES_DEPRECATED = kNppMsgBase + 8,
    // BOOL *deprecated* (wchar_t** fileNames, int nbFileNames) → 拷贝条数
    NPPM_GETOPENFILENAMESPRIMARY_DEPRECATED = kNppMsgBase + 17,
    NPPM_GETOPENFILENAMESSECOND_DEPRECATED  = kNppMsgBase + 18,
    // （同上签名；我们的副视图恒空集，SECOND 返回 0）

    NPPM_RELOADFILE               = kNppMsgBase + 36,
    // BOOL (BOOL withAlert, wchar_t* fullPath)。上游头内注释把 wParam 标注为
    // 未用与其签名自相矛盾 —— 取签名口径（wParam=withAlert），见文档 §5.6。
    NPPM_SWITCHTOFILE             = kNppMsgBase + 37,
    // BOOL (0, wchar_t* fullPath)：已打开则激活，否则打开
    NPPM_SAVECURRENTFILE          = kNppMsgBase + 38,
    // BOOL (0,0)：保存活动文档
    NPPM_SAVEALLFILES             = kNppMsgBase + 39,
    // BOOL (0,0)：保存全部；FALSE=没有需要保存的

    NPPM_GETNPPVERSION            = kNppMsgBase + 50,
    // DWORD (BOOL ADD_ZERO_PADDING, 0)：宿主报告版本号。上游：
    // HIWORD=主版本，LOWORD=次版本（Notepad++ 同款编码）。⚠ 实测官方
    // Notepad_plus_msgs.h 该值为 NPPMSG+50（非 +40；+40 是 NPPM_SETMENUITEMCHECK）。
    // NppExec 用它做最低版本门槛（≥5.1），不答=0 会被拒并弹
    // "requires Notepad++ ver. 5.1 or higher"模态框。

    NPPM_GETMENUHANDLE            = kNppMsgBase + 25,
    // HMENU (int menuChoice /*NppPluginMenu=0 / NppMainMenu=1*/, 0)：返回
    //   "Plugins" 子菜单或主菜单栏句柄。NppExec 拿主菜单栏句柄后自行
    //   ModifyMenu/CheckMenuItem（MF_BYCOMMAND 会递归命中 Plugins 子菜单里
    //   的命令项）；菜单结构仍由宿主 RebuildPluginMenu 构建。
    NPPM_SETMENUITEMCHECK         = kNppMsgBase + 40,
    // BOOL (UINT_PTR cmdID, BOOL doCheck)：按命令 id 勾选/取消勾选对应菜单项。
    //   插件命令（≥PluginCmdFirst）勾在 Plugins 子菜单，内建命令勾在主菜单栏。
    NPPM_GETMENUBAR               = kNppMsgBase + 52,
    // HMENU (0,0)：返回主菜单栏句柄（老插件兼容路径，等价 GETMENUHANDLE
    //   的 NppMainMenu 分支）。
    NPPM_GETSHORTCUTBYCMDID       = kNppMsgBase + 76,
    // BOOL (UINT_PTR cmdID, ShortcutKey* sk)：回填命令的快捷键
    //   （{bool ctrl, bool alt, bool shift, UCHAR key}，布局见 NppCompat.h）。
    //   NppExec 对每个非空 FuncItem 都查一次（实测 0x0834 ×16）；无快捷键
    //   或未知命令返回 FALSE。

    NPPM_GETPLUGINSCONFIGDIR      = kNppMsgBase + 46,
    // 两段式：第一次 (strLen, NULL) 返回所需 wchar_t 数（不含 NUL）；
    // 第二次按返回值+1 分配后传入，成功 TRUE 失败 FALSE

    NPPM_GETPOSFROMBUFFERID       = kNppMsgBase + 57,
    // int (UINT_PTR bufferID, int priorityView) → -1 或 VIEW<<30|INDEX
    NPPM_GETFULLPATHFROMBUFFERID  = kNppMsgBase + 58,
    // int (UINT_PTR bufferID, wchar_t* buf|NULL) → -1 无效；否则 wchar_t 数
    //   （buf==NULL 为查询长度；两段式，不含 NUL）
    NPPM_GETBUFFERIDFROMPOS       = kNppMsgBase + 59,
    // UINT_PTR (int index /*wParam*/, int view /*lParam*/) → 无效=NULL
    NPPM_GETCURRENTBUFFERID       = kNppMsgBase + 60,
    // UINT_PTR (0,0) → 活动文档 bufferID
    NPPM_RELOADBUFFERID           = kNppMsgBase + 61,
    // BOOL (UINT_PTR bufferID, BOOL alert)

    NPPM_DOOPEN                   = kNppMsgBase + 77,
    // BOOL (0, const wchar_t* fullPath)
    NPPM_ALLOCATECMDID            = kNppMsgBase + 81,
    // BOOL (int numberRequested, int* startNumber)

    // ---- 可停靠对话框族（4d，NPPM_DMM*）-------------------------------------
    // 结构 tTbData = DockedWidgetData（NppDocking.h）；对话框句柄一律 HWND。
    NPPM_DMMSHOW                  = kNppMsgBase + 30,
    // BOOL (0, HWND hDlg)：显示已注册的插件对话框
    NPPM_DMMHIDE                  = kNppMsgBase + 31,
    // BOOL (0, HWND hDlg)：隐藏已注册的插件对话框
    NPPM_DMMUPDATEDISPINFO        = kNppMsgBase + 32,
    // BOOL (0, HWND hDlg)：重绘/刷新该对话框
    NPPM_DMMREGASDCKDLG           = kNppMsgBase + 33,
    // BOOL (0, DockedWidgetData* pData)：把插件对话框注册为可停靠面板
    NPPM_DMMVIEWOTHERTAB          = kNppMsgBase + 35,
    // BOOL (0, const wchar_t* name)：切到指定名称的插件面板
    NPPM_DMMGETPLUGINHWNDBYNAME   = kNppMsgBase + 43,
    // HWND (const wchar_t* windowName, const wchar_t* moduleName)：
    //   按名称+模块查对话框句柄；windowName==NULL 时按模块名取首个

    // ---- RUNCOMMAND 族（另一基值！）----------------------------------------
    NPPM_GETFULLCURRENTPATH       = kRunCmdBase + 1,
    NPPM_GETCURRENTDIRECTORY      = kRunCmdBase + 2,
    NPPM_GETFILENAME              = kRunCmdBase + 3,
    NPPM_GETNAMEPART              = kRunCmdBase + 4,
    NPPM_GETEXTPART               = kRunCmdBase + 5,
    NPPM_GETCURRENTWORD           = kRunCmdBase + 6,
    // 两段式同 GETPLUGINSCONFIGDIR：(int strLen, wchar_t* str|NULL)
};

// 视图常量（上游 #define 的同名事实值）
constexpr int kAllOpenFiles = 0;
constexpr int kPrimaryView  = 1;
constexpr int kSecondView   = 2;
constexpr int kMainViewPos  = 0;   // GETPOSFROMBUFFERID 编码里的 MAIN_VIEW
constexpr int kSubViewPos   = 1;

// NPPM_GETMENUHANDLE 的 wParam 取值（上游 #define NPPPLUGINMENU 0 / NPPMAINMENU 1）
constexpr int NppPluginMenu = 0;   // "Plugins" 子菜单句柄
constexpr int NppMainMenu   = 1;   // 主菜单栏句柄

// ---- 通知码（beNotified 桥 4c）---------------------------------------------
// 语义事实（上游头文件 1091 行起）：宿主把 SCNotification 递给插件 beNotified，
// 其中 nmhdr.code = NPPN_*、nmhdr.hwndFrom = 主窗口 HWND、nmhdr.idFrom = BufferID
// （无文档时 0）。BufferID 沿用 4b 的 Document* 空间（见 §5.6 buffer-id 族）。
enum NppNotify : int {
    kNppnBase              = 1000,   // 上游宏 NPPN_FIRST
    NPPN_READY             = kNppnBase + 1,   // 宿主初始化完成，hwndFrom=主窗口
    NPPN_TBMODIFICATION    = kNppnBase + 2,   // 可注册工具栏图标（4d 预留）
    NPPN_FILEBEFORECLOSE   = kNppnBase + 3,   // 当前文件即将关闭
    NPPN_FILEOPENED        = kNppnBase + 4,   // 文件刚打开
    NPPN_FILECLOSED        = kNppnBase + 5,   // 文件刚关闭
    NPPN_FILEBEFOREOPEN    = kNppnBase + 6,   // 文件即将打开
    NPPN_FILEBEFORESAVE    = kNppnBase + 7,   // 文件即将保存
    NPPN_FILESAVED         = kNppnBase + 8,   // 文件刚保存
    NPPN_SHUTDOWN          = kNppnBase + 9,   // 宿主即将退出
    NPPN_BUFFERACTIVATED   = kNppnBase + 10,  // 缓冲区激活（前置到前台）
    NPPN_LANGCHANGED       = kNppnBase + 11,  // 当前文档语言变更（未合成）
    NPPN_WORDSTYLESUPDATED = kNppnBase + 12,  // 样式对话框变更（未合成）
    NPPN_SHORTCUTREMAPPED  = kNppnBase + 13,  // 插件命令快捷键重映射（未合成）
    NPPN_FILEBEFORELOAD    = kNppnBase + 14,  // 文件即将加载（未合成）
    NPPN_FILELOADFAILED    = kNppnBase + 15,  // 打开失败（未合成）
    NPPN_READONLYCHANGED   = kNppnBase + 16,  // 只读状态变更（未合成）
    NPPN_DOCORDERCHANGED   = kNppnBase + 17,  // 文档顺序变更（未合成）
    NPPN_SNAPSHOTDIRTYFILELOADED = kNppnBase + 18, // 快照脏文件加载（未合成）
    NPPN_BEFORESHUTDOWN    = kNppnBase + 19,  // 已触发退出、文件尚未关闭（未合成）
    NPPN_CANCELSHUTDOWN    = kNppnBase + 20,  // 退出被取消（未合成）
    NPPN_FILEBEFORERENAME  = kNppnBase + 21,  // 文件即将重命名（未合成）
    NPPN_FILERENAMECANCEL  = kNppnBase + 22,  // 重命名取消（未合成）
    NPPN_FILERENAMED       = kNppnBase + 23,  // 文件已重命名（未合成）
    NPPN_FILEBEFOREDELETE  = kNppnBase + 24,  // 文件即将删除（未合成）
    NPPN_FILEDELETEFAILED  = kNppnBase + 25,  // 删除失败（未合成）
    NPPN_FILEDELETED       = kNppnBase + 26,  // 文件已删除（未合成）
    NPPN_DARKMODECHANGED   = kNppnBase + 27,  // 深色模式切换（未合成）
    NPPN_CMDLINEPLUGINMSG  = kNppnBase + 28,  // 命令行插件参数（未合成）
    NPPN_EXTERNALLEXERBUFFER = kNppnBase + 29, // 外部词法器缓冲（未合成）
    NPPN_GLOBALMODIFIED    = kNppnBase + 30,  // Replace All 修改（未合成）
    NPPN_NATIVELANGCHANGED = kNppnBase + 31,  // 本地化语言切换（未合成）
    NPPN_TOOLBARICONSETCHANGED = kNppnBase + 32, // 工具栏图标集变更（未合成）
};

} // namespace npp
} // namespace xfs
