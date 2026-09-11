// test_findinfiles.cpp - end-to-end tests for Find/Replace In Files
// covering multi-encoding round trips, filters, recursion, case rules.
#include "../src/search/FindInFiles.h"
#include "../src/core/Util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

using namespace xfs;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace fs = std::filesystem;

static std::wstring Root() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"xfs_fif_test";
}

static void WriteBomFile(const std::wstring& path, const char* bom, size_t bomLen,
                         const std::string& body) {
    std::string raw(bom, bomLen);
    raw += body;
    WriteFileBytes(path, raw.data(), raw.size());
}

static std::string ReadAll(const std::wstring& path) {
    std::string out;
    ReadFileBytes(path, out);
    return out;
}

static void Cleanup() {
    std::error_code ec;
    fs::remove_all(Root(), ec);
}

static void Setup() {
    Cleanup();
    std::error_code ec;
    fs::create_directories(Root() + L"\\sub", ec);

    WriteBomFile(Root() + L"\\a.txt", "", 0,
                 "hello world\r\nfoo bar\r\nhello again\r\n");
    WriteBomFile(Root() + L"\\b.log", "\xEF\xBB\xBF", 3,
                 "ERROR x\r\nhello there\r\n");
    WriteBomFile(Root() + L"\\sub\\c.txt", "", 0,
                 "GBK: \xC4\xE3\xBA\xC3 hello mixed\r\n");   // 你好 in GBK
    // UTF-16LE with BOM: "hello utf16"
    {
        std::string body;
        const wchar_t* w = L"hello utf16\r\n";
        body.push_back((char)0xFF); body.push_back((char)0xFE);
        for (const wchar_t* p = w; *p; ++p) {
            body.push_back((char)((*p) & 0xFF));
            body.push_back((char)((*p) >> 8));
        }
        WriteBomFile(Root() + L"\\sub\\d.dat", "", 0, body);
    }
    WriteBomFile(Root() + L"\\e.md", "", 0, "nothing to see here\r\n");
}

static int CountHits(const std::vector<SearchHit>& hits, const wchar_t* file) {
    int n = 0;
    for (const auto& h : hits)
        if (h.path.find(file) != std::wstring::npos) ++n;
    return n;
}

int main() {
    Setup();
    FindInFilesOptions o;
    o.directory = Root();
    o.recursive = true;

    // --- 1. recursive find "hello" across encodings -------------------------
    {
        o.st = FindState{};  o.st.text = L"hello";
        o.filters.clear();
        auto hits = RunFindInFiles(o);
        CHECK(CountHits(hits, L"a.txt") == 2);
        CHECK(CountHits(hits, L"b.log") == 1);
        CHECK(CountHits(hits, L"c.txt") == 1);
        CHECK(CountHits(hits, L"d.dat") == 1);
        CHECK(CountHits(hits, L"e.md") == 0);
    }

    // --- 2. filter limits scope ---------------------------------------------
    {
        o.st = FindState{};  o.st.text = L"hello";
        o.filters = L"*.log";
        auto hits = RunFindInFiles(o);
        CHECK(hits.size() == 1);
        CHECK(CountHits(hits, L"b.log") == 1);
    }

    // --- 3. non-recursive skips subdirectory --------------------------------
    {
        o.st = FindState{};  o.st.text = L"hello";
        o.filters.clear();
        o.recursive = false;
        auto hits = RunFindInFiles(o);
        CHECK(CountHits(hits, L"a.txt") == 2);
        CHECK(CountHits(hits, L"c.txt") == 0);
        CHECK(CountHits(hits, L"d.dat") == 0);
        o.recursive = true;
    }

    // --- 4. case sensitivity --------------------------------------------------
    {
        o.st = FindState{};  o.st.text = L"HELLO";  o.st.matchCase = true;
        auto hits = RunFindInFiles(o);
        CHECK(hits.empty());
        o.st.matchCase = false;
        hits = RunFindInFiles(o);
        CHECK(hits.size() >= 5);   // case-insensitive finds all
    }

    // --- 5. replace in files: content + encoding preservation -----------------
    {
        o.st = FindState{};  o.st.text = L"hello";  o.st.replace = L"bye";
        o.filters.clear();
        int changed = RunReplaceInFiles(o, o.st.replace);
        CHECK(changed == 4);   // a.txt, b.log, c.txt, d.dat

        // a.txt: plain utf8, no BOM, CRLF kept
        std::string a = ReadAll(Root() + L"\\a.txt");
        CHECK(a.find("bye world") != std::string::npos);
        CHECK(a.find("hello") == std::string::npos);
        CHECK(a.find("\xEF\xBB\xBF") != 0);
        CHECK(a.find("\r\n") != std::string::npos);

        // b.log: UTF-8 BOM must survive
        std::string b = ReadAll(Root() + L"\\b.log");
        CHECK((unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB);
        CHECK(b.find("bye there") != std::string::npos);

        // c.txt: GBK bytes for 你好 must survive (re-encoded via ANSI)
        std::string c = ReadAll(Root() + L"\\sub\\c.txt");
        CHECK(c.find("\xC4\xE3\xBA\xC3") != std::string::npos);
        CHECK(c.find("bye mixed") != std::string::npos);

        // d.dat: UTF-16LE BOM must survive; "bye utf16" in UTF-16
        std::string d = ReadAll(Root() + L"\\sub\\d.dat");
        CHECK((unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xFE);
        CHECK(d.find("b\0y\0e\0", 2) != std::string::npos);
        CHECK(d.find("h\0e\0l\0l\0o\0", 2) == std::string::npos);
    }

    // --- 6. replace with no matches -> 0 --------------------------------------
    {
        o.st = FindState{};  o.st.text = L"does-not-exist";  o.st.replace = L"x";
        CHECK(RunReplaceInFiles(o, o.st.replace) == 0);
    }

    // --- 7. find after replace: no stale hits ---------------------------------
    {
        o.st = FindState{};  o.st.text = L"hello";
        auto hits = RunFindInFiles(o);
        CHECK(hits.empty());
    }

    // --- 8. regression: replacement text comes from the replaceWith PARAM -----
    // (the dialog passes the 文件中替换 tab's box here while st.replace holds
    //  a different tab's value; the engine must honor the parameter)
    {
        WriteBomFile(Root() + L"\\f.txt", "", 0,
                     "\xE9\xA1\xB9\xE7\x9B\xAE alpha \xE9\xA1\xB9\xE7\x9B\xAE\r\n"); // 项目 alpha 项目 (UTF-8)
        o.st = FindState{};  o.st.text = L"项目";   // st.replace deliberately EMPTY
        o.filters = L"*.txt";
        int changed = RunReplaceInFiles(o, L"item");
        CHECK(changed >= 1);
        std::string f = ReadAll(Root() + L"\\f.txt");
        CHECK(f.find("item alpha item") != std::string::npos);
        CHECK(f.find("\xE9\xA1\xB9\xE7\x9B\xAE") == std::string::npos);
        CHECK(f.find("\r\n") != std::string::npos);   // CRLF preserved
    }

    Cleanup();
    printf(g_fail == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return g_fail == 0 ? 0 : 1;
}
