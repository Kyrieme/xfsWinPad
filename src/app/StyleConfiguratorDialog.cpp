// StyleConfiguratorDialog.cpp — 语言样式配置器实现（阶段 2b）。
// 复用 PreferencesDialog 的配方（State + GWLP_USERDATA + 自建控件 + 模态循环）。
// 关键约定（settings-plan 踩坑 2/3 的教训）：
//   * 每个控件（含标签）都进显隐/追踪清单；
//   * 程序化改控件期间设置屏蔽标志，EN_CHANGE 等不当作用户输入；
//   * 关闭路径全部收口到 DestroyWindow，模态循环出口统一恢复宿主。
#include "StyleConfiguratorDialog.h"
#include "../theme/Styler.h"
#include "../theme/Theme.h"
#include "../core/JsonLite.h"
#include "../core/Log.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>
#include <utility>
#include <vector>

namespace xfs {

namespace {

constexpr unsigned IDC_THEME_L  = 3301;
constexpr unsigned IDC_THEME    = 3302;
constexpr unsigned IDC_LANG_L   = 3303;
constexpr unsigned IDC_LANG     = 3304;
constexpr unsigned IDC_STYLE_L  = 3305;
constexpr unsigned IDC_STYLE    = 3306;
constexpr unsigned IDC_FG_ON    = 3307;
constexpr unsigned IDC_FG_BTN   = 3308;
constexpr unsigned IDC_BG_ON    = 3309;
constexpr unsigned IDC_BG_BTN   = 3310;
constexpr unsigned IDC_BOLD     = 3311;
constexpr unsigned IDC_ITALIC   = 3312;
constexpr unsigned IDC_UNDER    = 3313;
constexpr unsigned IDC_OK       = 3314;
constexpr unsigned IDC_CANCEL   = 3315;
constexpr unsigned IDC_HINT     = 3316;
constexpr unsigned IDC_RESET    = 3317;   // 恢复默认（当前样式 → 主题色）
// 阶段 4 补项：Global Styles 编辑 + 每语言字体
constexpr unsigned IDC_LANGFONT_BTN = 3318;   // 每语言整行字体
constexpr unsigned IDC_G_FG_ON   = 3319;   // Global 前景开关
constexpr unsigned IDC_G_FG_BTN  = 3320;   // Global 前景色
constexpr unsigned IDC_G_BG_ON   = 3321;   // Global 背景开关
constexpr unsigned IDC_G_BG_BTN  = 3322;   // Global 背景色
constexpr unsigned IDC_G_RESET   = 3326;   // Global 恢复默认
constexpr unsigned IDC_THEMEDLG_BTN = 3327;   // 主题颜色…（常显，不属于任何页）

// 语言列表第 0 项 = Global Styles（override global ），其后为词法器家族
constexpr int kGlobalEntry = 0;

struct State {
    HINSTANCE inst = nullptr;
    HWND hOk = nullptr, hCancel = nullptr;
    HWND hFgBtn = nullptr, hBgBtn = nullptr, hResetDef = nullptr;
    HWND hLangFontBtn = nullptr;
    HWND hGfgBtn = nullptr, hGbgBtn = nullptr;
    IStyleApplier* applier = nullptr;
    StylerStore snapshot;         // 取消还原快照
    std::wstring origTheme;       // 主题还原
    std::wstring lexer;           // 当前选中的词法器（"" = Global Styles）
    int role = SR_Comment;        // 当前选中的样式角色
    bool loading = false;
    bool globalPage = false;
    bool fgPicked = false, bgPicked = false;   // 用户取过色即不回写主题种子
    std::vector<HWND> ctrls;
    std::vector<unsigned> ids;
    // 样式区的控件（随语言切换显隐；Global 页显示提示）
    std::vector<HWND> styleCtrls;
    // 阶段 4 补项：仅 Global 页可见（全局覆盖）vs 仅语言页可见（整行字体/用户关键字）
    std::vector<HWND> globalCtrls;
    std::vector<HWND> langCtrls;

