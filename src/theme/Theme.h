#pragma once
// xfsWinPad - built-in color themes (editor palette + chrome tokens)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

namespace xfs {

struct ThemeDef {
    const wchar_t* name;

    // editor base
    COLORREF editorBg, editorFg, caret, currentLineBack, selectionBack;
    // margins & markers
    COLORREF lineNumFg, lineNumBack, foldArrow, bookmark;
    // syntax roles
    COLORREF keyword, keyword2, comment, str, number, op, cls, preproc, special;
    // tab strip chrome (owner-drawn)
    COLORREF tabActiveBg, tabInactiveBg, tabActiveText, tabInactiveText,
             tabAccent, tabCloseGlyph, tabEdge;
};

namespace theme {

int Count();
const ThemeDef* At(int index);
// Case-insensitive lookup; falls back to the light theme.
const ThemeDef* Find(const wchar_t* name);

// 用户主题（%APPDATA%\xfsWinPad\themes\*.json，阶段 2a）：
// 启动时加载一次（内建 light/dark 永远兜底）；SaveThemeJson 把任一主题
// 写为同名 JSON（Style Configurator / 导入导出用）。
int LoadUserThemes();
bool SaveThemeJson(const ThemeDef* t);
// 写到任意路径（设置 > 导出主题 JSON… 用）；SaveThemeJson 即写注册表目录
bool SaveThemeJsonTo(const ThemeDef* t, const std::wstring& path);
std::wstring ThemesDir();

// ---- 阶段 4：主题基色编辑（Style Configurator > 主题颜色…）---------------------
// 用户主题的可变指针；内建主题/未找到返回 nullptr（Find 的 light 兜底不适用）
ThemeDef* MutableUserTheme(const wchar_t* name);
// 把 src（内建或用户）复制为新用户主题并落盘；同名覆盖。失败返回 nullptr
const ThemeDef* CreateUserThemeCopy(const wchar_t* src, const wchar_t* newName);
// 字段表访问（编辑器对话框按索引读写颜色）
int ThemeFieldCount();
const wchar_t* ThemeFieldKey(int i);
COLORREF ThemeGetColor(const ThemeDef* t, int i);
void ThemeSetColor(ThemeDef* t, int i, COLORREF c);

} // namespace theme
} // namespace xfs
