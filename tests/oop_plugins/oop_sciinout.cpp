// oop_sciinout.cpp — 进程外桥 e2e 用插件：验证 SCI **出入参**族过桥（批次 115）。
//
// 与其它 oop_* 夹具同规则：不包含宿主头文件，手写公开契约布局。
//
// 本族 = 入参族的一半 + 出参族的一半：形状是 (string, stringresult)，**入参串在
// wParam（NUL 结尾）、出参缓冲在 lParam**。所以这里同时钉两件事：
//   ① 出参侧（与 oop_sciout 同形）：缓冲区边界 —— 写对了几字节、第 N 格是 NUL、
//      N 之后仍是哨兵（一个字节都没多写）；
//   ② 入参侧（与 oop_scibridge 同形）：**插件进程里的那个 wParam 地址必须被换成
//      宿主本地的副本**。这一点由假 Scintilla 侧独立记账：它读到的 key 长度会被
//      编进返回值（key.size()*100 + value.size()），所以插件的 marker 就是
//      "串真的过来了"的现场证据 —— 只回 value.size() 的话，串没过来也一样绿。
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
// 本族 5 条全在这里（3 条 kNul + 2 条拒答桶）。
#define SCI_GETPROPERTY        4008   // (key, value)  kNul
#define SCI_GETPROPERTYEXPANDED 4009  // (key, value)  kNul（5.6.6 里就是 PropGet）
#define SCI_DESCRIBEPROPERTY   4016   // (key, desc)   kNul（LexerBase 恒回 ""）
#define SCI_ENCODEDFROMUTF8    2449   // (utf8, encoded) **拒答桶**
#define SCI_GETREPRESENTATION  2666   // (char, repr)    **拒答桶**
// ⚠ 4005 是 SetKeyWords（**可**过桥的 kStrInLp），别当"仍拒答"样例写错。

static NPData g_data{};
static FI items[1];
static SK skA = { false, false, false, 0 };

static constexpr UINT_PTR kValMagic = 0x4E50564C;   // 'NPVL'
static constexpr unsigned char kCanary = 0x5A;
static constexpr size_t kBufSize = 64;

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

// 一次调用的"现场证据"摘要。位含义（测试侧按同一编号断言）：
//   1  期望的前 n 字节逐字节写对了
//   2  第 n 格是 NUL（NUL 桶应有的样子）
//   4  第 n 格仍是哨兵（无 NUL 桶应有的样子）
//   8  n 之后**全是**哨兵 ⇒ 代理一个字节都没多写
//   16 整块缓冲一个字节都没被写 ⇒ 拒答路径
static INT_PTR Digest(const unsigned char* b, size_t n, const char* expect,
                      bool nulAtN) {
    INT_PTR f = 0;
    // ★ n==0 时**不**计这一位：空前缀不是"写对了"的证据（空真会让拒答用例
    //   多出一位，把 28 变成 29）。空值用例靠位 2/8 立命。
    bool prefix = (n > 0);
    for (size_t i = 0; i < n; ++i)
        if (b[i] != static_cast<unsigned char>(expect[i])) { prefix = false; break; }
    if (prefix) f |= 1;
    if (b[n] == 0) f |= 2;
    if (b[n] == kCanary) f |= 4;
    bool tail = true;
    for (size_t i = n + 1; i < kBufSize; ++i)
        if (b[i] != kCanary) { tail = false; break; }
    if (tail) f |= 8;
    bool untouched = true;
    for (size_t i = 0; i < kBufSize; ++i)
        if (b[i] != kCanary) { untouched = false; break; }
    if (untouched) f |= 16;
    return f;
}

