// xfsWinPad - Workshop: AI 插件工场实现。
//
// 设计要点见 Workshop.h 头注释。本文件只做三件事：
//   1. EnsureSdk: exe 内嵌 RCDATA -> plugins\_sdk（幂等，版本戳判断）
//   2. Scaffold:  任务描述 -> workshop\<name> 脚手架（模板占位符替换）
//   3. InstallBuilt: build\output.dll -> plugins\<name>\ + LoadNew 热加载

#include "Workshop.h"
#include "PluginManager.h"
#include "../core/Log.h"
#include "../core/Util.h"

#include <shlwapi.h>
#include <regex>

namespace xfs {

namespace {

// SDK 资源 id（resources/xfsWinPad.rc 同名定义）
constexpr int kResGuide    = 7001;   // PLUGIN_DEV_GUIDE.md
constexpr int kResApi      = 7002;   // xfs_plugin_api.h
constexpr int kResTplC     = 7003;   // template.c
constexpr int kResTplCpp   = 7004;   // template.cpp
constexpr int kResBuildCmd = 7005;   // build.cmd

// 版本戳：SDK 资源变化时 EnsureSdk 重写（内容指纹 = 指南字节数 + ABI 版本）
constexpr const char* kSdkStamp = "sdk-v3.1";

bool ReleaseResource(int resid, const wchar_t* ext, const std::wstring& dir,
                     const std::wstring& filename) {
    std::wstring path = dir + L"\\" + filename;
    HRSRC rs = ::FindResourceW(nullptr, MAKEINTRESOURCEW(resid), RT_RCDATA);
    if (!rs) {
        Logger::Error("Workshop: SDK resource missing id=" +
                      std::to_string(resid));
        return false;
    }
    HGLOBAL hg = ::LoadResource(nullptr, rs);
    if (!hg) return false;
    void* data = ::LockResource(hg);
    DWORD size = ::SizeofResource(nullptr, rs);
    if (!data || size == 0) return false;

    // 幂等：版本戳文件存在且同名资源长度一致 -> 跳过（省去每次写盘）
    std::string existing;
    if (ReadFileBytes(path, existing) && existing.size() == size &&
        memcmp(existing.data(), data, size) == 0)
        return true;
    return WriteFileBytes(path, data, size);
}

std::wstring ReadTextResource(int resid) {
    HRSRC rs = ::FindResourceW(nullptr, MAKEINTRESOURCEW(resid), RT_RCDATA);
    if (!rs) return L"";
    HGLOBAL hg = ::LoadResource(nullptr, rs);
    if (!hg) return L"";
    void* data = ::LockResource(hg);
    DWORD size = ::SizeofResource(nullptr, rs);
    if (!data || size == 0) return L"";
    return Utf8ToWide(std::string((const char*)data, size));
}

bool ReplaceAll(std::wstring& text, const std::wstring& from,
                const std::wstring& to) {
    if (from.empty()) return false;
    bool changed = false;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

} // namespace

Workshop::Workshop(PluginManager* mgr, std::wstring pluginsDir)
    : pluginsDir_(std::move(pluginsDir)), mgr_(mgr) {}

// ---- 可测纯函数 -----------------------------------------------------------

std::wstring Workshop::SanitizeFolderName(const std::wstring& raw) {
    std::wstring out;
    bool lastDash = false;
    for (wchar_t c : raw) {
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            out += c;
            lastDash = false;
        } else if (c >= L'A' && c <= L'Z') {
            out += (wchar_t)(c - L'A' + L'a');
            lastDash = false;
        } else if (c == L' ' || c == L'_' || c == L'-' || c == L'.' ||
                   ((unsigned)c >= 0x80)) {   // 非法/非 ASCII 一律折叠成 '-'
            if (!out.empty() && !lastDash) { out += L'-'; lastDash = true; }
        }
        // 其余标点直接丢弃
    }
    while (!out.empty() && out.back() == L'-') out.pop_back();
    if (out.empty()) out = L"plugin";
    if (out.size() > 40) out.resize(40);
    return out;
}

// ---- SDK 释放 -------------------------------------------------------------

bool Workshop::EnsureSdk(std::wstring* errOut) {
    std::wstring sdk = SdkDir();
    ::CreateDirectoryW(sdk.c_str(), nullptr);   // 已存在无害

    struct { int id; const wchar_t* name; } files[] = {
        { kResGuide,    L"PLUGIN_DEV_GUIDE.md" },
        { kResApi,      L"xfs_plugin_api.h" },
        { kResTplC,     L"template.c" },
        { kResTplCpp,   L"template.cpp" },
        { kResBuildCmd, L"build.cmd" },
    };
    for (auto& f : files) {
        if (!ReleaseResource(f.id, L"md/c/h/cmd", sdk, f.name)) {
            if (errOut) *errOut = sdk + L"\\" + f.name;
            return false;
        }
    }
    // build.cmd 必须是 ANSI/无 BOM 才能被 cmd 正确执行（WriteFileBytes 原样）
    return true;
}

// ---- 脚手架 ---------------------------------------------------------------

std::wstring Workshop::Scaffold(const std::wstring& taskDesc,
                                const std::wstring& folderName,
                                const std::wstring& cmdLabel,
                                const std::wstring& description) {
    std::wstring folder = SanitizeFolderName(folderName);
    std::wstring proj = RootDir() + L"\\" + folder;
    // 同名项目已存在：追加序号（AI 迭代同一插件也各有目录，可追溯）
    for (int i = 2; ; ++i) {
        if (!::PathFileExistsW(proj.c_str())) break;
        proj = RootDir() + L"\\" + folder + L"-" + std::to_wstring(i);
    }
    std::wstring src = proj + L"\\src";
    std::wstring build = proj + L"\\build";
    if (!::CreateDirectoryW(RootDir().c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    ::CreateDirectoryW(proj.c_str(), nullptr);
    if (!::CreateDirectoryW(src.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    ::CreateDirectoryW(build.c_str(), nullptr);

    // 模板占位符替换（C 模板为主；C++ 备用一并落盘）。
    // 模板从 SdkDir() 读（EnsureSdk 已释放；单测可预置文件注入）
    std::wstring cmd = cmdLabel.empty() ? L"Run " + folder : cmdLabel;
    auto fill = [&](std::wstring t) {
        ReplaceAll(t, L"__PLUGIN_NAME__", folder);
        ReplaceAll(t, L"__PLUGIN_DESC__",
                   description.empty() ? taskDesc.substr(0, 200) : description);
        ReplaceAll(t, L"__PLUGIN_CMD__", cmd);
        return t;
    };
    std::string rawC, rawCpp, rawBcmd;
    std::wstring cPath = SdkDir() + L"\\template.c";
    std::wstring cppPath = SdkDir() + L"\\template.cpp";
    std::wstring bcmdPath = SdkDir() + L"\\build.cmd";
    if (!ReadFileBytes(cPath, rawC) || !ReadFileBytes(bcmdPath, rawBcmd))
        return L"";
    ReadFileBytes(cppPath, rawCpp);   // C++ 模板可选
    std::string cUtf8 = WideToUtf8(fill(Utf8ToWide(rawC)));
    std::string cppUtf8 = WideToUtf8(fill(Utf8ToWide(rawCpp)));
    if (!WriteFileBytes(src + L"\\plugin.c", cUtf8.data(), cUtf8.size()))
        return L"";
    WriteFileBytes(src + L"\\plugin.cpp", cppUtf8.data(), cppUtf8.size());

    // plugin.json（元数据——给人与 AI 看的项目说明；构建不依赖它）
    std::string json = "{\n  \"name\": \"" + WideToUtf8(folder) +
        "\",\n  \"version\": \"0.1.0\",\n  \"description\": " +
        "\"" + WideToUtf8(description.empty() ? taskDesc.substr(0, 200)
                                               : description) + "\",\n"
        "  \"command\": \"" + WideToUtf8(cmd) + "\",\n"
        "  \"task\": \"" + WideToUtf8(taskDesc) + "\"\n}\n";
    WriteFileBytes(proj + L"\\plugin.json", json.data(), json.size());

    // build.cmd 副本（_sdk 固定管线拷贝 + /I 替换为 SDK 绝对路径——
    // 相对路径依赖嵌套层级，绝对路径对 AI 改目录布局免疫）
    std::wstring bcmdText = Utf8ToWide(rawBcmd);
    ReplaceAll(bcmdText, L"__SDK_INCLUDE_DIR__", SdkDir());
    std::string bUtf8 = WideToUtf8(bcmdText);
    WriteFileBytes(proj + L"\\build.cmd", bUtf8.data(), bUtf8.size());

    Logger::Info("Workshop: scaffolded " + WideToUtf8(proj));
    return proj;
}

// ---- 提示词 ---------------------------------------------------------------

std::wstring Workshop::BuildPrompt(const std::wstring& projectDir,
                                   const std::wstring& taskDesc) {
    std::wstring p;
    p += L"Develop an xfsWinPad plugin and build it.\n\n";
    p += L"Task: " + taskDesc + L"\n\n";
    p += L"MUST-READ specification (read it first): " + SdkDir() +
         L"\\PLUGIN_DEV_GUIDE.md\n";
    p += L"Plugin API header (do not copy its content, #include it via "
         "build's include path): "
         + SdkDir() + L"\\xfs_plugin_api.h\n\n";
    p += L"Project folder (already scaffolded for you): " + projectDir + L"\n";
    p += L"  - src/plugin.c is a working template; REPLACE the TODO with the "
         L"real feature. Delete src/plugin.cpp if you stay with C.\n";
    p += L"  - plugin.json already carries the metadata; keep them in sync "
         L"with xfsPlugin_getInfo.\n"
         L"  - build.cmd is a FIXED pipeline (vswhere + vcvars64 + cl /LD). "
         L"Do NOT modify it in any way: the include path is already absolute "
         L"and correct. Do NOT add /TP or any language/compiler flags - "
         L"sources must compile as plain C (/TC is already set). "
         L"Run it from the project folder to produce build/output.dll.\n\n";
    p += L"Steps: 1) read the guide. 2) implement src/plugin.c. "
         L"3) run build.cmd until it compiles with zero warnings. "
         L"4) reply DONE when build/output.dll exists. "
         L"If the build fails, fix and retry - do not leave a broken build.\n\n";
    p += L"Rules: single source file src/plugin.c (extra headers allowed in "
         L"src/), no third-party libraries, no registry writes, no network; "
         L"UI-thread callbacks must stay fast; strings are UTF-8. "
         L"Quality-check against the guide's checklist before replying.";
    return p;
}

// ---- 安装 -----------------------------------------------------------------

std::wstring Workshop::InstallBuilt(const std::wstring& projectDir,
                                    std::wstring* errOut) {
    std::wstring dll = projectDir + L"\\build\\output.dll";
    if (!::PathFileExistsW(dll.c_str())) {
        if (errOut) *errOut = L"no build output (build\\output.dll missing)";
        return L"";
    }
    // 产物名：<folder>.dll（PluginManager 扫描 *.dll；workshop 目录不在扫描
    // 范围，拷到 plugins\<folder>\<folder>.dll——子目录布局）
    std::wstring folder = projectDir.substr(projectDir.find_last_of(L'\\') + 1);
    std::wstring dest = pluginsDir_ + L"\\" + folder;
    ::CreateDirectoryW(dest.c_str(), nullptr);
    std::wstring destDll = dest + L"\\" + folder + L".dll";
    if (::PathFileExistsW(destDll.c_str()) &&
        !::DeleteFileW(destDll.c_str())) {
        // 旧版本已加载中——LoadLibrary 锁文件，无法覆盖（升级场景：
        // 本版先要求卸载重启或换名；记日志返回原因）
        if (errOut) *errOut = L"old version loaded (restart app to upgrade)";
        return L"";
    }
    if (!::CopyFileW(dll.c_str(), destDll.c_str(), FALSE)) {
        if (errOut) *errOut = L"copy failed gle=" +
                              std::to_wstring((int)::GetLastError());
        return L"";
    }
    int loaded = mgr_ ? mgr_->LoadNewFrom(pluginsDir_) : 0;
    if (loaded <= 0) {
        // LoadNew 没吃进去：扫失败账本找原因
        if (mgr_ && !mgr_->LoadFailures().empty()) {
            const auto& f = mgr_->LoadFailures().back();
            if (errOut) *errOut = L"load failed: " + f.reason;
        } else if (errOut) {
            *errOut = L"load failed (no new plugin registered)";
        }
        return L"";
    }
    Logger::Info("Workshop: installed+loaded plugin '" + WideToUtf8(folder) +
                 "' -> " + WideToUtf8(destDll));
    return folder;
}

} // namespace xfs
