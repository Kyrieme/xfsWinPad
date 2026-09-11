#pragma once
// xfsWinPad - 插件清单（plugin list）数据模型与解析器。
//
// 与 Notepad++ 的 nppPluginList 保持一致的清单布局：一个顶层对象
//   { "name","version","arch", "npp-plugins": [ {plugin...}, ... ] }
// 每个插件条目携带 Notepad++ 的 "folder-name"/"display-name"/"version"
// /"repository"/"description"/"author"/"homepage"/"id" 字段。
//
// 本地落地：%APPDATA%\xfsWinPad\plugins\pluginList.json；缺失时回落
// 到内嵌默认清单（见 DefaultPluginListJson），保证插件管理框零配置即可演示。

#include <string>
#include <vector>

namespace xfs {

struct CatalogPlugin {
    std::wstring folderName;    // 插件标识 / 安装目录名（与已安装插件匹配键）
    std::wstring displayName;   // 界面显示名
    std::wstring version;       // 当前可用版本
    std::wstring repository;    // 下载地址（预留：真实落盘时使用）
    std::wstring description;   // 描述
    std::wstring author;        // 作者
    std::wstring homepage;      // 主页
};

struct PluginCatalog {
    std::wstring listName;      // 清单名（N++ 顶层 "name"）
    std::wstring listVersion;   // 清单版本（N++ 顶层 "version"）
    std::vector<CatalogPlugin> plugins;
};

// 解析一份插件清单 JSON（兼容 nppPluginList 布局）。成功填满 catalog 并返回
// true；传入非法 JSON（缺 npp-plugins 数组等）返回 false，catalog 保持空。
bool ParsePluginList(const std::string& utf8Json, PluginCatalog& catalog);

// 清单在本地的路径。<pluginsDir>/pluginList.json
std::wstring PluginListFilePath(const std::wstring& pluginsDir);

// 加载清单：磁盘文件不存在/解析失败时返回内嵌默认清单（并置 usedDefaults=true）。
bool LoadPluginCatalog(const std::wstring& pluginsDir, PluginCatalog& catalog,
                       bool& usedDefaults);

// 内嵌默认清单（零配置可演示的种子数据；可被磁盘 pluginList.json 覆盖）。
const std::string& DefaultPluginListJson();

// 语义化版本比较：a>b→>0, a<b→<0, 相等→0。按 '.' 分段做数值比较，
// 例如 "1.10" > "1.9"。
int ComparePluginVersions(const std::wstring& a, const std::wstring& b);

} // namespace xfs