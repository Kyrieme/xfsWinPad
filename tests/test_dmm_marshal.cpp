// test_dmm_marshal.cpp — 停靠族载荷布局（DmmMarshal）的守卫（批次 117）。
//
// 为什么单开一个测试目标：这是**两侧唯一事实源** —— 代理侧 DmmBuild* 把
// DockedWidgetData / 两个串**展平**成 wire 载荷，宿主侧 DmmParse* 在自己的
// 地址空间里重建。折进别的目标里，一旦"忘了注册进 CMakeLists"，缺测会伪装成
// 100% 通过；而这份布局恰恰是"结构体字段错位 / 串槽顺序颠倒 / 把外来地址当
// 串用"那类**静默错**的唯一防线（症状是面板名字乱码、按名查不到、或宿主越界读）。
//
// 覆盖：
//   [1] 布局：两个定长头的字节数与字段偏移（无填充）
//   [2] 往返：值 + 三串逐字段相等；**槽顺序**不能反（用等长不同内容的串）
//   [3] NULL 与空串**可分**（chars==0 vs chars==1）—— 这是 wire 契约的一部分
//   [4] 解析侧重验：截断 / chars 越界 / 末字节非 NUL ⇒ ok=false
//   [5] ★ 解析侧**不越界读**：载荷紧贴保护页 + 头里 chars 撒谎 ⇒ 拒答而非 AV
//   [6] 组装侧拒答：不可读指针 / 界内无 NUL / 恰好等于上限 ⇒ Build=false
//   [7] kDmmTwoStr 同理（wp 在前的顺序同样不能反）
//   [8] ★ 图标段（批次 137）：HICON → 图像块 → 重建 HICON 的往返（尺寸 + 像素
//       逐字节一致）；过大 / 顶穿载荷上限 ⇒ 退化为无图标而**不**拒答注册；
//       头里字节数撒谎 / 尾随字节 / 截断 ⇒ 拒答
//
// ★ 本守卫的边界（写进输出，避免下一个读它的人高估它）：
//   它证明的是**字节层**的自洽（build ↔ parse 互逆 + 边界拒答）。它**不**证明
//   "代理侧真的调用了 DmmBuild*"或"宿主侧真的用 parse 的结果去 SetParent"——
//   那两条由 test_oop 场景 16 的 e2e 负责（跨进程 SetParent + 通知中继）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/plugin/oop/DmmMarshal.h"
#include "../src/plugin/oop/OopProtocol.h"    // kNppmPayloadMax（图标段的第二道闸）

namespace oop = xfs::oop;
namespace npp = xfs::npp;

static int g_fail = 0;
// 段落内新增的失败数：段落标签靠它保持**诚实**（见 SectionEnd）。
// ★ 这个机制不是装饰：本文件第一版无条件打印 "[n] ...: ok"，负控（把 NULL 当
//   空串）下 [3]/[7] 明明各有 4 条 FAIL，末行却照样说 ok —— "grep ok" 正是核对
//   守卫的常规手段，标签说谎比没有标签更危险。test_nppm_marshal 的 117a 修过
//   同一个缺陷，这里从一开始就带上。
static int g_sinceLabel = 0;
#define FAIL() do { ++g_fail; ++g_sinceLabel; } while (0)
#define CHECK(cond) do { if (!(cond)) { FAIL(); \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void SectionEnd(const char* label) {
    if (g_sinceLabel == 0) {
        std::printf("%s: ok\n", label);
    } else {
        std::printf("%s: FAILED (%d new failure(s))\n", label, g_sinceLabel);
    }
    g_sinceLabel = 0;
}

static bool Same(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return a == b;
    return std::wcscmp(a, b) == 0;
}

