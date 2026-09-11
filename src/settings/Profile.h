#pragma once
// Profile — 单文件配置 profile（.xfprofile）：换机迁移 / 备份恢复。
//
// 把 %APPDATA%\xfsWinPad 下的用户配置原样打包成一个 JSON 文件：
//   settings.json（首选项）、shortcuts.json（快捷键覆盖）、
//   stylers.json（语言样式覆盖）、session.json（会话快照）、
//   themes\*.json（用户主题，逐文件内嵌）。
//
// 设计原则：**字节级保真**——各配置文件以原始文本内嵌（JSON 字符串转义），
// 导入时原样写回。字段零丢失，未来配置格式加字段/改键也天然兼容，
// 不依赖逐键结构化迁移（那是 settings.json 自身版本号的事）。
//
// 导入安全：覆盖前把将触碰的文件备份到 profile-backup-<时间戳>\；
// 主题文件名拒绝路径穿越（\ / 与 ".."）。导入后需重启编辑器生效
// （配置在各模块是启动时加载的状态，不做热应用）。
#include <string>

namespace xfs {

// 配置根目录：%APPDATA%\xfsWinPad（不存在则创建）
std::wstring ProfileConfigDir();

// 导出配置到 path（.xfprofile）。缺省的配置文件自动跳过（如从未改过
// stylers）。err 返回人类可读的失败原因（成功时置空）。
bool ProfileExportTo(const std::wstring& configDir, const std::wstring& path,
                     std::wstring* err);
// 从 path 导入并写回 configDir（先备份）。文件不存在/格式非法返回 false。
bool ProfileImportTo(const std::wstring& configDir, const std::wstring& path,
                     std::wstring* err);

inline bool ProfileExport(const std::wstring& path, std::wstring* err) {
    return ProfileExportTo(ProfileConfigDir(), path, err);
}
inline bool ProfileImport(const std::wstring& path, std::wstring* err) {
    return ProfileImportTo(ProfileConfigDir(), path, err);
}

} // namespace xfs
