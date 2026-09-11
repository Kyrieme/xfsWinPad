#include "AiPanel.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Util.h"
#include "../theme/Theme.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <thread>

namespace xfs {

namespace {
constexpr wchar_t kPanelClass[] = L"xfsWinPadAiPanel";
constexpr UINT_PTR kInputSubclassId = 0x41495349;   // 'AISI'
constexpr int ID_LABEL = 1500;
constexpr int ID_CLOSE = 1501;
constexpr int ID_INPUT = 1502;
constexpr int ID_SEND  = 1503;
constexpr int ID_MODEL = 1504;
constexpr int ID_HIST  = 1505;

// transcript 样式槽位
enum AiStyle {
    AS_DEFAULT = 0,     // 正文
    AS_USER_TAG,        // 「你」标签
    AS_AI_TAG,          // 「AI」标签
    AS_REASONING,       // 思考过程（灰）
    AS_SYSTEM,          // 系统/错误信息（灰斜体）
    AS_DIFF_ADD,        // diff + 行（绿）
    AS_DIFF_DEL,        // diff - 行（红）
    AS_DIFF_META,       // diff 元信息行（Index/@@/===，灰）
    AS_COUNT
};

constexpr UINT kPmsgConnectDone = WM_APP + 71;   // wp=1 成功 / 0 失败
constexpr UINT kPmsgConnectNotInstalled = WM_APP + 72;

sptr_t SciSend(HWND h, UINT m, uptr_t wp = 0, LPARAM lp = 0) {
    return ::SendMessageW(h, m, (WPARAM)wp, lp);
}

// ---- openai 模式会话历史落盘（批次 25）---------------------------------
// 文件：%APPDATA%\xfsWinPad\ai-history.json（与 settings.json 同目录）。
// 量级：≤40 条 OaiMsg，几 KB，UI 线程同步写足够快。
std::wstring OaiHistoryFilePath() {
    std::wstring base;
    wchar_t* appData = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0,
                                         nullptr, &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    ::CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    return base + L"\\xfsWinPad\\ai-history.json";
}
} // namespace

bool AiPanel::Create(HWND parent, HINSTANCE hInst) {
    if (hwnd_) return true;
    inst_ = hInst;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kPanelClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    int dpi = ::GetDpiForWindow(parent);
    font_ = ::CreateFontW(-MulDiv(9, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    mono_ = nullptr;   // Layout 时按 DPI 建

    hwnd_ = ::CreateWindowExW(0, kPanelClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 380, 500, parent,
                              (HMENU)(INT_PTR)1107, hInst, nullptr);
    if (!hwnd_) return false;
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    label_ = ::CreateWindowExW(0, L"STATIC", Tr(L"panel.ai"),
                               WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                               8, 8, 300, 18, hwnd_,
                               (HMENU)(INT_PTR)ID_LABEL, hInst, nullptr);
    closeBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.ai.close"),
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 2, 76, 22, hwnd_,
                                  (HMENU)(INT_PTR)ID_CLOSE, hInst, nullptr);
    if (label_) ::SendMessageW(label_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (closeBtn_) ::SendMessageW(closeBtn_, WM_SETFONT, (WPARAM)font_, TRUE);

    modelCombo_ = ::CreateWindowExW(0, L"COMBOBOX", nullptr,
                                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                    WS_VSCROLL,
                                    0, 2, 120, 300, hwnd_,
                                    (HMENU)(INT_PTR)ID_MODEL, hInst, nullptr);
    if (modelCombo_) ::SendMessageW(modelCombo_, WM_SETFONT, (WPARAM)font_, TRUE);

    histBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.ai.history"),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 2, 56, 22, hwnd_,
                                 (HMENU)(INT_PTR)ID_HIST, hInst, nullptr);
    if (histBtn_) ::SendMessageW(histBtn_, WM_SETFONT, (WPARAM)font_, TRUE);

    sci_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"Scintilla", nullptr,
                             WS_CHILD | WS_VISIBLE,
                             6, 26, 360, 400, hwnd_, nullptr, hInst, nullptr);
    if (!sci_) return false;

    input_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                               6, 430, 290, 60, hwnd_,
                               (HMENU)(INT_PTR)ID_INPUT, hInst, nullptr);
    if (!input_) return false;
    ::SetWindowSubclass(input_, InputProcThunk, kInputSubclassId, (DWORD_PTR)this);

    sendBtn_ = ::CreateWindowExW(0, L"BUTTON", Tr(L"panel.ai.send"),
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 300, 430, 66, 60, hwnd_,
                                 (HMENU)(INT_PTR)ID_SEND, hInst, nullptr);
    if (sendBtn_) ::SendMessageW(sendBtn_, WM_SETFONT, (WPARAM)font_, TRUE);
    if (input_) ::SendMessageW(input_, WM_SETFONT, (WPARAM)font_, TRUE);

    // transcript 初始化：只读 + 自动换行 + 隐藏边距 + 无撤销
    SciSend(sci_, SCI_SETCODEPAGE, SC_CP_UTF8);
    SciSend(sci_, SCI_SETWRAPMODE, SC_WRAP_WORD);
    SciSend(sci_, SCI_SETCARETPERIOD, 0);   // 只读无光标闪烁
    SciSend(sci_, SCI_SETREADONLY, 1);
    SciSend(sci_, SCI_SETMARGINWIDTHN, 0, 0);
    SciSend(sci_, SCI_SETMARGINWIDTHN, 1, 0);
    SciSend(sci_, SCI_SETMARGINWIDTHN, 2, 0);
    SciSend(sci_, SCI_SETUNDOCOLLECTION, 0);
    SciSend(sci_, SCI_SETSCROLLWIDTHTRACKING, 1);

    ApplyTheme(*theme::Find(L"light"));
    UpdateLabel();
    return true;
}

void AiPanel::SystemLine(const std::wstring& text) {
    AppendLine(text, AS_SYSTEM);
}

void AiPanel::DiffBlock(const std::wstring& diff) {
    // unified diff 按行着色：+行绿 / -行红 / Index、@@、=== 等元信息灰，
    // 其余上下文行正文色。上限 200 行，超出截断（transcript 是 append-only）。
    constexpr int kMaxLines = 200;
    std::wstring head = diff;
    int lines = 0;
    size_t cut = std::wstring::npos;
    for (size_t i = 0; i < head.size() && lines <= kMaxLines; ++i)
        if (head[i] == L'\n') ++lines;
    if (lines > kMaxLines) {
        int n = 0;
        for (size_t i = 0; i < head.size(); ++i)
            if (head[i] == L'\n' && ++n == kMaxLines) { cut = i + 1; break; }
        if (cut != std::wstring::npos) head.resize(cut);
    }
    std::wstring line;
    for (size_t i = 0; i <= head.size(); ++i) {
        wchar_t ch = i < head.size() ? head[i] : L'\n';
        if (ch == L'\r') continue;
        if (ch != L'\n') { line += ch; continue; }
        if (!line.empty()) {
            int st = AS_DEFAULT;
            if (line.rfind(L"+++", 0) == 0 || line.rfind(L"---", 0) == 0 ||
                line.rfind(L"Index:", 0) == 0 || line.rfind(L"@@", 0) == 0 ||
                line.rfind(L"===", 0) == 0)
                st = AS_DIFF_META;
            else if (line.rfind(L"+", 0) == 0) st = AS_DIFF_ADD;
            else if (line.rfind(L"-", 0) == 0) st = AS_DIFF_DEL;
            AppendTextStyled(line + L"\n", st);
        }
        line.clear();
    }
}

void AiPanel::Destroy() {
    if (evtThreadUp_) {
        evtCancel_->store(true);
        evtThread_.detach();   // 线程 ≤10s 自行退出（hwnd 失效后 PostMessage 失败→包自删）
        evtThreadUp_ = false;
    }
    if (sci_) { ::DestroyWindow(sci_); sci_ = nullptr; }
    if (input_) { ::RemoveWindowSubclass(input_, InputProcThunk, kInputSubclassId);
                  ::DestroyWindow(input_); input_ = nullptr; }
    if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
    if (mono_) { ::DeleteObject(mono_); mono_ = nullptr; }
}

bool AiPanel::HasFocus() const {
    if (!hwnd_) return false;
    HWND f = ::GetFocus();
    return f == sci_ || f == input_ || f == sendBtn_;
}

void AiPanel::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
    ::KillTimer(hwnd_, kPollTimer);
}