// ---- [1] 布局 ---------------------------------------------------------------
static void TestLayout() {
    // 头的字段顺序 = 布局契约。偏移写错 ⇒ 宿主把 hClient 当 dlgID 用，静默错。
    CHECK(offsetof(oop::DmmRegHead, hClient) == 0);
    CHECK(offsetof(oop::DmmRegHead, uMask) == sizeof(void*));
    CHECK(offsetof(oop::DmmRegHead, dlgID) == 2 * sizeof(void*));
    CHECK(offsetof(oop::DmmRegHead, nameChars) == 2 * sizeof(void*) + sizeof(int));
    CHECK(offsetof(oop::DmmRegHead, addInfoChars) ==
          2 * sizeof(void*) + 2 * sizeof(int));
    CHECK(offsetof(oop::DmmRegHead, moduleChars) ==
          2 * sizeof(void*) + 3 * sizeof(int));
    CHECK(sizeof(oop::DmmRegHead) == 32);      // 无填充（static_assert 同款）
    CHECK(offsetof(oop::DmmIconHead, width) == 0);
    CHECK(offsetof(oop::DmmIconHead, height) == sizeof(unsigned));
    CHECK(offsetof(oop::DmmIconHead, colorBytes) == 2 * sizeof(unsigned));
    CHECK(offsetof(oop::DmmIconHead, maskBytes) == 3 * sizeof(unsigned));
    CHECK(sizeof(oop::DmmIconHead) == 16);     // 无填充（static_assert 同款）
    CHECK(offsetof(oop::DmmTwoStrHead, wpChars) == 0);
    CHECK(offsetof(oop::DmmTwoStrHead, lpChars) == sizeof(unsigned));
    CHECK(sizeof(oop::DmmTwoStrHead) == 8);
    SectionEnd("[1] layout");
}

// ---- [2] 往返 + 槽顺序 -------------------------------------------------------
static void TestRoundTrip() {
    npp::DockedWidgetData d{};
    d.hClient = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x1234));
    d.dlgID = 42;
    d.uMask = 0x30000004u;
    // ★ 三个串**等长、不同内容**：若 name/addInfo/module 的槽顺序被写反，
    //   长度校验仍然全过 ⇒ 只有逐字段内容比对才抓得到。
    d.pszName = L"AAA";
    d.pszAddInfo = L"BBB";
    d.pszModuleName = L"CCC";

    std::vector<unsigned char> buf;
    CHECK(oop::DmmBuildReg(d, buf));
    // 精确字节数：头 32 + (4+4+4) 个 wchar_t × 2 = 24 + 图标段 16（无图标）= 72。
    // 布局/编码任一漂移（加字段、改成 UTF-8、按字节计长、图标段位置变）都会变。
    CHECK(buf.size() == 72);
    CHECK(buf.size() == sizeof(oop::DmmRegHead) + 12 * sizeof(wchar_t) +
                            sizeof(oop::DmmIconHead));

    const oop::DmmRegView v =
        oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok);
    CHECK(v.hClient == 0x1234);
    CHECK(v.dlgID == 42);
    CHECK(v.uMask == 0x30000004u);
    CHECK(Same(v.name, L"AAA"));
    CHECK(Same(v.addInfo, L"BBB"));
    CHECK(Same(v.module, L"CCC"));

    // 长串（真实标题长度量级）也要原样往返
    d.pszName = L"D:\\proj\\plugins\\probe\\a-very-long-panel-title.dll";
    d.pszAddInfo = L"";
    d.pszModuleName = L"probe.dll";
    CHECK(oop::DmmBuildReg(d, buf));
    const oop::DmmRegView v2 =
        oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v2.ok);
    CHECK(Same(v2.name, d.pszName));
    CHECK(Same(v2.addInfo, L""));
    CHECK(Same(v2.module, L"probe.dll"));
    SectionEnd("[2] round-trip");
}

