#pragma once
// xfsWinPad - minimal VT100/ANSI parser for ConPTY output.
//
// The parser converts ConPTY's byte stream into styled UTF-8 text suitable for
// appending to a read-only Scintilla buffer. It handles SGR colour attributes,
// swallows unsupported sequences, and tracks \r (carriage return) by recording
// the current line start so the caller can overwrite from column 0.

#include <string>
#include <vector>

namespace xfs {

struct VtStyle {
    int fg = -1;   // -1 = default (theme), else ANSI 16-color palette 0..15
    int bg = -1;   // -1 = default, else ANSI 0..15
    bool bold = false;
};

namespace vt {

// Appends styled UTF-8 text produced from `data` into outText / outStyles.
// `base` is the absolute position in the terminal buffer where this chunk's
// output begins (i.e. the buffer length before this chunk is appended).
// When the parser sees a bare \r it sets lineStart to the absolute position
// (base + current offset) that subsequent text should overwrite from.
// When \n is seen, a newline is appended and lineStart is reset to -1.
void Parse(const std::string& data, std::string& outText,
           std::vector<VtStyle>& outStyles, int& lineStart, int base = 0);

} // namespace vt
} // namespace xfs
