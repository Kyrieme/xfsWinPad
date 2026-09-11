#include "ConPTY.h"
#include "../core/Log.h"
#include "../core/Util.h"

#include <cstring>
#include <vector>

namespace xfs {

namespace {
constexpr COORD kDefaultBuffer = { 120, 30 };
constexpr DWORD kReadBufferSize = 4096;
}

HRESULT (WINAPI* ConPTY::CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, void**) = nullptr;
VOID (WINAPI* ConPTY::ResizePseudoConsole)(void*, COORD) = nullptr;
VOID (WINAPI* ConPTY::ClosePseudoConsole)(void*) = nullptr;

bool ConPTY::Resolve() {
    if (CreatePseudoConsole) return true;
    HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
    if (!k) k = ::GetModuleHandleW(nullptr);
    CreatePseudoConsole =
        (HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**))
        ::GetProcAddress(k, "CreatePseudoConsole");
    ResizePseudoConsole =
        (VOID(WINAPI*)(void*, COORD))::GetProcAddress(k, "ResizePseudoConsole");
    ClosePseudoConsole =
        (VOID(WINAPI*)(void*))::GetProcAddress(k, "ClosePseudoConsole");
    if (!CreatePseudoConsole || !ResizePseudoConsole || !ClosePseudoConsole) {
        Logger::Error("ConPTY not available (Win10 1809+ required)");
        CreatePseudoConsole = nullptr;
        ResizePseudoConsole = nullptr;
        ClosePseudoConsole = nullptr;
        return false;
    }
    return true;
}

bool ConPTY::Start(std::wstring command, int cols, int rows) {
    Stop();
    if (!Resolve()) return false;
    if (cols < 10) cols = 10;
    if (rows < 2) rows = 2;

    // child's stdin = read end; we write the write end
    HANDLE inRead = nullptr;
    inPipeWrite_ = nullptr;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    if (!::CreatePipe(&inRead, &inPipeWrite_, &sa, 0)) {
        Logger::Error("ConPTY: stdin pipe failed gle=" + std::to_string(::GetLastError()));
        return false;
    }
    ::SetHandleInformation(inPipeWrite_, HANDLE_FLAG_INHERIT, 0);

    // child's stdout = write end; we read the read end
    HANDLE outWrite = nullptr;
    outPipeRead_ = nullptr;
    if (!::CreatePipe(&outPipeRead_, &outWrite, &sa, 0)) {
        Logger::Error("ConPTY: stdout pipe failed gle=" + std::to_string(::GetLastError()));
        ::CloseHandle(inRead);
        ::CloseHandle(inPipeWrite_); inPipeWrite_ = nullptr;
        return false;
    }
    ::SetHandleInformation(outPipeRead_, HANDLE_FLAG_INHERIT, 0);

    COORD sz{ (SHORT)cols, (SHORT)rows };
    HRESULT hr = CreatePseudoConsole(sz, inRead, outWrite, 0, &pty_);
    ::CloseHandle(inRead);
    ::CloseHandle(outWrite);
    if (FAILED(hr) || !pty_) {
        Logger::Error("ConPTY: CreatePseudoConsole failed hr=0x" +
                      std::to_string((unsigned long)hr) +
                      " gle=" + std::to_string(::GetLastError()));
        ::CloseHandle(inPipeWrite_); inPipeWrite_ = nullptr;
        ::CloseHandle(outPipeRead_); outPipeRead_ = nullptr;
        return false;
    }

    // launch the child attached to the pseudoconsole
    if (command.empty()) {
        wchar_t buf[MAX_PATH]; ::GetEnvironmentVariableW(L"COMSPEC", buf, MAX_PATH);
        command = buf ? buf : L"cmd.exe";
        // /K keeps the shell alive; chcp 65001 sets UTF-8 codepage.
        // Do NOT use >nul here — the > would be misinterpreted by the
        // command-line parser as a redirect operator, breaking all input.
        command += L" /K chcp 65001";
    }

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    // STARTF_USESTDHANDLES with NULL handles is essential: it prevents
    // NtCreateUserProcess from duplicating the parent's console handles
    // to the child, which would bypass the ConPTY's virtual terminal.
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdInput = NULL;
    siex.StartupInfo.hStdOutput = NULL;
    siex.StartupInfo.hStdError = NULL;
    size_t attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    static thread_local std::vector<char> attrBuf;
    attrBuf.resize(attrSize);
    siex.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (::InitializeProcThreadAttributeList(siex.lpAttributeList, 1, 0, &attrSize)) {
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x20016
#endif
        ::UpdateProcThreadAttribute(siex.lpAttributeList, 0,
                                    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                    pty_, sizeof(pty_), nullptr, nullptr);
    }

    std::wstring cmdline = command;   // CreateProcessW may modify this; copy
    std::vector<wchar_t> m(cmdline.begin(), cmdline.end());
    m.push_back(L'\0');
    PROCESS_INFORMATION pi{};
    // bInheritHandles must be TRUE: the internal conhost helper spawned by
    // CreatePseudoConsole inherits the pipe handles. Without inheritance
    // the helper cannot read/write them and the child sees broken stdio.
    BOOL ok = ::CreateProcessW(nullptr, m.data(), nullptr, nullptr, TRUE,
                               EXTENDED_STARTUPINFO_PRESENT,
                               nullptr, nullptr, &siex.StartupInfo, &pi);
    if (siex.lpAttributeList)
        ::DeleteProcThreadAttributeList(siex.lpAttributeList);
    if (!ok) {
        Logger::Error("ConPTY: CreateProcess failed gle=" + std::to_string(::GetLastError()));
        ClosePseudoConsole(pty_); pty_ = nullptr;
        ::CloseHandle(inPipeWrite_); inPipeWrite_ = nullptr;
        ::CloseHandle(outPipeRead_); outPipeRead_ = nullptr;
        return false;
    }

    proc_ = pi.hProcess;
    ::CloseHandle(pi.hThread);   // we don't need the primary thread handle
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = ::CreateThread(nullptr, 0, ReaderThread, this, 0, nullptr);
    started_ = true;
    Logger::Info("ConPTY started: " + WideToUtf8(command) + " cols=" +
                 std::to_string(cols) + " rows=" + std::to_string(rows));
    return true;
}