void AiPanel::Layout(int w, int h) {
    if (!hwnd_) return;
    int dpi = ::GetDpiForWindow(hwnd_);
    auto u = [dpi](int px) { return MulDiv(px, dpi, 96); };
    int inH = u(64);
    // 极窄（主窗口被压缩）时隐藏模型下拉+历史按钮保住 label/close 不重叠；
    // 正常最小宽 360 逻辑px 由宿主拖拽 clamp 保证
    bool narrow = (w < u(330));
    if (modelCombo_) ::ShowWindow(modelCombo_, narrow ? SW_HIDE : SW_SHOW);
    if (histBtn_) ::ShowWindow(histBtn_, narrow ? SW_HIDE : SW_SHOW);
    ::MoveWindow(label_, u(8), u(6),
                 std::max(u(40), w - u(110) - (narrow ? 0 : u(150)) - u(62)),
                 u(18), TRUE);
    if (!narrow)
        ::MoveWindow(histBtn_, std::max(0, w - u(110) - u(144) - u(56)), u(2),
                     u(52), u(22), TRUE);
    if (!narrow)
        ::MoveWindow(modelCombo_, std::max(0, w - u(110) - u(144)), u(2),
                     u(138), u(200), TRUE);
    ::MoveWindow(closeBtn_, std::max(0, w - u(90)), u(2), u(84), u(22), TRUE);
    ::MoveWindow(sci_, u(6), u(26), std::max(0, w - 12),
                 std::max(0, h - u(26) - inH - u(12)), TRUE);
    ::MoveWindow(input_, u(6), std::max(0, h - inH - u(6)),
                 std::max(0, w - u(88)), inH, TRUE);
    ::MoveWindow(sendBtn_, std::max(0, w - u(78)), std::max(0, h - inH - u(6)),
                 u(72), inH, TRUE);

    // mono 字体按 DPI 重建（低频，Layout 时幂等重建）
    HFONT nf = ::CreateFontW(-MulDiv(10, dpi, 96), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI");
    if (nf) {
        if (mono_) ::DeleteObject(mono_);
        mono_ = nf;
        ::SendMessageW(input_, WM_SETFONT, (WPARAM)mono_, TRUE);
        SciSend(sci_, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Segoe UI");
        SciSend(sci_, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
        SciSend(sci_, SCI_STYLECLEARALL);
        // 重建后重放主题色
        struct S { int idx; COLORREF fg; };
        S styles[] = {
            {AS_DEFAULT,    colors_.fg},
            {AS_USER_TAG,   colors_.userTag},
            {AS_AI_TAG,     colors_.aiTag},
            {AS_REASONING,  colors_.dim},
            {AS_SYSTEM,     colors_.dim},
            {AS_DIFF_ADD,   colors_.diffAdd},
            {AS_DIFF_DEL,   colors_.diffDel},
            {AS_DIFF_META,  colors_.dim},
        };
        for (auto& s : styles) SciSend(sci_, SCI_STYLESETFORE, s.idx, s.fg);
        SciSend(sci_, SCI_STYLESETITALIC, AS_REASONING, 1);
        SciSend(sci_, SCI_STYLESETITALIC, AS_SYSTEM, 1);
        SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_ADD, (LPARAM)"Consolas");
        SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_DEL, (LPARAM)"Consolas");
        SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_META, (LPARAM)"Consolas");
    }
}

// --------------------------------------------------------------- 连接与会话

bool AiPanel::EnsureReady(const std::wstring& workDir) {
    if (!hwnd_) return false;
    workDir_ = workDir;
    bool oai = (backend_ == L"openai");
    if (serverOk_ && (oai || !sessionId_.empty())) return true;   // 已就绪
    if (connecting_.load()) return true;                 // 连接中
    UpdateLabel();
    StartConnectThread(workDir);
    return true;   // 异步完成后再渲染
}

void AiPanel::StartConnectThread(const std::wstring& workDir) {
    if (connecting_.exchange(true)) return;
    if (backend_ == L"openai") {
        // openai 直连：GET /models 探活 + 拉模型列表（本地 2.5s 足够）
        Logger::Info("AiPanel: connecting to OpenAI-compatible endpoint " +
                     WideToUtf8(oaiEndpoint_));
        HWND h = hwnd_;
        std::wstring ep = oaiEndpoint_, key = oaiApiKey_, mdl = oaiModel_;
        std::thread([h, ep, key, mdl]() {
            auto* res = new std::vector<std::wstring>();
            bool ok = ai::OaiClient::ListModels(ep, key, res);
            ::PostMessageW(h, kPmsgOaiConnected, ok ? 1 : 0, (LPARAM)res);
        }).detach();
        UpdateLabel();
        return;
    }
    Logger::Info("AiPanel: connecting to opencode server, cwd=" +
                 WideToUtf8(workDir));
    std::wstring wd = workDir;
    std::wstring pp = prefProv_, pm = prefModel_;
    HWND h = hwnd_;
    std::thread([h, wd, pp, pm]() {
        auto* res = new ConnectResult();
        res->ok = ai::OcClient::EnsureServer(wd, &res->notInstalled);
        if (res->ok) {
            res->sid = ai::OcClient::CreateSession(L"xfsWinPad");
            if (res->sid.empty()) res->ok = false;
        }
        // 模型目录（下拉数据源）+ 默认模型映射
        if (res->ok) {
            ai::OcClient::FetchModelList(&res->models);
            std::vector<std::pair<std::wstring, std::wstring>> defs;
            ai::OcClient::FetchProviderDefaults(&defs);
            // 择优：settings 偏好（需在目录里）→ 服务端默认首位 → 目录首位
            bool picked = false;
            if (!pp.empty()) {
                for (const auto& m : res->models) {
                    if (m.provider == pp && m.modelId == pm) {
                        res->prov = pp; res->model = pm; picked = true; break;
                    }
                }
            }
            if (!picked) {
                for (auto& d : defs) {
                    // 跳过 opencode 官方免费档（big-pickle 等小模型能力弱，
                    // 2026-09-04 用户反馈"不听指挥"根因；偏好明确匹配时仍可选）
                    if (d.first != L"opencode") {
                        res->prov = d.first; res->model = d.second; picked = true;
                        break;
                    }
                }
                if (!picked && !defs.empty()) {
                    res->prov = defs.front().first;
                    res->model = defs.front().second;
                    picked = true;
                }
            }
            if (!picked && !res->models.empty()) {
                res->prov = res->models.front().provider;
                res->model = res->models.front().modelId;
            }
        }
        ::PostMessageW(h, res->notInstalled ? kPmsgConnectNotInstalled
                                            : kPmsgConnectDone,
                       res->ok ? 1 : 0, (LPARAM)res);
    }).detach();
}

void AiPanel::StartEventThread() {
    if (evtThreadUp_ && evtThread_.joinable()) {
        evtCancel_->store(true);
        evtThread_.join();
        evtThreadUp_ = false;
    }
    if (evtThreadUp_) return;
    evtThreadUp_ = true;
    evtCancel_ = std::make_shared<std::atomic<bool>>(false);
    HWND h = hwnd_;
    auto cancel = evtCancel_;
    evtThread_ = std::thread([h, cancel]() {
        for (;;) {
            if (cancel->load()) return;
            // 阻塞订阅：断开（serve 重启/网络抖动/超时）→ 2s 后重连。
            // 事件只当触发器用，丢事件无害（250ms timer 兜底 + 全量
            // 拉取渲染幂等）。
            bool connected = ai::OcClient::EventSubscribe(
                [](void* user, const std::wstring& type,
                   const std::wstring& sid) {
                    // 工作线程上下文：只 PostMessage，零共享状态
                    auto* e = new AiPanel::OcEvent{type, sid};
                    if (!::PostMessageW((HWND)user, AiPanel::kPmsgOcEvent,
                                        0, (LPARAM)e))
                        delete e;
                },
                h, cancel.get());
            if (cancel->load()) return;
            Logger::Warn(std::string("AiPanel: /event stream ended (ok=") +
                         (connected ? "1" : "0") + "), reconnect in 2s");
            for (int i = 0; i < 20 && !cancel->load(); ++i)
                ::Sleep(100);
        }
    });
}

