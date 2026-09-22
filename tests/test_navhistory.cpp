// test_navhistory.cpp - NavHistory 单测：游标语义、去重、分支截断、上限、空历史。
//
// 这个模块是纯逻辑（不碰 Win32 / Scintilla / IO），所以这里钉的是**语义**，
// 不是"某次运行的观感"。几条容易写错的语义，各自单独一节：
//   · 同一份文档里的**不同位置**必须各占一条（否则"在同一个文件里后退"就没得退）；
//   · 与"当前项"完全相同的点必须被吃掉（否则"跳转前后各记一次"每次多塞一条）；
//   · 后退之后再 Push 必须**截断前进链**（浏览器语义），不是追加到末尾。
#include "../src/core/NavHistory.h"
#include <cstdio>
#include <string>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// 造一个点：路径用 L"f<N>" 这种可读的假路径，位置就是 N。
static NavPoint P(const wchar_t* path, long long pos) {
    NavPoint p;
    p.path = path;
    p.pos = pos;
    return p;
}

static NavPoint P(int n) {
    return P(L"f", n);
}

int main() {
    // --- 1. 空历史：什么都不该发生 ------------------------------------------
    {
        NavHistory h;
        NavPoint out = P(L"sentinel", 12345);
        CHECK(h.Size() == 0);
        CHECK(!h.CanBack());
        CHECK(!h.CanForward());
        CHECK(!h.Back(&out));
        CHECK(!h.Forward(&out));
        // 失败时**不许**改写出参 —— 否则调用方会把上一次的目标当成这一次的。
        CHECK(out.path == L"sentinel" && out.pos == 12345);
    }

    // --- 2. 单点：没有可退也没有可进 ----------------------------------------
    {
        NavHistory h;
        h.Push(P(10));
        CHECK(h.Size() == 1);
        CHECK(!h.CanBack());
        CHECK(!h.CanForward());
    }

    // --- 3. 两点：退一格 / 进一格 -------------------------------------------
    {
        NavHistory h;
        h.Push(P(L"a", 10));
        h.Push(P(L"b", 20));
        CHECK(h.Size() == 2);
        CHECK(h.Cursor() == 1);
        CHECK(h.CanBack() && !h.CanForward());

        NavPoint out;
        CHECK(h.Back(&out));
        CHECK(out.path == L"a" && out.pos == 10);
        CHECK(h.Cursor() == 0);
        CHECK(!h.CanBack() && h.CanForward());
        CHECK(!h.Back(&out));                 // 已经在最早

        CHECK(h.Forward(&out));
        CHECK(out.path == L"b" && out.pos == 20);
        CHECK(h.Cursor() == 1);
        CHECK(!h.Forward(&out));              // 已经在最晚
    }

    // --- 4. 去重：完全相同的点只占一条 --------------------------------------
    {
        NavHistory h;
        h.Push(P(10));
        h.Push(P(10));
        h.Push(P(10));
        CHECK(h.Size() == 1);

        // "跳转前后各记一次"的真实序列：记来源（= 当前项，被吃掉）、记目的地。
        h.Push(P(10));            // 跳转前：no-op
        h.Push(P(20));            // 跳转后：新点
        CHECK(h.Size() == 2);
        NavPoint out;
        CHECK(h.Back(&out));
        CHECK(out.pos == 10);     // 一次 Back 就回到来源，而不是"退半步"
    }

    // --- 5. 同一份文档的不同位置**必须**各占一条 -----------------------------
    {
        NavHistory h;
        h.Push(P(L"same.txt", 10));
        h.Push(P(L"same.txt", 90));   // 路径相同、位置不同 ⇒ 新的一条
        CHECK(h.Size() == 2);
        NavPoint out;
        CHECK(h.Back(&out));
        CHECK(out.path == L"same.txt" && out.pos == 10);
    }

    // --- 6. 分支截断：后退之后 Push 取代旧的前进链 ---------------------------
    {
        NavHistory h;
        h.Push(P(L"a", 1));
        h.Push(P(L"b", 2));
        h.Push(P(L"c", 3));
        NavPoint out;
        CHECK(h.Back(&out));                  // 现在停在 b
        CHECK(out.path == L"b");
        CHECK(h.CanForward());                // c 还在前面

        h.Push(P(L"d", 4));                   // 走新分支
        CHECK(h.Size() == 3);                 // a b d —— c 被丢掉
        CHECK(h.Cursor() == 2);
        CHECK(!h.CanForward());

        CHECK(h.Back(&out)); CHECK(out.path == L"b");
        CHECK(h.Back(&out)); CHECK(out.path == L"a");
        CHECK(!h.CanBack());
        CHECK(h.Size() == 3);                 // 后退不改列表长度
    }

    // --- 7. 上限：超出后丢**最早**的，游标仍指在最后一个 ---------------------
    {
        NavHistory h;
        const int total = (int)NavHistory::kMax + 9;
        for (int i = 0; i < total; ++i) h.Push(P(i));
        CHECK(h.Size() == NavHistory::kMax);
        CHECK(h.Cursor() == NavHistory::kMax - 1);

        // 一路退到底：应当停在"第 9 个"（前 9 个被丢了）。
        NavPoint out;
        int steps = 0;
        while (h.Back(&out)) ++steps;
        CHECK(steps == (int)NavHistory::kMax - 1);
        CHECK(out.pos == 9);
        CHECK(!h.CanBack());
    }

    // --- 8. Clear：回到初始态（含游标） -------------------------------------
    {
        NavHistory h;
        h.Push(P(L"a", 1));
        h.Push(P(L"b", 2));
        h.Clear();
        CHECK(h.Size() == 0);
        CHECK(!h.CanBack() && !h.CanForward());
        // Clear 之后仍可正常使用（游标不能留在越界的位置上）。
        h.Push(P(L"c", 3));
        CHECK(h.Size() == 1);
        CHECK(!h.CanBack());
    }

    // --- 9. 出参可以为 nullptr（只想挪游标） --------------------------------
    {
        NavHistory h;
        h.Push(P(L"a", 1));
        h.Push(P(L"b", 2));
        CHECK(h.Back(nullptr));
        CHECK(h.Cursor() == 0);
        CHECK(h.Forward(nullptr));
        CHECK(h.Cursor() == 1);
    }

    if (g_fail == 0) { printf("ALL NAVHISTORY TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
