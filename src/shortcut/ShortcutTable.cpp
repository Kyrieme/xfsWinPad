// ShortcutTable.cpp — 默认表 + 组合串编解码 + 仅覆盖项 JSON 持久化。
// JSON 格式（扁平、手改友好）：{"100": "Ctrl+N", "6001": "", ...}
//   键 = 十进制命令 id；值 = 组合串，"" = 显式删除默认键。
#include "ShortcutTable.h"
#include "../core/CommandIds.h"
#include "../core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdlib>
#include <map>

namespace xfs {

// ---- 组合串编解码 -----------------------------------------------------------

namespace {

struct VkName { unsigned int vk; const wchar_t* name; };
// 高频命名键（字母/数字/F1-F24 走算法，不进表）。
constexpr VkName kNamed[] = {
    {VK_TAB, L"Tab"}, {VK_RETURN, L"Enter"}, {VK_ESCAPE, L"Esc"},
    {VK_SPACE, L"Space"}, {VK_BACK, L"Backspace"}, {VK_DELETE, L"Del"},
    {VK_INSERT, L"Ins"}, {VK_HOME, L"Home"}, {VK_END, L"End"},
    {VK_PRIOR, L"PgUp"}, {VK_NEXT, L"PgDn"},
    {VK_LEFT, L"Left"}, {VK_UP, L"Up"}, {VK_RIGHT, L"Right"}, {VK_DOWN, L"Down"},
    {VK_ADD, L"NumAdd"}, {VK_SUBTRACT, L"NumSub"}, {VK_MULTIPLY, L"NumMul"},
    {VK_DIVIDE, L"NumDiv"}, {VK_DECIMAL, L"NumDot"},
};

bool NameForVk(unsigned int vk, std::wstring* out) {
    if (vk >= 'A' && vk <= 'Z') { out->assign(1, wchar_t(vk)); return true; }
    if (vk >= '0' && vk <= '9') { out->assign(1, wchar_t(vk)); return true; }
    if (vk >= VK_F1 && vk <= VK_F24) {
        *out = L"F" + std::to_wstring(vk - VK_F1 + 1); return true;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        *out = L"Num" + std::to_wstring(vk - VK_NUMPAD0); return true;
    }
    for (const auto& n : kNamed)
        if (n.vk == vk) { *out = n.name; return true; }
    return false;
}

bool VkFromName(const std::wstring& s, unsigned int* out) {
    if (s.size() == 1) {
        wchar_t c = s[0];
        if (c >= L'A' && c <= L'Z') { *out = (unsigned)c; return true; }
        if (c >= L'a' && c <= L'z') { *out = (unsigned)(c - L'a' + L'A'); return true; }
        if (c >= L'0' && c <= L'9') { *out = (unsigned)c; return true; }
        return false;
    }
    if (s.size() >= 2 && (s[0] == L'F' || s[0] == L'f')) {
        int n = _wtoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) { *out = (unsigned)(VK_F1 + n - 1); return true; }
        return false;
    }
    if (s.size() == 4 && _wcsnicmp(s.c_str(), L"Num", 3) == 0) {
        wchar_t c = s[3];
        if (c >= L'0' && c <= L'9') { *out = (unsigned)(VK_NUMPAD0 + (c - L'0')); return true; }
        static const std::pair<const wchar_t*, unsigned int> extras[] = {
            {L"Add", VK_ADD}, {L"Sub", VK_SUBTRACT}, {L"Mul", VK_MULTIPLY},
            {L"Div", VK_DIVIDE}, {L"Dot", VK_DECIMAL},
        };
        for (const auto& e : extras)
            if (_wcsicmp(s.c_str() + 3, e.first) == 0) { *out = e.second; return true; }
        return false;
    }
    for (const auto& n : kNamed)
        if (_wcsicmp(s.c_str(), n.name) == 0) { *out = n.vk; return true; }
    // 十进制 VK 兜底
    if (s.find_first_not_of(L"0123456789") == std::wstring::npos) {
        long v = wcstol(s.c_str(), nullptr, 10);
        if (v > 0 && v < 256) { *out = (unsigned)v; return true; }
    }
    return false;
}

} // namespace