// ---- [3] NULL 与空串可分 -----------------------------------------------------
// DMMGETPLUGINHWNDBYNAME 的契约里 `windowName == NULL` 是"按模块名取首个"，
// 与"空串"是**两件事**（批次 115 的教训：wire 扩字段要让"不带串/空串"可分）。
// 所以这里必须同时断言"都能过"且"分得开"。
static void TestNullVsEmpty() {
    npp::DockedWidgetData d{};
    d.hClient = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x77));
    d.pszName = nullptr;
    d.pszAddInfo = nullptr;
    d.pszModuleName = nullptr;

    std::vector<unsigned char> buf;
    CHECK(oop::DmmBuildReg(d, buf));
    // 三个 NULL ⇒ 零尾随串字节；只剩头 + 恒在的图标段（无图标）
    CHECK(buf.size() == sizeof(oop::DmmRegHead) + sizeof(oop::DmmIconHead));
    oop::DmmRegView v = oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok);
    CHECK(v.name == nullptr && v.addInfo == nullptr && v.module == nullptr);

    // 空串：chars == 1，指针**非**空，首字符是 NUL
    d.pszName = L"";
    d.pszAddInfo = L"";
    d.pszModuleName = L"";
    CHECK(oop::DmmBuildReg(d, buf));
    CHECK(buf.size() ==
          sizeof(oop::DmmRegHead) + 3 * sizeof(wchar_t) + sizeof(oop::DmmIconHead));
    v = oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok);
    CHECK(v.name != nullptr && v.name[0] == L'\0');
    CHECK(v.addInfo != nullptr && v.addInfo[0] == L'\0');
    CHECK(v.module != nullptr && v.module[0] == L'\0');
    // ★ 可分性本身：NULL 与空串在视图上**不能**同形
    CHECK(!(v.name == nullptr));      // 空串必须给出非空指针

    // 混合：只有模块名有值（DMMGETPLUGINHWNDBYNAME 的常见调用）
    d.pszName = nullptr;
    d.pszAddInfo = nullptr;               // ★ 必须显式清掉上一段的 L""（否则测的是空串）
    d.pszModuleName = L"probe.dll";
    CHECK(oop::DmmBuildReg(d, buf));
    v = oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok);
    CHECK(v.name == nullptr);
    CHECK(v.addInfo == nullptr);
    CHECK(Same(v.module, L"probe.dll"));
    SectionEnd("[3] null-vs-empty");
}

// ---- [4] 解析侧全量重验 ------------------------------------------------------
static void TestParseValidation() {
    npp::DockedWidgetData d{};
    d.hClient = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x9));
    d.pszName = L"Panel";
    d.pszAddInfo = L"info";
    d.pszModuleName = L"m.dll";
    std::vector<unsigned char> good;
    CHECK(oop::DmmBuildReg(d, good));
    const unsigned total = static_cast<unsigned>(good.size());
    CHECK(oop::DmmParseReg(good.data(), total).ok);     // 正控：完整载荷必须过

    // (a) 空 / 过短 / 恰好短一个字节
    CHECK(!oop::DmmParseReg(nullptr, 0).ok);
    CHECK(!oop::DmmParseReg(nullptr, total).ok);
    CHECK(!oop::DmmParseReg(good.data(), 0).ok);
    CHECK(!oop::DmmParseReg(good.data(), sizeof(oop::DmmRegHead) - 1).ok);
    //     ★ 头齐了、三个串一个都没有 ⇒ 也必须拒答（头里 chars 非 0）。
    //       显式构造，避免依赖"载荷恰好怎么摆"的实现细节。
    {
        oop::DmmRegHead h{};
        h.nameChars = 6;                    // 说有一个 6 wchar_t 的串
        std::vector<unsigned char> onlyHead(sizeof(h));
        std::memcpy(onlyHead.data(), &h, sizeof(h));
        CHECK(!oop::DmmParseReg(onlyHead.data(),
                                static_cast<unsigned>(onlyHead.size())).ok);
    }
    // (b) 尾部每截断一个字节都必须拒答（截到只剩头时只剩 chars 全 0 的那份）
    for (unsigned cut = 1; cut <= total - sizeof(oop::DmmRegHead); ++cut) {
        if (oop::DmmParseReg(good.data(), total - cut).ok) {
            FAIL();
            std::printf("FAIL truncated by %u bytes was ACCEPTED\n", cut);
        }
    }
    // (c) chars 越界（改头里的字段；wire 是外来数据，必须重验而不是相信它）
    {
        std::vector<unsigned char> b = good;
        auto* h = reinterpret_cast<oop::DmmRegHead*>(b.data());
        h->nameChars = 0xFFFFFFFFu;
        CHECK(!oop::DmmParseReg(b.data(), static_cast<unsigned>(b.size())).ok);
        h->nameChars = 1000u;               // 不溢出但超出总字节
        CHECK(!oop::DmmParseReg(b.data(), static_cast<unsigned>(b.size())).ok);
        h->nameChars = oop::kDmmStrCharsMax + 1;   // 超上限
        CHECK(!oop::DmmParseReg(b.data(), static_cast<unsigned>(b.size())).ok);
    }
    // (d) 末字节不是 NUL：宿主会把一个"没有结尾的串"当串用 ⇒ 越界读
    {
        std::vector<unsigned char> b = good;
        b.back() = 0x41;                    // 破坏最后一个 NUL
        CHECK(!oop::DmmParseReg(b.data(), static_cast<unsigned>(b.size())).ok);
        // 只破坏**第一段**的结尾 NUL（偏移 = 头 + (6-1)*2）也要拒答
        std::vector<unsigned char> c = good;
        c[sizeof(oop::DmmRegHead) + 5 * sizeof(wchar_t)] = 0x41;
        CHECK(!oop::DmmParseReg(c.data(), static_cast<unsigned>(c.size())).ok);
    }
    // (e) ★ 尾随字节**必须拒答**（批次 137 起口径反转）。
    //     旧口径是"多余的忽略"—— 那时载荷尾就是最后一个串。图标段进来之后，
    //     尾随字节与"图标段被截断/拼接"同形：多一字节就意味着图标段没对齐，
    //     而它后面要被 memcpy 交给 GDI。允许忽略等于让畸形载荷冒充合法载荷。
    {
        std::vector<unsigned char> b = good;
        b.push_back(0xEE);
        CHECK(!oop::DmmParseReg(b.data(), static_cast<unsigned>(b.size())).ok);
    }
    SectionEnd("[4] parse-validation");
}

