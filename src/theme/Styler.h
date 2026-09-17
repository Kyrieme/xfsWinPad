#pragma once
// xfsWinPad - Styler: 语法配色数据层（docs/settings-plan.md 阶段 2a）。
//
// 分层解析（高→低优先级）：
//   1. Global override（用户显式开启的 fg/bg/字体全局覆盖）
//   2. 每语言 StyleOverride（stylers.json 里该 (lexer, role) 的条目，
//      colorStyle 决定 fg/bg 是否继承主题 Default，语义对齐 NPP：
//        -1 = fg+bg 都用覆盖值（配置器勾选两项/取第二个色时写入）
//         0 = fg+bg 都继承 Default
//         1 = fg 用覆盖值，bg 继承
//         2 = bg 用覆盖值，fg 继承）
//   3. 主题角色色（ThemeDef 的 keyword/comment/... 与 editorBg）
//
// 持久化：%APPDATA%\xfsWinPad\stylers.json（人类可读，颜色 #RRGGBB）。
// 线程约定：仅 UI 线程访问。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <map>
#include <set>
#include <vector>

namespace xfs {

struct ThemeDef;   // Theme.h（避免把前向声明泄漏到全局命名空间）

// 语法角色（Editor 的 per-lexer 样式表把 Sci 样式号映射到这些角色）
// 注意：新增角色一律追加在 SR_Special 之后，保证已有角色的数值稳定
// （stylers.json 以角色名为键，但数值稳定可避免任何按序索引的调用点错位）。
enum StyleRoleEnum {
    SR_Comment = 0, SR_String, SR_Number, SR_Keyword, SR_Keyword2,
    SR_Operator, SR_Class, SR_Preproc, SR_Special,
    SR_Pass, SR_Fail,   // 批次 72：ATE 语义色（PASS/FAIL 判定）
    SR_Dim,             // 批次 72：弱化色（.pat 的 mask 向量 X/N/Z/U）
    SR_COUNT
};
// 角色名 ↔ 枚举（stylers.json 的键）
const char* StyleRoleName(int role);
int StyleRoleFromName(const std::string& name);   // -1 = 未知

// 单个 (lexer, role) 的用户覆盖
struct StyleOverride {
    bool defined = false;
    int colorStyle = -1;      // 见文件头注释
    COLORREF fg = 0, bg = 0;
    bool bold = false, italic = false, underline = false;
    std::wstring font;        // 空 = 继承默认字体
    int fontSize = 0;         // 0 = 继承默认字号
};

// Global Styles > 各 override 开关
struct GlobalOverride {
    bool enableFg = false, enableBg = false;
    COLORREF fg = 0, bg = 0;
    bool bold = false, italic = false, underline = false;
};

class StylerStore {
public:
    // ---- 查询/解析链 -----------------------------------------------------
    const GlobalOverride& Global() const { return global_; }
    // (lexer, role) 的覆盖条目；无则返回未定义的静态空对象。
    // 词法器名是 ASCII，char*/wchar_t* 两个重载等价。
    const StyleOverride& Override(const char* lexer, int role) const;
    const StyleOverride& OverrideW(const wchar_t* lexer, int role) const;

    // 解析结果（应用 SCI_STYLESET* 前调用）
    COLORREF ResolveFg(const char* lexer, int role, const ThemeDef& theme) const;
    COLORREF ResolveBg(const char* lexer, int role, const ThemeDef& theme) const;
    COLORREF ResolveFgW(const wchar_t* lexer, int role, const ThemeDef& theme) const;
    COLORREF ResolveBgW(const wchar_t* lexer, int role, const ThemeDef& theme) const;
    // 输出可选字体覆盖（空 = 不改字体）
    void ResolveFont(const char* lexer, const ThemeDef& theme,
                     std::wstring* fontOut, int* sizeOut,
                     bool* boldOut, bool* italicOut, bool* underlineOut) const;

    // ---- 修改（Style Configurator 用） -------------------------------------
    StyleOverride& MutableOverride(const char* lexer, int role);
    StyleOverride& MutableOverrideW(const wchar_t* lexer, int role);   // 取出并标记 defined
    GlobalOverride& MutableGlobal() { return global_; }
    void ResetGlobal() { global_ = GlobalOverride{}; }                  // 清全局覆盖
    void ClearOverride(const char* lexer, int role);
    void ClearOverrideW(const wchar_t* lexer, int role);
    void ClearLexer(const char* lexer);
    bool HasLexer(const char* lexer) const;
    // 枚举已有覆盖的语言名（对话框语言列表用，按字母序）
    std::vector<std::wstring> Languages() const;

    // ---- 每语言整行字体（语言级字体覆盖，作用于该词法器的 STYLE_DEFAULT）----
    const std::wstring* LanguageFont(const char* lexer) const;
    int LanguageFontSize(const char* lexer) const;
    void SetLanguageFont(const char* lexer, const std::wstring& font, int size);
    void ClearLanguageFont(const char* lexer);

    // ---- 用户扩展名 → 词法器（优先于内置 DetectLanguage）--------------------
    // ext 小写、不带点；返回对应词法器名或 nullptr。
    const char* UserLexerForExt(const char* ext) const;
    const std::map<std::string, std::string>& UserExts() const { return userExts_; }
    void SetUserExt(const char* ext, const char* lexer);   // lexer 为空则删除
    void ClearUserExts() { userExts_.clear(); }

    // ---- 持久化 ------------------------------------------------------------
    static const wchar_t* FilePath();   // %APPDATA%\xfsWinPad\stylers.json
    bool Load(const std::wstring& path);    // 缺失/损坏 → 空覆盖层，返回 false
    bool Save(const std::wstring& path) const;
    void Reset();                           // 清空全部覆盖

    // 对话框快照（取消还原用）；全部成员皆值语义，默认拷贝即可
    StylerStore Snapshot() const { return *this; }
    void RestoreFrom(const StylerStore& s) { *this = s; }

    // ---- 词法器家族清单（与 Editor 的 kFamilies 同一names，供对话框列表）----
    static int LexerFamilyCount();
    static const char* LexerFamilyName(int i);

private:
    GlobalOverride global_;
    std::map<std::wstring, std::map<int, StyleOverride>> langs_;   // lexer → role → override
    std::map<std::wstring, std::pair<std::wstring, int>> langFonts_;  // lexer → (font, size)
    std::map<std::string, std::string> userExts_;   // ext(小写) → lexer
};

StylerStore& GlobalStyler();   // 进程级单例

} // namespace xfs