void ConPTY::Stop() {
    if (stopEvent_) ::SetEvent(stopEvent_);   // unblock reader if it's waiting
    if (thread_) {
        ::WaitForSingleObject(thread_, 2000);
        ::CloseHandle(thread_); thread_ = nullptr;
    }
    if (pty_) { ClosePseudoConsole(pty_); pty_ = nullptr; }
    if (proc_) { ::CloseHandle(proc_); proc_ = nullptr; }
    if (inPipeWrite_) { ::CloseHandle(inPipeWrite_); inPipeWrite_ = nullptr; }
    if (outPipeRead_) { ::CloseHandle(outPipeRead_); outPipeRead_ = nullptr; }
    if (stopEvent_) { ::CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    started_ = false;
}

void ConPTY::Write(const char* data, size_t len) {
    if (!started_ || !inPipeWrite_ || len == 0) return;
    // Log what we're sending (truncate long strings for readability)
    std::string preview(data, std::min(len, (size_t)80));
    Logger::Info("ConPTY::Write [" + std::to_string(len) + "B]: " + preview);
    DWORD written = 0;
    BOOL ok = ::WriteFile(inPipeWrite_, data, (DWORD)len, &written, nullptr);
    if (!ok) {
        DWORD err = ::GetLastError();
        Logger::Error("ConPTY::Write FAILED gle=" + std::to_string(err));
    } else if (written != len) {
        Logger::Error("ConPTY::Write partial: wrote " + std::to_string(written) +
                      " of " + std::to_string(len));
    }
}

void ConPTY::Resize(int cols, int rows) {
    if (!started_ || !pty_ || !ResizePseudoConsole) return;
    if (cols < 10) cols = 10;
    if (rows < 2) rows = 2;
    COORD sz{ (SHORT)cols, (SHORT)rows };
    ResizePseudoConsole(pty_, sz);
}

DWORD WINAPI ConPTY::ReaderThread(LPVOID param) {
    auto* self = static_cast<ConPTY*>(param);
    self->PumpOutput();
    if (self->onOutput) self->onOutput(std::string());   // EOF signal
    return 0;
}

void ConPTY::PumpOutput() {
    std::string buf;
    char raw[kReadBufferSize];
    while (true) {
        DWORD read = 0;
        if (stopEvent_ && ::WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
            break;
        if (::ReadFile(outPipeRead_, raw, kReadBufferSize, &read, nullptr)) {
            if (read == 0) break;   // EOF
            buf.append(raw, read);
            if (onOutput) onOutput(buf);
            buf.clear();
        } else {
            break;
        }
    }
    Logger::Info("ConPTY: PumpOutput exited (read failed or stopped)");
}

ConPTY::~ConPTY() { Stop(); }

} // namespace xfs