void AiPanel::NewSession() {
    if (!hwnd_ || busy_.load()) return;
    if (backend_ == L"openai") {
        // 本地对话：清内存历史 + 清 transcript + 清落盘历史
        oaiHistory_.clear();
        lastCtxSent_.clear();
        ::DeleteFileW(OaiHistoryFilePath().c_str());
        SciSend(sci_, SCI_SETREADONLY, 0);
        SciSend(sci_, SCI_CLEARALL);
        SciSend(sci_, SCI_SETREADONLY, 1);
        renderedMsgs_ = 0;
        streamingMsgIdx_ = -1;
        streamChars_ = 0;
        AppendLine(L"xfsWinPad AI (local/OpenAI-compatible)", AS_SYSTEM);
        return;
    }
    if (sessionId_.empty()) return;
    std::wstring old = sessionId_;
    sessionId_.clear();
    lastCtxSent_.clear();   // 新会话 → 下一条重新附带上文
    permNoticeShown_ = false;   // 权限提示也按会话重置
    sessCost_ = 0;          // 新会话累计归零（label 立刻隐藏 cost 段）
    sessTokIn_ = sessTokOut_ = sessTokReason_ = sessTokCacheRead_ = 0;
    HWND h = hwnd_;
    std::thread([h, old]() {
        std::wstring sid = ai::OcClient::CreateSession(L"xfsWinPad");
        ::PostMessageW(h, kPmsgNewSession, 0,
                       sid.empty() ? 0 : (LPARAM)(new std::wstring(sid)));
    }).detach();
    AppendLine(Tr(L"panel.ai.newsession"), AS_SYSTEM);
    UpdateLabel();
}

void AiPanel::SendUserText(const std::wstring& text) {
    Logger::Info("AiPanel: SendUserText len=" + std::to_string(text.size()) +
                 " backend=" + WideToUtf8(backend_) +
                 " sid=" + WideToUtf8(sessionId_) +
                 " busy=" + (busy_.load() ? "1" : "0"));
    if (text.empty() || busy_.load()) return;
    if (backend_ != L"openai" && sessionId_.empty()) return;   // 未连接
    std::wstring outgoing = text;   // 斜杠命令展开后替换

    // 外部数据块（批次 28）：SendExternal 排队后经 TryFlushExternalAsk 走
    // 到这里，或外部正文与本条消息同轮到来。本轮直接附加 extBody_（STDF
    // 统计等），优先于文档快照——外部块与文本问题强耦合，一次送达。
    std::wstring extBody;
    std::wstring extEcho;
    if (!extBody_.empty()) {
        extBody = std::move(extBody_);
        extBody_.clear();
        extEcho = std::move(extLabel_);
        extLabel_.clear();
    }

    // 斜杠快捷命令：/explain /fix /refactor /translate /summary —— 展开成
    // 预设 prompt 后走普通发送。选区文本由 contextProvider 的 [Editor context]
    // 块附带（若开），这里只指明"对选区/当前文档"做什么。
    // 命令后的剩余文字作为补充要求原样传给模型。
    static const struct { const wchar_t* cmd; const wchar_t* withSel; const wchar_t* noSel; } kSlash[] = {
        { L"/explain",
          L"Explain the selected text in my editor (see the selection in the "
          L"[Editor context] block). Be concise and specific; point out any "
          L"bugs or pitfalls you notice. Respond in the same language as the text.",
          L"Explain the active document in my editor (path in the [Editor "
          L"context] block - read it if you need to). Summarize what it does, "
          L"its structure, and anything suspicious. Respond in the same "
          L"language as the document." },
        { L"/fix",
          L"Find and fix the bug(s) in the selected text of my editor (see the "
          L"[Editor context] block). Show the corrected code and briefly "
          L"explain each change. If you are confident, apply the fix directly "
          L"to the file.",
          L"Review the active document in my editor for bugs and fix them. "
          L"Show what you changed and why." },
        { L"/refactor",
          L"Refactor the selected text in my editor for readability and "
          L"structure, keeping behavior identical. Show the refactored code "
          L"and briefly list what improved. Apply it to the file if confident.",
          L"Refactor the active document in my editor for readability and "
          L"structure, keeping behavior identical. Apply it to the file and "
          L"summarize the changes." },
        { L"/translate",
          L"Translate the selected text in my editor (see the [Editor context] "
          L"block) to Chinese if it is in another language, otherwise to "
          L"English. Keep code identifiers, formatting and URLs untouched.",
          L"Translate the active document in my editor to Chinese if it is in "
          L"another language, otherwise to English. Keep code identifiers and "
          L"formatting untouched." },
        { L"/summary",
          L"Summarize the active document in my editor in a short outline "
          L"(key points only). Respond in the same language as the document.",
          L"Summarize the active document in my editor in a short outline "
          L"(key points only). Respond in the same language as the document." },
    };
    for (const auto& sc : kSlash) {
        size_t n = wcslen(sc.cmd);
        if (outgoing.compare(0, n, sc.cmd) != 0) continue;
        if (outgoing.size() > n && outgoing[n] != L' ') break;   // /explanation 之类不算
        std::wstring rest = outgoing.substr(n);
        while (!rest.empty() && (rest.front() == L' ' || rest.front() == L'\t'))
            rest.erase(rest.begin());
        // 选区有没有由 contextProvider 快照决定；有选区用 withSel 文案
        bool hasSel = false;
        if (contextProvider_) {
            AiContext probe = contextProvider_();
            hasSel = probe.detail.find(L"选中") != std::wstring::npos ||
                     probe.detail.find(L"selection") != std::wstring::npos ||
                     probe.detail.find(L"selected") != std::wstring::npos;
        }
        std::wstring prompt = hasSel ? sc.withSel : sc.noSel;
        if (!rest.empty()) prompt += L"\n\nUser notes: " + rest;
        // transcript 里回显原始斜杠命令（比展开后的长 prompt 可读），
        // 再加一行系统说明；服务端的 user 消息（展开文本）被「已回显」
        // 跳过逻辑忽略——回显以本地为准。
        AppendLine(L"", AS_DEFAULT);
        AppendLine(Tr(L"panel.ai.you"), AS_USER_TAG);
        AppendTextStyled(outgoing + L"\n", AS_DEFAULT);
        AppendLine(L"→ " + std::wstring(sc.cmd) + L" (prompt expanded)", AS_SYSTEM);
        outgoing = prompt;
        break;
    }

    // /plugin <描述> —— AI 插件工场指令：转给宿主编排（脚手架+提示词+安装）
    if (outgoing.size() > 8 && outgoing.compare(0, 8, L"/plugin ") == 0) {
        if (backend_ == L"openai") {
            // 工场依赖 agent 工具链（写文件/bash），纯对话直连做不了
            AppendLine(L"", AS_DEFAULT);
            AppendLine(Tr(L"panel.ai.you"), AS_USER_TAG);
            AppendTextStyled(outgoing + L"\n", AS_DEFAULT);
            SystemLine(Tr(L"panel.ai.pluginlocal"));
            return;
        }
        std::wstring task = outgoing.substr(8);
        while (!task.empty() && (task.front() == L' ' || task.front() == L'\t'))
            task.erase(task.begin());
        if (!task.empty()) {
            AppendLine(L"", AS_DEFAULT);
            AppendLine(Tr(L"panel.ai.you"), AS_USER_TAG);
            AppendTextStyled(outgoing + L"\n", AS_DEFAULT);
            if (onPluginTask) onPluginTask(task);
            return;
        }
    }

    // 斜杠命令展开分支已在上面回显过 outgoing 原文——这里判断是否需要
    // 通用回显：展开后 outgoing != text（展开过的），跳过第二次回显。
    const bool expanded = (outgoing != text);
    if (onBeforeSend) onBeforeSend();   // MainWindow 快照磁盘状态（自动重载对比）

    // ---- openai 直连模式：纯对话，无工具/权限/diff -------------------------
    if (backend_ == L"openai") {
        // UI 回显（含展开后的完整 prompt —— 本地无服务端回放，直接显示）
        if (!expanded) {
            AppendLine(L"", AS_DEFAULT);
            AppendLine(Tr(L"panel.ai.you"), AS_USER_TAG);
        }
        AppendTextStyled(outgoing + L"\n", AS_DEFAULT);
        if (!extEcho.empty())
            AppendLine(extEcho + L"\n", AS_SYSTEM);

        // 上下文注入（与 opencode 模式同规则：开关 + 去重）
        std::wstring body = extBody;
        if (attachContext_ && contextProvider_) {
            AiContext c = contextProvider_();
            if (!c.block.empty() && c.block != lastCtxSent_) {
                if (!body.empty()) body += L"\n\n";
                body += c.block;
                lastCtxSent_ = c.block;
                AppendLine(I18n::Instance().Fmt(L"panel.ai.ctxattached",
                                                {c.detail}), AS_SYSTEM);
            }
        }

        busy_.store(true);
        oaiAbort_->store(false);
        UpdateBusyUi();
        std::wstring prompt = (body.empty() ? L"" : body + L"\n\n") + outgoing;
        std::vector<ai::OaiMsg> msgs;
        msgs.push_back({L"system",
            L"You are an assistant embedded in the xfsWinPad text editor. "
            L"Answer concisely in the user's language. You can read the "
            L"editor context provided by the user, but you cannot modify "
            L"files."});
        for (const auto& m : oaiHistory_) msgs.push_back(m);
        msgs.push_back({L"user", prompt});
        oaiHistory_.push_back({L"user", prompt});
        // 历史上限 40 条（20 轮），防长对话撑爆本地小模型上下文
        if (oaiHistory_.size() > 40)
            oaiHistory_.erase(oaiHistory_.begin(),
                              oaiHistory_.begin() + (oaiHistory_.size() - 40));
        xfs::WriteFileBytes(OaiHistoryFilePath(),
                            ai::OaiClient::HistoryToJson(oaiHistory_).data(),
                            ai::OaiClient::HistoryToJson(oaiHistory_).size());
        StartOaiChat(std::move(msgs));
        return;
    }

    // 上下文注入：开关开 + 活跃文档快照与上次不同才附带（去重省 token）。
    // 附带时 transcript 里以灰斜体回显一行简述，用户可感知模型看到了什么。
    // 外部数据块（批次 28）优先：extBody 非空则直接用它，跳过文档快照。
    std::wstring ctx;
    bool ctxAttached = false;
    if (!extBody.empty()) {
        ctx = extBody + L"\n\n";
        ctxAttached = true;
        AppendLine(I18n::Instance().Fmt(L"panel.ai.ctxattached",
                                        {extEcho}), AS_SYSTEM);
    } else if (attachContext_ && contextProvider_) {
        AiContext c = contextProvider_();
        if (!c.block.empty() && c.block != lastCtxSent_) {
            ctx = c.block + L"\n\n";
            lastCtxSent_ = c.block;
            ctxAttached = true;
            AppendLine(I18n::Instance().Fmt(L"panel.ai.ctxattached",
                                            {c.detail}), AS_SYSTEM);
        }
    }

    busy_.store(true);
    UpdateBusyUi();

    // UI 即时回显用户消息（服务端列表稍后也会给出 user 消息，
    // 渲染层对已回显的 user 消息跳过；历史恢复时 pendingReplay_ 才补画）
    if (!expanded) {
        AppendLine(L"", AS_DEFAULT);
        AppendLine(Tr(L"panel.ai.you"), AS_USER_TAG);
        AppendTextStyled(outgoing + L"\n", AS_DEFAULT);
    }
    if (ctxAttached && !extEcho.empty())
        AppendLine(extEcho, AS_SYSTEM);

    std::wstring sid = sessionId_;
    std::wstring body = ctx + outgoing;
    std::wstring prov = modelProvider_, mdl = modelName_;
    HWND h = hwnd_;
    busyStart_ = ::GetTickCount();
    busyStalled_ = false;
    std::thread([h, sid, body, prov, mdl]() {
        // PromptAsync 只覆盖"提交"这一步（10s 超时）；提交失败立即回报
        // UI（zen 不可达/服务端 4xx/5xx/连接拒绝）。提交成功后生成阶段的
        // 卡死由 PollMessages 的看门狗处理。
        std::wstring err;
        if (!ai::OcClient::PromptAsync(sid, body, prov, mdl, &err)) {
            if (err.empty()) err = L"request failed";
            ::PostMessageW(h, kPmsgPromptFail, 0,
                           (LPARAM)new OaiReply{L"", err});
            return;
        }
        // 发送成功与否都由 timer 轮询观察状态
    }).detach();

    ::SetTimer(hwnd_, kPollTimer, 250, nullptr);
}

