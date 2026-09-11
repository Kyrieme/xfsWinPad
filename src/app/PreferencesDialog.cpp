// PreferencesDialog.cpp — 首选项对话框实现（阶段 1）。
// 结构复用 PluginAdminDialog 的 State + GWLP_USERDATA + 自建控件 + 模态循环。
// 分页控件一次性创建、按页显隐；任何改动即时 ApplyAll（live 预览）。
#include "PreferencesDialog.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../core/I18n.h"
#include <commctrl.h>
#include <commdlg.h>
#include <cstdlib>
#include <utility>
#include <vector>

namespace xfs {

namespace {

// ---- 控件 id ----------------------------------------------------------------
constexpr unsigned IDC_TAB       = 3201;
constexpr unsigned IDC_OK        = 3202;
constexpr unsigned IDC_CANCEL    = 3203;
// 常规
constexpr unsigned IDC_THEME_L   = 3210; constexpr unsigned IDC_THEME = 3211;
constexpr unsigned IDC_FONT_L    = 3212; constexpr unsigned IDC_FONT  = 3213;
constexpr unsigned IDC_TABW_L    = 3214; constexpr unsigned IDC_TABW  = 3215;
constexpr unsigned IDC_WRAP      = 3216;
constexpr unsigned IDC_AUTOCLOSE = 3217;
// 编辑
constexpr unsigned IDC_LINENUM   = 3220;
constexpr unsigned IDC_CURLINE   = 3221;
constexpr unsigned IDC_CARET_L   = 3222; constexpr unsigned IDC_CARET = 3223;
constexpr unsigned IDC_AUTOIND   = 3224;
constexpr unsigned IDC_AUTOCOMP  = 3225;
// 新建文档
constexpr unsigned IDC_EOL_L     = 3230; constexpr unsigned IDC_EOL = 3231;
// 备份
constexpr unsigned IDC_AUTOSAVE  = 3240;
constexpr unsigned IDC_ASEC_L    = 3241; constexpr unsigned IDC_ASEC = 3242;
// 终端
constexpr unsigned IDC_TFONT_L   = 3250; constexpr unsigned IDC_TFONT = 3251;
// 界面
constexpr unsigned IDC_SB        = 3260;
constexpr unsigned IDC_TBBAR     = 3261;
constexpr unsigned IDC_TOOLBAR   = 3262;
// 最近文件
constexpr unsigned IDC_RECENT_L  = 3263; constexpr unsigned IDC_RECENT = 3264;
// 高亮
constexpr unsigned IDC_BRACE     = 3265;
constexpr unsigned IDC_GUIDES    = 3266;
constexpr unsigned IDC_WS        = 3267;
// 搜索
constexpr unsigned IDC_SMC       = 3268;
constexpr unsigned IDC_SWW       = 3269;
// 杂项
constexpr unsigned IDC_FULLPATH  = 3270;
constexpr unsigned IDC_AUTODET   = 3271;
// 界面语言（挂界面页）
constexpr unsigned IDC_LANG_L    = 3272;
constexpr unsigned IDC_LANG      = 3273;
// AI 页
constexpr unsigned IDC_AICTX     = 3280;
constexpr unsigned IDC_AIAUTO    = 3281;
constexpr unsigned IDC_AIBACKEND = 3282;
constexpr unsigned IDC_AIENDPOINT = 3283;
constexpr unsigned IDC_AIAPIKEY  = 3284;
constexpr unsigned IDC_AIMODEL   = 3285;
constexpr unsigned IDC_AIBACKEND_L = 3286;
constexpr unsigned IDC_AIENDPOINT_L = 3287;
constexpr unsigned IDC_AIAPIKEY_L = 3288;
constexpr unsigned IDC_AIMODEL_L = 3289;
constexpr unsigned IDC_AIAIMODEL_MAX = IDC_AIMODEL_L;

enum Page : int { kGeneral = 0, kEditing, kNewDoc, kBackup, kTerminal,
                  kUI, kRecent, kHighlight, kSearch, kMisc, kAi, kPageCount };

// ---- 界面语言表（顺序即下拉顺序；语言名用各自母语书写，不做翻译）--------------
struct UiLangEntry { const wchar_t* code; const wchar_t* label; };
constexpr UiLangEntry kUiLangs[] = {
    {L"zh-CN", L"简体中文"},
    {L"en",    L"English"},
    {L"zh-TW", L"繁體中文"},
    {L"ja",    L"日本語"},
    {L"ko",    L"한국어"},
};
constexpr int kUiLangCount = 5;

int UiLangIndexOf(const std::wstring& code) {
    for (int i = 0; i < kUiLangCount; ++i)
        if (code == kUiLangs[i].code) return i;
    return 0;   // 未知值回落简体中文（与旧版语义一致）
}

Page PageOf(unsigned id) {
    if (id >= IDC_THEME_L && id <= IDC_AUTOCLOSE) return kGeneral;
    if (id >= IDC_LINENUM && id <= IDC_AUTOCOMP)  return kEditing;
    if (id >= IDC_EOL_L && id <= IDC_EOL)         return kNewDoc;
    if (id >= IDC_AUTOSAVE && id <= IDC_ASEC)     return kBackup;
    if (id >= IDC_TFONT_L && id <= IDC_TFONT)     return kTerminal;
    if (id >= IDC_SB && id <= IDC_TOOLBAR)        return kUI;
    if (id >= IDC_LANG_L && id <= IDC_LANG)       return kUI;
    if (id >= IDC_RECENT_L && id <= IDC_RECENT)   return kRecent;
    if (id >= IDC_BRACE && id <= IDC_WS)          return kHighlight;
    if (id >= IDC_SMC && id <= IDC_SWW)           return kSearch;
    if (id >= IDC_FULLPATH && id <= IDC_AUTODET)  return kMisc;
    if (id >= IDC_AICTX && id <= IDC_AIMODEL_L) return kAi;
    return kGeneral;
}

const wchar_t* PageTitle(int page) {
    switch (page) {
        case kGeneral:   return Tr(L"prefs.pg.general");
        case kEditing:   return Tr(L"prefs.pg.editing");
        case kNewDoc:    return Tr(L"prefs.pg.newdoc");
        case kBackup:    return Tr(L"prefs.pg.backup");
        case kUI:        return Tr(L"prefs.pg.ui");
        case kRecent:    return Tr(L"prefs.pg.recent");
        case kHighlight: return Tr(L"prefs.pg.highlight");
        case kSearch:    return Tr(L"prefs.pg.search");
        case kMisc:      return Tr(L"prefs.pg.misc");
        case kAi:        return Tr(L"prefs.pg.ai");
        default:         return Tr(L"prefs.pg.terminal");
    }
}

struct State {
    HINSTANCE inst = nullptr;
    HWND hTab = nullptr, hOk = nullptr, hCancel = nullptr;
    AppSettings* current = nullptr;     // 宿主对象（确定时写回）
    IPrefsApplier* applier = nullptr;   // 实时应用通道（非拥有）
    AppSettings working;                // 对话框内编辑副本
    AppSettings original;               // 取消还原快照
    int page = kGeneral;
    bool loading = false;               // LoadFromSettings 编程改控件期间禁止 ApplyLive
    std::vector<HWND> ctrls;            // 与 ids 平行
    std::vector<unsigned> ids;