    HWND CtrlOf(unsigned id) const {
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == id) return ctrls[i];
        return nullptr;
    }
    void Track(HWND c, unsigned id) {
        if (c) { ctrls.push_back(c); ids.push_back(id); }
    }
    static State* Get(HWND h) { return (State*)::GetWindowLongPtrW(h, GWLP_USERDATA); }
};

int U(HWND h, int px) { return ::MulDiv(px, ::GetDpiForWindow(h), 96); }

// ---- 控件工厂（同 PreferencesDialog 的教训：全部 Track） --------------------

HWND MkLabel(HWND dlg, HINSTANCE inst, unsigned id, const wchar_t* text,
             int x, int y, int w, HFONT font) {
    HWND c = ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                               U(dlg, x), U(dlg, y), U(dlg, w), U(dlg, 20),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

HWND MkButton(HWND dlg, HINSTANCE inst, unsigned id, const wchar_t* text,
              int x, int y, int w, int h, DWORD extra, HFONT font) {
    HWND c = ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | extra,
                               U(dlg, x), U(dlg, y), U(dlg, w), U(dlg, h),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

HWND MkCheck(HWND dlg, HINSTANCE inst, unsigned id, const wchar_t* text,
             int x, int y, int w, HFONT font) {
    return MkButton(dlg, inst, id, text, x, y, w, 20,
                    WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, font);
}

HWND MkCombo(HWND dlg, HINSTANCE inst, unsigned id, int x, int y, int w,
             HFONT font) {
    HWND c = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                               U(dlg, x), U(dlg, y), U(dlg, w), U(dlg, 160),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

HWND MkList(HWND dlg, HINSTANCE inst, unsigned id, int x, int y, int w, int h,
            HFONT font) {
    HWND c = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY |
                               LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                               U(dlg, x), U(dlg, y), U(dlg, w), U(dlg, h),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

HWND MkEdit(HWND dlg, HINSTANCE inst, unsigned id, int x, int y, int w, int h,
            DWORD extra, HFONT font) {
    HWND c = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
                               U(dlg, x), U(dlg, y), U(dlg, w), U(dlg, h),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

// ---- 数据 ↔ 控件 ------------------------------------------------------------

// 词法器家族名恒为 ASCII：宽→窄显式转换（词法器名仅 ASCII，安全窄化）
std::string LexerNarrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t ch : w) s.push_back((char)ch);
    return s;
}

void LoadLanguageList(State& st, HWND hLang) {
    (void)st;
    ::SendMessageW(hLang, LB_RESETCONTENT, 0, 0);
    ::SendMessageW(hLang, LB_ADDSTRING, 0, (LPARAM)L"Global Styles");
    const int n = StylerStore::LexerFamilyCount();
    for (int i = 0; i < n; ++i) {
        const char* fam = StylerStore::LexerFamilyName(i);
        std::wstring w(fam, fam + strlen(fam));
        ::SendMessageW(hLang, LB_ADDSTRING, 0, (LPARAM)w.c_str());
    }
    ::SendMessageW(hLang, LB_SETCURSEL, 0, 0);
}

void LoadStyleList(State& st, HWND hStyle) {
    ::SendMessageW(hStyle, LB_RESETCONTENT, 0, 0);
    for (int r = 0; r < SR_COUNT; ++r) {
        const char* rn = StyleRoleName(r);
        std::wstring w(rn, rn + strlen(rn));
        ::SendMessageW(hStyle, LB_ADDSTRING, 0, (LPARAM)w.c_str());
    }
}

std::wstring CurrentFgText(const State& st) {
    if (st.globalPage) {
        const GlobalOverride& g = GlobalStyler().Global();
        return g.enableFg ? json::ColorToWstr(g.fg) : Tr(L"style.themedefault");
    }
    const StyleOverride& o = st.lexer.empty()
        ? StyleOverride{} : GlobalStyler().OverrideW(st.lexer.c_str(), st.role);
    if (!o.defined || (o.colorStyle == 0) || (o.colorStyle == 2))
        return Tr(L"style.themedefault");
    return json::ColorToWstr(o.fg);
}

std::wstring CurrentBgText(const State& st) {
    if (st.globalPage) {
        const GlobalOverride& g = GlobalStyler().Global();
        return g.enableBg ? json::ColorToWstr(g.bg) : Tr(L"style.themedefault");
    }
    const StyleOverride& o = st.lexer.empty()
        ? StyleOverride{} : GlobalStyler().OverrideW(st.lexer.c_str(), st.role);
    if (!o.defined || (o.colorStyle == 0) || (o.colorStyle == 1))
        return Tr(L"style.themedefault");
    return json::ColorToWstr(o.bg);
}

void RefreshStyleUi(State& st) {
    auto setCheck = [&](unsigned id, bool on) {
        if (HWND c = st.CtrlOf(id))
            ::SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    auto setText = [&](HWND c, const std::wstring& t) {
        if (c) ::SetWindowTextW(c, t.c_str());
    };

    if (st.globalPage) {
        const GlobalOverride& g = GlobalStyler().Global();
        setCheck(IDC_G_FG_ON, g.enableFg);
        setCheck(IDC_G_BG_ON, g.enableBg);
        setText(st.hGfgBtn, g.enableFg ? json::ColorToWstr(g.fg) : Tr(L"style.themedefault"));
        setText(st.hGbgBtn, g.enableBg ? json::ColorToWstr(g.bg) : Tr(L"style.themedefault"));
        setCheck(IDC_BOLD, g.bold);
        setCheck(IDC_ITALIC, g.italic);
        setCheck(IDC_UNDER, g.underline);
        if (HWND hint = st.CtrlOf(IDC_HINT))
            ::SetWindowTextW(hint, Tr(L"style.hint.global"));
        return;
    }

    const StyleOverride* po = nullptr;
    if (!st.lexer.empty())
        po = &GlobalStyler().OverrideW(st.lexer.c_str(), st.role);
    const StyleOverride none{};
    const StyleOverride& o = po ? *po : none;

    const bool fgOwn = o.defined && (o.colorStyle == -1 || o.colorStyle == 1);
    const bool bgOwn = o.defined && (o.colorStyle == -1 || o.colorStyle == 2);
    setCheck(IDC_FG_ON, fgOwn);
    setCheck(IDC_BG_ON, bgOwn);
    setText(st.hFgBtn, fgOwn ? json::ColorToWstr(o.fg) : Tr(L"style.themedefault"));
    setText(st.hBgBtn, bgOwn ? json::ColorToWstr(o.bg) : Tr(L"style.themedefault"));
    setCheck(IDC_BOLD, o.defined && o.bold);
    setCheck(IDC_ITALIC, o.defined && o.italic);
    setCheck(IDC_UNDER, o.defined && o.underline);
    // 每语言整行字体按钮 + 用户关键字
    if (st.hLangFontBtn) {
        std::string lexerN = LexerNarrow(st.lexer);
        const std::wstring* lf = GlobalStyler().LanguageFont(lexerN.c_str());
        int lsz = GlobalStyler().LanguageFontSize(lexerN.c_str());
        std::wstring t = (lf && !lf->empty())
            ? I18n::Instance().Fmt(L"prefs.fontfmt",
                  {*lf, lsz > 0 ? std::to_wstring(lsz) : Tr(L"style.defsize")})
            : Tr(L"style.langfontdefault");
        ::SetWindowTextW(st.hLangFontBtn, t.c_str());
    }
    if (HWND hint = st.CtrlOf(IDC_HINT))
        ::SetWindowTextW(hint, Tr(L"style.hint.style"));
}

void ApplyLive(State& st) {
    // 由控件状态重建当前覆盖项，然后实时刷新全部编辑器
    auto check = [&](unsigned id) -> bool {
        HWND c = st.CtrlOf(id);
        return c && ::SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };

    // Global 页：写 GlobalOverride（阶段 4 补项）
    if (st.globalPage) {
        GlobalOverride& g = GlobalStyler().MutableGlobal();
        const bool fgOn = check(IDC_G_FG_ON);
        const bool bgOn = check(IDC_G_BG_ON);
        g.enableFg = fgOn;
        g.enableBg = bgOn;
        // 取色按钮直接写 g.fg/g.bg；这里只补主题种子（若从未设置过）
        const ThemeDef* curTheme = theme::Find(
            st.applier && st.applier->CurrentThemeName() ? st.applier->CurrentThemeName()
                                                         : L"light");
        if (fgOn && g.fg == 0 && curTheme) g.fg = curTheme->editorFg;
        if (bgOn && g.bg == 0 && curTheme) g.bg = curTheme->editorBg;
        g.bold = check(IDC_BOLD);
        g.italic = check(IDC_ITALIC);
        g.underline = check(IDC_UNDER);
        RefreshStyleUi(st);
        if (st.applier) st.applier->RestyleAll();
        return;
    }

    const bool fgOn = check(IDC_FG_ON);
    const bool bgOn = check(IDC_BG_ON);
    // 首次启用时以【当前主题】的解析色为起点——必须在写入 colorStyle 之前
    // 解析（colorStyle=1 会让 Resolve 返回 o.fg 自身，形成循环读取，
    // 起点/黑色覆盖用户所选颜色的历史 bug 即源于此）。
    const ThemeDef* curTheme = theme::Find(
        st.applier && st.applier->CurrentThemeName() ? st.applier->CurrentThemeName()
                                                     : L"light");
    const COLORREF themeFg = curTheme
        ? GlobalStyler().ResolveFgW(st.lexer.c_str(), st.role, *curTheme)
        : RGB(0, 0, 0);
    const COLORREF themeBg = curTheme
        ? GlobalStyler().ResolveBgW(st.lexer.c_str(), st.role, *curTheme)
        : RGB(0xFF, 0xFF, 0xFF);
    StyleOverride& o = GlobalStyler().MutableOverrideW(st.lexer.c_str(), st.role);
    const bool wasDefined = o.defined;
    o.defined = fgOn || bgOn || check(IDC_BOLD) || check(IDC_ITALIC) || check(IDC_UNDER);
    if (fgOn && bgOn)      o.colorStyle = -1;
    else if (fgOn)         o.colorStyle = 1;
    else if (bgOn)         o.colorStyle = 2;
    else                   o.colorStyle = 0;
    // 种子：仅在该侧从未设置过时以主题色起步；用户取过色（fgPicked/bgPicked）
    // 或已有覆盖值时不得回写，否则会冲掉用户选择（含黑色 0,0,0）。
    if (fgOn && !st.fgPicked && !wasDefined && o.fg == 0) o.fg = themeFg;
    if (bgOn && !st.bgPicked && !wasDefined && o.bg == 0) o.bg = themeBg;
    o.bold = check(IDC_BOLD);
    o.italic = check(IDC_ITALIC);
    o.underline = check(IDC_UNDER);
    if (!o.defined) {
        // 完全解绑：清拾取标记与残留色值——否则再勾选会把上次的颜色
        // 原样带回（用户视角即"恢复默认不生效"）。
        st.fgPicked = false;
        st.bgPicked = false;
        o.fg = 0;
        o.bg = 0;
        GlobalStyler().ClearOverrideW(st.lexer.c_str(), st.role);
    }
    RefreshStyleUi(st);
    if (st.applier) st.applier->RestyleAll();
}

void PickColor(State& st, bool fg, bool global) {
    COLORREF custom[16] = {};
    COLORREF cur = fg ? RGB(0, 0, 0) : RGB(0xFF, 0xFF, 0xFF);
    if (global) {
        const GlobalOverride& g = GlobalStyler().Global();
        cur = fg ? (g.enableFg ? g.fg : cur) : (g.enableBg ? g.bg : cur);
    } else {
        const StyleOverride* po = st.lexer.empty()
            ? nullptr : &GlobalStyler().OverrideW(st.lexer.c_str(), st.role);
        if (po) cur = fg ? po->fg : po->bg;
    }
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = ::GetParent(st.hFgBtn);
    cc.lpCustColors = custom;
    cc.rgbResult = cur;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!::ChooseColorW(&cc)) return;
    // 取色即启用该侧覆盖（N++ 语义：选色 = 使用自定义色）
    if (global) {
        if (HWND c = st.CtrlOf(fg ? IDC_G_FG_ON : IDC_G_BG_ON))
            ::SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
        GlobalOverride& g = GlobalStyler().MutableGlobal();
        if (fg) g.fg = cc.rgbResult; else g.bg = cc.rgbResult;
        RefreshStyleUi(st);
        if (st.applier) st.applier->RestyleAll();
        return;
    }
    if (HWND c = st.CtrlOf(fg ? IDC_FG_ON : IDC_BG_ON))
        ::SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
    StyleOverride& o = GlobalStyler().MutableOverrideW(st.lexer.c_str(), st.role);
    o.defined = true;
    if (fg) {
        o.fg = cc.rgbResult;
        st.fgPicked = true;
        if (o.colorStyle == 2) o.colorStyle = -1; else if (o.colorStyle == 0) o.colorStyle = 1;
    } else {
        o.bg = cc.rgbResult;
        st.bgPicked = true;
        if (o.colorStyle == 1) o.colorStyle = -1; else if (o.colorStyle == 0) o.colorStyle = 2;
    }
    RefreshStyleUi(st);
    if (st.applier) st.applier->RestyleAll();
}

// ChooseFont 封装：写语言级整行字体
void PickFont(HWND dlg, State& st) {
    LOGFONTW lf{};
    std::string lexerN = LexerNarrow(st.lexer);
    const std::wstring* cur = GlobalStyler().LanguageFont(lexerN.c_str());
    const std::wstring empty;
    const std::wstring& name = cur && !cur->empty() ? *cur : empty;
    wcscpy_s(lf.lfFaceName, name.empty() ? L"Consolas" : name.c_str());
    HDC dc = ::GetDC(dlg);
    const int curSize = GlobalStyler().LanguageFontSize(lexerN.c_str());
    lf.lfHeight = -::MulDiv(curSize > 0 ? curSize : 10,
                            ::GetDeviceCaps(dc, LOGPIXELSY), 72);

    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = dlg;
    cf.lpLogFont = &lf;
    cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_FIXEDPITCHONLY |
               CF_FORCEFONTEXIST;
    const BOOL ok = ::ChooseFontW(&cf);
    const int pt = -::MulDiv(lf.lfHeight, 72, ::GetDeviceCaps(dc, LOGPIXELSY));
    ::ReleaseDC(dlg, dc);
    if (!ok) return;
    if (pt < 5 || pt > 72) return;

    GlobalStyler().SetLanguageFont(lexerN.c_str(), lf.lfFaceName, pt);
    RefreshStyleUi(st);
    if (st.applier) st.applier->RestyleAll();
}

// ---- 页面/语言切换 -----------------------------------------------------------

void ApplyLanguageSelection(State& st, HWND hLang, HWND hStyle) {
    const int sel = (int)::SendMessageW(hLang, LB_GETCURSEL, 0, 0);
    st.globalPage = (sel <= kGlobalEntry);
    if (!st.globalPage) {
        const char* fam = StylerStore::LexerFamilyName(sel - 1);
        st.lexer.assign(fam, fam + strlen(fam));
    } else {
        st.lexer.clear();
    }
    st.role = SR_Comment;
    st.fgPicked = false;
    st.bgPicked = false;
    ::SendMessageW(hStyle, LB_SETCURSEL, 0, 0);
    // 样式编辑区：Global 页显示全局覆盖控件，语言页显示 per-style + per-language
    if (st.globalPage) {
        for (HWND c : st.styleCtrls) { ::ShowWindow(c, SW_HIDE); ::EnableWindow(c, FALSE); }
        for (HWND c : st.globalCtrls) { ::ShowWindow(c, SW_SHOW); ::EnableWindow(c, TRUE); }
        for (HWND c : st.langCtrls) { ::ShowWindow(c, SW_HIDE); ::EnableWindow(c, FALSE); }
    } else {
        for (HWND c : st.globalCtrls) { ::ShowWindow(c, SW_HIDE); ::EnableWindow(c, FALSE); }
        for (HWND c : st.langCtrls) { ::ShowWindow(c, SW_SHOW); ::EnableWindow(c, TRUE); }
        for (HWND c : st.styleCtrls) { ::ShowWindow(c, SW_SHOW); ::EnableWindow(c, TRUE); }
    }
    RefreshStyleUi(st);
}

// ---- 主题基色编辑子对话框（阶段 4 补项：主题颜色本身的编辑）--------------------
//
// 只编辑「用户主题」：内建 light/dark 打开时先复制为 <名>-custom 副本再编辑。
// 改动直接写注册表内 ThemeDef 并 SwitchThemeByName 即时预览；取消 = 快照还原
// 内存（未落盘无需回滚文件）；保存 = SaveThemeJson 写 themes\<名>.json。

constexpr unsigned IDC_TED_NAME   = 3340;
constexpr unsigned IDC_TED_LIST   = 3341;
constexpr unsigned IDC_TED_SWATCH = 3342;    // BS_OWNERDRAW 色板
constexpr unsigned IDC_TED_PICK   = 3343;
constexpr unsigned IDC_TED_HINT   = 3344;
constexpr unsigned IDC_TED_OK     = 3345;
constexpr unsigned IDC_TED_CANCEL = 3346;

struct ThemeEdState {
    IStyleApplier* applier = nullptr;
    ThemeDef* def = nullptr;                 // 注册表内可变条目（deque 指针稳定）
    ThemeDef snapshot{};                     // 取消还原
    std::wstring name;
    int field = 0;
    static ThemeEdState* Get(HWND h) {
        return (ThemeEdState*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    }
};

void ThemeEdPick(HWND h, ThemeEdState& s) {
    if (!s.def) return;
    COLORREF custom[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = h;
    cc.lpCustColors = custom;
    cc.rgbResult = theme::ThemeGetColor(s.def, s.field);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!::ChooseColorW(&cc)) return;
    theme::ThemeSetColor(s.def, s.field, cc.rgbResult);
    if (HWND sw = ::GetDlgItem(h, IDC_TED_SWATCH))
        ::InvalidateRect(sw, nullptr, TRUE);
    if (s.applier) s.applier->SwitchThemeByName(s.name.c_str());   // 实时预览
}

LRESULT CALLBACK ThemeEdProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ThemeEdState* s = ThemeEdState::Get(h);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;

        case WM_COMMAND:
            if (!s) return 0;
            switch (LOWORD(wp)) {
                case IDC_TED_PICK:
                    ThemeEdPick(h, *s);
                    return 0;
                case IDC_TED_LIST:
                    if (HIWORD(wp) == LBN_SELCHANGE) {
                        s->field = (int)::SendMessageW((HWND)lp, LB_GETCURSEL, 0, 0);
                        if (s->field < 0) s->field = 0;
                        if (HWND sw = ::GetDlgItem(h, IDC_TED_SWATCH))
                            ::InvalidateRect(sw, nullptr, TRUE);
                    } else if (HIWORD(wp) == LBN_DBLCLK) {
                        ThemeEdPick(h, *s);
                    }
                    return 0;
                case IDC_TED_OK:
                    theme::SaveThemeJson(s->def);      // 写 themes\<名>.json
                    ::DestroyWindow(h);
                    return 0;
                case IDC_TED_CANCEL:
                case IDCANCEL:
                    if (s->def) *s->def = s->snapshot; // 还原预览（未落盘）
                    if (s->applier) s->applier->SwitchThemeByName(s->name.c_str());
                    ::DestroyWindow(h);
                    return 0;
            }
            break;

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
            if (di->CtlID == IDC_TED_SWATCH && s && s->def) {
                HBRUSH br = ::CreateSolidBrush(
                    theme::ThemeGetColor(s->def, s->field));
                ::FillRect(di->hDC, &di->rcItem, br);
                ::DeleteObject(br);
                ::FrameRect(di->hDC, &di->rcItem,
                            (HBRUSH)::GetStockObject(BLACK_BRUSH));
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            if (s && s->def) *s->def = s->snapshot;
            if (s && s->applier) s->applier->SwitchThemeByName(s->name.c_str());
            ::DestroyWindow(h);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

// 打开主题基色编辑器；返回编辑的主题名（副本场景=新名），失败返回空。
std::wstring RunThemeColorEditor(HWND parent, HINSTANCE hInst,
                                 IStyleApplier* applier) {
    const std::wstring cur = (applier && applier->CurrentThemeName())
                                 ? applier->CurrentThemeName() : L"light";
    std::wstring target = cur;
    if (!theme::MutableUserTheme(cur.c_str())) {
        // 内建主题 → 复制为 <名>-custom（重名续编 -custom2…）后编辑
        auto exists = [](const std::wstring& n) {
            for (int i = 0; i < theme::Count(); ++i)
                if (_wcsicmp(theme::At(i)->name, n.c_str()) == 0) return true;
            return false;
        };
        std::wstring cand = cur + L"-custom";
        for (int n = 2; exists(cand); ++n)
            cand = cur + L"-custom" + std::to_wstring(n);
        if (!theme::CreateUserThemeCopy(cur.c_str(), cand.c_str())) return L"";
        target = cand;
    }
    ThemeDef* def = theme::MutableUserTheme(target.c_str());
    if (!def) return L"";

    static const wchar_t kClass[] = L"xfsWinPadThemeEd";
    static bool regDone = false;
    if (!regDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ThemeEdProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        regDone = true;
    }

    ThemeEdState* s = new ThemeEdState();
    s->applier = applier;
    s->def = def;
    s->snapshot = *def;
    s->name = target;

    const int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return ::MulDiv(px, dpi, 96); };
    HFONT font = ::CreateFontW(-u(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    constexpr int kDlgW = 460, kDlgH = 360;
    const int W = u(kDlgW), H = u(kDlgH);
    RECT rc{0, 0, W, H};
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr{}; ::GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - (rc.bottom - rc.top)) / 2;

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass,
                                 Tr(L"themeedit.title"), gstyle, x, y,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 parent, nullptr, hInst, s);
    if (!dlg) { delete s; ::DeleteObject(font); return L""; }

    const std::wstring title =
        std::wstring(Tr(L"themeedit.theme")) + L" " + target;
    MkLabel(dlg, hInst, IDC_TED_NAME, title.c_str(), 12, 12, kDlgW - 24, font);
    HWND list = MkList(dlg, hInst, IDC_TED_LIST, 12, 36, 200, 250, font);
    for (int i = 0; i < theme::ThemeFieldCount(); ++i) {
        const wchar_t* lbl =
            Tr((std::wstring(L"theme.f.") + theme::ThemeFieldKey(i)).c_str());
        ::SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)lbl);
    }
    ::SendMessageW(list, LB_SETCURSEL, 0, 0);
    MkButton(dlg, hInst, IDC_TED_SWATCH, L"", 232, 36, 200, 56,
             WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, font);
    MkButton(dlg, hInst, IDC_TED_PICK, Tr(L"themeedit.pick"), 232, 104, 200, 26,
             WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    MkLabel(dlg, hInst, IDC_TED_HINT, Tr(L"themeedit.hint"),
            12, kDlgH - 66, kDlgW - 24, font);
    MkButton(dlg, hInst, IDC_TED_CANCEL, Tr(L"input.cancel"),
             kDlgW - 105, kDlgH - 42, 95, 28,
             WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    MkButton(dlg, hInst, IDC_TED_OK, Tr(L"style.saveclose"),
             kDlgW - 215, kDlgH - 42, 100, 28,
             WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, font);

    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);
    MSG m;
    while (::IsWindow(dlg) && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &m)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    ::EnableWindow(parent, TRUE);
    ::SetForegroundWindow(parent);
    ::DeleteObject(font);
    return target;
}

LRESULT CALLBACK StyleProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    State* st = State::Get(h);
    switch (msg) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(h, GWLP_USERDATA,
                                (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;

        case WM_COMMAND: {
            if (!st) return 0;
            const unsigned id = LOWORD(wp);
            switch (id) {
                case IDC_OK:
                    GlobalStyler().Save(GlobalStyler().FilePath());
                    ::DestroyWindow(h);
                    return 0;
                case IDC_CANCEL:
                case IDCANCEL:
                    GlobalStyler().RestoreFrom(st->snapshot);
                    if (st->applier) st->applier->RestyleAll();
                    if (st->applier) st->applier->SwitchThemeByName(st->origTheme.c_str());
                    ::DestroyWindow(h);
                    return 0;
                case IDC_FG_BTN: PickColor(*st, true, false);  return 0;
                case IDC_BG_BTN: PickColor(*st, false, false); return 0;
                case IDC_G_FG_BTN: PickColor(*st, true, true);  return 0;
                case IDC_G_BG_BTN: PickColor(*st, false, true); return 0;
                case IDC_G_RESET: {
                    // Global 恢复默认：清空全部全局覆盖，回到纯主题色
                    GlobalStyler().ResetGlobal();
                    RefreshStyleUi(*st);
                    if (st->applier) st->applier->RestyleAll();
                    return 0;
                }
                case IDC_LANGFONT_BTN: PickFont(h, *st); return 0;
                case IDC_THEMEDLG_BTN:
                    // 阶段 4：主题基色编辑。返回新主题名时同步主题下拉与
                    // origTheme（X/取消回到用户进配置器前的主题语义不变）
                    if (std::wstring edited = RunThemeColorEditor(h, st->inst, st->applier);
                        !edited.empty()) {
                        st->origTheme = edited;
                        if (HWND c = st->CtrlOf(IDC_THEME)) {
                            for (int i = 0; i < theme::Count(); ++i) {
                                if (_wcsicmp(theme::At(i)->name, edited.c_str()) == 0) {
                                    ::SendMessageW(c, CB_SETCURSEL, i, 0);
                                    break;
                                }
                            }
                        }
                    }
                    return 0;
                case IDC_RESET: {
                    // 恢复默认：清除当前 (lexer, role) 的覆盖与拾取标记，
                    // 回到主题角色色（docs/settings-plan.md 阶段 2b 补项）。
                    if (st->lexer.empty()) return 0;
                    GlobalStyler().ClearOverrideW(st->lexer.c_str(), st->role);
                    st->fgPicked = false;
                    st->bgPicked = false;
                    RefreshStyleUi(*st);
                    if (st->applier) st->applier->RestyleAll();
                    return 0;
                }
                case IDC_THEME: {
                    if (HIWORD(wp) == CBN_SELCHANGE && st->applier) {
                        HWND c = st->CtrlOf(IDC_THEME);
                        wchar_t name[128]{};
                        if (c) {
                            int sel = (int)::SendMessageW(c, CB_GETCURSEL, 0, 0);
                            ::SendMessageW(c, CB_GETLBTEXT, sel, (LPARAM)name);
                        }
                        st->applier->SwitchThemeByName(name);
                    }
                    return 0;
                }
                case IDC_LANG: {
                    if (HIWORD(wp) == LBN_SELCHANGE) {
                        HWND c = st->CtrlOf(IDC_LANG);
                        HWND s2 = st->CtrlOf(IDC_STYLE);
                        ApplyLanguageSelection(*st, c, s2);
                    }
                    return 0;
                }
                case IDC_STYLE: {
                    if (HIWORD(wp) == LBN_SELCHANGE) {
                        HWND c = st->CtrlOf(IDC_STYLE);
                        st->role = (int)::SendMessageW(c, LB_GETCURSEL, 0, 0);
                        if (st->role < 0) st->role = SR_Comment;
                        RefreshStyleUi(*st);
                    }
                    return 0;
                }
                default:
                    if (id == IDC_FG_ON || id == IDC_BG_ON || id == IDC_BOLD ||
                        id == IDC_ITALIC || id == IDC_UNDER ||
                        id == IDC_G_FG_ON || id == IDC_G_BG_ON) {
                        if (!st->loading) ApplyLive(*st);
                        return 0;
                    }
                    break;
            }
            break;
        }

        case WM_CLOSE:
            // X = 取消语义（同 PreferencesDialog 的教训：必须显式处理）
            GlobalStyler().RestoreFrom(st->snapshot);
            if (st->applier) {
                st->applier->RestyleAll();
                st->applier->SwitchThemeByName(st->origTheme.c_str());
            }
            ::DestroyWindow(h);
            return 0;

        case WM_DESTROY:
            delete st;
            ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void StyleConfiguratorDialog::Run(HWND parent, HINSTANCE hInst, IStyleApplier* applier) {
    static const wchar_t kClass[] = L"xfsWinPadStyleConfig";
    static bool regDone = false;
    if (!regDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StyleProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        regDone = true;
    }

    State* st = new State();
    st->inst = hInst;
    st->applier = applier;
    st->snapshot = GlobalStyler().Snapshot();
    st->origTheme = (applier && applier->CurrentThemeName())
                        ? applier->CurrentThemeName() : L"light";

    const int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return ::MulDiv(px, dpi, 96); };
    HFONT font = ::CreateFontW(-u(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    // 逻辑尺寸常量 = 控件布局唯一坐标基准（Mk* 工厂内部统一缩放）；
    // W/H 物理值只用于窗口外框。与首选项按钮的二次缩放教训同源。
    constexpr int kDlgW = 640, kDlgH = 520;
    const int W = u(kDlgW), H = u(kDlgH);
    RECT rc{0, 0, W, H};
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr{}; ::GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    const int y = pr.top + u(30);

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, Tr(L"style.title"),
                                 gstyle, x, y, rc.right - rc.left,
                                 rc.bottom - rc.top, parent, nullptr, hInst, st);
    if (!dlg) { delete st; ::DeleteObject(font); return; }

    // 顶部：主题下拉
    MkLabel(dlg, hInst, IDC_THEME_L, Tr(L"style.theme"), 12, 10, 60, font);
    HWND theme = MkCombo(dlg, hInst, IDC_THEME, 80, 7, 220, font);
    int curSel = 0;
    const wchar_t* curName = (applier && applier->CurrentThemeName())
                                 ? applier->CurrentThemeName() : L"light";
    for (int i = 0; i < theme::Count(); ++i) {
        const ThemeDef* t = theme::At(i);
        ::SendMessageW(theme, CB_ADDSTRING, 0, (LPARAM)t->name);
        if (_wcsicmp(t->name, curName) == 0) curSel = i;
    }
    ::SendMessageW(theme, CB_SETCURSEL, curSel, 0);
    st->Track(theme, IDC_THEME);

    // 左：语言列表；右：样式列表
    MkLabel(dlg, hInst, IDC_LANG_L, Tr(L"style.lang"), 12, 40, 80, font);
    MkLabel(dlg, hInst, IDC_STYLE_L, Tr(L"style.role"), kDlgW / 2, 40, 80, font);
    HWND lang = MkList(dlg, hInst, IDC_LANG, 12, 62, kDlgW / 2 - 24, 260, font);
    HWND style = MkList(dlg, hInst, IDC_STYLE, kDlgW / 2 + 8, 62,
                        kDlgW / 2 - 20, 260, font);
    st->Track(lang, IDC_LANG);
    st->Track(style, IDC_STYLE);
    LoadLanguageList(*st, lang);
    LoadStyleList(*st, style);

    // 样式编辑区（底部，全部逻辑坐标）。三组控件布局重叠：
    // styleCtrls=per-style 仅语言页、globalCtrls=全局覆盖仅 Global 页、
    // langCtrls=整行字体+用户关键字仅语言页；BOLD/ITALIC/UNDER 两页共用。
    st->Track(MkCheck(dlg, hInst, IDC_FG_ON, Tr(L"style.fgcustom"), 12, 366, 140, font),
              IDC_FG_ON);
    st->hFgBtn = MkButton(dlg, hInst, IDC_FG_BTN, L"", 160, 362, 120, 26,
                          WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    st->Track(st->hFgBtn, IDC_FG_BTN);
    st->Track(MkCheck(dlg, hInst, IDC_BG_ON, Tr(L"style.bgcustom"), 12, 398, 140, font),
              IDC_BG_ON);
    st->hBgBtn = MkButton(dlg, hInst, IDC_BG_BTN, L"", 160, 394, 120, 26,
                          WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    st->Track(st->hBgBtn, IDC_BG_BTN);
    st->Track(MkCheck(dlg, hInst, IDC_BOLD, Tr(L"style.bold"), 300, 366, 80, font), IDC_BOLD);
    st->Track(MkCheck(dlg, hInst, IDC_ITALIC, Tr(L"style.italic"), 390, 366, 80, font), IDC_ITALIC);
    st->Track(MkCheck(dlg, hInst, IDC_UNDER, Tr(L"style.underline"), 478, 366, 110, font), IDC_UNDER);
    st->hResetDef = MkButton(dlg, hInst, IDC_RESET, Tr(L"style.reset"), 300, 398, 110, 26,
                             WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    st->Track(st->hResetDef, IDC_RESET);
    for (unsigned id : { IDC_FG_ON, IDC_FG_BTN, IDC_BG_ON, IDC_BG_BTN, IDC_RESET })
        st->styleCtrls.push_back(st->CtrlOf(id));
    st->Track(MkLabel(dlg, hInst, IDC_HINT, L"", 12, 340, kDlgW - 24, font),
              IDC_HINT);

    // 阶段 4：Global 页专用控件（勾选=启用该侧覆盖 + 取色 + 全局字体）
    st->Track(MkCheck(dlg, hInst, IDC_G_FG_ON, Tr(L"style.gfg"), 12, 366, 140, font),
              IDC_G_FG_ON);
    st->hGfgBtn = MkButton(dlg, hInst, IDC_G_FG_BTN, L"", 160, 362, 120, 26,
                           WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    st->Track(st->hGfgBtn, IDC_G_FG_BTN);
    st->Track(MkCheck(dlg, hInst, IDC_G_BG_ON, Tr(L"style.gbg"), 12, 398, 140, font),
              IDC_G_BG_ON);
    st->hGbgBtn = MkButton(dlg, hInst, IDC_G_BG_BTN, L"", 160, 394, 120, 26,
                           WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    st->Track(st->hGbgBtn, IDC_G_BG_BTN);
    st->Track(MkButton(dlg, hInst, IDC_G_RESET, Tr(L"style.greset"), 300, 394, 130, 26,
                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font),
              IDC_G_RESET);
    for (unsigned id : { IDC_G_FG_ON, IDC_G_FG_BTN, IDC_G_BG_ON, IDC_G_BG_BTN,
                         IDC_G_RESET })
        st->globalCtrls.push_back(st->CtrlOf(id));

    // 阶段 4：语言页专用控件（整行字体）
    st->Track(MkButton(dlg, hInst, IDC_LANGFONT_BTN, Tr(L"style.langfont"), 12, 430, 150, 26,
                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font),
              IDC_LANGFONT_BTN);
    for (unsigned id : { IDC_LANGFONT_BTN })
        st->langCtrls.push_back(st->CtrlOf(id));

    // 阶段 4：主题基色编辑（常显，不属于任何页）
    st->Track(MkButton(dlg, hInst, IDC_THEMEDLG_BTN, Tr(L"style.themecolors"),
                       170, 430, 150, 26,
                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font),
              IDC_THEMEDLG_BTN);

    // 底部按钮（逻辑坐标）
    st->hCancel = MkButton(dlg, hInst, IDC_CANCEL, Tr(L"input.cancel"), kDlgW - 105, kDlgH - 42,
                           95, 28, WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    st->hOk = MkButton(dlg, hInst, IDC_OK, Tr(L"style.saveclose"), kDlgW - 215, kDlgH - 42,
                       100, 28, WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, font);

    LoadLanguageList(*st, lang);
    LoadStyleList(*st, style);
    ApplyLanguageSelection(*st, lang, style);
    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);

    MSG m;
    while (::IsWindow(dlg) && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &m)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    ::EnableWindow(parent, TRUE);
    ::SetForegroundWindow(parent);
    ::DeleteObject(font);
}

} // namespace xfs