std::wstring ShortcutInfo::ToString() const {
    if (!Valid()) return L"";
    std::wstring s;
    if (ctrl)  s += L"Ctrl+";
    if (alt)   s += L"Alt+";
    if (shift) s += L"Shift+";
    std::wstring name;
    if (NameForVk(vk, &name)) s += name;
    else s += std::to_wstring(vk);
    return s;
}

ShortcutInfo ShortcutInfo::FromString(const std::wstring& combo) {
    ShortcutInfo r;
    size_t start = 0;
    // 空串 = 显式删除（合法、Valid()==false）
    if (combo.empty()) return r;
    for (;;) {
        size_t plus = combo.find(L'+', start);
        std::wstring tok = combo.substr(start, plus == std::wstring::npos
                                                ? std::wstring::npos : plus - start);
        if (tok.empty()) return ShortcutInfo{};
        if (_wcsicmp(tok.c_str(), L"Ctrl") == 0)  r.ctrl = true;
        else if (_wcsicmp(tok.c_str(), L"Alt") == 0)  r.alt = true;
        else if (_wcsicmp(tok.c_str(), L"Shift") == 0) r.shift = true;
        else {
            unsigned int vk = 0;
            if (!VkFromName(tok, &vk)) return ShortcutInfo{};   // 未知键名 → 无效
            r.vk = vk;   // 主键必须是最后一个 token
            if (plus != std::wstring::npos) return ShortcutInfo{};
            break;
        }
        if (plus == std::wstring::npos) return ShortcutInfo{};   // 只有修饰键
        start = plus + 1;
    }
    return r;
}

// ---- 默认表（对齐历史 BuildAccelerators 硬编码） ------------------------------

const std::vector<std::pair<unsigned int, ShortcutInfo>>&
ShortcutTable::Defaults() {
    static const std::vector<std::pair<unsigned int, ShortcutInfo>> d = [] {
        auto mk = [](unsigned int cmd, bool ctrl, bool alt, bool shift, unsigned vk) {
            ShortcutInfo i; i.ctrl = ctrl; i.alt = alt; i.shift = shift; i.vk = vk;
            return std::make_pair(cmd, i);
        };
        return std::vector<std::pair<unsigned int, ShortcutInfo>>{
            mk(Cmd::FileNew, true, false, false, 'N'),
            mk(Cmd::FileOpen, true, false, false, 'O'),
            mk(Cmd::FileOpenFolder, true, false, true, 'O'),
            mk(Cmd::FileSave, true, false, false, 'S'),
            mk(Cmd::FileSaveAs, true, false, true, 'S'),
            mk(Cmd::FileClose, true, false, false, 'W'),
            mk(Cmd::SearchFind, true, false, false, 'F'),
            mk(Cmd::SearchReplace, true, false, false, 'H'),
            mk(Cmd::SearchFindNext, false, false, false, VK_F3),
            mk(Cmd::SearchFindPrev, false, false, true, VK_F3),
            mk(Cmd::BookmarkToggle, true, false, false, VK_F2),
            mk(Cmd::BookmarkNext, false, false, false, VK_F2),
            mk(Cmd::BookmarkPrev, false, false, true, VK_F2),
            mk(Cmd::SearchGotoLine, true, false, false, 'G'),
            mk(Cmd::PaletteShow, true, false, true, 'P'),
            mk(Cmd::EditTimeDate, false, false, false, VK_F5),
            // Scintilla 自处理的标准编辑键也登记进表（菜单尾注/tooltip 单一来源；
            // ACCEL 与 Scintilla 内建处理同效，不产生双触发）
            mk(Cmd::EditUndo, true, false, false, 'Z'),
            mk(Cmd::EditRedo, true, false, false, 'Y'),
            mk(Cmd::EditCut, true, false, false, 'X'),
            mk(Cmd::EditCopy, true, false, false, 'C'),
            mk(Cmd::EditPaste, true, false, false, 'V'),
            mk(Cmd::ReopenClosedTab, true, false, true, 'T'),   // Ctrl+Shift+T
            mk(6001, true, false, false, VK_TAB),          // Ctrl+Tab 下一标签
            mk(6002, true, false, true, VK_TAB),           // 上一标签
            mk(Cmd::ViewZoomIn, true, false, false, VK_ADD),
            mk(Cmd::ViewZoomOut, true, false, false, VK_SUBTRACT),
            mk(Cmd::ViewZoomReset, true, false, false, VK_NUMPAD0),
            mk(Cmd::ViewHexView, true, true, false, 'H'),
            mk(Cmd::ViewLogPanel, true, true, false, 'L'),
            mk(Cmd::ViewTerminal, true, true, false, 'T'),
            mk(Cmd::ViewAiPanel, true, true, false, 'A'),
            mk(Cmd::MacroStart, true, false, true, 'R'),
            mk(Cmd::MacroPlayback, true, false, true, 'M'),
            // 批次 29 行变换
            mk(Cmd::EditToggleComment, true, false, false, 'Q'),   // Ctrl+Q
            mk(Cmd::EditJoinLines, true, false, false, 'J'),       // Ctrl+J
            mk(Cmd::EditSplitLines, true, false, false, 'I'),      // Ctrl+I
        };
    }();
    return d;
}

