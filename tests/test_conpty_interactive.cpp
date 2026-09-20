// test_conpty_interactive.cpp - tests ConPTY with interactive input
#include "../src/terminal/ConPTY.h"
#include "../src/core/Log.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// 轮询等待：条件成立立刻返回，超时才放弃。
//
// 为什么不能再用固定 Sleep：`cmd.exe /K chcp 65001` 在本机要一秒多才就绪，
// 期间它先打一串"清屏 + 设窗口标题"的控制序列
// （`\x1b[?25l \x1b[2J \x1b[m \x1b[H \x1b]0;C:\WINDOWS\SYSTEM32\cmd.exe - chcp  65001\x07 \x1b[?25h`）。
// 在它准备好之前写进去的命令只会堆在控制台输入缓冲里，到点检查自然找不到回显
// —— 实测这条用例**稳定失败**（三次都是 68 字节、三次都找不到），
// 而同一个 shell 后来跑 `dir` 却正常。改成等"条件成立"，就与机器快慢解耦了。
template <typename Fn>
static bool WaitUntil(Fn cond, int timeoutMs, int stepMs = 50) {
    for (int waited = 0;; waited += stepMs) {
        if (cond()) return true;
        if (waited >= timeoutMs) return false;
        Sleep(stepMs);
    }
}

int main() {
    ConPTY pty;

    bool started = pty.Start(L"cmd.exe /K chcp 65001", 80, 24);
    if (!started) {
        printf("NOTE: ConPTY unavailable, skipping interactive test\n");
        return 0;
    }

    // onOutput 是在**读线程**上回调的（ConPTY.h 里写明了"it runs on a thread"），
    // 而主线程同时要读这块缓冲 ⇒ 两边都得持锁，否则是数据竞争。
    // 本用例原来没有锁：靠固定 Sleep 把时间错开，只是让竞争"不容易"发生。
    std::mutex mtx;
    std::string allOutput;
    pty.onOutput = [&](const std::string& chunk) {
        std::lock_guard<std::mutex> lk(mtx);
        allOutput += chunk;
    };
    auto outSize = [&] {
        std::lock_guard<std::mutex> lk(mtx);
        return allOutput.size();
    };
    auto hasText = [&](const char* needle) {
        std::lock_guard<std::mutex> lk(mtx);
        return allOutput.find(needle) != std::string::npos;
    };

    // 1) 先等 shell **真正就绪**再发命令：等它吐出第一段输出（启动横幅 /
    //    换码页的清屏序列），再等它安静下来（连续 400ms 没有新输出）。
    const bool sawStartup = WaitUntil([&] { return outSize() > 0; }, 10000);
    {
        std::size_t last = outSize();
        for (int quiet = 0; quiet < 400;) {
            Sleep(50);
            const std::size_t now = outSize();
            if (now != last) { last = now; quiet = 0; } else quiet += 50;
        }
    }
    printf("Initial output (%zu bytes): %s\n", outSize(),
           sawStartup ? "shell ready" : "TIMED OUT waiting for the shell to start");

    printf("Sending: echo hello_conpty_test\r\n");
    pty.Write("echo hello_conpty_test\r\n");

    // 2) 轮询等回显出现，而不是睡固定时长后再看一眼
    const bool foundEcho = WaitUntil([&] { return hasText("hello_conpty_test"); }, 10000);
    printf("Output after echo (%zu bytes):\n", outSize());
    printf("Found 'hello_conpty_test' in output: %s\n", foundEcho ? "YES" : "NO");

    printf("Sending: dir\r\n");
    pty.Write("dir\r\n");

    const bool foundDir =
        WaitUntil([&] { return hasText("Directory of") || hasText("<DIR>"); }, 10000);
    printf("Output after dir (%zu bytes):\n", outSize());
    printf("Found directory listing: %s\n", foundDir ? "YES" : "NO");

    pty.Stop();

    CHECK(foundEcho);
    CHECK(foundDir);

    if (g_fail == 0) { printf("ALL INTERACTIVE TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
