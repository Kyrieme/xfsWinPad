#pragma once
// xfsWinPad - ConPTY host: drives a pseudo-console (cmd.exe/PowerShell) over
// Windows' ConPTY API so we get a real VT stream (colors, cursor, etc.) that we
// render into a Scintilla buffer.
//
// This implementation uses CreatePseudoConsole + CreateProcess with pipe handles
// for I/O. The child's stdin/stdout are connected to the pseudo console, and
// the host reads/writes via the pipe endpoints.
//
// Input is sent via WriteFile on the input pipe. The STARTF_USESTDHANDLES
// flag with NULL handles in the process startup info is essential to prevent
// handle inheritance issues that bypass the ConPTY's virtual terminal.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <string>

namespace xfs {

// WM_APP_PTY_OUTPUT for the console reader thread -> panel window.
// wparam = a heap-allocated std::string* (UTF-8); the receiver must delete it.
constexpr UINT WM_APP_PTY_OUTPUT = WM_APP + 20;

class ConPTY {
public:
    ~ConPTY();

    // Spawn `command` (default: %COMSPEC% = cmd.exe) in a pseudoconsole.
    // `cols`/`rows` set the initial buffer size.
    bool Start(std::wstring command, int cols, int rows);
    void Stop();

    bool IsRunning() const { return pty_ != nullptr; }

    // Forward keyboard input to the child's console.
    // Internally converts UTF-8 to keyboard events via WriteConsoleInput.
    void Write(uint8_t byte) { Write(reinterpret_cast<const char*>(&byte), 1); }
    void Write(const char* data, size_t len);
    void Write(const std::string& utf8) { Write(utf8.data(), utf8.size()); }

    // Resize the underlying buffer (called by the panel's WM_SIZE).
    void Resize(int cols, int rows);

    // called on the reader thread with an output chunk (UTF-8). If empty, the
    // stream is about to be torn down (e.g. child exited). Guard all state
    // this callback touches; it runs on a thread.
    std::function<void(const std::string&)> onOutput;

private:
    static DWORD WINAPI ReaderThread(LPVOID param);
    void PumpOutput();

    // kernel32 ConPTY functions, resolved lazily.
    static HRESULT (WINAPI* CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, void**);
    static VOID (WINAPI* ResizePseudoConsole)(void*, COORD);
    static VOID (WINAPI* ClosePseudoConsole)(void*);
    static bool Resolve();

    void* pty_ = nullptr;               // HPCON
    HANDLE inPipeWrite_ = nullptr;      // we write child's stdin here
    HANDLE outPipeRead_ = nullptr;      // we read child's stdout from here
    HANDLE proc_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    bool started_ = false;
};

} // namespace xfs