// ---- 查询 / 修改 --------------------------------------------------------------

ShortcutInfo ShortcutTable::Get(unsigned cmdId) const {
    for (const auto& o : overrides_)
        if (o.first == cmdId) return o.second;    // 含显式删除（vk=0）
    for (const auto& d : Defaults())
        if (d.first == cmdId) return d.second;
    return ShortcutInfo{};
}

unsigned ShortcutTable::FindByCombo(const ShortcutInfo& info) const {
    if (!info.Valid()) return 0;
    for (const auto& d : Defaults())
        if (d.second.SameAs(info)) return d.first;
    for (const auto& o : overrides_)
        if (o.second.Valid() && o.second.SameAs(info)) return o.first;
    return 0;
}

bool ShortcutTable::SetShortcut(unsigned cmdId, const ShortcutInfo& info,
                                unsigned* conflictCmd) {
    if (conflictCmd) *conflictCmd = 0;
    if (info.Valid()) {
        unsigned other = FindByCombo(info);
        if (other != 0 && other != cmdId) {
            if (conflictCmd) *conflictCmd = other;
            return false;
        }
    }
    for (auto& o : overrides_)
        if (o.first == cmdId) { o.second = info; return true; }
    overrides_.emplace_back(cmdId, info);
    return true;
}

bool ShortcutTable::ClearOverride(unsigned cmdId) {
    for (size_t i = 0; i < overrides_.size(); ++i)
        if (overrides_[i].first == cmdId) {
            overrides_.erase(overrides_.begin() + i);
            return true;
        }
    return false;
}

std::vector<std::pair<unsigned int, ShortcutInfo>> ShortcutTable::All() const {
    std::vector<std::pair<unsigned int, ShortcutInfo>> out;
    for (const auto& d : Defaults()) {
        if (DisplayOnly(d.first)) continue;   // 仅展示（Scintilla 自处理，非真加速器）
        ShortcutInfo eff = Get(d.first);
        if (eff.Valid()) out.emplace_back(d.first, eff);
    }
    for (const auto& o : overrides_) {
        if (!o.second.Valid()) continue;
        bool inDefaults = false;
        for (const auto& d : Defaults())
            if (d.first == o.first) { inDefaults = true; break; }
        if (!inDefaults) out.emplace_back(o.first, o.second);
    }
    return out;
}