// ---- [5] ★ 解析侧不越界读（紧贴保护页 + 头里撒谎）-----------------------------
// 这是本文件最有力的一条：其余断言只证明"返回 false"，这一条证明"**没有**读过界"。
// 若 ReadWideStr 不看 bytes 就按头里的 chars 取串，它会在 PAGE_NOACCESS 上越界读
// ⇒ 本进程当场 AV（可读的红：前面所有 PASS 已落盘，进程随后死）。
static void TestNoOverread() {
    auto* raw = static_cast<unsigned char*>(
        ::VirtualAlloc(nullptr, 8192, MEM_COMMIT, PAGE_READWRITE));
    CHECK(raw != nullptr);
    if (!raw) { std::printf("[5] no-overread: SKIPPED (alloc failed)\n"); return; }
    DWORD old = 0;
    CHECK(::VirtualProtect(raw + 4096, 4096, PAGE_NOACCESS, &old) != FALSE);

    // 载荷紧贴保护页：头 + 8 字节（4 个 wchar_t 的串）+ 16 字节图标段（width==0）
    // —— 图标段必须一起摆进来，否则"刚好放得下"的负控会因为**缺图标段**而拒答，
    // 那就证不出"是按 bytes 校验的"了。
    oop::DmmRegHead h{};
    h.hClient = 0x1234;
    h.uMask = 0;
    h.dlgID = 1;
    h.nameChars = 64;                       // 撒谎：实际只放得下 4 个 wchar_t
    h.addInfoChars = 0;
    h.moduleChars = 0;
    const unsigned tail = 8 + static_cast<unsigned>(sizeof(oop::DmmIconHead));
    const unsigned payload = static_cast<unsigned>(sizeof(h)) + tail;
    unsigned char* at = raw + 4096 - payload;
    std::memcpy(at, &h, sizeof(h));
    std::memset(at + sizeof(h), 0, tail);

    // 到这里为止没崩，就说明它按 bytes 拒答了
    CHECK(!oop::DmmParseReg(at, payload).ok);

    // 同样的位置，头里 chars 改成"刚好放得下"⇒ 必须**通过**（否则上面的拒答
    // 可能只是"碰巧越界"，而不是"真的按 bytes 校验"）
    h.nameChars = 4;
    std::memcpy(at, &h, sizeof(h));
    CHECK(oop::DmmParseReg(at, payload).ok);

    ::VirtualFree(raw, 0, MEM_RELEASE);
    SectionEnd("[5] no-overread");
}

