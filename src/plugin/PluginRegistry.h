#pragma once
// xfsWinPad - 已安装插件判定与本地安装记录。
//
// 与 Notepad++ 一致：已安装插件 = plugins 目录下真实存在的 DLL。
// 额外叠加一份本地记录（<pluginsDir>\installed.json），支撑「仅 UI 流转」
// 的安装/更新演示——用户点安装/移除时只写记录、不动磁盘真实 DLL。
//
// 列表来源（并有取并集）：
//   * 磁盘扫描：根目录 *.dll 以 base 名记；一级子目录内存在 DLL 时以子目录名记
//     （N++ 惯例 plugins\<folder>\*.dll）。
//   * 本地记录：文件里 folder -> version（虚拟安装/更新产物）。

#include <map>
#include <string>
#include <vector>

namespace xfs {

struct InstalledPlugin {
    std::wstring folder;     // 与 CatalogPlugin.folderName 匹配的键
    std::wstring version;    // 判定「可更新」的既装版本
    bool realFile = false;   // 磁盘上真有 DLL
    std::wstring path;       // 磁盘路径（real 才有值）
};

class PluginRegistry {
public:
    explicit PluginRegistry(std::wstring pluginsDir);
    // 重扫磁盘 + 重载记录，重建已安装集合。
    void Refresh();
    const std::vector<InstalledPlugin>& Installed() const { return installed_; }

    bool IsInstalled(const std::wstring& folder) const;
    std::wstring VersionOf(const std::wstring& folder) const;
    bool IsRealFile(const std::wstring& folder) const;

    // 「安装/更新」：写本地记录（真实文件不动）。立即生效并持久化。
    void MarkInstalled(const std::wstring& folder, const std::wstring& version);
    // 「移除」：清本地记录。若这是磁盘真实 DLL，不清文件并返回 false
    // （调用方据此提示需手动删除），仅清除记录。
    bool TryUninstall(const std::wstring& folder);

private:
    void LoadRecords();
    void SaveRecords();
    void ScanDisk();

    std::wstring pluginsDir_;
    std::vector<InstalledPlugin> installed_;          // folder -> 条目
    std::map<std::wstring, std::wstring> records_;    // folder -> version
};

// 读取一个 DLL 的版本资源（FileVersion 数字段），失败返回空串。
std::wstring ReadDllFileVersion(const std::wstring& path);

} // namespace xfs