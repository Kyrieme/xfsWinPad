#include "MacroRecorder.h"
#include "../core/Log.h"
#include <Scintilla.h>

#include <sstream>

namespace xfs {

void MacroRecorder::Start() {
    events_.clear();
    recording_ = true;
    Logger::Info("Macro: recording started");
}

void MacroRecorder::Stop() {
    recording_ = false;
    Logger::Info("Macro: recording stopped, events=" +
                 std::to_string(events_.size()));
}

void MacroRecorder::Clear() {
    events_.clear();
    recording_ = false;
}

void MacroRecorder::RecordChar(wchar_t ch) {
    if (!recording_) return;
    events_.push_back({MacroEvent::Type::Char, (unsigned int)ch});
}

void MacroRecorder::RecordKey(unsigned int vk) {
    if (!recording_) return;
    events_.push_back({MacroEvent::Type::Key, vk});
}

void MacroRecorder::RecordCommand(unsigned int cmdId) {
    if (!recording_) return;
    events_.push_back({MacroEvent::Type::Command, cmdId});
}

int MacroRecorder::Playback(std::function<bool(unsigned int)> onCommand,
                            HWND editorHwnd) const {
    if (events_.empty()) return 0;
    int played = 0;
    for (const auto& ev : events_) {
        switch (ev.type) {
            case MacroEvent::Type::Char: {
                wchar_t ch = (wchar_t)ev.code;
                // convert to UTF-8 and insert at caret (Scintilla is UTF-8)
                char utf8[8] = {};
                int n = ::WideCharToMultiByte(CP_UTF8, 0, &ch, 1,
                                              utf8, sizeof(utf8), nullptr, nullptr);
                if (n > 0) {
                    ::SendMessageW(editorHwnd, SCI_REPLACESEL, 0, (LPARAM)utf8);
                    Logger::Debug(std::string("Macro replay char: ") + utf8);
                }
                ++played;
                break;
            }
            case MacroEvent::Type::Key:
                switch (ev.code) {
                    case VK_RETURN: {
                        // newline consistent with document EOL; LF is fine -
                        // Scintilla normalizes per EOL mode on its own? It
                        // does not, so query the EOL mode.
                        LONG eol = (LONG)::SendMessageW(editorHwnd,
                                                        SCI_GETEOLMODE, 0, 0);
                        const char* nl = "\r\n";
                        if (eol == SC_EOL_CR) nl = "\r";
                        else if (eol == SC_EOL_LF) nl = "\n";
                        ::SendMessageW(editorHwnd, SCI_REPLACESEL, 0, (LPARAM)nl);
                        break;
                    }
                    case VK_TAB:
                        ::SendMessageW(editorHwnd, SCI_TAB, 0, 0);
                        break;
                    case VK_BACK:
                        ::SendMessageW(editorHwnd, SCI_DELETEBACK, 0, 0);
                        break;
                    case VK_DELETE:
                        ::SendMessageW(editorHwnd, SCI_CLEAR, 0, 0);
                        break;
                    default:
                        break;   // unsupported key: skip
                }
                ++played;
                break;
            case MacroEvent::Type::Command:
                if (onCommand && onCommand(ev.code)) ++played;
                break;
        }
    }
    Logger::Info("Macro: played " + std::to_string(played) + "/" +
                 std::to_string(events_.size()) + " events");
    return played;
}

std::wstring MacroRecorder::Serialize() const {
    // Format: one event per line "T code" where T = C(har)/K(ey)/M(acro cmd)
    std::wstring out;
    for (const auto& ev : events_) {
        wchar_t prefix = ev.type == MacroEvent::Type::Char ? L'C'
                       : ev.type == MacroEvent::Type::Key ? L'K' : L'M';
        out += prefix;
        out += L' ';
        out += std::to_wstring(ev.code);
        out += L'\n';
    }
    return out;
}

bool MacroRecorder::LoadSerialized(const std::wstring& text) {
    std::vector<MacroEvent> parsed;
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (line.size() < 3 || line[1] != L' ') continue;
        MacroEvent ev;
        wchar_t p = line[0];
        try {
            ev.code = (unsigned int)std::stoul(line.substr(2));
        } catch (...) { continue; }
        if (p == L'C') ev.type = MacroEvent::Type::Char;
        else if (p == L'K') ev.type = MacroEvent::Type::Key;
        else if (p == L'M') ev.type = MacroEvent::Type::Command;
        else continue;
        parsed.push_back(ev);
    }
    if (parsed.empty()) return false;
    events_ = std::move(parsed);
    recording_ = false;
    return true;
}

} // namespace xfs