    HWND CtrlOf(unsigned id) const {
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == id) return ctrls[i];
        return nullptr;
    }
    void Track(HWND c, unsigned id) {
        if (c) { ctrls.push_back(c); ids.push_back(id); }
    }

    static State* Get(HWND h) {
        return (State*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    }
};

int U(HWND h, int px) {
    const int d = ::GetDpiForWindow(h);
    return ::MulDiv(px, d, 96);
}

// ---- 控件工厂 ----------------------------------------------------------------

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
    HWND c = ::CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | extra, U(dlg, x), U(dlg, y),
                               U(dlg, w), U(dlg, h),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

HWND MkCheck(HWND dlg, HINSTANCE inst, unsigned id, const wchar_t* text,
             int x, int y, int w, HFONT font) {
    return MkButton(dlg, inst, id, text, x, y, w, 20,
                    WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, font);
}

HWND MkEdit(HWND dlg, HINSTANCE inst, unsigned id, int x, int y, int w,
            HFONT font) {
    HWND c = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT,
                               U(dlg, x), U(dlg, y), U(dlg, w), U(dlg, 22),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

// 密码输入（API Key 用；ES_PASSWORD 显示 ●）
HWND MkEditPass(HWND dlg, HINSTANCE inst, unsigned id, int x, int y, int w,
                HFONT font) {
    HWND c = MkEdit(dlg, inst, id, x, y, w, font);
    ::SendMessageW(c, EM_SETPASSWORDCHAR, (WPARAM)L'●', 0);
    return c;
}

HWND MkCombo(HWND dlg, HINSTANCE inst, unsigned id, int x, int y, int w,
             HFONT font) {
    HWND c = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               CBS_DROPDOWNLIST,
                               U(dlg, x), U(dlg, y), U(dlg, w), U(dlg, 120),
                               dlg, (HMENU)(UINT_PTR)id, inst, nullptr);
    ::SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

// ---- 控件 ↔ working 双向同步 ---------------------------------------------------
// ⚠️ LoadFromSettings 用 SetWindowTextW 填编辑框会触发 EN_CHANGE；不设
// loading 屏蔽的话，每个 EN_CHANGE 都会把"半加载"的控件状态当成用户输入
// 灌进真实设置（曾把 showLineNumber 等毒化为 false 并随 OK 落盘）。
void LoadFromSettings(State& st) {
    st.loading = true;
    const AppSettings& s = st.working;
    auto setText = [&](unsigned id, const std::wstring& t) {
        if (HWND c = st.CtrlOf(id)) ::SetWindowTextW(c, t.c_str());
    };
    auto setCheck = [&](unsigned id, bool on) {
        if (HWND c = st.CtrlOf(id))
            ::SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    if (HWND c = st.CtrlOf(IDC_THEME))
        ::SendMessageW(c, CB_SETCURSEL, s.theme == L"dark" ? 1 : 0, 0);
    setText(IDC_FONT, I18n::Instance().Fmt(L"prefs.fontfmt",
              {s.fontName, std::to_wstring(s.fontSize)}));
    setText(IDC_TABW, std::to_wstring(s.tabWidth));
    setCheck(IDC_WRAP, s.wrapOn);
    setCheck(IDC_AUTOCLOSE, s.autoCloseBrackets);
    setCheck(IDC_LINENUM, s.showLineNumber);
    setCheck(IDC_CURLINE, s.currentLineHighlight);
    if (HWND c = st.CtrlOf(IDC_CARET))
        ::SendMessageW(c, CB_SETCURSEL,
                       s.caretWidth >= 1 && s.caretWidth <= 3 ? s.caretWidth - 1 : 1,
                       0);
    setCheck(IDC_AUTOIND, s.autoIndent);
    setCheck(IDC_AUTOCOMP, s.autoComplete);
    if (HWND c = st.CtrlOf(IDC_EOL))
        ::SendMessageW(c, CB_SETCURSEL,
                       s.defaultEol >= 0 && s.defaultEol <= 2 ? s.defaultEol : 0, 0);
    setCheck(IDC_AUTOSAVE, s.autosaveEnabled);
    setText(IDC_ASEC, std::to_wstring(s.autosaveSeconds));
    setText(IDC_TFONT, I18n::Instance().Fmt(L"prefs.fontfmt",
              {s.termFontName, std::to_wstring(s.termFontSize)}));
    setCheck(IDC_SB, s.showStatusBar);
    setCheck(IDC_TBBAR, s.showTabBar);
    setCheck(IDC_TOOLBAR, s.showToolbar);
    if (HWND c = st.CtrlOf(IDC_LANG))
        ::SendMessageW(c, CB_SETCURSEL, UiLangIndexOf(s.uiLang), 0);
    setText(IDC_RECENT, std::to_wstring(s.recentFilesMax));
    setCheck(IDC_BRACE, s.braceMatch);
    setCheck(IDC_GUIDES, s.indentGuides);
    setCheck(IDC_WS, s.showWhitespace);
    setCheck(IDC_SMC, s.searchMatchCase);
    setCheck(IDC_SWW, s.searchWholeWord);
    setCheck(IDC_FULLPATH, s.fullPathTitle);
    setCheck(IDC_AUTODET, s.autoDetectLang);
    setCheck(IDC_AICTX, s.aiAttachContext);
    setCheck(IDC_AIAUTO, s.aiAutoApprove);
    if (HWND c = st.CtrlOf(IDC_AIBACKEND))
        ::SendMessageW(c, CB_SETCURSEL,
                       s.aiBackend == L"openai" ? 1 : 0, 0);  // 0=opencode 1=openai
    setText(IDC_AIENDPOINT, s.aiEndpoint);
    setText(IDC_AIAPIKEY, s.aiApiKey);
    setText(IDC_AIMODEL, s.aiLocalModel);
    st.loading = false;   // 最后一个 SetWindowTextW 的 EN_CHANGE 是同步发送的，
                          // 走到这里说明风暴已结束（SendMessage 队列语义）
}

// 读取全部控件 → working。返回 false 表示有字段非法（不阻塞预览，调用方钳制）。
void SyncToSettings(State& st) {
    AppSettings& s = st.working;
    auto text = [&](unsigned id) -> std::wstring {
        HWND c = st.CtrlOf(id);
        wchar_t buf[128]{};
        if (c) ::GetWindowTextW(c, buf, 128);
        return buf;
    };
    auto check = [&](unsigned id) -> bool {
        HWND c = st.CtrlOf(id);
        return c && ::SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };
    s.wrapOn = check(IDC_WRAP);
    s.autoCloseBrackets = check(IDC_AUTOCLOSE);
    s.showLineNumber = check(IDC_LINENUM);
    s.currentLineHighlight = check(IDC_CURLINE);
    s.autoIndent = check(IDC_AUTOIND);
    s.autoComplete = check(IDC_AUTOCOMP);
    s.autosaveEnabled = check(IDC_AUTOSAVE);
    s.tabWidth = _wtoi(text(IDC_TABW).c_str());
    if (s.tabWidth < 1 || s.tabWidth > 64) s.tabWidth = 4;
    s.caretWidth = (int)::SendMessageW(st.CtrlOf(IDC_CARET), CB_GETCURSEL, 0, 0) + 1;
    s.defaultEol = (int)::SendMessageW(st.CtrlOf(IDC_EOL), CB_GETCURSEL, 0, 0);
    s.autosaveSeconds = _wtoi(text(IDC_ASEC).c_str());
    if (s.autosaveSeconds < 5 || s.autosaveSeconds > 600) s.autosaveSeconds = 30;
    if (HWND c = st.CtrlOf(IDC_THEME)) {
        int sel = (int)::SendMessageW(c, CB_GETCURSEL, 0, 0);
        s.theme = (sel == 1) ? L"dark" : L"light";
    }
    s.showStatusBar = check(IDC_SB);
    s.showTabBar = check(IDC_TBBAR);
    s.showToolbar = check(IDC_TOOLBAR);
    if (HWND c = st.CtrlOf(IDC_LANG)) {
        int sel = (int)::SendMessageW(c, CB_GETCURSEL, 0, 0);
        // CB_ERR(-1)=尚未选中 → 简体中文（与旧版语义一致）
        s.uiLang = (sel >= 0 && sel < kUiLangCount)
                       ? kUiLangs[sel].code : L"zh-CN";
    }
    s.recentFilesMax = _wtoi(text(IDC_RECENT).c_str());
    if (s.recentFilesMax < 1 || s.recentFilesMax > 30) s.recentFilesMax = 10;
    s.braceMatch = check(IDC_BRACE);
    s.indentGuides = check(IDC_GUIDES);
    s.showWhitespace = check(IDC_WS);
    s.searchMatchCase = check(IDC_SMC);
    s.searchWholeWord = check(IDC_SWW);
    s.fullPathTitle = check(IDC_FULLPATH);
    s.autoDetectLang = check(IDC_AUTODET);
    s.aiAttachContext = check(IDC_AICTX);
    s.aiAutoApprove = check(IDC_AIAUTO);
    if (HWND c = st.CtrlOf(IDC_AIBACKEND)) {
        int sel = (int)::SendMessageW(c, CB_GETCURSEL, 0, 0);
        s.aiBackend = (sel == 1) ? L"openai" : L"opencode";
    }
    s.aiEndpoint = text(IDC_AIENDPOINT);
    s.aiApiKey = text(IDC_AIAPIKEY);
    s.aiLocalModel = text(IDC_AIMODEL);
}

void ApplyLive(State& st) {
    SyncToSettings(st);
    if (st.applier) st.applier->ApplyAll(st.working);
}

// ---- ChooseFont 封装 ----------------------------------------------------------

void PickFont(HWND dlg, State& st, bool terminal) {
    LOGFONTW lf{};
    const std::wstring& name = terminal ? st.working.termFontName
                                        : st.working.fontName;
    const int& size = terminal ? st.working.termFontSize : st.working.fontSize;
    wcscpy_s(lf.lfFaceName, name.c_str());
    HDC dc = ::GetDC(dlg);
    lf.lfHeight = -::MulDiv(size, ::GetDeviceCaps(dc, LOGPIXELSY), 72);

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
    if (pt < 5 || pt > 72) return;   // ChooseFont 已约束；防御极端 DPI

    if (terminal) {
        st.working.termFontName = lf.lfFaceName;
        st.working.termFontSize = pt;
    } else {
        st.working.fontName = lf.lfFaceName;
        st.working.fontSize = pt;
    }
    LoadFromSettings(st);          // 刷新按钮显示文本
    ApplyLive(st);
}

// ---- 页切换 -------------------------------------------------------------------

void SwitchPage(State& st, int page) {
    st.page = page;
    for (size_t i = 0; i < st.ctrls.size(); ++i) {
        const bool show = PageOf(st.ids[i]) == (Page)page;
        ::ShowWindow(st.ctrls[i], show ? SW_SHOW : SW_HIDE);
    }
}

// ---- 窗口过程 -----------------------------------------------------------------

LRESULT CALLBACK PrefsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
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
                    ApplyLive(*st);
                    if (st->current) *st->current = st->working;
                    ::DestroyWindow(h);          // 收尾统一走循环出口
                    return 0;
                case IDC_CANCEL:
                case IDCANCEL:
                    if (st->applier) st->applier->ApplyAll(st->original);
                    ::DestroyWindow(h);
                    return 0;
                case IDC_FONT:  PickFont(h, *st, false); return 0;
                case IDC_TFONT: PickFont(h, *st, true);  return 0;
                default:
                    if (PageOf(id) != kPageCount) {
                        switch (HIWORD(wp)) {
                            case BN_CLICKED:
                            case CBN_SELCHANGE:
                            case EN_CHANGE:
                                // 加载期间的编程改控件事件不是用户输入
                                if (st->loading) return 0;
                                ApplyLive(*st);
                                return 0;
                        }
                    }
                    break;
            }
            break;
        }

