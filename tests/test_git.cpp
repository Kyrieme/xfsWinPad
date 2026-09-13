#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../src/git/GitStatus.h"
#include "../src/git/GitClient.h"
#include "../src/core/Util.h"

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

    // --- AggregateDirs ----------------------------------------------------------
    AggregateDirs(m, L"C:\\Repo");  // mixed-case root must be tolerated
    CHECK(m.size() == 11);                                        // +src +repo
    CHECK(m[L"c:\\repo\\src"] == FileState::Modified);
    CHECK(m[L"c:\\repo"] == FileState::Conflict);                 // red wins all
    CHECK(m[L"c:\\repo\\docs"] == FileState::Untracked);          // untouched
    CHECK(m.count(L"c:") == 0 && m.count(L"c:\\") == 0);          // stays in repo

    StateMap m2;
    m2[L"c:\\repo\\p\\a.txt"] = FileState::Added;
    m2[L"c:\\repo\\p\\b.txt"] = FileState::Modified;
    m2[L"c:\\repo\\p"] = FileState::Untracked;   // explicit dir entry loses
    m2[L"c:\\repo\\q\\deep\\x.txt"] = FileState::Conflict;
    AggregateDirs(m2, L"c:\\repo");
    CHECK(m2[L"c:\\repo\\p"] == FileState::Modified);
    CHECK(m2[L"c:\\repo\\q\\deep"] == FileState::Conflict);
    CHECK(m2[L"c:\\repo\\q"] == FileState::Conflict);
    CHECK(m2[L"c:\\repo"] == FileState::Conflict);
    CHECK(m2.size() == 7);

    StateMap m3;
    m3[L"c:\\repo\\solo.txt"] = FileState::Deleted;
    m3[L"c:\\repoX\\s.txt"] = FileState::Untracked;  // sibling-prefix dir
    AggregateDirs(m3, L"c:\\repo");
    CHECK(m3.size() == 3);                            // +c:\repo only
    CHECK(m3.count(L"c:\\repoX") == 0);
    CHECK(m3[L"c:\\repo"] == FileState::Deleted);
    AggregateDirs(m3, L"c:\\other");            // unrelated root: propagate none
    AggregateDirs(m3, L"");                     // empty root: no-op
    CHECK(m3.size() == 3);
    CHECK(m3.count(L"c:\\other") == 0);

    // --- QuoteArg (batch 48) ---------------------------------------------------
    CHECK(QuoteArg(L"plain.txt") == L"plain.txt");
    CHECK(QuoteArg(L"") == L"\"\"");
    CHECK(QuoteArg(L"two words") == L"\"two words\"");
    CHECK(QuoteArg(L"a\"b") == L"\"a\\\"b\"");
    CHECK(QuoteArg(L"end\\") == L"\"end\\\\\"");
    CHECK(QuoteArg(L"x\\\"y") == L"\"x\\\\\\\"y\"");
    {   // round-trip through the real CommandLineToArgvW parser
        const wchar_t* raw[] = {L"a b", L"c\"d", L"e\\", L"f g\\h", L"\\i j"};
        std::wstring line;
        for (const wchar_t* r : raw) line += QuoteArg(r) + L' ';
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(line.c_str(), &argc);
        CHECK(argv != nullptr && argc == 5);
        if (argv) {
            for (int i = 0; i < argc && i < 5; ++i)
                CHECK(std::wstring(argv[i]) == std::wstring(raw[i]));
            ::LocalFree(argv);
        }
    }

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

    // --- real repo stage/commit chain (batch 48; skips when git.exe missing) ---
    std::string ver;
    bool noGit = false;
    xfs::GitClient::Run(L"", L"--version", ver, &noGit);
    if (noGit) {
        printf("note: git.exe unavailable, skipped repo-chain checks\n");
    } else {
        fs::path repo = fs::temp_directory_path() / "xfsGitTest48";
        std::error_code ec48;
        fs::remove_all(repo, ec48);
        fs::create_directories(repo);
        auto git = [&](const std::wstring& args) {
            std::string o;
            bool sf = false;
            bool r = xfs::GitClient::Run(repo.wstring(), args, o, &sf);
            return !sf && r;
        };
        const wchar_t* id = L"-c user.email=t@t -c user.name=t ";
        CHECK(git(L"init -q"));
        { std::ofstream(repo / "seed.txt") << "seed\n"; }
        CHECK(git(L"add -- seed.txt"));
        CHECK(git(std::wstring(id) + L"commit -qm seed"));
        { std::ofstream(repo / "w e.txt") << "hello\n"; }
        CHECK(git(L"add -- " + QuoteArg(L"w e.txt")));
        {   // Added now in the index
            std::string so;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"status --porcelain=v1 -z", so));
            StateMap sm = ParseStatusPorcelainZ(so, repo.wstring());
            CHECK(sm[ToAbsPath(repo.wstring(), "w e.txt")] == FileState::Added);
        }
        CHECK(git(L"restore --staged -- " + QuoteArg(L"w e.txt")));
        {   // back to untracked
            std::string so;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"status --porcelain=v1 -z", so));
            StateMap sm = ParseStatusPorcelainZ(so, repo.wstring());
            CHECK(sm[ToAbsPath(repo.wstring(), "w e.txt")] == FileState::Untracked);
        }
        CHECK(git(L"add -- " + QuoteArg(L"w e.txt")));
        CHECK(git(std::wstring(id) + L"commit -m " +
                  QuoteArg(L"two words \"q\" end\\")));
        {   // commit landed: status clean, subject round-tripped
            std::string so;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"status --porcelain=v1 -z", so));
            CHECK(ParseStatusPorcelainZ(so, repo.wstring()).empty());
            std::string lo;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"log -1 --pretty=%s", lo));
            CHECK(xfs::Utf8ToWide(lo).find(L"two words \"q\" end\\") != std::wstring::npos);
        }
        fs::remove_all(repo, ec48);
    }

    printf(g_fail ? "test_git: %d FAILED\n" : "test_git: all passed (%d)\n", g_fail);
    return g_fail;
}
