#include "language/CraftHost.h"

#include <windows.h>

#include <vector>

namespace xfs {
namespace craft {

namespace {

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
        out.append(buf, got);
    ::CloseHandle(h);
    return true;
}

} // namespace

std::wstring ReadEnv(const wchar_t* name) {
    wchar_t buf[32768] = {0};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, 32768);
    if (n == 0 || n >= 32768) return std::wstring();
    return std::wstring(buf, n);
}

std::wstring DirOf(const std::wstring& path) {
    const std::size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return std::wstring();
    if (p == 0) return path.substr(0, 1);          // "\foo" → "\"
    // "C:\foo" → "C:" 这种要去掉反斜杠，否则拼出来是 "C:\\\makefile"
    if (p == 2 && path[1] == L':') return path.substr(0, 2);
    return path.substr(0, p);
}

std::wstring ParentOf(const std::wstring& dir) {
    if (dir.empty()) return std::wstring();
    std::wstring d = dir;
    while (d.size() > 1 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    // 已经是盘根（"C:" 或 "C:\" 或 "\"）→ 没有上一层
    if (d.size() == 2 && d[1] == L':') return std::wstring();
    if (d.size() == 1) return std::wstring();
    const std::size_t p = d.find_last_of(L"\\/");
    if (p == std::wstring::npos) return std::wstring();
    if (p == 2 && d[1] == L':') return d.substr(0, 2);
    if (p == 0) return d.substr(0, 1);
    return d.substr(0, p);
}

bool FindOnPath(const std::wstring& candidate, std::wstring& full) {
    if (candidate.empty()) return false;
    wchar_t buf[4096] = {0};
    // SearchPathW 在候选名不带扩展名时会自己补 .exe（现场两种 makefile 都有）。
    const DWORD n = ::SearchPathW(nullptr, candidate.c_str(), nullptr, 4096, buf,
                                  nullptr);
    if (n == 0 || n >= 4096) return false;
    full.assign(buf, n);
    return true;
}

Project LoadProject(const std::wstring& anyPath) {
    const std::wstring dir = DirOf(anyPath);

    std::string mf;
    const bool hasMf = !dir.empty() && ReadFileBytes(dir + L"\\makefile", mf);

    std::string parentMf;
    const std::wstring parent = ParentOf(dir);
    const bool hasParentMf =
        !parent.empty() && ReadFileBytes(parent + L"\\makefile", parentMf);

    // 注意：makefileText 是 `const std::string&`。**不要传 nullptr** ——
    // 那会走 std::string(const char*) 构造，是 UB（实测直接崩）。
    return BuildProject(anyPath, hasMf ? mf : std::string(), hasMf,
                        hasParentMf, hasParentMf ? parentMf : std::string());
}

Toolchain DetectToolchainFromEnv(const std::wstring& settingsDir,
                                 const std::wstring& craftHomeOverride) {
    const std::wstring home =
        craftHomeOverride.empty() ? ReadEnv(L"CRAFT_HOME") : craftHomeOverride;
    return DetectToolchain(settingsDir, home, &FindOnPath);
}

} // namespace craft
} // namespace xfs