        case WM_NOTIFY: {
            const NMHDR* hdr = (const NMHDR*)lp;
            if (st && hdr->code == TCN_SELCHANGE && hdr->idFrom == IDC_TAB) {
                SwitchPage(*st, (int)::SendMessageW(st->hTab, TCM_GETCURSEL, 0, 0));
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            // X 按钮 = 取消语义（对齐 N++ 首选项）。⚠️ 必须显式处理：
            // 走 DefWindowProc 会直接 DestroyWindow，宿主永远停留在
            // EnableWindow(FALSE) 的禁用态（2026-08-28 卡死 bug 的根因）。
            if (st && st->applier) st->applier->ApplyAll(st->original);
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

void PreferencesDialog::Run(HWND parent, HINSTANCE hInst, AppSettings* current,
                            IPrefsApplier* applier) {
    static const wchar_t kClass[] = L"xfsWinPadPreferences";
    static bool regDone = false;
    if (!regDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PrefsProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        ::RegisterClassExW(&wc);
        regDone = true;
    }

    State* st = new State();
    st->inst = hInst;
    st->current = current;
    st->applier = applier;
    if (current) {
        st->working = *current;
        st->original = *current;
    }

    const int dpi = ::GetDpiForWindow(parent);
    auto u = [dpi](int px) { return ::MulDiv(px, dpi, 96); };
    HFONT font = ::CreateFontW(-u(9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    // 逻辑尺寸常量：所有 Mk* 控件工厂内部会再做 DPI 缩放，调用方必须传
    // 逻辑坐标。W/H（物理）仅用于窗口外框尺寸计算——曾经把物理值再传给
    // MkButton 二次缩放，按钮被定位到客户区之外（125% DPI 下"确定/取消"
    // 完全不可见，用户只能 X 关闭=取消，首选项改动永远不生效）。
    constexpr int kDlgW = 640, kDlgH = 420;      // logical
    const int W = u(kDlgW), H = u(kDlgH);        // physical (window frame only)
    RECT rc{0, 0, W, H};
    const DWORD gstyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    ::AdjustWindowRectEx(&rc, gstyle, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr{}; ::GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - (rc.right - rc.left)) / 2;
    const int y = pr.top + u(30);

    HWND dlg = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, Tr(L"prefs.title"),
                                 gstyle, x, y, rc.right - rc.left,
                                 rc.bottom - rc.top, parent, nullptr, hInst, st);
    if (!dlg) { delete st; ::DeleteObject(font); return; }

    const int w = W - u(16);

    // 标签页
    st->hTab = ::CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH,
        u(8), u(8), w, u(26), dlg, (HMENU)(UINT_PTR)IDC_TAB, hInst, nullptr);
    for (int i = 0; i < kPageCount; ++i) {
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(PageTitle(i));
        ::SendMessageW(st->hTab, TCM_INSERTITEMW, i, (LPARAM)&ti);
    }
    ::SendMessageW(st->hTab, TCM_SETITEMSIZE, 0,
                   MAKELPARAM((w - u(8)) / kPageCount, u(26)));
    ::SendMessageW(st->hTab, WM_SETFONT, (WPARAM)font, TRUE);

    // ---- 常规页 --------------------------------------------------------------
    // ⚠️ 每个控件（含标签）都必须 Track：SwitchPage 按 id→页 显隐全部控件，
    // 漏 Track 的控件会永远可见并与其它页叠加。
    st->Track(MkLabel(dlg, hInst, IDC_THEME_L, Tr(L"prefs.theme"), 12, 46, 70, font),
              IDC_THEME_L);
    HWND theme = MkCombo(dlg, hInst, IDC_THEME, 90, 43, 160, font);
    ::SendMessageW(theme, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.theme.light"));
    ::SendMessageW(theme, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.theme.dark"));
    st->Track(theme, IDC_THEME);
    st->Track(MkLabel(dlg, hInst, IDC_FONT_L, Tr(L"prefs.font"), 12, 76, 80, font),
              IDC_FONT_L);
    st->Track(MkButton(dlg, hInst, IDC_FONT, L"", 100, 72, 220, 26,
                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font), IDC_FONT);
    st->Track(MkLabel(dlg, hInst, IDC_TABW_L, Tr(L"prefs.tabwidth"), 12, 106, 80, font),
              IDC_TABW_L);
    st->Track(MkEdit(dlg, hInst, IDC_TABW, 100, 103, 60, font), IDC_TABW);
    st->Track(MkCheck(dlg, hInst, IDC_WRAP, Tr(L"prefs.wrap"), 12, 134, 200, font), IDC_WRAP);
    st->Track(MkCheck(dlg, hInst, IDC_AUTOCLOSE, Tr(L"prefs.autoclose"), 12, 158, 240, font),
              IDC_AUTOCLOSE);

    // ---- 编辑页 --------------------------------------------------------------
    st->Track(MkCheck(dlg, hInst, IDC_LINENUM, Tr(L"prefs.linenumber"), 12, 46, 240, font),
              IDC_LINENUM);
    st->Track(MkCheck(dlg, hInst, IDC_CURLINE, Tr(L"prefs.currentline"), 12, 70, 240, font),
              IDC_CURLINE);
    st->Track(MkLabel(dlg, hInst, IDC_CARET_L, Tr(L"prefs.caretwidth"), 12, 98, 80, font),
              IDC_CARET_L);
    HWND caret = MkCombo(dlg, hInst, IDC_CARET, 100, 95, 120, font);
    ::SendMessageW(caret, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.caret1"));
    ::SendMessageW(caret, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.caret2"));
    ::SendMessageW(caret, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.caret3"));
    st->Track(caret, IDC_CARET);
    st->Track(MkCheck(dlg, hInst, IDC_AUTOIND, Tr(L"prefs.autoindent"), 12, 126, 240, font),
              IDC_AUTOIND);
    st->Track(MkCheck(dlg, hInst, IDC_AUTOCOMP, Tr(L"prefs.autocomplete"), 12, 150, 240, font),
              IDC_AUTOCOMP);

    // ---- 新建文档页 ------------------------------------------------------------
    st->Track(MkLabel(dlg, hInst, IDC_EOL_L, Tr(L"prefs.eol"), 12, 46, 90, font),
              IDC_EOL_L);
    HWND eol = MkCombo(dlg, hInst, IDC_EOL, 110, 43, 180, font);
    ::SendMessageW(eol, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.eol.crlf"));
    ::SendMessageW(eol, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.eol.lf"));
    ::SendMessageW(eol, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.eol.cr"));
    st->Track(eol, IDC_EOL);

    // ---- 备份页 ----------------------------------------------------------------
    st->Track(MkCheck(dlg, hInst, IDC_AUTOSAVE, Tr(L"prefs.autosave"),
                      12, 46, 300, font), IDC_AUTOSAVE);
    st->Track(MkLabel(dlg, hInst, IDC_ASEC_L, Tr(L"prefs.asec"), 12, 74, 130, font),
              IDC_ASEC_L);
    st->Track(MkEdit(dlg, hInst, IDC_ASEC, 150, 71, 60, font), IDC_ASEC);

    // ---- 终端页 ----------------------------------------------------------------
    st->Track(MkLabel(dlg, hInst, IDC_TFONT_L, Tr(L"prefs.tfont"), 12, 46, 80, font),
              IDC_TFONT_L);
    st->Track(MkButton(dlg, hInst, IDC_TFONT, L"", 100, 42, 220, 26,
                       WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font), IDC_TFONT);

    // ---- 界面页 ----------------------------------------------------------------
    st->Track(MkCheck(dlg, hInst, IDC_SB, Tr(L"prefs.showstatusbar"), 12, 46, 240, font),
              IDC_SB);
    st->Track(MkCheck(dlg, hInst, IDC_TBBAR, Tr(L"prefs.showtabbar"), 12, 70, 240, font),
              IDC_TBBAR);
    st->Track(MkCheck(dlg, hInst, IDC_TOOLBAR, Tr(L"prefs.showtoolbar"), 12, 94, 240, font),
              IDC_TOOLBAR);
    st->Track(MkLabel(dlg, hInst, IDC_LANG_L, Tr(L"prefs.uilang"), 12, 122, 100, font),
              IDC_LANG_L);
    {
        // 选项用各自母语书写（语言名不做翻译是通行惯例）
        HWND lang = MkCombo(dlg, hInst, IDC_LANG, 120, 119, 160, font);
        for (const auto& e : kUiLangs)
            ::SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)e.label);
        st->Track(lang, IDC_LANG);
    }

    // ---- 最近文件页 --------------------------------------------------------------
    st->Track(MkLabel(dlg, hInst, IDC_RECENT_L, Tr(L"prefs.recentmax"),
                      12, 46, 160, font), IDC_RECENT_L);
    st->Track(MkEdit(dlg, hInst, IDC_RECENT, 180, 43, 60, font), IDC_RECENT);

    // ---- 高亮页 ----------------------------------------------------------------
    st->Track(MkCheck(dlg, hInst, IDC_BRACE, Tr(L"prefs.brace"), 12, 46, 240, font),
              IDC_BRACE);
    st->Track(MkCheck(dlg, hInst, IDC_GUIDES, Tr(L"prefs.guides"), 12, 70, 240, font),
              IDC_GUIDES);
    st->Track(MkCheck(dlg, hInst, IDC_WS, Tr(L"prefs.ws"),
                      12, 94, 300, font), IDC_WS);

    // ---- 搜索页 ----------------------------------------------------------------
    st->Track(MkCheck(dlg, hInst, IDC_SMC, Tr(L"prefs.matchcase"), 12, 46, 240, font),
              IDC_SMC);
    st->Track(MkCheck(dlg, hInst, IDC_SWW, Tr(L"prefs.wholeword"), 12, 70, 240, font),
              IDC_SWW);

    // ---- 杂项页 ----------------------------------------------------------------
    st->Track(MkCheck(dlg, hInst, IDC_FULLPATH, Tr(L"prefs.fullpath"), 12, 46, 260, font),
              IDC_FULLPATH);
    st->Track(MkCheck(dlg, hInst, IDC_AUTODET, Tr(L"prefs.autodetect"),
                      12, 70, 320, font), IDC_AUTODET);

    // ---- AI 页 ----------------------------------------------------------------
    st->Track(MkCheck(dlg, hInst, IDC_AICTX, Tr(L"prefs.aictx"), 12, 46, 380, font),
              IDC_AICTX);
    st->Track(MkCheck(dlg, hInst, IDC_AIAUTO, Tr(L"prefs.aiauto"), 12, 70, 400, font),
              IDC_AIAUTO);
    // 本地/第三方直连（OpenAI-compatible；Ollama /v1、DeepSeek、Qwen…）
    // ⚠️ 标签也必须 Track（漏了会永远可见、叠在其它页上——2026-09-07 用户反馈）
    st->Track(MkLabel(dlg, hInst, IDC_AIBACKEND_L, Tr(L"prefs.aibackend"),
                      12, 100, 120, font), IDC_AIBACKEND_L);
    {
        HWND be = MkCombo(dlg, hInst, IDC_AIBACKEND, 140, 97, 200, font);
        ::SendMessageW(be, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.aibackend.oc"));
        ::SendMessageW(be, CB_ADDSTRING, 0, (LPARAM)Tr(L"prefs.aibackend.oai"));
        st->Track(be, IDC_AIBACKEND);
    }
    st->Track(MkLabel(dlg, hInst, IDC_AIENDPOINT_L, Tr(L"prefs.aiendpoint"),
                      12, 128, 120, font), IDC_AIENDPOINT_L);
    st->Track(MkEdit(dlg, hInst, IDC_AIENDPOINT, 140, 125, 240, font),
              IDC_AIENDPOINT);
    st->Track(MkLabel(dlg, hInst, IDC_AIAPIKEY_L, Tr(L"prefs.aiapikey"),
                      12, 156, 120, font), IDC_AIAPIKEY_L);
    st->Track(MkEditPass(dlg, hInst, IDC_AIAPIKEY, 140, 153, 240, font),
              IDC_AIAPIKEY);
    st->Track(MkLabel(dlg, hInst, IDC_AIMODEL_L, Tr(L"prefs.aimodel"),
                      12, 184, 120, font), IDC_AIMODEL_L);
    st->Track(MkEdit(dlg, hInst, IDC_AIMODEL, 140, 181, 240, font),
              IDC_AIMODEL);

    // ---- 底部按钮（逻辑坐标；MkButton 内部统一缩放） --------------------------
    st->hCancel = MkButton(dlg, hInst, IDC_CANCEL, Tr(L"input.cancel"),
                           kDlgW - 96, kDlgH - 44, 80, 28,
                           WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, font);
    st->hOk = MkButton(dlg, hInst, IDC_OK, Tr(L"input.ok"),
                       kDlgW - 190, kDlgH - 44, 80, 28,
                       WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, font);

    LoadFromSettings(*st);
    SwitchPage(*st, kGeneral);
    ::EnableWindow(parent, FALSE);
    ::ShowWindow(dlg, SW_SHOW);

    // 模态消息循环
    MSG m;
    while (::IsWindow(dlg) && ::GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dlg, &m)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    // 兜底：无论哪条路径退出（确定/取消/X/系统关闭），宿主都必须恢复可用，
    // 并拿回前台——防止任何新关闭路径再漏掉重启用（2026-08-28 卡死教训）。
    ::EnableWindow(parent, TRUE);
    ::SetForegroundWindow(parent);
    ::DeleteObject(font);
}

} // namespace xfs
