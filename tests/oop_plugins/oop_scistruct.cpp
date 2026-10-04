// oop_scistruct.cpp — 进程外桥 e2e 用插件：验证 SCI **结构族**过桥（批次 116）。
//
// 与其它 oop_* 夹具同规则：不包含宿主头文件，手写公开契约布局。
//
// 本族 = 指针指向**结构体**、结构体里**还有指针** ⇒ 中转窗要**展平**字段 + **内联**
// 串，宿主侧再重建。四类（8 条）：
//   * kRangeOut    GetTextRange(2162) / GetTextRangeFull(2039) —— 只读结构 + 出缓冲
//   * kStyledOut   GetStyledText(2015) / GetStyledTextFull(2778) —— 出缓冲是 (char,style)
//   * kFindInOut   FindText(2150) / FindTextFull(2196) —— 入串 + **条件**回写 chrgText
//   * kRefuseHandle FormatRange(2151) / FormatRangeFull(2777) —— 进程私有 HDC ⇒ 永久拒答
//
// ★ 本族是本项目第一个**真正的 in-out 族**：同一条消息里既有入参侧（chrg / needle）
//   又有出参侧（出缓冲 / chrgText）。于是这里同时钉三件事：
//     ① 出缓冲边界（与 oop_sciout 同形）：内容字节写对、NUL 位置对、之后仍是哨兵；
//     ② needle 真的跨过来了 —— 由"宿主找到了 needle"自证（返回的位置 > 0 只在
//        它真的读到了那串时才可能）；
//     ③ ★ **条件回写**：FindText 没找到时 Scintilla **一个字节都不写** chrgText。
//        插件侧把 chrgText 预置成哨兵，于是"没被写"与"被写成 (0,0)"能分开 ——
//        这正是桥接必须回带 hasChrg 那位的原因（照写就会把插件的原值覆盖成 (0,0)）。
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
#define SCI_GETSTYLEDTEXT      2015   // (tr)   kStyledOut
#define SCI_GETTEXTRANGEFULL   2039   // (trf)  kRangeOut
#define SCI_FINDTEXT           2150   // (opt, tf)   kFindInOut
#define SCI_FORMATRANGE        2151   // (draw, fr)  kRefuseHandle ★ 永久拒答
#define SCI_GETTEXTRANGE       2162   // (tr)   kRangeOut
#define SCI_FINDTEXTFULL       2196   // (opt, tff)  kFindInOut
#define SCI_GETSTYLEDTEXTFULL  2778   // (trf)  kStyledOut

// ---- 公开结构体的手写镜像（与 SciBridge.h 的 abi::* 同源，尺寸与真
//      Scintilla.h 由 test_sci_struct.cpp 对撞）--------------------------------
//   Sci_PositionCR = long（非 Full）      Sci_Position = intptr_t（Full）
struct CRange     { long cpMin; long cpMax; };
struct CRangeFull { INT_PTR cpMin; INT_PTR cpMax; };
struct TRange     { CRange chrg; char* lpstrText; };
struct TRangeFull { CRangeFull chrg; char* lpstrText; };
struct TFind      { CRange chrg; const char* lpstrText; CRange chrgText; };
struct TFindFull  { CRangeFull chrg; const char* lpstrText; CRangeFull chrgText; };
// FormatRange 的入参（56 字节）：HDC / HDC / 2×RECT / CHARRANGE，**没有** HWND
// （Scintilla.h:1400 与 Windows 的 FORMATRANGE 一致）。这里只当"调用方内存"用 ——
// 桥接在**中转窗**就按消息号拒答，绝不会碰它。
struct FmtRange {
    void* hdc; void* hdcTarget;
    long rc[4]; long rcPage[4]; long cpMin; long cpMax;
};

static NPData g_data{};
static FI items[1];
static SK skA = { false, false, false, 0 };

static constexpr UINT_PTR kValMagic = 0x4E50564C;   // 'NPVL'
static constexpr unsigned char kCanary = 0x5A;
static constexpr size_t kBufSize = 64;

// chrgText 的"没被写"哨兵：一个真实文档位置不可能是负数。
static constexpr INT_PTR kChrgSentinelMin = -7;
static constexpr INT_PTR kChrgSentinelMax = -8;

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

// 一次结构族调用的"现场证据"摘要（位含义，测试侧按同一编号断言）：
//   1  期望的前 n 字节逐字节写对了（n==0 时**不计**这一位 —— 空前缀不是证据，
//      否则空值用例会白得一位，把 6 说成 7）
//   2  紧接内容的 nulCount 格是 NUL
//   4  NUL 之后**全是**哨兵 ⇒ 桥接一个字节都没多写
static INT_PTR Digest(const unsigned char* b, size_t n, const unsigned char* expect,
                      unsigned nulCount) {
    INT_PTR f = 0;
    bool prefix = (n > 0);
    for (size_t i = 0; i < n; ++i)
        if (b[i] != expect[i]) { prefix = false; break; }
    if (prefix) f |= 1;
    bool nuls = (nulCount > 0);
    for (unsigned k = 0; k < nulCount; ++k)
        if (b[n + k] != 0) { nuls = false; break; }
    if (nuls) f |= 2;
    bool tail = true;
    for (size_t i = n + nulCount; i < kBufSize; ++i)
        if (b[i] != kCanary) { tail = false; break; }
    if (tail) f |= 4;
    return f;
}

