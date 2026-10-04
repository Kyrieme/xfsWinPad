// oop_sciout.cpp — 进程外桥 e2e 用插件：验证 SCI **出参指针**族过桥（批次 114）。
//
// 与其它 oop_* 夹具同规则：不包含宿主头文件，手写公开契约布局。
//
// 与 oop_scibridge（入参族）的差别：那边搬的是**插件 → 宿主**的串；这边搬的是
// **宿主 → 插件**的字节，而难点不在"搬内容"而在**"搬多少"** —— 容量不在消息里
// （唯一的来源是 Scintilla 出参协议自带的 lParam==0 调用）。回传字节数由实现的
// NUL 语义决定，而 StringResult 写 len+1、BytesResult 写 len，**只差一个字节**。
//
// 本夹具的核心断言是**缓冲区边界**：每个用例给一块 64 字节、填满哨兵 0x5A 的
// 缓冲，调用后同时验三件事 ——
//   ① 期望的内容逐字节写对了；
//   ② 第 N 格（NUL 桶应为 NUL / 无 NUL 桶应仍是哨兵）符合该桶的语义；
//   ③ N 之后**仍是哨兵**（代理没多写一个字节）。
// ③ 是"差一个字节就是越界写"的唯一现场证据：错一个字节就会把哨兵踩掉。
// 若只断言"内容对"，少写一个 NUL 或多写一个字节都会**看不出来**。
//
// 另一半电池是拒答方向：拒答桶（TARGETASUTF8 / GETTAG / GETREPRESENTATION）、
// 以及**只读**的接收缓冲 —— 都必须换来"返回 0 且一个字节都没写"，同时代理进程
// 还活着。
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
// 出参族的四个桶各取代表：
#define SCI_GETCURLINE        2027   // (length, text)  kClampWpNul ★ 返回值是光标列号
#define SCI_GETLINE           2153   // (line, text)    kNoNul   ★ 不写 NUL
#define SCI_GETSELTEXT        2161   // (text)          kNul
#define SCI_GETTEXT           2182   // (length, text)  kClampWpNul
#define SCI_GETWORDCHARS      2646   // (characters)    kNoNul   ★ 不写 NUL
#define SCI_GETFONTLOCALE     2761   // (localeName)    kNul     ★ 空值：need=0 仍写 NUL
#define SCI_TARGETASUTF8      2447   // (s)             **拒答桶**（NUL 语义随编码变）
#define SCI_GETTAG            2616   // (tagNumber, tagValue) **拒答桶**（写入量随 regex 变）
#define SCI_GETREPRESENTATION 2666   // (char, repr)    **拒答桶**（probe 分不了两种 0）

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
    //   多出一位，把 28 变成 29 —— 本批实测踩过）。空值用例靠位 2/8 立命。
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
//   80..91  返回值          120..131 现场证据摘要
// 成功用例：kNul ⇒ 11（1|2|8）；kNoNul ⇒ 13（1|4|8）
// 空值成功用例（n==0，没有前缀可验）⇒ 10（2|8）
// 拒答用例：28（4|8|16，一个字节都没写）
static const char kText20[] = "0123456789abcdefghij";   // 假 Scintilla 的 GETTEXT 内容

// 造一块"可读但不可写"的缓冲：代理侧的 LocalWritable 必须把它判成不可写 ⇒ 拒答。
// 这是"隔离能力"的断言：进程内调用会直接往只读页写而崩。
static unsigned char* MakeReadOnlyBuf(unsigned char** raw) {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    unsigned char* p = static_cast<unsigned char*>(
        ::VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!p) { *raw = nullptr; return nullptr; }
    ::memset(p, kCanary, si.dwPageSize);
    DWORD old = 0;
    if (!::VirtualProtect(p, si.dwPageSize, PAGE_READONLY, &old)) {
        ::VirtualFree(p, 0, MEM_RELEASE);
        *raw = nullptr;
        return nullptr;
    }
    *raw = p;
    return p;
}