// 外部请求（批次 28）：宿主模块直接投递「指令 + 数据块」。body 作为本轮
// 上下文附加（优先于文档快照注入），label 原文回显。busy/未连接时排队，
// 连接完成或本轮回复完成后自动补发（TryFlushExternalAsk）。
void AiPanel::SendExternal(const std::wstring& label, const std::wstring& body) {
    if (!hwnd_ || body.empty()) return;
    extLabel_ = label;
    extBody_ = body;
    Logger::Info("AiPanel: SendExternal queued label=" + WideToUtf8(label) +
                 " body=" + std::to_string(body.size()) + " chars" +
                 " busy=" + (busy_.load() ? "1" : "0") +
                 " ready=" + (serverOk_ ? "1" : "0"));
    if (busy_.load() || connecting_.load() ||
        !(serverOk_ && (backend_ == L"openai" || !sessionId_.empty()))) {
        SystemLine(Tr(L"panel.ai.extqueued"));
        return;
    }
    TryFlushExternalAsk();
}

// extBody_ 待发 → 组装指令并走 SendUserText（其内部消费 extBody_）。
// 未就绪/busy 时静默保留（由连接完成/回复完成挂点再试）。
void AiPanel::TryFlushExternalAsk() {
    if (extBody_.empty() || busy_.load() || connecting_.load()) return;
    if (!(serverOk_ && (backend_ == L"openai" || !sessionId_.empty()))) return;
    std::wstring q =
        L"Analyze the data block attached to this message and answer in the "
        L"user's language.\n\n" + extLabel_;
    Logger::Info("AiPanel: flushing external ask, body=" +
                 std::to_string(extBody_.size()) + " chars");
    SendUserText(q);
}

void AiPanel::StartOaiChat(std::vector<ai::OaiMsg> msgs) {
    HWND h = hwnd_;
    std::wstring ep = oaiEndpoint_, key = oaiApiKey_;
    std::wstring mdl = oaiModel_.empty() ? L"default" : oaiModel_;
    // 回调上下文放堆（shared_ptr 随线程 lambda 存活）：面板销毁后回调只
    // 操作堆数据 + PostMessage 到失效 hwnd（安全 no-op），不触碰 this。
    struct OaiStreamCtx { HWND h; std::wstring full; };
    auto ctx = std::make_shared<OaiStreamCtx>(OaiStreamCtx{h, L""});
    std::thread([h, ep, key, mdl, msgs, ctx, abort = oaiAbort_]() {
        auto* r = new OaiReply();
        if (!ai::OaiClient::ChatStream(
                ep, key, mdl, msgs,
                [](void* u, const std::wstring& delta) {
                    auto* c = (OaiStreamCtx*)u;
                    ::PostMessageW(c->h, kPmsgOaiDelta, 0,
                                   (LPARAM)new std::wstring(delta));
                },
                ctx.get(),
                [](void* u, const std::wstring& full) {
                    ((OaiStreamCtx*)u)->full = full;
                },
                &r->err, 300000, abort.get()))
            r->text.clear();
        else
            r->text = ctx->full;
        ::PostMessageW(h, kPmsgOaiReply, 0, (LPARAM)r);
    }).detach();
}

// --------------------------------------------------------------- 渲染

void AiPanel::AppendLine(const std::wstring& text, int styleBase) {
    AppendTextStyled(text + L"\n", styleBase);
}

void AiPanel::AppendTextStyled(const std::wstring& text, int styleBase) {
    if (!sci_ || text.empty()) return;
    std::string utf8 = WideToUtf8(text);
    SciSend(sci_, SCI_SETREADONLY, 0);
    sptr_t pos = SciSend(sci_, SCI_GETLENGTH);
    SciSend(sci_, SCI_APPENDTEXT, (uptr_t)utf8.size(), (LPARAM)utf8.c_str());
    SciSend(sci_, SCI_STARTSTYLING, pos, 0x1F);
    for (size_t i = 0; i < utf8.size(); ++i)
        SciSend(sci_, SCI_SETSTYLING, 1, (uptr_t)styleBase);
    SciSend(sci_, SCI_SETREADONLY, 1);
    SciSend(sci_, SCI_GOTOPOS, SciSend(sci_, SCI_GETLENGTH));
}