// 拒答用例：整块缓冲一个字节都没被写
static INT_PTR Untouched(const unsigned char* b) {
    for (size_t i = 0; i < kBufSize; ++i)
        if (b[i] != kCanary) return 0;
    return 1;
}

// 宿主夹具的文档是 "hello world"（11 字节）⇒ 期望内容按这个算。
// styled 族的字节流是 (char, style) 对；宿主文档没有词法器 ⇒ style 恒为 0。
static const unsigned char kStyled3[6] = { 'h', 0, 'e', 0, 'l', 0 };
static const unsigned char kStyled2[4] = { 'h', 0, 'e', 0 };

// 标记 tag 分配（测试侧按同一编号断言）：
//   160/161 GETTEXTRANGE(0,5)      ret / 摘要      期望 5  / 7（"hello" + NUL）
//   162/163 GETTEXTRANGEFULL(0,11) ret / 摘要      期望 11 / 7（"hello world" + NUL）
//   164/165 GETTEXTRANGE(5,5)      ret / 摘要      期望 0  / 6（n==0：只写一个 NUL）
//   166/167 GETSTYLEDTEXT(0,3)     ret / 摘要      期望 6  / 7（6 字节 + 2 个 NUL）
//   168/169 GETSTYLEDTEXTFULL(0,2) ret / 摘要      期望 4  / 7（4 字节 + 2 个 NUL）
//   170..172 FINDTEXT("world")     ret / chrgMin / chrgMax   期望 6 / 6 / 11
//   173/174 FINDTEXT("zzz")        ret / chrgText 未动       期望 -1 / 1
//   175..177 FINDTEXTFULL("hello") ret / chrgMin / chrgMax   期望 0 / 0 / 5
//   184..186 FINDTEXT("hello")@0   ret / chrgMin / chrgMax   期望 0 / 0 / 5
//   （184 是负控③ 的回归：非 Full 的 canary 若被截断成 0，"命中在 0" 会变成"没写"）
//   178/179 FORMATRANGE            ret / 结构体未动          期望 0 / 1
//   180 FINDTEXT(lp=0x1000) 结构体不可读 ⇒ 拒答    期望 0
//   181 FINDTEXT(needle=nullptr) 原生会 strlen(nullptr) 崩 ⇒ 拒答 期望 0
//   182/183 GETTEXTRANGE(0,2000) 超出文档 ⇒ 拒答   期望 0 / 1
//   74 多次恶意调用后代理仍活着   99 全部完成
static void probe(void) {
    // ---- kRangeOut：只读结构 + 出缓冲 --------------------------------------
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        TRange tr{ { 0, 5 }, reinterpret_cast<char*>(buf) };
        SendMarker(160, (INT_PTR)Sci(SCI_GETTEXTRANGE, 0, (LPARAM)&tr));
        SendMarker(161, Digest(buf, 5, reinterpret_cast<const unsigned char*>("hello"), 1));
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        // ★ 用**显式** cpMax（不用 -1）：宿主夹具的 GETTEXTLENGTH 是它自己的哨兵值，
        //   "到文档末尾"那条路在 test_sci_struct.cpp 里对**真 Scintilla** 验。
        TRangeFull tr{ { 0, 11 }, reinterpret_cast<char*>(buf) };
        SendMarker(162, (INT_PTR)Sci(SCI_GETTEXTRANGEFULL, 0, (LPARAM)&tr));
        SendMarker(163, Digest(buf, 11, reinterpret_cast<const unsigned char*>("hello world"), 1));
    }
    {
        // 空范围：len == 0 ⇒ 容量仍是 1（只写结尾 NUL）。★ 这条钉"n==0 空真"那个坑：
        // 摘要在 n==0 时不给"内容对"这一位，所以期望是 6 而不是 7。
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        TRange tr{ { 5, 5 }, reinterpret_cast<char*>(buf) };
        SendMarker(164, (INT_PTR)Sci(SCI_GETTEXTRANGE, 0, (LPARAM)&tr));
        SendMarker(165, Digest(buf, 0, nullptr, 1));
    }

    // ---- kStyledOut：(char, style) 对 + 两个 NUL ---------------------------
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        TRange tr{ { 0, 3 }, reinterpret_cast<char*>(buf) };
        SendMarker(166, (INT_PTR)Sci(SCI_GETSTYLEDTEXT, 0, (LPARAM)&tr));
        SendMarker(167, Digest(buf, 6, kStyled3, 2));
    }
    {
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        TRangeFull tr{ { 0, 2 }, reinterpret_cast<char*>(buf) };
        SendMarker(168, (INT_PTR)Sci(SCI_GETSTYLEDTEXTFULL, 0, (LPARAM)&tr));
        SendMarker(169, Digest(buf, 4, kStyled2, 2));
    }

    // ---- kFindInOut：入串 + 条件回写 ---------------------------------------
    {
        TFind ft{ { 0, 11 }, "world", { kChrgSentinelMin, kChrgSentinelMax } };
        SendMarker(170, (INT_PTR)Sci(SCI_FINDTEXT, 0, (LPARAM)&ft));
        SendMarker(171, (INT_PTR)ft.chrgText.cpMin);
        SendMarker(172, (INT_PTR)ft.chrgText.cpMax);
    }
    {
        // ★ 没找到：Scintilla **一个字节都不写** chrgText（Editor.cxx:4318/4349）。
        //   哨兵必须原封不动 —— 若桥接照写，插件的原值会被覆盖成 (0,0)，"没找到"
        //   与"找到了空匹配"就分不开了。
        TFind ft{ { 0, 11 }, "zzz", { kChrgSentinelMin, kChrgSentinelMax } };
        SendMarker(173, (INT_PTR)Sci(SCI_FINDTEXT, 0, (LPARAM)&ft));
        SendMarker(174, (ft.chrgText.cpMin == kChrgSentinelMin &&
                         ft.chrgText.cpMax == kChrgSentinelMax) ? 1 : 0);
    }
    {
        TFindFull ft{ { 0, 11 }, "hello", { kChrgSentinelMin, kChrgSentinelMax } };
        SendMarker(175, (INT_PTR)Sci(SCI_FINDTEXTFULL, 0, (LPARAM)&ft));
        SendMarker(176, (INT_PTR)ft.chrgText.cpMin);
        SendMarker(177, (INT_PTR)ft.chrgText.cpMax);
    }
    {
        // ★★ 负控③ 的 e2e 回归：**非 Full** 的 FindText 命中位置 **0**。
        //   非 Full 变体的 chrgText 字段是 long（4 字节），宿主侧若把 INT_PTR 的
        //   canary **截断**过来就得到 0 —— 而 0 正是这一格的真值 ⇒ "命中在 0" 会被
        //   读成"没写"，插件拿回一个原封不动的哨兵（= "没找到"）。
        //   上面 175 的 FINDTEXTFULL("hello") 走的是**另一条** canary 分支（INT_PTR，
        //   不截断），所以只测它**抓不到**这个缺陷 —— 这一格是必需的。
        TFind ft{ { 0, 11 }, "hello", { kChrgSentinelMin, kChrgSentinelMax } };
        SendMarker(184, (INT_PTR)Sci(SCI_FINDTEXT, 0, (LPARAM)&ft));
        SendMarker(185, (INT_PTR)ft.chrgText.cpMin);
        SendMarker(186, (INT_PTR)ft.chrgText.cpMax);
    }

    // ---- kRefuseHandle：进程私有 HDC ⇒ 永久拒答 ----------------------------
    {
        FmtRange fr{};
        ::memset(&fr, 'Z', sizeof(fr));
        SendMarker(178, (INT_PTR)Sci(SCI_FORMATRANGE, 0, (LPARAM)&fr));
        SendMarker(179, reinterpret_cast<const char*>(&fr)[0] == 'Z' ? 1 : 0);
    }

    // ---- 恶意/越界：必须拒答，且**不能**把代理拖死 -------------------------
    SendMarker(180, (INT_PTR)Sci(SCI_FINDTEXT, 0, (LPARAM)0x1000));   // 结构体不可读
    {
        TFind ft{ { 0, 11 }, nullptr, { kChrgSentinelMin, kChrgSentinelMax } };
        SendMarker(181, (INT_PTR)Sci(SCI_FINDTEXT, 0, (LPARAM)&ft));  // needle 空指针
    }
    {
        // ★ 故意**收窄**的那条：cpEnd > docLen 原生会留下"前面全是垃圾 + 末尾一个
        //   NUL"（CellBuffer::GetCharRange 一个字节都不写就返回）。桥接一律拒答。
        unsigned char buf[kBufSize];
        ::memset(buf, kCanary, sizeof(buf));
        TRange tr{ { 0, 2000 }, reinterpret_cast<char*>(buf) };
        SendMarker(182, (INT_PTR)Sci(SCI_GETTEXTRANGE, 0, (LPARAM)&tr));
        SendMarker(183, Untouched(buf));
    }

    SendMarker(74, 1);   // 三次恶意/越界调用之后，代理进程还在跑
    SendMarker(99, 1);
}

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-scistruct"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"SCI Struct Probe");
    items[0].func = probe;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