static void probe(void) {
    // ---- 过桥方向（4 个桶各取代表）----------------------------------------
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(80, (INT_PTR)Sci(SCI_GETSELTEXT, 0, (LPARAM)buf));
        SendMarker(120, Digest(buf, 5, "hello", true));      // kNul，期望 11
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // wp 是**行号**（不是容量）：假 Scintilla 对第 0 行返回 7 字节、不写 NUL
        SendMarker(81, (INT_PTR)Sci(SCI_GETLINE, 0, (LPARAM)buf));
        SendMarker(121, Digest(buf, 7, "lineA\r\n", false)); // kNoNul，期望 13
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(82, (INT_PTR)Sci(SCI_GETTEXT, 64, (LPARAM)buf));
        SendMarker(122, Digest(buf, 20, kText20, true));     // kClampWpNul，期望 11
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // ★ 返回值是光标列号(3)，不是长度(20)：按返回值算字节数的实现会搬错
        SendMarker(83, (INT_PTR)Sci(SCI_GETCURLINE, 64, (LPARAM)buf));
        SendMarker(123, Digest(buf, 20, kText20, true));     // 期望 11
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // wp=4 < need=20 ⇒ kClampWpNul 必须截到 4+1 字节
        SendMarker(84, (INT_PTR)Sci(SCI_GETTEXT, 4, (LPARAM)buf));
        SendMarker(124, Digest(buf, 4, kText20, true));      // 期望 11
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(85, (INT_PTR)Sci(SCI_GETWORDCHARS, 0, (LPARAM)buf));
        SendMarker(125, Digest(buf, 4, "abcd", false));      // kNoNul，期望 13
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 空值：need=0，但 kNul 桶仍要写一个 NUL ⇒ 第 0 格是 NUL、其余不动
        SendMarker(86, (INT_PTR)Sci(SCI_GETFONTLOCALE, 0, (LPARAM)buf));
        SendMarker(126, Digest(buf, 0, "", true));           // 期望 10（n==0，无前缀位）
    }

    // ---- 拒答方向 ----------------------------------------------------------
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(87, (INT_PTR)Sci(SCI_TARGETASUTF8, 0, (LPARAM)buf));
        SendMarker(127, Digest(buf, 0, "", true));           // 期望 28
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        SendMarker(88, (INT_PTR)Sci(SCI_GETTAG, 1, (LPARAM)buf));
        SendMarker(128, Digest(buf, 0, "", true));           // 期望 28
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // 出入参族里的**拒答桶**：probe 分不了两种 0（repr 不存在 ⇒ 写 0 字节；
        // repr 存在但为空串 ⇒ 写 1 个 NUL）。两个字节数都藏在同一个 need=0 后面。
        // ⚠ 批次 115 起 GETPROPERTY(4008) **已可过桥**，别再拿它当"仍拒答"样例：
        //   它过桥后的真值是空串 ⇒ 返回 0，与"拒答返回 0"同形，断言会绿而失效。
        SendMarker(89, (INT_PTR)Sci(SCI_GETREPRESENTATION, (WPARAM)"a", (LPARAM)buf));
        SendMarker(129, Digest(buf, 0, "", true));           // 期望 28
    }
    {
        // 只读接收缓冲：代理侧的 LocalWritable 必须判不可写 ⇒ 拒答
        unsigned char* raw = nullptr;
        unsigned char* ro = MakeReadOnlyBuf(&raw);
        SendMarker(90, ro ? (INT_PTR)Sci(SCI_GETSELTEXT, 0, (LPARAM)ro) : -1);
        SendMarker(130, ro ? Digest(ro, 0, "", true) : 0);   // 期望 28
        if (raw) ::VirtualFree(raw, 0, MEM_RELEASE);
    }
    {
        unsigned char* raw = nullptr;
        unsigned char* ro = MakeReadOnlyBuf(&raw);
        SendMarker(91, ro ? (INT_PTR)Sci(SCI_GETTEXT, 64, (LPARAM)ro) : -1);
        SendMarker(131, ro ? Digest(ro, 0, "", true) : 0);   // 期望 28
        if (raw) ::VirtualFree(raw, 0, MEM_RELEASE);
    }

    SendMarker(74, 1);   // 两次只读缓冲 + 三次拒答之后，代理进程还在跑
    SendMarker(99, 1);
}

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-sciout"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"SCI Out Probe");
    items[0].func = probe;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
