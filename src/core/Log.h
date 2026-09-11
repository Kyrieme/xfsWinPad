#pragma once
// xfsWinPad - minimal leveled file logger (TRACE..FATAL)

#include <string>

namespace xfs {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal };

class Logger {
public:
    static void Init();   // %LOCALAPPDATA%\xfsWinPad\logs\xfsWinPad.log
                          // + exe 同目录 debug.log（现场排查用，两个都写）
    static void Write(LogLevel level, const std::string& msg);
    // exe 同目录 debug.log 的路径（供 serve 输出重定向等复用；空=未初始化）
    static const std::wstring& DebugLogPath() { return g_logPath2; }

    static void Trace(const std::string& m) { Write(LogLevel::Trace, m); }
    static void Debug(const std::string& m) { Write(LogLevel::Debug, m); }
    static void Info(const std::string& m)  { Write(LogLevel::Info, m); }
    static void Warn(const std::string& m)  { Write(LogLevel::Warn, m); }
    static void Error(const std::string& m) { Write(LogLevel::Error, m); }
    static void Fatal(const std::string& m) { Write(LogLevel::Fatal, m); }
private:
    static std::wstring g_logPath2;
};

} // namespace xfs
