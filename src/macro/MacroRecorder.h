#pragma once
// xfsWinPad - Macro: record / playback editor keystrokes.
//
// Events are recorded at the EditorKeyProc subclass level (character input,
// Enter, Tab, Backspace, Delete) plus menu commands routed through
// ExecuteCommand while recording. Playback replays them against the active
// document's Scintilla control in order. Macros persist as simple UTF-8
// text files (one event per line) under %APPDATA%\xfsWinPad\macros\.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

namespace xfs {

struct MacroEvent {
    enum class Type {
        Char,        // wp = wchar_t character (text input)
        Key,         // wp = VK_ code (VK_RETURN/VK_TAB/VK_BACK/VK_DELETE)
        Command,     // wp = Cmd id executed from the menu/accelerator
    };
    Type type = Type::Char;
    unsigned int code = 0;   // char value, VK code or command id
};

class MacroRecorder {
public:
    bool Recording() const { return recording_; }

    void Start();
    void Stop();
    void Clear();

    void RecordChar(wchar_t ch);
    void RecordKey(unsigned int vk);
    void RecordCommand(unsigned int cmdId);

    // Replays the recorded events on the current active editor.
    // onCommand is invoked for each recorded command so MainWindow keeps a
    // single dispatch path. Returns number of events played.
    int Playback(std::function<bool(unsigned int)> onCommand, HWND editorHwnd) const;

    bool Empty() const { return events_.empty(); }
    size_t Count() const { return events_.size(); }
    const std::vector<MacroEvent>& Events() const { return events_; }

    // persistence ------------------------------------------------------
    std::wstring Serialize() const;
    bool LoadSerialized(const std::wstring& text);

private:
    bool recording_ = false;
    std::vector<MacroEvent> events_;
};

} // namespace xfs