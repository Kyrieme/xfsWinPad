#include "Log.h"
#include "Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace xfs {

std::wstring Logger::g_logPath2;   // member 定义（exe 同目录 debug.log）

namespace {

std::mutex g_mutex;
bool g_ready = false;
std::wstring g_logPath;

const char* LevelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

} // namespace

void Logger::Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ready) return;
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    g_logPath = base + L"\\xfsWinPad\\logs";
    ::CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    ::CreateDirectoryW(g_logPath.c_str(), nullptr);
    g_logPath += L"\\xfsWinPad.log";

    // 第二落点：exe 同目录 debug.log——用户现场拷工程排查时日志跟着走
    wchar_t exeBuf[MAX_PATH];
    DWORD exeLen = ::GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        std::wstring exe(exeBuf);
        size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            g_logPath2 = exe.substr(0, slash + 1) + L"debug.log";
        }
    }
    g_ready = true;

    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        char buf[128];
        sprintf_s(buf, "\r\n==== xfsWinPad session %04u-%02u-%02u %02u:%02u:%02u ====\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        DWORD written = 0;
        ::WriteFile(h, buf, (DWORD)strlen(buf), &written, nullptr);
        ::CloseHandle(h);
    }
}

void Logger::Write(LogLevel level, const std::string& msg) {
    if (!g_ready) Init();
    std::lock_guard<std::mutex> lock(g_mutex);
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char buf[2048];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\r\n",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                        st.wMilliseconds, LevelName(level), msg.c_str());
    if (n <= 0) return;
    DWORD written = 0;

    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::WriteFile(h, buf, (DWORD)n, &written, nullptr);
        ::CloseHandle(h);
    }

    // exe 同目录 debug.log：写入失败静默（只读目录/盘满不干扰主日志）。
    // 共享模式必须含 FILE_SHARE_WRITE：serve 的 stderr 句柄（spawn 时继承，
    // 常驻）持有本文件写访问权，若这里只声明 SHARE_READ 会共享冲突打不开，
    // 之后 debug.log 永久静默（15:41 e2e 实锤，两处打开都按写共享声明）。
    if (!g_logPath2.empty()) {
        HANDLE h2 = ::CreateFileW(g_logPath2.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h2 != INVALID_HANDLE_VALUE) {
            ::WriteFile(h2, buf, (DWORD)n, &written, nullptr);
            ::CloseHandle(h2);
        }
    }

    if (level >= LogLevel::Error) {
        ::OutputDebugStringW(xfs::Utf8ToWide(buf).c_str());
    }
}

} // namespace xfs
