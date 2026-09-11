// test_terminal.cpp - unit tests for the terminal VT parser and the ConPTY host.
#include "../src/terminal/VtParser.h"
#include "../src/terminal/ConPTY.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// Helper: parse with default lineStart (no overwrite tracking needed for tests)
static void ParseNoCR(const std::string& data, std::string& out,
                       std::vector<VtStyle>& st) {
    int ls = -1;
    vt::Parse(data, out, st, ls);
}

// ------------------------------------------------------------------ VtParser
static void TestPlainText() {
    std::string data("hello\r\nworld\n");
    std::string out;
    std::vector<VtStyle> st;
    int ls = -1;
    vt::Parse(data, out, st, ls);
    CHECK(out == "hello\nworld\n");          // \r consumed, \n kept
    CHECK(out.size() == st.size());          // one style per byte
    for (const auto& s : st) CHECK(s.fg == -1 && !s.bold);  // default style
}

static void TestSgrColors() {
    // "hi" red, reset, "bye"
    std::string data("\x1B[31mhi\x1B[0mbye");
    std::string out;
    std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(out == "hibye");
    CHECK(st.size() == 5);
    CHECK(st[0].fg == 1);   // red = palette index 1
    CHECK(st[1].fg == 1);
    CHECK(st[2].fg == -1);  // after reset
    CHECK(st[3].fg == -1);
}

static void TestSgrBoldBoth() {
    // bold + cyan
    std::string data("\x1B[1;36mX");
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(out == "X");
    CHECK(st[0].bold && st[0].fg == 6);
}

static void TestBrightFg() {
    // bright green (90..97 range)
    std::string data("\x1B[92mQ");
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(st[0].fg == 10);   // 2 + 8
}

static void TestBg() {
    std::string data("\x1B[44mK");
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(st[0].bg == 4);    // blue bg
}

static void TestUnsupportedSequencesSwallowed() {
    // cursor move (CSI A), clear line, erase - should produce no visible text
    std::string data("\x1B[1A\x1B[2K\x1B[0JAB");
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(out == "AB");
}

static void TestOscSwallowed() {
    std::string data("\x1B]0;title\x07" "after");
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(out == "after");
}

static void TestEscUnknown() {
    // unrecognized 2-byte escape (e.g. ESC 7 = DECSC) -> consumed, text continues
    std::string data = "\x1B" "7FLAG";
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(out == "FLAG");
}

static void TestSplitSequenceAcrossChunks() {
    // sequence split mid-way: chunk1 = up to "...31", chunk2 = "mhi"
    std::string a("\x1B[3"), b("1mhi");
    std::string out; std::vector<VtStyle> st;
    int ls = -1;
    vt::Parse(a, out, st, ls);
    vt::Parse(b, out, st, ls);
    CHECK(out == "hi");
    CHECK(st.size() == 2 && st[0].fg == 1 && st[1].fg == 1);
}

static void TestUtf8Passthrough() {
    std::string data("\xE4\xBD\xA0\xE5\xA5\xBD\n");
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(out == data);                       // bytes preserved
    CHECK(out.size() == st.size());           // per-byte styles line up
}

static void TestCupToCol1Newline() {
    // cmd writes a new prompt via CUP-to-column-1 (no \n): ESC[6;1H
    std::string data("hello\x1B[6;1HD:\\dir>");
    std::string out; std::vector<VtStyle> st;
    ParseNoCR(data, out, st);
    CHECK(out == "hello\nD:\\dir>");
    // empty CUP (defaults 1;1) also starts a fresh line
    std::string data2("a\x1B[Hb");
    std::string out2; std::vector<VtStyle> st2;
    ParseNoCR(data2, out2, st2);
    CHECK(out2 == "a\nb");
    // CHA 'G' to column 1
    std::string data3("a\x1B[1Gb");
    std::string out3; std::vector<VtStyle> st3;
    ParseNoCR(data3, out3, st3);
    CHECK(out3 == "a\nb");
    // CUP to a non-first column is swallowed (not modelled yet)
    std::string data4("a\x1B[1;5Hb");
    std::string out4; std::vector<VtStyle> st4;
    ParseNoCR(data4, out4, st4);
    CHECK(out4 == "ab");
}

// ------------------------------------------------- cross-chunk append model
// Replicates TerminalPanel::AppendPty (overwrite from an absolute \r position,
// else append), so we can verify the parser + append boundary end-to-end.
static void SimAppend(std::string& buf, const std::string& text, int& lineStart) {
    if (text.empty()) return;
    size_t bufLen = buf.size();
    if (lineStart >= 0 && (size_t)lineStart < bufLen) {
        buf.replace((size_t)lineStart, bufLen - lineStart, text);
        lineStart = -1;
    } else {
        buf += text;
        lineStart = -1;
    }
}

// Feed one ConPTY chunk exactly as TerminalPanel does (base = current length).
static void FeedChunk(std::string& buf, const std::string& chunk, int& lineStart) {
    std::string text; std::vector<VtStyle> st;
    int base = (int)buf.size();
    vt::Parse(chunk, text, st, lineStart, base);
    SimAppend(buf, text, lineStart);
}

static void TestCrlfSplitAcrossChunks() {
    // A CRLF that ConPTY splits between two reads must become one \n, and the
    // following text must not overwrite from a wrong (old relative) position.
    std::string buf; int ls = -1;
    FeedChunk(buf, "abc\r", ls);
    FeedChunk(buf, "\n", ls);
    CHECK(buf == "abc\n");
    // same split, next chunk carries the next line's text
    std::string buf2; int ls2 = -1;
    FeedChunk(buf2, "line1\r", ls2);
    FeedChunk(buf2, "\nline2\n", ls2);
    CHECK(buf2 == "line1\nline2\n");
}

static void TestCrAtEndOfBufferThenText() {
    // A bare \r landing exactly at the buffer end in one chunk: the next
    // chunk's text appends cleanly (no truncation of prior content).
    std::string buf; int ls = -1;
    FeedChunk(buf, "aaaa\r", ls);
    FeedChunk(buf, "b", ls);
    CHECK(buf == "aaaab");
}

// ---------------------------------------------------------------- ConPTY host
static void TestConptyStartStop() {
    ConPTY pty;
    // If ConPTY is unavailable on this machine, this must fail cleanly (not crash).
    if (!pty.Start(L"cmd.exe /C echo xfs_conpty_smoke", 80, 24)) {
        printf("NOTE: ConPTY unavailable, skipping host smoke test\n");
        return;
    }
    // The command echoed text; the reader thread should deliver a chunk.
    // We just assert the host is alive and stop cleanly (no hang / no crash).
    CHECK(pty.IsRunning());
    pty.Stop();
    CHECK(!pty.IsRunning());
}

int main() {
    TestPlainText();
    TestSgrColors();
    TestSgrBoldBoth();
    TestBrightFg();
    TestBg();
    TestUnsupportedSequencesSwallowed();
    TestOscSwallowed();
    TestEscUnknown();
    TestSplitSequenceAcrossChunks();
    TestUtf8Passthrough();
    TestCupToCol1Newline();
    TestCrlfSplitAcrossChunks();
    TestCrAtEndOfBufferThenText();
    TestConptyStartStop();

    if (g_fail == 0) { printf("ALL TERMINAL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