std::vector<std::pair<unsigned int, ShortcutInfo>> ShortcutTable::Everything() const {
    std::vector<std::pair<unsigned int, ShortcutInfo>> out;
    for (const auto& d : Defaults()) {
        // 仅展示项以默认值参与（用户可覆盖/解绑它们，ACCEL 构建侧再过滤）
        ShortcutInfo eff = Get(d.first);
        bool hasOverride = false;
        for (const auto& o : overrides_)
            if (o.first == d.first) { hasOverride = true; break; }
        // 显式解绑（vk=0）的命令也必须出现在列表里，否则无法再恢复默认
        if (eff.Valid() || hasOverride || DisplayOnly(d.first))
            out.emplace_back(d.first, eff.Valid() ? eff : d.second);
    }
    for (const auto& o : overrides_) {
        if (!o.second.Valid()) continue;
        bool inDefaults = false;
        for (const auto& d : Defaults())
            if (d.first == o.first) { inDefaults = true; break; }
        if (!inDefaults) out.emplace_back(o.first, o.second);
    }
    return out;
}

// Scintilla 自处理的标准编辑键：登记进表只为菜单尾注/tooltip 单一来源，
// 不进 ACCEL 表（否则终端/对话框聚焦时 Ctrl+C 会错误作用到编辑器）。
bool ShortcutTable::DisplayOnly(unsigned cmdId) {
    switch (cmdId) {
        case Cmd::EditUndo: case Cmd::EditRedo:
        case Cmd::EditCut:  case Cmd::EditCopy: case Cmd::EditPaste:
            return true;
    }
    return false;
}

// ---- 持久化（扁平 JSON，仅覆盖项） ---------------------------------------------

const wchar_t* ShortcutTable::FilePath() {
    wchar_t* appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr,
                                         &appData))) {
        base = appData;
        ::CoTaskMemFree(appData);
    }
    ::CreateDirectoryW((base + L"\\xfsWinPad").c_str(), nullptr);
    static const std::wstring path = base + L"\\xfsWinPad\\shortcuts.json";
    return path.c_str();
}

bool ShortcutTable::Load(const std::wstring& path) {
    overrides_.clear();
    std::string raw;
    if (!ReadFileBytes(path, raw)) return false;   // 缺失/不可读 = 默认表
    std::wstring j = Utf8ToWide(raw);
    if (!j.empty() && j[0] == 0xFEFF) j.erase(0, 1);

    // 顶层必须是能闭合的对象（损坏文件直接判失败，保留默认表）。
    {
        size_t b = j.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos || j[b] != L'{') return false;
        if (j.find(L'}', b) == std::wstring::npos) return false;
    }

    // 极简解析：仅在顶层对象里匹配 "digits" : "combo" 对。
    size_t i = 0;
    const size_t n = j.size();
    while (i < n) {
        if (j[i] != L'"') { ++i; continue; }
        size_t k1 = i + 1;
        size_t k2 = j.find(L'"', k1);
        if (k2 == std::wstring::npos) break;
        std::wstring key = j.substr(k1, k2 - k1);
        i = k2 + 1;
        size_t colon = j.find(L':', i);
        if (colon == std::wstring::npos) break;
        size_t v1 = j.find(L'"', colon);
        if (v1 == std::wstring::npos) break;
        size_t v2 = j.find(L'"', v1 + 1);
        if (v2 == std::wstring::npos) break;
        // 键必须是纯数字，否则不属于本表（容错跳过）
        if (key.find_first_not_of(L"0123456789") != std::wstring::npos) {
            i = v2 + 1;
            continue;
        }
        ShortcutInfo info = ShortcutInfo::FromString(j.substr(v1 + 1, v2 - v1 - 1));
        unsigned cmd = (unsigned)wcstoul(key.c_str(), nullptr, 10);
        overrides_.emplace_back(cmd, info);   // 含空串=删除项
        i = v2 + 1;
    }
    return true;
}

bool ShortcutTable::Save(const std::wstring& path) const {
    std::wstring j = L"{\r\n";
    bool first = true;
    for (const auto& o : overrides_) {
        if (!first) j += L",\r\n";
        first = false;
        j += L"  \"" + std::to_wstring(o.first) + L"\": \"" +
             o.second.ToString() + L"\"";
    }
    j += (first ? L"}\r\n" : L"\r\n}\r\n");
    std::string utf8 = WideToUtf8(j);
    return WriteFileBytes(path, utf8.data(), utf8.size());
}

ShortcutTable& GlobalShortcuts() {
    static ShortcutTable t;
    return t;
}

} // namespace xfs