// ---- [6] 组装侧拒答（有界扫描）-----------------------------------------------
static void TestBuildRefusal() {
    npp::DockedWidgetData d{};
    d.hClient = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x1));
    std::vector<unsigned char> buf;

    // (a) 指向不可读页的串 ⇒ 拒答（代理侧不能把外来指针拿去无界扫描）
    void* rsv = ::VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(rsv != nullptr);
    d.pszName = reinterpret_cast<const wchar_t*>(rsv);
    d.pszAddInfo = nullptr;
    d.pszModuleName = nullptr;
    CHECK(!oop::DmmBuildReg(d, buf));

    // (b) 界内没有 NUL ⇒ 拒答（合法但未终止的缓冲不能让代理读穿）
    {
        std::vector<wchar_t> noNul(oop::kDmmStrCharsMax + 8, L'A');
        d.pszName = noNul.data();
        CHECK(!oop::DmmBuildReg(d, buf));
    }
    // (c) 恰好等于上限（含 NUL 共 kDmmStrCharsMax 个）⇒ **通过**（边界在内侧）
    {
        std::vector<wchar_t> exact(oop::kDmmStrCharsMax, L'A');
        exact.back() = L'\0';
        d.pszName = exact.data();
        CHECK(oop::DmmBuildReg(d, buf));
        const oop::DmmRegView v =
            oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
        CHECK(v.ok);
        CHECK(v.name != nullptr && v.name[0] == L'A' &&
              v.name[oop::kDmmStrCharsMax - 2] == L'A' &&
              v.name[oop::kDmmStrCharsMax - 1] == L'\0');
    }
    // (d) 超一个字符 ⇒ 拒答（"上限+1"这一侧必须有闸）
    {
        std::vector<wchar_t> over(oop::kDmmStrCharsMax + 1, L'A');
        over.back() = L'\0';
        d.pszName = over.data();
        CHECK(!oop::DmmBuildReg(d, buf));
    }
    // (e) 第二/第三个串也会被验（不是只验第一个）
    {
        d.pszName = L"ok";
        d.pszAddInfo = L"ok";
        d.pszModuleName = reinterpret_cast<const wchar_t*>(rsv);
        CHECK(!oop::DmmBuildReg(d, buf));
        d.pszModuleName = nullptr;
        d.pszAddInfo = reinterpret_cast<const wchar_t*>(rsv);
        CHECK(!oop::DmmBuildReg(d, buf));
    }
    // (f) 拒答后 out 里不能留下"半个载荷"（调用方可能直接把它发出去）
    {
        std::vector<unsigned char> junk(64, 0xCD);
        d.pszName = L"ok";
        d.pszAddInfo = nullptr;
        d.pszModuleName = nullptr;
        CHECK(oop::DmmBuildReg(d, junk));
        const std::size_t okSize = junk.size();
        d.pszName = reinterpret_cast<const wchar_t*>(rsv);
        CHECK(!oop::DmmBuildReg(d, junk));
        CHECK(junk.size() == sizeof(oop::DmmRegHead));   // 清空后只剩头
        CHECK(okSize > junk.size());
    }
    ::VirtualFree(rsv, 0, MEM_RELEASE);
    SectionEnd("[6] build-refusal");
}

