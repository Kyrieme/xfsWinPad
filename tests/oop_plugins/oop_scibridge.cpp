// oop_scibridge.cpp — 进程外桥 e2e 用插件：验证 SCI 入参指针过桥（批次 113）。
//
// 与其它 oop_* 夹具同规则：不包含宿主头文件，手写公开契约布局。
//
// 与 oop_sci 的差别：oop_sci 验证"值类型直发 + 不可桥接族拒答"（批次 112）；
// 本夹具验证**入参族（kInStr + kInBytes，54 条）真的过桥了**。54 条里有三种
// 反直觉形态，这里每一样都有一条电池 —— 因为它们错了都**不崩**，只是行为错：
//   * 33 条的串在 **lParam**（含 SETTEXT / REPLACESEL —— iface 写的是
//     `(, string text)`：槽 0 空、串在槽 1）⇒ 只看 wp 会"文本没改"；
//   * 7 条的串在 **wParam**（首参非空的那 7 条，如 CLEARREPRESENTATION
//     `(string encodedCharacter,)`）⇒ 只看 lp 会**把 0 当串指针**（原生直接崩）；
//   * 2 条 wp 与 lp 都是串（SETPROPERTY/SETREPRESENTATION）⇒ 要内联两段；
//   * AUTOCSHOW 的首参 lengthEntered 是"已输入字符数"而**不是**缓冲长度 ⇒
//     当长度用会把补全列表截断（本夹具用 "one two three" + lenEntered=3 钉它）。
//
// ⚠ 批次 116 修了一处**真缺陷**：批次 113 的生成器把 iface 的**空参数槽**压掉了，
//   于是 SETTEXT / REPLACESEL 等 12 条被判成"串在 wParam"。而本夹具当时是照着
//   同一张错表写的（把串塞进 wParam）⇒ 与假 Scintilla **自洽**、测试全绿，
//   真机上却是"文本没改"。现在 62/63 按**真实调用方**的写法给参数（串在 lp），
//   并在 tests/test_sci_bridge.cpp [10] 加了"对真 Scintilla 反证"的判据。
//
// 另一半电池是**拒答方向**：指针不可读 / 长度超限 / 串未终止且越界，都必须
// 换来"返回 0 且代理进程还活着" —— 隔离的意义就在这里（这些在进程内调用
// 会让宿主直接崩）。
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

// Sci_RangeToFormat 的公开布局（手写镜像，不 include Scintilla 头）：
// HDC / HDC / 2×RECT / CHARRANGE = 56 字节（**没有** HWND，见 Scintilla.h:1400）。
// 用途只有一个：当作"结构族拒答桶"的入参，钉住"过桥层不许碰它"。
struct FmtRange {
    void* hdc;
    void* hdcTarget;
    long rc[4];
    long rcPage[4];
    long cpMin;
    long cpMax;
};

// ---- 线上消息常量（与 src/plugin/scintilla 同源事实值）----------------------
#define SCI_ADDTEXT           2001   // (length, text)      长度桶
#define SCI_ADDSTYLEDTEXT     2002   // (length, cells)     长度桶 + 裸字节
#define SCI_INSERTTEXT        2003   // (position, text)    NUL 扫描
#define SCI_AUTOCSHOW         2100   // (lengthEntered, itemList) ★ 首参不是长度
#define SCI_REPLACESEL        2170   // (, text)            串在 **lp**（槽 0 空）
#define SCI_SETTEXT           2181   // (, text)            串在 **lp**（槽 0 空）
#define SCI_FORMATRANGE       2151   // (bool, fr) ★ 结构族里的**拒答桶**：
                                     // 结构里的 hdc 是**调用进程私有的 GDI 句柄**
                                     // ⇒ 永远不过桥（批次 116 定案）。
                                     // ⚠ 别拿 GETSTYLEDTEXT(2015) / GETTEXTRANGE(2162)
                                     //   当"仍拒答"样例：批次 116 起它们**已可过桥**
                                     //   （它们不带外来指针，正向电池在 oop_scistruct.cpp）
                                     // ⚠ 4005 是 SetKeyWords（**可**过桥），别写错
#define SCI_APPENDTEXT        2282   // (length, text)      长度桶
#define SCI_SEARCHINTARGET    2197   // (length, pattern)   长度桶
#define SCI_SETREPRESENTATION 2665   // (a, b)              双指针
#define SCI_SETPROPERTY       4004   // (key, value)        双指针

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

// 造一个"4 字节可读、无 NUL，紧接着就是不可读页"的指针。
// 用途：钉住"未终止且越界 ⇒ 拒答，而不是越界读死代理"。进程内调用同样会崩，
// 所以这是隔离能力本身的断言，不是产品缺陷。
static char* g_tailBase = nullptr;