// 全量重绘（连接成功恢复历史时；平时增量渲染走轮询路径）
void AiPanel::RenderMessages(const std::vector<ai::OcMessage>& msgs, bool finalPass) {
    // 增量规则：
    //  * renderedMsgs_ 之前的消息不重绘（append-only transcript）
    //  * 正在流式的消息（最后一条 assistant 无 finish）按字符偏移续写
    //  * finish 出现 → 该消息收尾（换行），renderedMsgs_ 前进
    if (!sci_) return;
    int n = (int)msgs.size();
    if (n == 0) return;

    bool fullReplay = pendingReplay_;
    pendingReplay_ = false;
    if (fullReplay) {
        SciSend(sci_, SCI_SETREADONLY, 0);
        SciSend(sci_, SCI_CLEARALL);
        SciSend(sci_, SCI_SETREADONLY, 1);
        streamChars_ = 0;
        streamingMsgIdx_ = -1;
        renderedMsgs_ = 0;
        shownTools_.clear();
    }

    int start = renderedMsgs_;
    if (streamingMsgIdx_ >= 0 && streamingMsgIdx_ < n) start = streamingMsgIdx_;

    for (int i = start; i < n; ++i) {
        const ai::OcMessage& m = msgs[i];
        bool isLast = (i == n - 1);
        bool streaming = isLast && m.role == L"assistant" && m.finish.empty();

        if (m.role == L"user") {
            // user 消息已由 SendUserText 即时回显，轮询路径跳过；
            // 历史恢复（fullReplay）时补画
            if (fullReplay) {
                AppendLine(Tr(L"panel.ai.you"), AS_USER_TAG);
                for (const auto& p : m.parts)
                    if (p.type == L"text" && !p.text.empty())
                        AppendTextStyled(p.text + L"\n", AS_DEFAULT);
            }
            if (!streaming) renderedMsgs_ = i + 1;
            continue;
        }
        if (m.role != L"assistant") continue;

        if (i != streamingMsgIdx_) {
            AppendLine(Tr(L"panel.ai.ai"), AS_AI_TAG);
            streamingMsgIdx_ = i;
            streamChars_ = 0;
        }
        // 拼接 text 增量（reasoning 不上屏；tool 调用单独灰字显示）
        std::wstring pending;
        for (const auto& p : m.parts) {
            if (p.type == L"text" && !p.text.empty()) pending += p.text;
            else if (p.type == L"tool" && !p.tool.empty()) {
                // 同一次调用状态流转只显示一次；running → completed 再补一行
                std::wstring key = p.callId.empty() ? p.tool : p.callId;
                key += L":" + p.toolState;
                if (shownTools_.insert(key).second) {
                    AppendLine(I18n::Instance().Fmt(L"panel.ai.toolcall",
                        {p.tool, p.toolState.empty() ? L"…" : p.toolState}),
                        AS_SYSTEM);
                }
            }
        }

        if ((int)pending.size() > streamChars_) {
            AppendTextStyled(pending.substr(streamChars_), AS_DEFAULT);
            streamChars_ = (int)pending.size();
        }
        if (!streaming) {
            if (!pending.empty() && pending.back() != L'\n')
                AppendTextStyled(L"\n", AS_DEFAULT);
            AppendLine(L"", AS_DEFAULT);
            renderedMsgs_ = i + 1;
            streamingMsgIdx_ = -1;
            streamChars_ = 0;
        }
    }
    (void)finalPass;
}

void AiPanel::PollMessages() {
    if (backend_ == L"openai") return;   // 本地模式无服务端可轮询
    if (sessionId_.empty()) {
        ::KillTimer(hwnd_, kPollTimer);
        return;
    }
    // 连接成功后的首轮拉历史：pendingReplay_ 未消费前持续轮询
    if (!busy_.load() && !pendingReplay_) {
        ::KillTimer(hwnd_, kPollTimer);
        return;
    }
    std::vector<ai::OcMessage> msgs;
    if (!ai::OcClient::ListMessages(sessionId_, &msgs)) return;
    RenderMessages(msgs, true);
    pendingReplay_ = false;   // 拉到即消费（无论是否为空）

    // ---- 消息级错误上屏（info.error） ------------------------------------
    // serve 吞掉上游错误不发文本 part：zen 401 "Invalid API key"、网关 5xx
    // 等只出现在 assistant 消息的 info.error 字段（批次 18 实锤）。不去重
    // 的话 250ms 轮询每轮刷一条。
    for (const auto& m : msgs) {
        if (m.errText.empty()) continue;
        std::wstring key = m.role + L":" + m.errText;
        if (!shownMsgErrs_.insert(key).second) continue;
        AppendLine(L"", AS_DEFAULT);
        SystemLine(Tr(L"panel.ai.msgerror") + m.errText);
        Logger::Error("AiPanel: session message error: " +
                      WideToUtf8(m.errText));
        // 错误即终局：解除"生成中"（上游已失败，继续轮询无意义）
        if (busy_.load()) {
            ai::OcClient::Abort(sessionId_);
            busy_.store(false);
            ::KillTimer(hwnd_, kPollTimer);
            UpdateBusyUi();
        }
    }

    // ---- 看门狗：busy 但服务端毫无动静 → 提示并自动终止 ------------------
    // 场景（新机实测）：免费模型网关不可达（网络被墙/DNS 污染），serve 接受
    // prompt 后 provider 连接失败，无任何消息产生，busy 永挂「生成中」。
    // 进展信号 = 消息数/最后一条内容长度变化；120s 零进展即判死（正常长
    // agent 任务每个 step 都有新消息，不会误伤）。
    if (busy_.load()) {
        std::wstring sig = std::to_wstring(msgs.size());
        if (!msgs.empty())
            sig += L":" + std::to_wstring(msgs.back().parts.size());
        if (sig != lastProgressSig_) {
            if (!lastProgressSig_.empty())
                Logger::Info("AiPanel: watchdog progress " +
                             WideToUtf8(sig));
            lastProgressSig_ = sig;
            busyStalled_ = false;
        } else if (::GetTickCount() - busyStart_ > 120000) {
            if (!busyStalled_) {
                busyStalled_ = true;
                SystemLine(Tr(L"panel.ai.stuck"));
                Logger::Warn("AiPanel: no progress for 120s, aborting");
            }
            ai::OcClient::Abort(sessionId_);
            busy_.store(false);
            ::KillTimer(hwnd_, kPollTimer);
            UpdateBusyUi();
            return;
        }
    } else {
        lastProgressSig_.clear();
    }

    // 生成结束判定：最后一条 assistant finish 非空 + 服务端确认不再 busy。
    // agent 循环每个 step 都是一条独立 assistant 消息且中间步 finish 也非空
    //（2026-09-05 实锤：6 步任务在 step 0 就被误判完成，重载窗口提前耗尽），
    // 所以必须用 /session/status 兜底：busy 期间继续轮询。
    if (busy_.load()) {
        // 自动放行本会话的权限请求（external_directory 等）。serve 无 UI，
        // 无人应答时 read/write 工具永久 running——上午实测挂死 30+ 分钟。
        // always+remember：服务端同目录（含跨 session）记住，通常一问一放。
        // 消息轮询 250ms 太密，permission 检查 2s 节流一次。
        DWORD now = ::GetTickCount();
        if (now - lastPermCheck_ >= 2000) {
            lastPermCheck_ = now;
        std::vector<std::pair<std::wstring, std::wstring>> perms;
        if (ai::OcClient::ListPendingPermissions(sessionId_, &perms)) {
            for (const auto& p : perms) {
                if (!autoApprove_) {
                    // 策略关：不代答，只提示一次（每会话一条）。
                    // serve 无 UI，请求会挂起——用户需到 opencode 终端放行。
                    if (!permNoticeShown_) {
                        permNoticeShown_ = true;
                        SystemLine(Tr(L"panel.ai.permhold"));
                        Logger::Warn("AiPanel: permission pending, auto-approve "
                                     "off - user must approve externally: " +
                                     WideToUtf8(p.second));
                    }
                    continue;
                }
                if (ai::OcClient::AnswerPermission(sessionId_, p.first))
                        Logger::Info("AiPanel: auto-approved permission " +
                                     WideToUtf8(p.second) + " (" +
                                     WideToUtf8(p.first) + ")");
                }
            }
        }
        bool finishSet = false;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->role == L"assistant") { finishSet = !it->finish.empty(); break; }
        }
        if (finishSet) {
            if (ai::OcClient::SessionBusy(sessionId_))
                return;   // 后续 step 还在跑，UI 保持「生成中」，下轮再看
            busy_.store(false);
            ::KillTimer(hwnd_, kPollTimer);
            UpdateBusyUi();
            Logger::Info("AiPanel: response complete");
            RefreshSessionTokens();   // 会话累计 cost/tokens 上 label（批次 27）            // 收集本轮写类工具的目标路径（跨全部 assistant 消息去重）+
            // edit/patch 的 unified diff（metadata.diff）上屏，用户直接看到
            // AI 改了什么，不必去文件里 diff。
            std::vector<std::wstring> touched;
            std::set<std::wstring> diffSeen;
            for (const auto& m : msgs) {
                if (m.role != L"assistant") continue;
                for (const auto& p : m.parts) {
                    if (p.type != L"tool" || p.toolState != L"completed") continue;
                    bool isWrite = p.tool == L"write" || p.tool == L"edit" ||
                                   p.tool == L"patch";
                    if (!isWrite) continue;
                    if (!p.filePath.empty() &&
                        std::find(touched.begin(), touched.end(),
                                  p.filePath) == touched.end())
                        touched.push_back(p.filePath);
                    if (!p.diff.empty()) {
                        std::wstring key = p.callId.empty()
                            ? p.tool + L":" + p.filePath : p.callId;
                        if (diffSeen.insert(key).second) {
                            std::wstring label = p.filePath;
                            if (!label.empty()) {
                                // diff 自带 Index: 行；标签行只留文件名避免过长
                                size_t slash = label.find_last_of(L"\\/");
                                if (slash != std::wstring::npos)
                                    label = label.substr(slash + 1);
                                AppendLine(I18n::Instance().Fmt(
                                    L"panel.ai.diffheader", {label}), AS_SYSTEM);
                            }
                            DiffBlock(p.diff);
                        }
                    }
                }
            }
            if (onResponseDone) onResponseDone(touched);   // 磁盘对比 → 重载/提示
            TryFlushExternalAsk();   // 生成期间排队的外部请求（批次 28）补发
        }
    }
}

