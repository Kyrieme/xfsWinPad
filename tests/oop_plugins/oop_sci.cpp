// oop_sci.cpp — 进程外桥 e2e 用插件：验证 SCI 通道（批次 112）。
//
// 与其它 oop_* 夹具同规则：不包含宿主头文件，手写公开契约布局。
//
// 与 oop_nppm 的关键差别：本插件把 SCI_* 发给 g_data.sciMain，而 v2.3 起
// g_data.sciMain 是**代理进程里的 SCI 中转窗**（不再是宿主的真 Scintilla 控件）。
// SCI_* 与 NPPM_* 的差别在于：826 条里 716 条**根本不带指针** ⇒ 中转窗把它们
// **直发**宿主真 Scintilla（没有指针可封送，跨进程是安全的）；带指针的按形状表
// 分流（批次 113 入参族 / 114 出参族 / 115 出入参族 / 116 结构族过桥，其余拒答）。
// 所以本夹具的电池分两半：
//   * 值类型（无指针）—— 必须**拿到宿主真值**（证明直发真的到了 Scintilla）；
//   * 永久拒答族（结构族里的进程私有 HDC / 返回内部指针 / 裸指针 / 表外）——
//     必须返回 0，且宿主的 Scintilla **从未收到**它们（测试侧计数）。
//
// 观察出口：'NPVL' {tag, value} 经 WM_COPYDATA 回传（npp 中转窗转发给编辑器窗口）。
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

// ---- 线上消息常量（与 src/plugin/scintilla 同源事实值）----------------------
// Scintilla 的 SCI_START = 2000，全部编号在 WM_USER 之上。
// ⚠ 本夹具的"仍拒答"样例必须选**永久拒答**的族 ——「现在恰好没桥接」是一个
//   **会过期的**性质：
//   * GETTEXT(2182) 批次 114 起已可过桥（出参族）；
//   * GETPROPERTY(4008) 批次 115 起已可过桥（出入参族）；
//   * GETSTYLEDTEXT(2015) / GETTEXTRANGE(2162) 批次 116 起**也已可过桥**（结构族）
//     —— 而且它们过桥后的真值（空范围 / 空串）也会返回 0，与"拒答返回 0"**同形**。
//   * 现在用的是两条**语义上永久不可桥接**的：
//     - FORMATRANGE(2151)：结构族里的 kRefuseHandle —— hdc/hdcTarget 是**进程
//       私有**的 GDI 句柄，且 FormatRange 是**绘制**操作。lp 指向调用方内存，
//       正好继续钉"拒答不写调用方内存"这一条（marker 50）。
//     - GETCHARACTERPOINTER(2520)：kPtrRet —— 返回文档**内部指针**，跨进程无意义。
// ⚠ 别把这个编号写成 4005：那是 **SetKeyWords**（kInStr / kStrInLp）—— 它
//   **可以**过桥。写错编号会让这条"拒答样例"被正常中转到宿主，于是场景 11/12
//   的"宿主从未收到不可桥接消息"承重计数被悄悄抬高（批次 114 实测踩过，见 LESSONS）。
#define SCI_FORMATRANGE         2151  // kStruct/kRefuseHandle：进程私有 HDC ⇒ 永久拒答
#define SCI_GETCHARACTERPOINTER 2520  // kPtrRet：返回文档内部指针 ⇒ 永久拒答
#define SCI_SETDOCPOINTER       2358  // kRawPtr：语义上不可桥接 ⇒ 永久拒答
#define SCI_GETCURRENTPOS       2008
#define SCI_GETLINECOUNT        2154
#define SCI_GETTEXTLENGTH       2183
#define SCI_UNKNOWN_IN_RANGE    3000  // 落在 [2001,4033] 内但不在形状表里
#define SCI_OUT_OF_RANGE        5000  // 区间之外

// Sci_RangeToFormat 的布局（HDC / HDC / 2×RECT / CHARRANGE = 56 字节，**没有**
// HWND —— Scintilla.h:1400 与 Windows 的 FORMATRANGE 一致）。
// 本夹具只把它当"调用方内存"的载体：桥接在**中转窗**就按消息号拒答，绝不会碰它。
struct FmtRange {
    void* hdc; void* hdcTarget;
    long rc[4]; long rcPage[4]; long cpMin; long cpMax;
};

static NPData g_data{};
static FI items[1];
static SK skA = { false, false, false, 0 };

static constexpr UINT_PTR kValMagic = 0x4E50564C;   // 'NPVL'