static char* MakeUnterminatedTail() {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const SIZE_T pg = si.dwPageSize;
    char* base = static_cast<char*>(
        ::VirtualAlloc(nullptr, pg * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!base) return nullptr;
    ::memset(base, 'Z', pg * 2);          // 整两页都是 'Z'：没有 NUL
    DWORD old = 0;
    ::VirtualProtect(base + pg, pg, PAGE_NOACCESS, &old);
    g_tailBase = base;
    return base + pg - 4;                 // 只剩 4 字节可读
}

// 标记 tag 分配（测试侧按同一编号断言）：
//   60 ADDTEXT(5,"alpha")        61 APPENDTEXT(4,"beta")
//   62 SETTEXT("gamma")          63 REPLACESEL("delta")   ← 串在 **lp**
//   64 SETPROPERTY("k1","v1")    65 SETREPRESENTATION("rep","REPX")
//   66 AUTOCSHOW(3,"one two three")   ★ 截断守门
//   67 ADDSTYLEDTEXT(4, {a,1,b,2})
//   68 SEARCHINTARGET(5,"hello") 69 INSERTTEXT(7,"insert")
//   70 FORMATRANGE（结构族拒答桶，进程私有 HDC）  71 指针不可读 ⇒ 拒答
//   72 长度超限 ⇒ 拒答            73 串未终止且越界 ⇒ 拒答
//   74 代理在三次拒答后仍活着     75 拒答没碰调用方内存（哨兵完好）
//   99 全部完成
// ⚠ 别拿 GETTEXT(2182) / GETPROPERTY(4008) / GETSTYLEDTEXT(2015) 当"仍拒答"样例：
//   它们分别在批次 114 / 115 / 116 **已可过桥**，电池在 oop_sciout.cpp /
//   oop_sciinout.cpp / oop_scistruct.cpp。这里用的是结构族里**永久**拒答的那一支
//   （进程私有 HDC），过桥层按消息号就拒，绝不会碰调用方内存。
static void probe(void) {
    // ---- 过桥方向 ----------------------------------------------------------
    SendMarker(60, (INT_PTR)Sci(SCI_ADDTEXT, 5, (LPARAM)"alpha"));
    SendMarker(61, (INT_PTR)Sci(SCI_APPENDTEXT, 4, (LPARAM)"beta"));
    // 串在 **lParam** 的两条（iface: `(, string text)` —— 槽 0 空）：
    // ⚠ 这是**真实调用方**的写法。批次 113 的表把这两条错判成"串在 wp"，
    //   本夹具当时也跟着把串塞进 wp ⇒ 两边自洽、全绿，真机上却"文本没改"。
    SendMarker(62, (INT_PTR)Sci(SCI_SETTEXT, 0, (LPARAM)"gamma"));
    SendMarker(63, (INT_PTR)Sci(SCI_REPLACESEL, 0, (LPARAM)"delta"));
    // 双指针两条：只搬一段的实现会丢掉第二段
    SendMarker(64, (INT_PTR)Sci(SCI_SETPROPERTY, (WPARAM)"k1", (LPARAM)"v1"));
    SendMarker(65, (INT_PTR)Sci(SCI_SETREPRESENTATION, (WPARAM)"rep", (LPARAM)"REPX"));
    // ★ lengthEntered=3 而列表长 13：把它当缓冲长度会只剩 "one"
    SendMarker(66, (INT_PTR)Sci(SCI_AUTOCSHOW, 3, (LPARAM)"one two three"));
    // cells：裸字节缓冲，wp 是字节数
    {
        static const char cells[4] = { 'a', 1, 'b', 2 };
        SendMarker(67, (INT_PTR)Sci(SCI_ADDSTYLEDTEXT, 4, (LPARAM)cells));
    }
    SendMarker(68, (INT_PTR)Sci(SCI_SEARCHINTARGET, 5, (LPARAM)"hello"));
    SendMarker(69, (INT_PTR)Sci(SCI_INSERTTEXT, 7, (LPARAM)"insert"));

    // ---- 拒答方向（每一条在进程内都会崩或静默出错）--------------------------
    {
        // kStruct 里的**永久拒答**支：FormatRange 的 hdc/hdcTarget 是**调用进程
        // 私有**的 GDI 句柄，且 FormatRange 本身是**绘制**操作 ⇒ 语义上不可桥接。
        // ⚠ 别拿 GETSTYLEDTEXT(2015) 当"仍拒答"样例：批次 116 起结构族里不带外来
        //   指针的 6 条**已可过桥**（正向电池在 oop_scistruct.cpp），且它们过桥后
        //   的真值（空范围/空串）也返回 0，与"拒答返回 0"同形。
        // 缓冲先填 'Z' 哨兵：只有它能把"没被碰过"与"被写了 0"区分开（marker 75）。
        FmtRange fr{};
        ::memset(&fr, 'Z', sizeof(fr));
        SendMarker(70, (INT_PTR)Sci(SCI_FORMATRANGE, 0, (LPARAM)&fr));
        SendMarker(75, reinterpret_cast<const char*>(&fr)[0] == 'Z' ? 1 : 0);
    }
    SendMarker(71, (INT_PTR)Sci(SCI_INSERTTEXT, 0, (LPARAM)0x1000));  // 未映射地址
    SendMarker(72, (INT_PTR)Sci(SCI_ADDTEXT, 40000, (LPARAM)"x"));    // 超上限
    {
        char* tail = MakeUnterminatedTail();
        SendMarker(73, (INT_PTR)Sci(SCI_INSERTTEXT, 0, (LPARAM)tail));
        if (g_tailBase) { ::VirtualFree(g_tailBase, 0, MEM_RELEASE); g_tailBase = nullptr; }
    }
    SendMarker(74, 1);   // 三次恶意/越界调用之后，代理进程还在跑

    SendMarker(99, 1);
}

extern "C" __declspec(dllexport) void setInfo(NPData* d) { if (d) g_data = *d; }
extern "C" __declspec(dllexport) const wchar_t* getName(void) { return L"oop-scibridge"; }
extern "C" __declspec(dllexport) BOOL isUnicode(void) { return TRUE; }
extern "C" __declspec(dllexport) FI* getFuncsArray(int* nbF) {
    if (!nbF) return nullptr;
    *nbF = 1;
    wcscpy_s(items[0].itemName, L"SCI Bridge Probe");
    items[0].func = probe;
    items[0].cmdID = 0;
    items[0].initCheck = false;
    items[0].shortcut = &skA;
    return items;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return 0; }