void AiPanel::UpdateLabel() {
    if (!label_) return;
    std::wstring s = Tr(L"panel.ai");
    if (connecting_.load()) {
        s += L" — ";
        s += Tr(L"panel.ai.connecting");
    } else if (!serverOk_) {
        s += L" — ";
        s += Tr(L"panel.ai.offline");
    } else if (busy_.load()) {
        s += L" — ";
        s += Tr(L"panel.ai.thinking");
    } else if (backend_ == L"openai" && !oaiModel_.empty()) {
        s += L" (" + oaiModel_ + L")";
    } else if (!serverVersion_.empty()) {
        s += L" (v" + serverVersion_ + L")";
    }
    // 会话累计 cost/tokens（批次 27）：仅 opencode 模式有服务端累计。
    // 格式 " · $0.80 · 1.2k↑ 9.5k↓"；cost=0 整段省略（免费模型/新会话）。
    if (backend_ != L"openai" && serverOk_ && sessCost_ > 0) {
        s += L" · " + ai::FormatCost(sessCost_);
        std::wstring toks =
            ai::FormatTokens(sessTokIn_) + L"↑ " +
            ai::FormatTokens(sessTokOut_ + sessTokReason_) + L"↓";
        s += L" · " + toks;
    }
    ::SetWindowTextW(label_, s.c_str());
}

// 会话累计 cost/tokens 拉取（UI 线程同步请求，挂点：历史切换 + 生成完成）。
// 拉取失败静默（label 保持旧值——纯信息展示，不值得报错打扰）。
void AiPanel::RefreshSessionTokens() {
    if (backend_ == L"openai" || sessionId_.empty()) return;
    ai::OcSessionEntry detail;
    if (!ai::OcClient::GetSession(sessionId_, &detail)) return;
    sessCost_ = detail.cost;
    sessTokIn_ = detail.tokIn;
    sessTokOut_ = detail.tokOut;
    sessTokReason_ = detail.tokReason;
    sessTokCacheRead_ = detail.tokCacheRead;
    UpdateLabel();
    Logger::Info("AiPanel: session tokens in=" +
                 std::to_string(detail.tokIn) + " out=" +
                 std::to_string(detail.tokOut) + " cost=" +
                 std::to_string(detail.cost));
}

void AiPanel::UpdateBusyUi() {
    if (input_) ::EnableWindow(input_, !busy_.load());
    if (modelCombo_) ::EnableWindow(modelCombo_, !busy_.load());
    if (histBtn_) ::EnableWindow(histBtn_, !busy_.load() && serverOk_);
    if (sendBtn_) {
        ::SetWindowTextW(sendBtn_, busy_.load() ? Tr(L"panel.ai.abort")
                                                : Tr(L"panel.ai.send"));
    }
    UpdateLabel();
}

void AiPanel::Retranslate() {
    if (closeBtn_) ::SetWindowTextW(closeBtn_, Tr(L"panel.ai.close"));
    if (histBtn_) ::SetWindowTextW(histBtn_, Tr(L"panel.ai.history"));
    if (sendBtn_) ::SetWindowTextW(sendBtn_, Tr(L"panel.ai.send"));
    UpdateLabel();
}

void AiPanel::ApplyTheme(const ThemeDef& t) {
    colors_.bg = t.editorBg;
    colors_.fg = t.editorFg;
    colors_.userTag = t.keyword;     // 蓝
    colors_.aiTag = t.str;           // 绿
    colors_.dim = t.comment;
    colors_.diffAdd = t.number;      // 绿（两套内建主题 number 均为绿系）
    colors_.diffDel = t.str;         // 红/橘红系（str 是暖色）
    if (!sci_) return;
    struct S { int idx; COLORREF fg; };
    S styles[] = {
        {AS_DEFAULT,    colors_.fg},
        {AS_USER_TAG,   colors_.userTag},
        {AS_AI_TAG,     colors_.aiTag},
        {AS_REASONING,  colors_.dim},
        {AS_SYSTEM,     colors_.dim},
        {AS_DIFF_ADD,   colors_.diffAdd},
        {AS_DIFF_DEL,   colors_.diffDel},
        {AS_DIFF_META,  colors_.dim},
    };
    for (auto& s : styles) SciSend(sci_, SCI_STYLESETFORE, s.idx, s.fg);
    SciSend(sci_, SCI_STYLESETBACK, STYLE_DEFAULT, colors_.bg);
    SciSend(sci_, SCI_STYLESETITALIC, AS_REASONING, 1);
    SciSend(sci_, SCI_STYLESETITALIC, AS_SYSTEM, 1);
    SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_ADD, (LPARAM)"Consolas");
    SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_DEL, (LPARAM)"Consolas");
    SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_META, (LPARAM)"Consolas");
    SciSend(sci_, SCI_STYLECLEARALL);
    // STYLECLEARALL 重置前景，再刷一遍各槽位
    for (auto& s : styles) SciSend(sci_, SCI_STYLESETFORE, s.idx, s.fg);
    SciSend(sci_, SCI_STYLESETITALIC, AS_REASONING, 1);
    SciSend(sci_, SCI_STYLESETITALIC, AS_SYSTEM, 1);
    SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_ADD, (LPARAM)"Consolas");
    SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_DEL, (LPARAM)"Consolas");
    SciSend(sci_, SCI_STYLESETFONT, AS_DIFF_META, (LPARAM)"Consolas");
}

// ------------------------------------------------------------------ input

LRESULT AiPanel::InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN && !(::GetKeyState(VK_SHIFT) & 0x8000)) {
                int len = ::GetWindowTextLengthW(input_);
                std::wstring text(len + 1, L'\0');
                ::GetWindowTextW(input_, &text[0], len + 1);
                text.resize(len);
                // 尾部 \r\n 清理
                while (!text.empty() &&
                       (text.back() == L'\r' || text.back() == L'\n'))
                    text.pop_back();
                ::SetWindowTextW(input_, L"");
                SendUserText(text);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                ::SetFocus(::GetParent(::GetParent(hwnd_)));
                return 0;
            }
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK AiPanel::InputProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                         UINT_PTR, DWORD_PTR ref) {
    auto* self = (AiPanel*)ref;
    return self->InputProc(h, m, wp, lp);
}

// --------------------------------------------------------------- 会话历史

void AiPanel::ShowHistoryPicker() {
    if (!hwnd_ || busy_.load() || sessionId_.empty() || !serverOk_) return;

    // 同步拉列表（本地 HTTP，PollMessages 已有 UI 线程同步请求先例）
    std::vector<ai::OcSessionEntry> all;
    if (!ai::OcClient::ListSessions(&all)) {
        SystemLine(Tr(L"panel.ai.historyfail"));
        return;
    }
    // 只留当前工作区的会话，按最近更新排序，取前 12
    std::vector<ai::OcSessionEntry> mine;
    for (auto& e : all) {
        if (e.id == sessionId_) continue;   // 当前会话不进菜单
        if (_wcsicmp(e.directory.c_str(), workDir_.c_str()) != 0) continue;
        mine.push_back(e);
    }
    std::sort(mine.begin(), mine.end(),
              [](const ai::OcSessionEntry& a, const ai::OcSessionEntry& b) {
                  return a.updated > b.updated;
              });
    if (mine.size() > 12) mine.resize(12);
    if (mine.empty()) {
        SystemLine(Tr(L"panel.ai.historyempty"));
        return;
    }

    HMENU menu = ::CreatePopupMenu();
    for (size_t i = 0; i < mine.size(); ++i) {
        // "title · M-dd HH:mm · $0.80"；title 空 → 会话 id
        std::wstring label = mine[i].title.empty() ? mine[i].id : mine[i].title;
        if (label.size() > 60) label = label.substr(0, 60) + L"…";
        SYSTEMTIME st{};
        ULARGE_INTEGER li{};
        li.QuadPart = (ULONGLONG)(mine[i].updated * 10000LL) +
                      116444736000000000ULL;
        FILETIME ft{ li.LowPart, li.HighPart };
        if (::FileTimeToSystemTime(&ft, &st)) {
            wchar_t ts[32];
            swprintf_s(ts, L" · %d-%02d %02d:%02d", st.wMonth, st.wDay,
                       st.wHour, st.wMinute);
            label += ts;
        }
        std::wstring cost = ai::FormatCost(mine[i].cost);
        if (!cost.empty()) label += L" · " + cost;
        MENUITEMINFOW mi{ sizeof(mi) };
        mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
        mi.wID = (UINT)(i + 1);
        mi.dwTypeData = label.data();
        mi.cch = (UINT)label.size();
        mi.fState = MFS_ENABLED;
        ::InsertMenuItemW(menu, (UINT)i, TRUE, &mi);
    }

    RECT rc;
    ::GetWindowRect(histBtn_, &rc);
    ::SetForegroundWindow(hwnd_);
    int picked = (int)::TrackPopupMenu(
        menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
        rc.left, rc.bottom, 0, hwnd_, nullptr);
    ::DestroyMenu(menu);
    if (picked <= 0 || (size_t)picked > mine.size()) return;
    const ai::OcSessionEntry& pick = mine[(size_t)picked - 1];

    // 切换：拉消息全量回放（本地请求，PollMessages 同款同步先例）
    std::vector<ai::OcMessage> msgs;
    if (!ai::OcClient::ListMessages(pick.id, &msgs)) {
        SystemLine(Tr(L"panel.ai.historyfail"));
        return;
    }
    sessionId_ = pick.id;
    lastCtxSent_.clear();          // 换会话 → 下一条重新附带上文
    permNoticeShown_ = false;
    busy_.store(false);
    ::KillTimer(hwnd_, kPollTimer);
    pendingReplay_ = true;
    RenderMessages(msgs, true);    // 内部按 pendingReplay_ 清屏全量重绘
    UpdateBusyUi();
    RefreshSessionTokens();        // 会话累计（cost/tokens）随切换刷新
    Logger::Info("AiPanel: switched to session " + WideToUtf8(pick.id));
}