static void SendMarker(UINT_PTR tag, INT_PTR value) {
    if (!g_data.npp) return;
    struct { UINT_PTR tag; INT_PTR value; } m{ tag, value };
    COPYDATASTRUCT cds{};
    cds.dwData = kValMagic;
    cds.cbData = sizeof(m);
    cds.lpData = &m;
    ::SendMessageW(g_data.npp, WM_COPYDATA, 0, (LPARAM)&cds);
}

static LRESULT Sci(UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_data.sciMain) return 0;
    return ::SendMessageW(g_data.sciMain, msg, wp, lp);
}

// 标记 tag 分配（测试侧按同一编号断言）：
//   40 收到的 scintillaMain 句柄   41 收到的 npp 句柄
//   42 GETCURRENTPOS（值）        43 GETLINECOUNT（值）   44 GETTEXTLENGTH（值）
//   45 FORMATRANGE（结构族拒答桶）  46 GETCHARACTERPOINTER（kPtrRet） 47 SETDOCPOINTER（kRawPtr）
//   48 区间内表外编号             49 区间外编号
//   50 出参缓冲未被写（拒答不写调用方内存）
//   99 全部完成
//
// ⚠ 批次 113 起入参串族（kInStr/kInBytes 54 条）、114 起出参族（kOutStr 30 条）、
//   115 起出入参族（kInOutStr 5 条）、116 起**结构族里不带外来指针的 6 条**
//   （GETTEXTRANGE/GETSTYLEDTEXT/FINDTEXT 各两版）**都已可过桥** —— 各自的电池在
//   oop_scibridge.cpp / oop_sciout.cpp / oop_sciinout.cpp / oop_scistruct.cpp。
//   本夹具只钉"值类型直发 + 永久拒答族一律拒答 + 表外拒答"，几者互补。
//   ⇒ 别再把 GETTEXT / GETPROPERTY / GETSTYLEDTEXT / GETTEXTRANGE 当成"拒答"
//     样例：它们现在都会真的过桥（且过桥后的真值也常是 0，与拒答**同形**）。
static void probe(void) {
    // 0. 把两个句柄原样报回去 —— 测试断言它们**都不是**宿主的真句柄
    SendMarker(40, (INT_PTR)g_data.sciMain);
    SendMarker(41, (INT_PTR)g_data.npp);

    // 1. 值类型：应当直发宿主真 Scintilla 并拿到真值
    SendMarker(42, (INT_PTR)Sci(SCI_GETCURRENTPOS, 0, 0));
    SendMarker(43, (INT_PTR)Sci(SCI_GETLINECOUNT, 0, 0));
    SendMarker(44, (INT_PTR)Sci(SCI_GETTEXTLENGTH, 0, 0));

    // 2. 带指针：应当被中转窗拒答（0），且宿主**从未**收到
    {
        // ⚠ 缓冲先填**非零**哨兵：零初始化的缓冲"被写了 NUL"与"没被碰过"是
        //   同一个字节，那样这条断言没有区分力 —— 批次 115 实测踩过：GETPROPERTY
        //   从"拒答"变成"过桥"后，marker 45 仍然是 0，只有哨兵能看出差别。
        //   FORMATRANGE 的 lp 是一个**结构体**指针（原生会读 hdc/hdcTarget 去绘制）；
        //   本用例的前提正是"它到不了宿主"，所以只给一块填满哨兵的结构体。
        FmtRange fr{};
        ::memset(&fr, 'Z', sizeof(fr));
        SendMarker(45, (INT_PTR)Sci(SCI_FORMATRANGE, 0, (LPARAM)&fr));
        SendMarker(50, reinterpret_cast<const char*>(&fr)[0] == 'Z' ? 1 : 0);
    }
    // 46: kPtrRet（返回文档内部指针）—— 同样永久拒答。原生会返回一个非 0 指针，
    //     所以"0"在这里是有区分力的（不是"本来就没值"）。
    SendMarker(46, (INT_PTR)Sci(SCI_GETCHARACTERPOINTER, 0, 0));
    SendMarker(47, (INT_PTR)Sci(SCI_SETDOCPOINTER, 0, (LPARAM)0x1000));

    // 3. 表外编号：区间内但不在表里 / 区间外 —— 都必须拒答（返回 0）
    SendMarker(48, (INT_PTR)Sci(SCI_UNKNOWN_IN_RANGE, 0, 0));
    SendMarker(49, (INT_PTR)Sci(SCI_OUT_OF_RANGE, 0, 0));

    SendMarker(99, 1);
}

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-sci"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"SCI Channel Probe");
    items[0].func = probe;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
