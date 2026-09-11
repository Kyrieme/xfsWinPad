#pragma once
// xfsWinPad - Workshop: AI 插件工场。
//
// 让 AI 按本仓库的插件开发标准（PLUGIN_DEV_GUIDE.md + xfs_plugin_api.h）
// 在受控目录里生成插件源码，用固定构建管线（build.cmd + vswhere/vcvars64）
// 编译，产物自动安装进 PluginManager 并热加载——「对话即扩展」。
//
// 目录布局（全部在 %APPDATA%\xfsWinPad\plugins\ 下）：
//   plugins\
//     _sdk\                     <- EnsureSdk() 从 exe 资源释放（版本戳幂等）
//       PLUGIN_DEV_GUIDE.md
//       xfs_plugin_api.h
//       template.c / template.cpp
//       build.cmd
//     workshop\                 <- AI 的工作区（每次 /plugin 一个子目录）
//       <name>\
//         plugin.json           <- 元数据（name/version/description/cmd）
//         src\plugin.c          <- 模板替换占位符后落盘，AI 之override
//         build.cmd              <- _sdk 固定管线副本（勿改）
//         build\output.dll      <- 构建产物
//
// 线程模型：EnsureSdk/Scaffold/CollectAndInstall 均 UI 线程调用（纯文件
// 操作 + 目录枚举，毫秒级）；编译由 AI 通过 bash 工具自己跑 build.cmd
//（serve 工作目录 = 项目根，workshop 在 %APPDATA%，路径会以绝对形式给出）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace xfs {

class PluginManager;

class Workshop {
public:
    Workshop(PluginManager* mgr, std::wstring pluginsDir);

    // 幂等释放 SDK 到 plugins\_sdk（资源内嵌 RCDATA）。
    // SDK 资源版本变化时覆盖重写（版本戳 = ABI 版本 + 指南文件长度哈希）。
    bool EnsureSdk(std::wstring* errOut = nullptr);
    std::wstring SdkDir() const { return pluginsDir_ + L"\\_sdk"; }
    std::wstring RootDir() const { return pluginsDir_ + L"\\workshop"; }

    // 按用户任务描述生成插件项目脚手架。folderName 合法化（只留
    // [a-z0-9-]，空则 "plugin"）。返回项目目录（空=失败）。
    // cmdLabel: 命令面板里显示的命令名（英文）。
    std::wstring Scaffold(const std::wstring& taskDesc,
                          const std::wstring& folderName,
                          const std::wstring& cmdLabel,
                          const std::wstring& description);

    // 构建提示词：标准文档路径 + SDK 路径 + 脚手架路径 + 任务描述。
    std::wstring BuildPrompt(const std::wstring& projectDir,
                             const std::wstring& taskDesc);

    // 安装：扫描 projectDir\build\output.dll → 拷到 plugins\<folder>\
    // → PluginManager::LoadNew() 热加载。成功返回插件名。
    // 没有产物/加载失败时 errOut 给出原因（喂回 AI 修一轮）。
    std::wstring InstallBuilt(const std::wstring& projectDir,
                              std::wstring* errOut);

    // ---- 可测纯函数 ----
    // 文件夹名合法化：小写、空格转 '-'、非法字符丢弃、空回退 "plugin"。
    static std::wstring SanitizeFolderName(const std::wstring& raw);

private:
    std::wstring pluginsDir_;
    PluginManager* mgr_;
};

} // namespace xfs
