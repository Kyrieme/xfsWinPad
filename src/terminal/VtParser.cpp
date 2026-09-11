#include "VtParser.h"

namespace xfs {
namespace vt {

namespace {
enum class State { Text, Esc, Csi, CsiEsc, Osc };
constexpr int kAnsiCol[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

// CUP/HVP ('H'/'f') carry "row;col", CHA ('G') carries "col" only. Returns
// true when the sequence positions the cursor at column 1 — i.e. the start of
// a fresh line in our linear buffer. cmd.exe writes each new prompt by moving
// to column 1 (e.g. ESC[6;1H) instead of emitting a line feed, so we translate
// that into a newline here.
bool CsiToCol1(const std::string& param, char final) {
    std::string colStr;
    if (final == 'G') {
        colStr = param;
    } else {
        size_t semi = param.find(';');
        colStr = (semi == std::string::npos) ? std::string() : param.substr(semi + 1);
    }
    if (colStr.empty()) return true;   // default column is 1
    int col = 0;
    for (char c : colStr) {
        if (c < '0' || c > '9') return false;
        col = col * 10 + (c - '0');
        if (col > 1) return false;      // early exit for large columns
    }
    return true;
}
} // namespace

void ApplySgr(const std::string& param, VtStyle& st);

// Appends a UTF-8 byte with the current style. Style vectors are grown to match
// text byte-for-byte so Scintilla colouring can index them directly.
static void Push(char c, const VtStyle& s, std::string& text, std::vector<VtStyle>& styles) {
    text += c;
    styles.push_back(s);
}

// We keep GLOBAL state across calls because ConPTY may deliver an escape
// sequence split mid-way across a read chunk. ConPTY delivers line-oriented
// output so in practice a sequence is rarely split, but carrying the state is
// cheap and correct. A single process has one terminal panel, so globals are
// fine.
namespace {
std::string g_pending;            // buffered CSI param text across chunk boundary
VtStyle g_cur;
State g_reg = State::Text;        // resume state for a split sequence
bool g_inSeq = false;             // true if the chunk ended inside a sequence
}

void Parse(const std::string& data, std::string& outText,
           std::vector<VtStyle>& outStyles, int& lineStart, int base) {
    std::string text;
    std::vector<VtStyle> styles;

    State state = g_inSeq ? g_reg : State::Text;

    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char c = (unsigned char)data[i];
        switch (state) {
            case State::Text:
                if (c == 0x1B) { state = State::Esc; break; }
                if (c == '\n') {
                    // Line feed: append newline, reset overwrite position
                    Push((char)c, g_cur, text, styles);
                    lineStart = -1;
                    break;
                }
                if (c == '\r') {
                    // Carriage return: mark the ABSOLUTE position of the
                    // cursor (base + current offset) as the overwrite point.
                    // Subsequent text replaces from here. A following \n (in
                    // this or the next chunk) resets it, turning the pair into
                    // a plain CRLF line ending.
                    lineStart = base + (int)text.size();
                    break;
                }
                if (c == '\t') { Push('\t', g_cur, text, styles); break; }
                if (c < 0x20 || c == 0x7F) break;   // swallow other control
                Push((char)c, g_cur, text, styles);
                break;
            case State::Esc:
                if (c == '[') { state = State::Csi; g_pending.clear(); break; }
                if (c == ']') { state = State::Osc; break; }
                // two-byte ESC sequences (DECSC etc.) we don't model
                state = State::Text;
                break;
            case State::Csi:
                if (c >= '0' && c <= '9') { g_pending += (char)c; break; }
                if (c == ';' || c == ':') { g_pending += ';'; break; }
                if (c >= '@' && c <= '~') {
                    // final byte
                    if (c == 'm') {
                        ApplySgr(g_pending, g_cur);
                    } else if ((c == 'H' || c == 'f' || c == 'G') && CsiToCol1(g_pending, c)) {
                        // Cursor moved to column 1: start a fresh line. cmd
                        // writes its prompt via CUP-to-column-1 (no \n), so
                        // without this the prompt would glue to the previous
                        // output (e.g. "helloD:\...>").
                        Push('\n', g_cur, text, styles);
                        lineStart = -1;
                    }
                    g_pending.clear();
                    state = State::Text;
                    break;
                }
                if (c >= 0x20 && c < 0x40) break;   // private/intermediate ignore
                state = State::Text;
                break;
            case State::CsiEsc:
                // not used; kept for symmetry
                state = State::Text;
                break;
            case State::Osc:
                if (c == 0x07) { state = State::Text; break; }
                if (c == 0x1B) { state = State::Text; break; }   // ST end (2-byte)
                break;
        }
    }

    outText.append(text);
    outStyles.insert(outStyles.end(), styles.begin(), styles.end());

    // remember where we stopped so a sequence split across chunks continues
    g_reg = state;
    g_inSeq = (state != State::Text);
}

void ApplySgr(const std::string& param, VtStyle& st) {
    if (param.empty()) { st = VtStyle{}; return; }   // ESC[0m reset

    // split on ';'
    std::vector<std::string> parts;
    std::string cur;
    for (char c : param) {
        if (c == ';') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);

    for (const std::string& p : parts) {
        if (p.empty()) continue;
        int n = 0; bool ok = true;
        for (char c : p) { if (c < '0' || c > '9') { ok = false; break; } n = n * 10 + (c - '0'); }
        if (!ok) continue;
        if (n == 0) st = VtStyle{};
        else if (n == 1) st.bold = true;
        else if (n == 22) st.bold = false;
        else if (n >= 30 && n <= 37) st.fg = kAnsiCol[n - 30];
        else if (n == 39) st.fg = -1;
        else if (n >= 90 && n <= 97) st.fg = kAnsiCol[n - 90] + 8;
        else if (n >= 40 && n <= 47) st.bg = kAnsiCol[n - 40];
        else if (n == 49) st.bg = -1;
        else if (n >= 100 && n <= 107) st.bg = kAnsiCol[n - 100] + 8;
        // 38/48;5;n and rgb ignored for now
    }
}

} // namespace vt
} // namespace xfs
