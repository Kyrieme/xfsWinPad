// xfsWinPad - application entry point
#include "app/MainWindow.h"
#include "core/Log.h"
#include "core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <objbase.h>
#include <shellapi.h>
#include <gdiplus.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

static ULONG_PTR g_gdipToken = 0;
#pragma comment(lib, "shell32.lib")

using namespace xfs;
using namespace Gdiplus;

// --- 单实例握手（Notepad++ mono-instance 模式）-------------------------------
// 默认只允许一个进程：后续启动把命令行经 WM_COPYDATA 转发给已有窗口后退出；
// `--new` 显式绕过（"移动到新窗口"等需要真多进程的场景）。
static constexpr wchar_t kInstanceMutex[] = L"Local\\xfsWinPad.SingleInstance";
static constexpr UINT_PTR kForwardMagic = 0x58465750;   // 'XFWP'

// 若已有实例则转发命令行并返回 true（调用方应直接退出进程）。
// 互斥量句柄刻意不关闭：进程存活期间标记实例存在，退出由 OS 回收。
static bool ForwardToRunningInstance() {
    HWND prev = nullptr;
    for (int i = 0; i < 40 && !prev; ++i) {   // 首实例可能仍在建窗（双击竞态）
        prev = ::FindWindowW(L"xfsWinPadMainWindow", nullptr);
        if (!prev) ::Sleep(100);
    }
    if (!prev) return false;                  // 首实例已死 → 正常启动

    std::wstring cmdline = GetCommandLineW();
    COPYDATASTRUCT cds{};
    cds.dwData = kForwardMagic;
    cds.cbData = (DWORD)((cmdline.size() + 1) * sizeof(wchar_t));
    cds.lpData = (PVOID)cmdline.c_str();
    ::AllowSetForegroundWindow(ASFW_ANY);     // 让首实例有权限把自己带到前台
    ::SendMessageW(prev, WM_COPYDATA, 0, (LPARAM)&cds);
    Logger::Info("Forwarded command line to running instance; exiting");
    return true;
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    HRESULT cohr = ::CoInitializeEx(nullptr,
                                    COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ::GdiplusStartup(&g_gdipToken, &gdiplusStartupInput, nullptr);
    Logger::Init();
    Logger::Info(std::string("xfsWinPad starting... build ") + __DATE__ + " " + __TIME__);

    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    // Scintilla.dll registers its "Scintilla" window class inside DllMain.
    // Nothing imports its symbols, so load it explicitly.
    if (!LoadLibraryW(L"Scintilla.dll")) {
        Logger::Fatal("Scintilla.dll failed to load, gle=" + std::to_string(::GetLastError()));
        MessageBoxW(nullptr, L"Scintilla.dll not found next to xfsWinPad.exe.", L"xfsWinPad",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    StartupOptions opts = xfs::ParseCommandLine(GetCommandLineW());

    // 单实例：把文件转发给已运行窗口开标签，本进程退出（--new 除外）。
    // firstInstance 决定会话归属：primary 写 session.json，多开的窗口各写
    // session-<pid>.json 槽位（批次 67），互不覆盖。
    ::CreateMutexW(nullptr, FALSE, kInstanceMutex);
    opts.firstInstance = (::GetLastError() != ERROR_ALREADY_EXISTS);
    if (!opts.forceNew && !opts.firstInstance && ForwardToRunningInstance()) {
        if (g_gdipToken) ::GdiplusShutdown(g_gdipToken);
        if (SUCCEEDED(cohr)) ::CoUninitialize();
        return 0;
    }

    MainWindow win;
    if (!win.Create(hInst, opts)) {
        Logger::Fatal("MainWindow creation FAILED");
        MessageBoxW(nullptr, L"Failed to create main window.", L"xfsWinPad",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(win.Hwnd(), nCmdShow);
    UpdateWindow(win.Hwnd());
    Logger::Info("App initialized OK");

    int rc = win.RunMessageLoop();
    Logger::Info("Message loop exited");
    if (g_gdipToken) ::GdiplusShutdown(g_gdipToken);
    if (SUCCEEDED(cohr)) ::CoUninitialize();
    return rc;
}
