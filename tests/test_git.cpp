#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "../src/git/GitStatus.h"

namespace fs = std::filesystem;
using namespace xfs::git;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_fail; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string Z(const std::string& s) { return s + '\0'; }

int main() {
    // --- ParseBranch ---------------------------------------------------------
    CHECK(ParseBranch("master\r\n") == L"master");
    CHECK(ParseBranch("(detached HEAD)\n") == L"(detached HEAD)");
    CHECK(ParseBranch("") == L"");

    // --- ToAbsPath -----------------------------------------------------------
    CHECK(ToAbsPath(L"C:\\Repo", "src/a.txt") == L"c:\\repo\\src\\a.txt");
    CHECK(ToAbsPath(L"C:\\Repo\\", "b.md") == L"c:\\repo\\b.md");

    // --- ParseStatusPorcelainZ -------------------------------------------------
    std::string out;
    out += Z("M  src/a.txt");
    out += Z(" M b.md");
    out += Z("?? new.py");
    out += Z("?? docs/");
    out += Z("R  new.cpp") + Z("old.cpp");
    out += Z("A  added.txt");
    out += Z("D  gone.txt");
    out += Z("UU both.txt");
    out += Z("T  type.txt");
    out += Z("?? \xE4\xB8\xAD.txt");
    StateMap m = ParseStatusPorcelainZ(out, L"C:\\Repo");
    CHECK(m.size() == 9);
    CHECK(m[L"c:\\repo\\src\\a.txt"] == FileState::Modified);
    CHECK(m[L"c:\\repo\\b.md"] == FileState::Modified);
    CHECK(m[L"c:\\repo\\new.py"] == FileState::Untracked);
    CHECK(m[L"c:\\repo\\docs"] == FileState::Untracked);       // dir slash stripped
    CHECK(m[L"c:\\repo\\new.cpp"] == FileState::Renamed);      // old path skipped
    CHECK(m.count(L"c:\\repo\\old.cpp") == 0);
    CHECK(m[L"c:\\repo\\added.txt"] == FileState::Added);
    CHECK(m[L"c:\\repo\\gone.txt"] == FileState::Deleted);
    CHECK(m[L"c:\\repo\\both.txt"] == FileState::Conflict);
    CHECK(m.count(L"c:\\repo\\type.txt") == 0);                // 'T' ignored
    CHECK(m[L"c:\\repo\\\u4E2D.txt"] == FileState::Untracked);

    // --- FindRepoRoot (real temp tree) ------------------------------------------
    fs::path tmp = fs::temp_directory_path() / "xfsGitTest46";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::path proj = tmp / "proj";
    fs::create_directories(proj / ".git");
    fs::create_directories(proj / "sub");
    fs::path file = proj / "sub" / "f.txt";
    CHECK(FindRepoRoot(file.wstring(), tmp.wstring()) == proj.wstring());
    CHECK(FindRepoRoot(proj.wstring(), tmp.wstring()) == proj.wstring());
    fs::path bare = tmp / "bare";
    fs::create_directories(bare);
    CHECK(FindRepoRoot((bare / "x.txt").wstring(), tmp.wstring()).empty());
    fs::remove_all(tmp, ec);

    printf(g_fail ? "test_git: %d FAILED\n" : "test_git: all passed (%d)\n", g_fail);
    return g_fail;
}