// ---- [7] kDmmTwoStr ----------------------------------------------------------
static void TestTwoStr() {
    std::vector<unsigned char> buf;
    // 正控：两个串（等长不同内容 ⇒ 顺序写反会被抓）
    CHECK(oop::DmmBuildTwoStr(L"WWW", L"LLL", buf));
    CHECK(buf.size() == sizeof(oop::DmmTwoStrHead) + 8 * sizeof(wchar_t));
    oop::DmmTwoStrView v =
        oop::DmmParseTwoStr(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok);
    CHECK(Same(v.wp, L"WWW"));        // ★ wp 在**前**（这是全表唯一把指针放 wp 的形状）
    CHECK(Same(v.lp, L"LLL"));

    // NULL 一侧（契约：windowName == NULL ⇒ 只按模块名查）
    CHECK(oop::DmmBuildTwoStr(nullptr, L"m.dll", buf));
    v = oop::DmmParseTwoStr(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok && v.wp == nullptr && Same(v.lp, L"m.dll"));

    CHECK(oop::DmmBuildTwoStr(L"Panel", nullptr, buf));
    v = oop::DmmParseTwoStr(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok && Same(v.wp, L"Panel") && v.lp == nullptr);

    // 空串 vs NULL 同样要可分
    CHECK(oop::DmmBuildTwoStr(L"", L"", buf));
    v = oop::DmmParseTwoStr(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok && v.wp != nullptr && v.wp[0] == L'\0' &&
          v.lp != nullptr && v.lp[0] == L'\0');

    // 两个都是 NULL ⇒ 只有头
    CHECK(oop::DmmBuildTwoStr(nullptr, nullptr, buf));
    CHECK(buf.size() == sizeof(oop::DmmTwoStrHead));
    v = oop::DmmParseTwoStr(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok && v.wp == nullptr && v.lp == nullptr);

    // 解析侧重验：截断 / chars 越界 / 末字节非 NUL
    CHECK(oop::DmmBuildTwoStr(L"Panel", L"m.dll", buf));
    const unsigned total = static_cast<unsigned>(buf.size());
    CHECK(!oop::DmmParseTwoStr(nullptr, 0).ok);
    CHECK(!oop::DmmParseTwoStr(nullptr, total).ok);
    CHECK(!oop::DmmParseTwoStr(buf.data(), sizeof(oop::DmmTwoStrHead) - 1).ok);
    for (unsigned cut = 1; cut <= total - sizeof(oop::DmmTwoStrHead); ++cut)
        if (oop::DmmParseTwoStr(buf.data(), total - cut).ok) {
            FAIL();
            std::printf("FAIL two-str truncated by %u bytes was ACCEPTED\n", cut);
        }
    {
        std::vector<unsigned char> b = buf;
        auto* h = reinterpret_cast<oop::DmmTwoStrHead*>(b.data());
        h->wpChars = 0xFFFFFFFFu;
        CHECK(!oop::DmmParseTwoStr(b.data(), static_cast<unsigned>(b.size())).ok);
        h->wpChars = 6;
        h->lpChars = 999u;
        CHECK(!oop::DmmParseTwoStr(b.data(), static_cast<unsigned>(b.size())).ok);
    }
    {
        std::vector<unsigned char> b = buf;
        b.back() = 0x41;                  // 破坏 lp 的结尾 NUL
        CHECK(!oop::DmmParseTwoStr(b.data(), static_cast<unsigned>(b.size())).ok);
        std::vector<unsigned char> c = buf;
        c[sizeof(oop::DmmTwoStrHead) + 5 * sizeof(wchar_t)] = 0x41;  // wp 的结尾 NUL
        CHECK(!oop::DmmParseTwoStr(c.data(), static_cast<unsigned>(c.size())).ok);
    }
    // 组装侧：不可读 / 无 NUL / 超上限（两侧都要验，不是只验 wp）
    {
        void* rsv = ::VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
        CHECK(rsv != nullptr);
        CHECK(!oop::DmmBuildTwoStr(reinterpret_cast<const wchar_t*>(rsv), L"x", buf));
        CHECK(!oop::DmmBuildTwoStr(L"x", reinterpret_cast<const wchar_t*>(rsv), buf));
        std::vector<wchar_t> over(oop::kDmmStrCharsMax + 1, L'A');
        over.back() = L'\0';
        CHECK(!oop::DmmBuildTwoStr(over.data(), L"x", buf));
        CHECK(!oop::DmmBuildTwoStr(L"x", over.data(), buf));
        ::VirtualFree(rsv, 0, MEM_RELEASE);
    }
    SectionEnd("[7] two-str");
}

// ---- [8] 图标段（批次 137）---------------------------------------------------
// 造一个真 HICON：32bpp 彩色 + 1bpp 掩码，与解码侧同款构造（DmmIconFromView）。
// 像素值随位置变化（不是行常量）—— 这样"上下颠倒"这类行序错只有逐像素比对
// 才抓得到：一张图看着"像原图"仍然可能是翻过来的。
static HICON MakeTestIcon(int w, int h, BYTE seed) {
    HDC dc = ::GetDC(nullptr);
    if (!dc) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* cbits = nullptr;
    HBITMAP color = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &cbits, nullptr, 0);
    struct M { BITMAPINFOHEADER h; RGBQUAD pal[2]; } mb{};
    mb.h.biSize = sizeof(BITMAPINFOHEADER);
    mb.h.biWidth = w;
    mb.h.biHeight = -h;
    mb.h.biPlanes = 1;
    mb.h.biBitCount = 1;
    mb.h.biCompression = BI_RGB;
    mb.pal[1].rgbBlue = mb.pal[1].rgbGreen = mb.pal[1].rgbRed = 255;
    void* mbits = nullptr;
    HBITMAP mask = ::CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&mb),
                                      DIB_RGB_COLORS, &mbits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (!color || !mask || !cbits || !mbits) {
        if (color) ::DeleteObject(color);
        if (mask) ::DeleteObject(mask);
        return nullptr;
    }
    DWORD* px = static_cast<DWORD*>(cbits);
    for (int i = 0; i < w * h; ++i)
        px[i] = 0xFF000000u | (DWORD)((i * 37 + seed) & 0xFF) * 0x00010101u;
    std::memset(mbits, 0, static_cast<std::size_t>(((w + 31) / 32) * 4) * h);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = ::CreateIconIndirect(&ii);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return ic;
}