// 期望值（测试侧逐条对同一编号断言）：
//   150..157 返回值        190..197 现场证据摘要
// 成功用例（kNul）⇒ 11（1|2|8）；空值成功用例（n==0）⇒ 10（2|8）
// 拒答用例 ⇒ 28（4|8|16，一个字节都没写）
// 返回值编码 = 宿主读到的 key 长度 * 100 + 值的字节数（假 Scintilla 的约定）：
//   GetProperty("k1") 有值 "v1"  ⇒ 2*100+2 = 202
//   DescribeProperty  值 "desc:k1"（7 字节）⇒ 2*100+7 = 207
//   "k2" / 未设过的键 / 空键      ⇒ 2*100+0 = 200（"k2" 也是 2 字节）
//   200 字节的长键                ⇒ 200*100+0 = 20000（★ 载荷不截断）
static char g_longKey[201];

static void probe(void) {
    // ---- 过桥方向 ----------------------------------------------------------
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(150, (INT_PTR)Sci(SCI_GETPROPERTY, (WPARAM)"k1", (LPARAM)buf));
        SendMarker(190, Digest(buf, 2, "v1", true));       // 期望 11
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(151, (INT_PTR)Sci(SCI_GETPROPERTYEXPANDED, (WPARAM)"k1", (LPARAM)buf));
        SendMarker(191, Digest(buf, 2, "v1", true));       // 期望 11
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(152, (INT_PTR)Sci(SCI_DESCRIBEPROPERTY, (WPARAM)"k1", (LPARAM)buf));
        SendMarker(192, Digest(buf, 7, "desc:k1", true));  // 期望 11
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 空值：need=0，但 kNul 桶仍要写一个 NUL ⇒ 第 0 格是 NUL、其余不动
        SendMarker(153, (INT_PTR)Sci(SCI_GETPROPERTY, (WPARAM)"k2", (LPARAM)buf));
        SendMarker(193, Digest(buf, 0, "", true));         // 期望 10（n==0，无前缀位）
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 200 字节的键：载荷必须整段过桥（若被截断，宿主读到的 key 会变短 ⇒
        // 返回值里的 *100 那一项立刻不对）
        SendMarker(154, (INT_PTR)Sci(SCI_GETPROPERTY, (WPARAM)g_longKey, (LPARAM)buf));
        SendMarker(194, Digest(buf, 0, "", true));         // 期望 10
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 空键：inBytes == 1（只有一个 NUL）—— 与"不带串"是两回事，必须照常过桥
        SendMarker(155, (INT_PTR)Sci(SCI_GETPROPERTY, (WPARAM)"", (LPARAM)buf));
        SendMarker(195, Digest(buf, 0, "", true));         // 期望 10
    }

    // ---- 拒答方向 ----------------------------------------------------------
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 拒答桶：写入量随运行期状态变（GetRepresentation 分不了两种 0）
        SendMarker(156, (INT_PTR)Sci(SCI_GETREPRESENTATION, (WPARAM)"a", (LPARAM)buf));
        SendMarker(196, Digest(buf, 0, "", true));         // 期望 28
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 拒答桶：容量来自伴生消息 + NUL 语义随编码模式变
        SendMarker(157, (INT_PTR)Sci(SCI_ENCODEDFROMUTF8, (WPARAM)"A", (LPARAM)buf));
        SendMarker(197, Digest(buf, 0, "", true));         // 期望 28
    }
    // ---- 恶意入参串：必须拒答，且**不能**把代理拖死 -------------------------
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 空指针当入参串
        SendMarker(158, (INT_PTR)Sci(SCI_GETPROPERTY, (WPARAM)nullptr, (LPARAM)buf));
        SendMarker(178, Digest(buf, 0, "", true));         // 期望 28
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 未映射地址当入参串（0x1000）：有界扫描必须把它变成拒答而不是访问违例
        SendMarker(159, (INT_PTR)Sci(SCI_GETPROPERTY, (WPARAM)0x1000, (LPARAM)buf));
        SendMarker(179, Digest(buf, 0, "", true));         // 期望 28
    }

    SendMarker(74, 1);   // 两条恶意入参串 + 两条拒答之后，代理进程还在跑
    SendMarker(99, 1);
}

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-sciinout"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    for (int i = 0; i < 200; ++i) g_longKey[i] = 'L';
    g_longKey[200] = '\0';
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"SCI InOut Probe");
    items[0].func = probe;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