// --------------------------------------------------------------- wnd proc

LRESULT AiPanel::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kPmsgConnectDone: {
            connecting_.store(false);
            bool ok = (wp == 1);
            if (lp) {
                auto* res = (ConnectResult*)lp;
                if (ok && !res->sid.empty()) {
                    sessionId_ = res->sid;
                    modelProvider_ = res->prov;
                    modelName_ = res->model;
                    models_ = std::move(res->models);
                    // 填模型下拉 + 选中当前模型
                    if (modelCombo_) {
                        ::SendMessageW(modelCombo_, CB_RESETCONTENT, 0, 0);
                        int sel = -1;
                        for (size_t i = 0; i < models_.size(); ++i) {
                            const auto& m = models_[i];
                            std::wstring label = m.provider + L" · " +
                                (!m.modelName.empty() ? m.modelName : m.modelId);
                            int idx = (int)::SendMessageW(modelCombo_,
                                CB_ADDSTRING, 0, (LPARAM)label.c_str());
                            ::SendMessageW(modelCombo_, CB_SETITEMDATA,
                                           (WPARAM)idx, (LPARAM)i);
                            if (m.provider == modelProvider_ &&
                                m.modelId == modelName_)
                                sel = idx;
                        }
                        if (sel >= 0)
                            ::SendMessageW(modelCombo_, CB_SETCURSEL,
                                           (WPARAM)sel, 0);
                    }
                    pendingReplay_ = true;   // 首轮轮询 → 全量重绘（含历史）
                }
                delete res;
            }
            if (ok) {
                serverOk_ = true;
                ai::OcClient::ProbeHealth(&serverVersion_);
                if (ai::OcClient::DetectPoisonedZenAuth() &&
                    !poisonAuthShown_) {
                    poisonAuthShown_ = true;
                    SystemLine(Tr(L"panel.ai.poisonauth"));
                }
                Logger::Info("AiPanel: session ready " + WideToUtf8(sessionId_) +
                             " model=" + WideToUtf8(modelProvider_) + "/" +
                             WideToUtf8(modelName_));
                StartEventThread();   // /event 长连接（250ms 轮询降为兜底）
                UpdateLabel();
                ::SetFocus(input_);
                busy_.store(false);
                UpdateBusyUi();
                ::SetTimer(hwnd_, kPollTimer, 250, nullptr);   // 首轮拉历史
                TryFlushExternalAsk();   // 排队的外部请求（批次 28）自动补发
            } else {
                AppendLine(Tr(L"panel.ai.connectfail"), AS_SYSTEM);
                UpdateLabel();
            }
            return 0;
        }
        case kPmsgConnectNotInstalled: {
            connecting_.store(false);
            serverOk_ = false;
            if (!notInstalledShown_) {
                notInstalledShown_ = true;
                AppendLine(Tr(L"panel.ai.notinstalled"), AS_SYSTEM);
            }
            UpdateLabel();
            return 0;
        }
        case kPmsgOaiConnected: {
            connecting_.store(false);
            bool ok = (wp == 1);
            auto* models = (std::vector<std::wstring>*)lp;
            if (ok) {
                serverOk_ = true;
                serverVersion_.clear();
                if (models && modelCombo_) {
                    ::SendMessageW(modelCombo_, CB_RESETCONTENT, 0, 0);
                    int sel = -1;
                    for (size_t i = 0; i < models->size(); ++i) {
                        int idx = (int)::SendMessageW(modelCombo_,
                            CB_ADDSTRING, 0,
                            (LPARAM)(*models)[i].c_str());
                        ::SendMessageW(modelCombo_, CB_SETITEMDATA,
                                       (WPARAM)idx, (LPARAM)i);
                        if ((*models)[i] == oaiModel_) sel = idx;
                    }
                    if (sel < 0 && !models->empty()) {
                        // 无偏好/偏好不在列表 → 首个，并回写偏好
                        oaiModel_ = models->front();
                        sel = 0;
                        ::SendMessageW(modelCombo_, CB_SETCURSEL, 0, 0);
                        if (onOaiModelChanged)
                            onOaiModelChanged(oaiModel_);
                    } else if (sel >= 0) {
                        ::SendMessageW(modelCombo_, CB_SETCURSEL,
                                       (WPARAM)sel, 0);
                    }
                }
                // 本地历史恢复：有 ai-history.json 则回放 transcript + 内存
                oaiHistory_.clear();
                {
                    std::string raw;
                    if (xfs::ReadFileBytes(OaiHistoryFilePath(), raw) &&
                        !raw.empty() &&
                        ai::OaiClient::ParseHistory(raw, &oaiHistory_)) {
                        SciSend(sci_, SCI_SETREADONLY, 0);
                        for (const auto& m : oaiHistory_) {
                            AppendLine(m.role == L"user"
                                           ? Tr(L"panel.ai.you")
                                           : Tr(L"panel.ai.ai"),
                                       m.role == L"user" ? AS_USER_TAG
                                                         : AS_AI_TAG);
                            AppendTextStyled(m.content + L"\n", AS_DEFAULT);
                        }
                        SciSend(sci_, SCI_SETREADONLY, 1);
                        Logger::Info("AiPanel: local history restored, " +
                                     std::to_string(oaiHistory_.size()) +
                                     " msg(s)");
                    } else {
                        oaiHistory_.clear();
                        AppendLine(
                            L"xfsWinPad AI (local/OpenAI-compatible)",
                            AS_SYSTEM);
                    }
                }
                renderedMsgs_ = 0;
                Logger::Info("AiPanel: OpenAI-compatible endpoint ready, " +
                             std::to_string(models ? (int)models->size() : 0) +
                             " models");
                UpdateLabel();
                ::SetFocus(input_);
                busy_.store(false);
                UpdateBusyUi();
                TryFlushExternalAsk();   // 排队的外部请求（批次 28）自动补发
            } else {
                serverOk_ = false;
                AppendLine(Tr(L"panel.ai.connectfail"), AS_SYSTEM);
                if (models && !models->empty()) {
                    // ListModels 成功返回 false 时不该带列表；此分支防御
                }
                UpdateLabel();
            }
            delete models;
            return 0;
        }
        case kPmsgOaiDelta: {
            // SSE 增量（工作线程逐块转交）：首个 delta 前打 AI: 标签，
            // 之后只 append 增量。Abort 后迟到的 delta 丢弃。
            if (lp && !oaiAbort_->load()) {
                auto* d = (std::wstring*)lp;
                if (!oaiStreaming_) {
                    AppendLine(Tr(L"panel.ai.ai"), AS_AI_TAG);
                    oaiStreaming_ = true;
                }
                AppendTextStyled(*d, AS_DEFAULT);
                delete d;
            } else if (lp) {
                delete (std::wstring*)lp;
            }
            return 0;
        }
        case kPmsgOaiReply: {
            if (lp) {
                auto* r = (OaiReply*)lp;
                if (!oaiAbort_->exchange(false)) {   // 用户已 Abort → 丢弃
                    if (!r->text.empty()) {
                        oaiHistory_.push_back({L"assistant", r->text});
                        if (oaiHistory_.size() > 40)
                            oaiHistory_.erase(oaiHistory_.begin(),
                                oaiHistory_.begin() + (oaiHistory_.size() - 40));
                        {
                            std::string hj = ai::OaiClient::HistoryToJson(
                                oaiHistory_);
                            xfs::WriteFileBytes(OaiHistoryFilePath(),
                                                hj.data(), hj.size());
                        }
                        // 流式路径：delta 已上屏，这里只收尾换行；
                        // 非流式回退（无 delta 却有全文）才补打全文。
                        if (!oaiStreaming_) {
                            AppendLine(Tr(L"panel.ai.ai"), AS_AI_TAG);
                            AppendTextStyled(r->text + L"\n", AS_DEFAULT);
                        } else {
                            AppendTextStyled(L"\n", AS_DEFAULT);
                        }
                        AppendLine(L"", AS_DEFAULT);
                    } else if (!r->err.empty()) {
                        SystemLine(Tr(L"panel.ai.oaierror") + r->err);
                    }
                }
                delete r;
            }
            oaiStreaming_ = false;
            busy_.store(false);
            ::KillTimer(hwnd_, kPollTimer);
            UpdateBusyUi();
            Logger::Info("AiPanel: local model response complete");
            TryFlushExternalAsk();   // 生成期间排队的外部请求（批次 28）补发
            return 0;
        }
        case kPmsgOcEvent: {
            // /event 触发器（批次 26）：本会话的 message/session 事件 →
            // 立即拉一次消息（比 250ms 轮询快 ~10 倍的出字延迟）。
            if (lp) {
                auto* e = (OcEvent*)lp;
                Logger::Debug("AiPanel: /event trigger type=" +
                              WideToUtf8(e->type));
                if (!e->sid.empty() && e->sid == sessionId_)
                    PollMessages();
                delete e;
            }
            return 0;
        }
        case kPmsgPromptFail: {
            // prompt_async 提交失败（服务端拒绝/不可达）——立刻解除「生成中」
            if (lp) {
                auto* r = (OaiReply*)lp;
                AppendLine(L"", AS_DEFAULT);
                SystemLine(Tr(L"panel.ai.sendfail") + r->err);
                delete r;
            }
            busy_.store(false);
            ::KillTimer(hwnd_, kPollTimer);
            UpdateBusyUi();
            return 0;
        }
        case kPmsgNewSession: {
            if (lp) {
                auto* sid = (std::wstring*)lp;
                if (!sid->empty()) {
                    sessionId_ = *sid;
                    // 清空 transcript 重新开始
                    SciSend(sci_, SCI_SETREADONLY, 0);
                    SciSend(sci_, SCI_CLEARALL);
                    SciSend(sci_, SCI_SETREADONLY, 1);
                    renderedMsgs_ = 0;
                    streamingMsgIdx_ = -1;
                    streamChars_ = 0;
                    pendingReplay_ = false;
                    busy_.store(false);
                    shownTools_.clear();
                    lastCtxSent_.clear();   // 新会话 → 下一条重新附带上文
                    permNoticeShown_ = false;
                    ::KillTimer(hwnd_, kPollTimer);
                    UpdateBusyUi();
                    Logger::Info("AiPanel: new session " + WideToUtf8(sessionId_));
                }
                delete sid;
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kPollTimer) { PollMessages(); return 0; }
            break;
        case WM_NCHITTEST: {
            POINT htpt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ::ScreenToClient(h, &htpt);
            if (htpt.x < 6) return HTCLIENT;   // 左缘 6px = 拖宽热区（视觉由 WM_PAINT 画）
            break;
        }
        case WM_SETCURSOR: {
            POINT pt; ::GetCursorPos(&pt);
            ::ScreenToClient(h, &pt);
            if (pt.x < 6) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x < 6) {
                resizing_ = true;
                POINT sp; ::GetCursorPos(&sp);
                dragStartScreenX_ = sp.x;
                RECT rc; ::GetWindowRect(h, &rc);
                dragStartW_ = rc.right - rc.left;
                ::SetCapture(h);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (resizing_) {
                POINT sp; ::GetCursorPos(&sp);
                // 面板停靠右缘：分割线（左缘）向左拖 = 面板变宽，
                // 向右拖 = 变窄（2026-09-07 用户反馈方向反了，delta 取负）
                int newW = dragStartW_ + (dragStartScreenX_ - sp.x);
                newW = std::max(240, std::min(1200, newW));
                if (onWidthChange) onWidthChange(newW);
                return 0;
            }
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool hot = pt.x < 6;
            if (hot) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
            }
            if (hot != splitHot_) {
                splitHot_ = hot;
                RECT rc; ::GetClientRect(h, &rc); rc.right = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (splitHot_) {
                splitHot_ = false;
                RECT rc; ::GetClientRect(h, &rc); rc.right = 6;
                ::InvalidateRect(h, &rc, FALSE);
            }
            break;
        case WM_LBUTTONUP:
            if (resizing_) { resizing_ = false; ::ReleaseCapture(); return 0; }
            break;
        case WM_CAPTURECHANGED:
            resizing_ = false;
            break;
        case WM_PAINT: {
            DefWindowProcW(h, msg, wp, lp);
            HDC dc = ::GetDC(h);
            RECT rc; ::GetClientRect(h, &rc);
            RECT band{0, 0, 5, rc.bottom};
            HBRUSH b = ::CreateSolidBrush(splitHot_ || resizing_
                ? RGB(0xCC, 0xE3, 0xFF) : RGB(0xE9, 0xE9, 0xE9));
            ::FillRect(dc, &band, b);
            ::DeleteObject(b);
            HPEN pen = ::CreatePen(PS_SOLID, 1, splitHot_ || resizing_
                ? RGB(0x00, 0x78, 0xD4) : RGB(0xAC, 0xAC, 0xAC));
            HPEN old = (HPEN)::SelectObject(dc, pen);
            ::MoveToEx(dc, 5, 0, nullptr);
            ::LineTo(dc, 5, rc.bottom);
            ::SelectObject(dc, old);
            ::DeleteObject(pen);
            ::ReleaseDC(h, dc);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_CLOSE && onClose) { onClose(); return 0; }
            if (id == ID_HIST && HIWORD(wp) == BN_CLICKED) {
                ShowHistoryPicker();
                return 0;
            }
            if (id == ID_MODEL && HIWORD(wp) == CBN_SELCHANGE) {
                int idx = (int)::SendMessageW(modelCombo_, CB_GETCURSEL, 0, 0);
                if (idx >= 0) {
                    if (backend_ == L"openai") {
                        wchar_t buf[256] = L"";
                        ::SendMessageW(modelCombo_, CB_GETLBTEXT,
                                       (WPARAM)idx, (LPARAM)buf);
                        oaiModel_ = buf;
                        Logger::Info("AiPanel: local model switched to " +
                                     WideToUtf8(oaiModel_));
                        if (onOaiModelChanged) onOaiModelChanged(oaiModel_);
                        return 0;
                    }
                    size_t i = (size_t)::SendMessageW(modelCombo_,
                        CB_GETITEMDATA, (WPARAM)idx, 0);
                    if (i < models_.size()) {
                        modelProvider_ = models_[i].provider;
                        modelName_ = models_[i].modelId;
                        Logger::Info("AiPanel: model switched to " +
                                     WideToUtf8(modelProvider_) + "/" +
                                     WideToUtf8(modelName_));
                        if (onModelChanged)
                            onModelChanged(modelProvider_, modelName_);
                    }
                }
                return 0;
            }
            if (id == ID_SEND) {
                if (busy_.load()) {
                    // Abort：中断生成（openai 模式：标记丢弃迟到回复）
                    if (backend_ == L"openai") oaiAbort_->store(true);
                    else ai::OcClient::Abort(sessionId_);
                    busy_.store(false);
                    ::KillTimer(hwnd_, kPollTimer);
                    UpdateBusyUi();
                } else {
                    int len = ::GetWindowTextLengthW(input_);
                    std::wstring text(len + 1, L'\0');
                    ::GetWindowTextW(input_, &text[0], len + 1);
                    text.resize(len);
                    while (!text.empty() &&
                           (text.back() == L'\r' || text.back() == L'\n'))
                        text.pop_back();
                    ::SetWindowTextW(input_, L"");
                    SendUserText(text);
                }
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            return (LRESULT)::GetSysColorBrush(COLOR_BTNFACE);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK AiPanel::WndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* self = (AiPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->WndProc(h, m, wp, lp) : ::DefWindowProcW(h, m, wp, lp);
}

} // namespace xfs