// 把一个 HICON 的颜色位取出来（32bpp top-down）。**参考值**由它给：解码侧与它
// 走同款 GDI 路径 ⇒ 两者相等就说明"代理编的字节 = 图标真像素"，且反向亦然。
static bool ReadIconColor(HICON ic, int w, int h, std::vector<unsigned char>& out) {
    ICONINFO ii{};
    if (!ic || !::GetIconInfo(ic, &ii)) return false;
    bool ok = false;
    if (ii.hbmColor) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.assign(static_cast<std::size_t>(w) * 4 * h, 0);
        HDC dc = ::GetDC(nullptr);
        ok = dc && ::GetDIBits(dc, ii.hbmColor, 0, (UINT)h, out.data(), &bi,
                               DIB_RGB_COLORS) != 0;
        if (dc) ::ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
    if (ii.hbmMask) ::DeleteObject(ii.hbmMask);
    return ok;
}

static void TestIconSegment() {
    const int W = 15, H = 9;                       // 宽 15 ⇒ 掩码行跨度 4 字节
    const unsigned colorBytes = static_cast<unsigned>(W) * 4u * static_cast<unsigned>(H);
    const unsigned maskBytes = ((static_cast<unsigned>(W) + 31) / 32) * 4u * static_cast<unsigned>(H);
    HICON ic = MakeTestIcon(W, H, 5);
    CHECK(ic != nullptr);

    npp::DockedWidgetData d{};
    d.hClient = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x55));
    d.dlgID = 9;
    d.pszName = L"P";                              // 2 wchar_t
    d.pszModuleName = L"m.dll";                    // 6 wchar_t（addInfo 为 NULL）
    d.hIconTab = ic;

    std::vector<unsigned char> buf;
    CHECK(oop::DmmBuildReg(d, buf));
    CHECK(buf.size() == sizeof(oop::DmmRegHead) + 8 * sizeof(wchar_t) +
                            sizeof(oop::DmmIconHead) + colorBytes + maskBytes);

    oop::DmmRegView v = oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok);
    CHECK(v.iconWidth == (unsigned)W && v.iconHeight == (unsigned)H);
    CHECK(v.iconColorBytes == colorBytes && v.iconMaskBytes == maskBytes);
    CHECK(v.iconColor != nullptr && v.iconMask != nullptr);
    // 编码保真：wire 上的颜色位 == 直接从原图标取出的颜色位（逐字节）
    if (v.ok && ic) {
        std::vector<unsigned char> ref;
        CHECK(ReadIconColor(ic, W, H, ref));
        CHECK(ref.size() == colorBytes);
        CHECK(std::memcmp(ref.data(), v.iconColor, colorBytes) == 0);
    }
    // 解码保真：重建的 HICON 与原件像素逐字节相同（必须是**另一个**句柄）
    HICON back = oop::DmmIconFromView(v);
    CHECK(back != nullptr);
    CHECK(back != ic);
    if (back) {
        std::vector<unsigned char> ref2;
        CHECK(ReadIconColor(back, W, H, ref2));
        CHECK(ref2.size() == colorBytes);
        CHECK(ref2.size() == colorBytes &&
              std::memcmp(ref2.data(), v.iconColor, colorBytes) == 0);
        ::DestroyIcon(back);
    }

    // (b) 无图标：解析通过、视图 width==0、重建得 nullptr
    d.hIconTab = nullptr;
    CHECK(oop::DmmBuildReg(d, buf));
    v = oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
    CHECK(v.ok && v.iconWidth == 0 && v.iconColor == nullptr);
    CHECK(oop::DmmIconFromView(v) == nullptr);

    // (c) 过大图标（64×64 ⇒ colorBytes 16384 > kDmmIconBytesMax）⇒ **退化为无图标**
    //     而不是拒答注册（Build 仍 true，面板照常注册）
    if (HICON big = MakeTestIcon(64, 64, 1)) {
        d.hIconTab = big;
        CHECK(oop::DmmBuildReg(d, buf));
        v = oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
        CHECK(v.ok && v.iconWidth == 0);
        ::DestroyIcon(big);
    }

    // (d) 顶穿整单载荷上限 ⇒ 同样退化（否则代理侧的总量闸会拒掉**整个**注册）
    //     15×128:色 4*15*128=7680 + 掩码 4*128=512 = 8192（刚好不被图标上限拦），
    //     而 头32 + 三串 3×4096×2 + 图标头16 + 8192 = 32816 > kNppmPayloadMax。
    {
        std::vector<wchar_t> bigStr(oop::kDmmStrCharsMax, L'A');
        bigStr.back() = L'\0';
        d.pszName = bigStr.data();
        d.pszAddInfo = bigStr.data();
        d.pszModuleName = bigStr.data();
        if (HICON tall = MakeTestIcon(15, 128, 2)) {
            d.hIconTab = tall;
            CHECK(oop::DmmBuildReg(d, buf));
            CHECK(buf.size() <= oop::kNppmPayloadMax);
            v = oop::DmmParseReg(buf.data(), static_cast<unsigned>(buf.size()));
            CHECK(v.ok && v.iconWidth == 0);
            ::DestroyIcon(tall);
        }
        d.pszName = L"P";
        d.pszAddInfo = nullptr;
        d.pszModuleName = L"m.dll";
        d.hIconTab = nullptr;
    }

    // (e) 头里骗人 / 尾随 / 截断都要拒答（wire 是外来数据）
    if (ic) {
        d.hIconTab = ic;
        CHECK(oop::DmmBuildReg(d, buf));
        const unsigned total = static_cast<unsigned>(buf.size());
        const unsigned iconAt =
            static_cast<unsigned>(sizeof(oop::DmmRegHead) + 8 * sizeof(wchar_t));
        {   // colorBytes 撒谎（+4）：必须与 width*4*height 重算值相等
            std::vector<unsigned char> b = buf;
            auto* ih = reinterpret_cast<oop::DmmIconHead*>(b.data() + iconAt);
            ih->colorBytes += 4;
            CHECK(!oop::DmmParseReg(b.data(), total).ok);
        }
        {   // width==0 却带着非零 colorBytes：头自相矛盾
            std::vector<unsigned char> b = buf;
            auto* ih = reinterpret_cast<oop::DmmIconHead*>(b.data() + iconAt);
            ih->width = 0;
            CHECK(!oop::DmmParseReg(b.data(), total).ok);
        }
        {   // 尾随一个字节
            std::vector<unsigned char> b = buf;
            b.push_back(0xEE);
            CHECK(!oop::DmmParseReg(b.data(), static_cast<unsigned>(b.size())).ok);
        }
        // 图标段内每一处截断都要拒答（含"整段砍掉"）
        for (unsigned cut = 1; cut <= total - iconAt; ++cut)
            if (oop::DmmParseReg(buf.data(), total - cut).ok) {
                FAIL();
                std::printf("FAIL icon truncated by %u bytes was ACCEPTED\n", cut);
            }
    }
    if (ic) ::DestroyIcon(ic);
    SectionEnd("[8] icon-segment");
}

int main() {
    // ★ 本守卫的 [5] 专门检测"解析侧越界读"——它的负控形态是**本进程当场 AV**。
    //   若 stdout 是块缓冲（重定向到文件时就是），AV 会把已打印的 PASS 一起丢掉，
    //   负控就变成"零输出"（批次 111 实测过）。所以这里强制无缓冲。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== test_dmm_marshal: NPPM_DMM* payload layout guard ==\n");
    TestLayout();
    TestRoundTrip();
    TestNullVsEmpty();
    TestParseValidation();
    TestNoOverread();
    TestBuildRefusal();
    TestTwoStr();
    TestIconSegment();
    std::printf("NOTE: this guard proves the BYTE layer (build <-> parse + bounds).\n");
    std::printf("      It does NOT prove that the proxy/host actually call it, nor\n");
    std::printf("      that cross-process SetParent works -- that is test_oop scen 16.\n");
    std::printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
