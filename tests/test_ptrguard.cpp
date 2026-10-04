// test_ptrguard.cpp — 外来指针判据单测（npp/NppPointerGuard.h）。
//
// 为什么单独一个目标：NPPM_* 编号在 WM_USER 之上 ⇒ Windows **不封送参数**。
// 进程外插件（OOP 代理进程）把 NPPM_* 发给宿主主窗口时，wp/lp 里的指针是
// 代理进程地址空间的地址；宿主一旦解引用就是访问违例（那些分支没有 SEH）。
// 判据必须**保守**：合法调用方的指针一定通过（不误报），外来/不可用指针
// 一定不通过（不崩）。本测试把两侧都钉住。
//
// 判据"在量对的东西"的自证方式（约束 21）：对每个"不可用"用例，都同时断言
// **它为什么不可用**（例如只读页：可读为真、可写为假），而不是只断言
// "和上一个不一样"——否则把判据改成恒 false 也能全绿。
#include "../src/plugin/npp/NppPointerGuard.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {
constexpr SIZE_T kPage = 4096;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== test_ptrguard: foreign-pointer predicate ==\n");

    // ---- 1. 本进程自己的合法指针：必须通过（不误报）--------------------------
    {
        wchar_t stackBuf[64] = {};
        CHECK(npp::LocalReadable(stackBuf, sizeof(stackBuf)));
        CHECK(npp::LocalWritable(stackBuf, sizeof(stackBuf)));
        CHECK(npp::LocalReadable(stackBuf, sizeof(stackBuf) - 1));
        CHECK(npp::LocalWideStrChars(stackBuf, 64) == 1);   // 空串 ⇒ 含 NUL 共 1

        auto* heap = new wchar_t[128];
        CHECK(npp::LocalReadable(heap, 128 * sizeof(wchar_t)));
        CHECK(npp::LocalWritable(heap, 128 * sizeof(wchar_t)));
        wcscpy_s(heap, 128, L"abc");
        CHECK(npp::LocalWideStrChars(heap, 128) == 4);
        // 界比串短：界内没有 NUL ⇒ 0（宁可判不可用，也不越界扫）
        CHECK(npp::LocalWideStrChars(heap, 3) == 0);
        delete[] heap;
    }

    // ---- 2. 空指针与零长度 ---------------------------------------------------
    {
        CHECK(!npp::LocalReadable(nullptr, 8));
        CHECK(!npp::LocalWritable(nullptr, 8));
        CHECK(!npp::LocalWideStrChars(nullptr, 8));
        // 零长度：无需触碰内存 ⇒ 通过（调用方不会真的读）
        wchar_t c = 0;
        CHECK(npp::LocalReadable(&c, 0));
        CHECK(npp::LocalWritable(&c, 0));
        CHECK(npp::LocalWideStrChars(&c, 0) == 0);
    }

    // ---- 3. 长度溢出 ---------------------------------------------------------
    {
        wchar_t c = 0;
        CHECK(!npp::LocalReadable(&c, static_cast<std::size_t>(-1)));
        CHECK(!npp::LocalWritable(&c, static_cast<std::size_t>(-1)));
    }

    // ---- 4. 只读页：可读、不可写（写侧判据的靶子）----------------------------
    {
        void* ro = ::VirtualAlloc(nullptr, kPage, MEM_COMMIT, PAGE_READONLY);
        CHECK(ro != nullptr);
        CHECK(npp::LocalReadable(ro, 16));        // ← 证明"能读"
        CHECK(!npp::LocalWritable(ro, 16));       // ← 但"不能写"
        CHECK(!npp::LocalWritable(ro, 1));
        CHECK(npp::LocalReadable(ro, kPage));     // 整页可读
        ::VirtualFree(ro, 0, MEM_RELEASE);
    }

    // ---- 5. 无访问页 / 保留区 / 空闲区：不可读也不可写 ------------------------
    {
        void* na = ::VirtualAlloc(nullptr, kPage, MEM_COMMIT, PAGE_NOACCESS);
        CHECK(na != nullptr);
        CHECK(!npp::LocalReadable(na, 1));
        CHECK(!npp::LocalWritable(na, 1));

        // 保留但未提交：整段都不可用
        void* rsv = ::VirtualAlloc(nullptr, kPage * 2, MEM_RESERVE, PAGE_NOACCESS);
        CHECK(rsv != nullptr);
        CHECK(!npp::LocalReadable(rsv, 1));
        CHECK(!npp::LocalReadable(rsv, kPage * 2));
        // 紧邻保留区之后（多半是 MEM_FREE）同样不可用
        CHECK(!npp::LocalReadable(reinterpret_cast<char*>(rsv) + kPage * 2, 1));
        ::VirtualFree(rsv, 0, MEM_RELEASE);
        ::VirtualFree(na, 0, MEM_RELEASE);
    }

    // ---- 6. guard 页：判据取保守口径（拒绝）----------------------------------
    {
        void* gd = ::VirtualAlloc(nullptr, kPage, MEM_COMMIT,
                                  PAGE_READWRITE | PAGE_GUARD);
        CHECK(gd != nullptr);
        CHECK(!npp::LocalReadable(gd, 1));
        CHECK(!npp::LocalWritable(gd, 1));
        ::VirtualFree(gd, 0, MEM_RELEASE);
    }

    // ---- 7. 跨区域：两页连续，改第二页保护位 ⇒ 写失败而读仍成功 --------------
    {
        auto* two = static_cast<char*>(::VirtualAlloc(nullptr, kPage * 2,
                                                     MEM_COMMIT, PAGE_READWRITE));
        CHECK(two != nullptr);
        CHECK(npp::LocalReadable(two, kPage * 2));      // 跨两区域
        CHECK(npp::LocalWritable(two, kPage * 2));

        DWORD old = 0;
        CHECK(::VirtualProtect(two + kPage, kPage, PAGE_READONLY, &old) != FALSE);
        CHECK(npp::LocalReadable(two, kPage * 2));      // 读仍可以
        CHECK(!npp::LocalWritable(two, kPage * 2));     // 写不行
        CHECK(npp::LocalWritable(two, kPage));          // 只碰第一页 ⇒ 可以
        ::VirtualFree(two, 0, MEM_RELEASE);
    }

    // ---- 8. 贴页尾、界内无 NUL、下一页不可用 ⇒ 0 且不崩 -----------------------
    {
        auto* base = static_cast<char*>(::VirtualAlloc(nullptr, kPage * 2,
                                                      MEM_RESERVE, PAGE_NOACCESS));
        CHECK(base != nullptr);
        CHECK(::VirtualAlloc(base, kPage, MEM_COMMIT, PAGE_READWRITE) == base);
        auto* tail = reinterpret_cast<wchar_t*>(base + kPage) - 1;   // 页内最后一个 wchar
        *tail = L'Z';
        CHECK(npp::LocalReadable(tail, sizeof(wchar_t)));
        CHECK(npp::LocalWideStrChars(tail, 16) == 0);   // 读到页尾即停，不碰下一页
        CHECK(npp::LocalWideStrChars(tail, 1) == 0);
        // 反向自证：把 NUL 放进界内 ⇒ 立刻能数出来（证明上面不是恒 0）
        *tail = L'\0';
        CHECK(npp::LocalWideStrChars(tail, 1) == 1);
        ::VirtualFree(base, 0, MEM_RELEASE);
    }

    // ---- 9. 只读页上的字符串照样能读（读侧不因保护位过严）--------------------
    {
        auto* rw = static_cast<wchar_t*>(::VirtualAlloc(nullptr, kPage, MEM_COMMIT,
                                                        PAGE_READWRITE));
        CHECK(rw != nullptr);
        wcscpy_s(rw, kPage / sizeof(wchar_t), L"readonly-ok");
        DWORD old = 0;
        CHECK(::VirtualProtect(rw, kPage, PAGE_READONLY, &old) != FALSE);
        CHECK(npp::LocalWideStrChars(rw, 64) == 12);    // 11 字符 + NUL
        CHECK(!npp::LocalWritable(rw, 4));
        ::VirtualFree(rw, 0, MEM_RELEASE);
    }

    std::printf("== %s ==\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
