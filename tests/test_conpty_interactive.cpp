// test_conpty_interactive.cpp - tests ConPTY with interactive input
#include "../src/terminal/ConPTY.h"
#include "../src/core/Log.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main() {
    ConPTY pty;

    bool started = pty.Start(L"cmd.exe /K chcp 65001", 80, 24);
    if (!started) {
        printf("NOTE: ConPTY unavailable, skipping interactive test\n");
        return 0;
    }

    std::string allOutput;
    pty.onOutput = [&](const std::string& chunk) {
        allOutput += chunk;
    };

    Sleep(500);
    printf("Initial output (%zu bytes):\n", allOutput.size());

    printf("Sending: echo hello_conpty_test\r\n");
    pty.Write("echo hello_conpty_test\r\n");

    Sleep(1000);
    printf("Output after echo (%zu bytes):\n", allOutput.size());

    bool foundEcho = allOutput.find("hello_conpty_test") != std::string::npos;
    printf("Found 'hello_conpty_test' in output: %s\n", foundEcho ? "YES" : "NO");

    printf("Sending: dir\r\n");
    pty.Write("dir\r\n");
    Sleep(1500);

    printf("Output after dir (%zu bytes):\n", allOutput.size());

    bool foundDir = allOutput.find("Directory of") != std::string::npos ||
                    allOutput.find("<DIR>") != std::string::npos;
    printf("Found directory listing: %s\n", foundDir ? "YES" : "NO");

    pty.Stop();

    CHECK(foundEcho);
    CHECK(foundDir);

    if (g_fail == 0) { printf("ALL INTERACTIVE TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
