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

    // --- ParseTracking -------------------------------------------------------
    auto ZL = [](std::initializer_list<const char*> toks) {
        std::string s;
        for (auto t : toks) { s += t; s += '\0'; }
        return s;
    };
    {
        auto t = ParseTracking(ZL({"## master...origin/master"}));
        CHECK(!t.detached && t.hasUpstream && !t.gone && t.ahead == 0 && t.behind == 0);
    }
    {
        auto t = ParseTracking(ZL({"## master...origin/master [ahead 2, behind 1]",
                                  "?? a.txt"}));
        CHECK(t.hasUpstream && t.ahead == 2 && t.behind == 1 && !t.gone);
    }
    {
        auto t = ParseTracking(ZL({"## main...origin/main [ahead 10]", "M  x"}));
        CHECK(t.ahead == 10 && t.behind == 0);
    }
    {
        auto t = ParseTracking(ZL({"## main...origin/main [behind 3]"}));
        CHECK(t.behind == 3 && t.ahead == 0);
    }
    {
        auto t = ParseTracking(ZL({"## main...origin/old [gone]"}));
        CHECK(t.hasUpstream && t.gone && t.ahead == 0 && t.behind == 0);
    }
    {
        auto t = ParseTracking(ZL({"## HEAD (no branch)", "M  x"}));
        CHECK(t.detached && !t.hasUpstream && t.ahead == 0 && t.behind == 0);
    }
    {
        auto t = ParseTracking(ZL({"## main"}));
        CHECK(!t.detached && !t.hasUpstream);
    }
    {
        auto t = ParseTracking(ZL({"## (no branch, rebasing main)"}));
        CHECK(t.detached && !t.hasUpstream);
    }
    {
        auto t = ParseTracking(ZL({"## main...origin/main [ahead 1, behind 2, gone]"}));
        CHECK(t.ahead == 1 && t.behind == 2 && t.gone);
    }
    {
        auto t = ParseTracking(ZL({"?? a.txt", "M  b"}));  // no -b header
        CHECK(!t.hasUpstream && !t.detached && t.ahead == 0 && t.behind == 0);
    }
    {
        auto t = ParseTracking(ZL({"## master...origin/master [ahead 12345]"}));
        CHECK(t.ahead == 12345);
    }
    CHECK(ParseTracking("").hasUpstream == false);

    // --- ParseBranchList (batch 50/51) ------------------------------------------
    {
        auto l = ParseBranchList(
            "*refs/heads/main\r\n refs/remotes/origin/HEAD\n"
            " refs/remotes/origin/main\n");
        CHECK(l.size() == 2 && l[0].current && l[0].name == L"main" && !l[0].remote &&
              !l[1].current && l[1].remote && l[1].name == L"origin/main");
    }
    {
        auto l = ParseBranchList(" refs/heads/feat/one-line-no-star\n");
        CHECK(l.size() == 1 && !l[0].current && !l[0].remote &&
              l[0].name == L"feat/one-line-no-star");
    }
    {
        auto l = ParseBranchList("*refs/heads/\xe4\xb8\xad\xe6\x96\x87\n");
        CHECK(l.size() == 1 && l[0].current && l[0].name == L"\u4e2d\u6587");
    }
    CHECK(ParseBranchList("").empty());
    CHECK(ParseBranchList("\n \n").empty());          // degenerate/empty names
    CHECK(ParseBranchList(" refs/tags/v1\n").empty());  // unknown namespace
    {
        auto l = ParseBranchList("*refs/heads/no-trailing-newline");
        CHECK(l.size() == 1 && l[0].name == L"no-trailing-newline");
    }

    // --- BranchNameOk (batch 51) --------------------------------------------------
    CHECK(BranchNameOk(L"main"));
    CHECK(BranchNameOk(L"feat/x"));
    CHECK(BranchNameOk(L"v1.2.3"));
    CHECK(BranchNameOk(L"\u4e2d\u6587"));
    CHECK(BranchNameOk(L"foo-bar"));
    CHECK(BranchNameOk(L"1234"));
    CHECK(!BranchNameOk(L""));
    CHECK(!BranchNameOk(L"a b"));
    CHECK(!BranchNameOk(L"a\tb"));
    CHECK(!BranchNameOk(L"a..b"));
    CHECK(!BranchNameOk(L"a//b"));
    CHECK(!BranchNameOk(L"/lead"));
    CHECK(!BranchNameOk(L"trail/"));
    CHECK(!BranchNameOk(L"trail."));
    CHECK(!BranchNameOk(L"-lead"));
    CHECK(!BranchNameOk(L"x.lock"));
    CHECK(!BranchNameOk(L"a~b"));
    CHECK(!BranchNameOk(L"a^b"));
    CHECK(!BranchNameOk(L"a:b"));
    CHECK(!BranchNameOk(L"a?b"));
    CHECK(!BranchNameOk(L"a*b"));
    CHECK(!BranchNameOk(L"a[b"));
    CHECK(!BranchNameOk(L"a\\b"));
    CHECK(!BranchNameOk(L"a@{b"));


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
        // batch 50: branch list + checkout round-trip in the same repo
        {
            std::string bo;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"rev-parse --abbrev-ref HEAD", bo));
            std::wstring head = ParseBranch(bo);
            CHECK(!head.empty());
            CHECK(git(L"branch feat50x"));
            std::string lo;
            CHECK(xfs::GitClient::Run(repo.wstring(),
                                       L"for-each-ref --format=" +
                                           QuoteArg(L"%(HEAD)%(refname)") +
                                           L" refs/heads refs/remotes", lo));
            auto list = ParseBranchList(lo);
            CHECK(list.size() == 2);
            int curIdx = -1, featIdx = -1;
            for (size_t k = 0; k < list.size(); ++k) {
                if (list[k].current) curIdx = (int)k;
                if (list[k].name == L"feat50x") featIdx = (int)k;
            }
            CHECK(curIdx >= 0 && featIdx >= 0 && curIdx != featIdx);
            CHECK(list[curIdx].name == head);
            CHECK(git(L"checkout -q " + QuoteArg(list[featIdx].name)));
            std::string bo2;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"rev-parse --abbrev-ref HEAD", bo2));
            CHECK(ParseBranch(bo2) == L"feat50x");
            CHECK(git(L"checkout -q " + QuoteArg(head)));
            std::string bo3;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"rev-parse --abbrev-ref HEAD", bo3));
            CHECK(ParseBranch(bo3) == head);

            // batch 51: remote branch list + DWIM tracking checkout
            fs::path bare = fs::temp_directory_path() / "xfsGitTest51origin";
            std::error_code ec51;
            fs::remove_all(bare, ec51);
            fs::create_directories(bare);
            CHECK(git(L"init -q --bare " + QuoteArg(bare.wstring())));
            CHECK(git(L"remote add origin " + QuoteArg(bare.wstring())));
            CHECK(git(L"push -q origin " + QuoteArg(head)));
            CHECK(git(L"branch feat51r"));
            CHECK(git(L"push -q origin feat51r"));
            CHECK(git(L"branch -D feat51r"));   // drop local; remote-tracking stays
            {   // list now has 1 current local + remote entries
                std::string ro;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                        L"for-each-ref --format=" + QuoteArg(L"%(HEAD)%(refname)") +
                        L" refs/heads refs/remotes", ro));
                auto rl = ParseBranchList(ro);
                bool sawLocal = false, sawRemote = false;
                for (auto& be : rl) {
                    if (!be.remote && be.current && be.name == head) sawLocal = true;
                    if (be.remote && be.name == L"origin/feat51r") sawRemote = true;
                }
                CHECK(sawLocal && sawRemote);
                CHECK(git(L"checkout -q feat51r"));   // DWIM creates tracking
                std::string bt;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                        L"rev-parse --abbrev-ref HEAD", bt));
                CHECK(ParseBranch(bt) == L"feat51r");
                CHECK(git(L"checkout -q " + QuoteArg(head)));
            }
            fs::remove_all(bare, ec51);
        }
        // batch 52: push/fetch round-trip (exercises timeout + blockPrompts Run)
        {
            auto firstline = [](std::string s) {
                size_t n = s.find('\n');
                if (n != std::string::npos) s.resize(n);
                if (!s.empty() && s.back() == '\r') s.pop_back();
                return s;
            };
            fs::path bare2 = fs::temp_directory_path() / "xfsGitTest52origin";
            fs::path w2 = fs::temp_directory_path() / "xfsGitTest52w2";
            std::error_code ec52;
            fs::remove_all(bare2, ec52);
            fs::remove_all(w2, ec52);
            fs::create_directories(bare2);
            CHECK(git(L"init -q --bare " + QuoteArg(bare2.wstring())));
            CHECK(git(L"remote add or52 " + QuoteArg(bare2.wstring())));
            std::string bo0;
            CHECK(xfs::GitClient::Run(repo.wstring(), L"rev-parse --abbrev-ref HEAD", bo0));
            std::wstring head2 = ParseBranch(bo0);
            { std::ofstream(repo / "b52.txt") << "x\n"; }
            CHECK(git(L"add -- b52.txt"));
            CHECK(git(std::wstring(id) + L"commit -qm b52"));
            {   // push via Run with blockPrompts enabled
                std::string po;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"push -q -u or52 " + QuoteArg(head2),
                                          po, nullptr, 60000, true));
                std::string lo;
                CHECK(xfs::GitClient::Run(bare2.wstring(),
                                          L"rev-parse " + QuoteArg(head2), lo));
                std::string local;
                CHECK(xfs::GitClient::Run(repo.wstring(), L"rev-parse HEAD", local));
                CHECK(firstline(lo) == firstline(local));
                std::string so;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"status --porcelain=v1 -b -z", so));
                auto tr = ParseTracking(so);
                CHECK(tr.hasUpstream && tr.ahead == 0 && tr.behind == 0);
            }
            {   // second clone advances the remote branch; fetch sees behind=1
                std::string co;
                CHECK(xfs::GitClient::Run(
                    L"", L"clone -q -b " + QuoteArg(head2) + L" " +
                         QuoteArg(bare2.wstring()) + L" " + QuoteArg(w2.wstring()), co));
                { std::ofstream(w2 / "r.txt") << "r\n"; }
                CHECK(xfs::GitClient::Run(w2.wstring(), L"add -- r.txt", co));
                CHECK(xfs::GitClient::Run(w2.wstring(),
                                          std::wstring(id) + L"commit -qm remote", co));
                std::string po;
                CHECK(xfs::GitClient::Run(w2.wstring(), L"push -q origin HEAD",
                                          po, nullptr, 60000, true));
                std::string fo;
                CHECK(xfs::GitClient::Run(repo.wstring(), L"fetch --prune or52",
                                          fo, nullptr, 60000, true));
                std::string so;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"status --porcelain=v1 -b -z", so));
                auto tr = ParseTracking(so);
                CHECK(tr.hasUpstream && tr.behind == 1 && tr.ahead == 0);
            }
            {   // batch 53: pull --ff-only fast-forwards, then diverges and must refuse
                std::string lo;
                CHECK(xfs::GitClient::Run(repo.wstring(), L"pull --ff-only or52",
                                          lo, nullptr, 60000, true));
                std::string so;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"status --porcelain=v1 -b -z", so));
                auto tr = ParseTracking(so);
                CHECK(tr.hasUpstream && tr.behind == 0 && tr.ahead == 0);
                { std::ofstream(repo / "mine53.txt") << "m\n"; }
                std::string co;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"add -- mine53.txt", co));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          std::wstring(id) + L"commit -qm local53", co));
                std::ofstream(w2 / "rr.txt") << "rr\n";
                CHECK(xfs::GitClient::Run(w2.wstring(), L"add -- rr.txt", co));
                CHECK(xfs::GitClient::Run(w2.wstring(),
                                          std::wstring(id) + L"commit -qm remote53", co));
                CHECK(xfs::GitClient::Run(w2.wstring(), L"push -q origin HEAD",
                                          co, nullptr, 60000, true));
                std::string lo3;
                CHECK(!xfs::GitClient::Run(repo.wstring(), L"pull --ff-only or52",
                                           lo3, nullptr, 60000, true));
                CHECK(lo3.find("Not possible to fast-forward") != std::string::npos ||
                      lo3.find("divergent branches") != std::string::npos ||
                      lo3.find("Need to specify how to reconcile") != std::string::npos);
            }
            {   // batch 54: revert single file discards local modifications
                { std::ofstream(repo / "mine53.txt") << "changed54\n"; }
                std::string so;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"status --porcelain=v1 -z", so));
                CHECK(so.find("M ") != std::string::npos ||
                      so.find(" M ") != std::string::npos);
                std::string rv;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"checkout -- mine53.txt", rv));
                std::string so2;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"status --porcelain=v1 -z", so2));
                CHECK(so2.find("mine53.txt") == std::string::npos);
                std::string got;
                std::ifstream in(repo / "mine53.txt");
                std::getline(in, got);
                CHECK(got == "m");
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"checkout HEAD -- mine53.txt", rv));
            }
            {   // batch 55: merge the remote branch into the diverged head
                std::string mo;
                CHECK(xfs::GitClient::Run(
                    repo.wstring(),
                    std::wstring(id) + L"-c core.editor=true merge --no-edit " +
                        QuoteArg(L"or52/" + head2),
                    mo, nullptr, 60000, false));
                std::error_code ec55;
                CHECK(fs::exists(repo / "rr.txt", ec55));
                std::string so;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"status --porcelain=v1 -b -z", so));
                auto tr = ParseTracking(so);
                CHECK(tr.hasUpstream && tr.ahead == 2 && tr.behind == 0);
            }
            {   // batch 56: stash push/pop round-trip
                { std::ofstream(repo / "mine53.txt") << "stashed56\n"; }
                std::string so;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"status --porcelain=v1 -z", so));
                CHECK(so.find("mine53.txt") != std::string::npos);
                std::string sto;
                CHECK(xfs::GitClient::Run(
                    repo.wstring(),
                    std::wstring(id) + L"stash push -m " + QuoteArg(L"t56"),
                    sto, nullptr, 60000, false));
                std::string got;
                { std::ifstream in(repo / "mine53.txt"); std::getline(in, got); }
                CHECK(got == "m");
                std::string lst;
                CHECK(xfs::GitClient::Run(repo.wstring(), L"stash list", lst));
                CHECK(lst.find("stash@{0}") != std::string::npos);
                std::string pop;
                CHECK(xfs::GitClient::Run(repo.wstring(), L"stash pop", pop,
                                          nullptr, 60000, false));
                { std::ifstream in(repo / "mine53.txt"); std::getline(in, got); }
                CHECK(got == "stashed56");
                std::string lst2;
                CHECK(xfs::GitClient::Run(repo.wstring(), L"stash list", lst2));
                CHECK(lst2.find("stash@{") == std::string::npos);
            }
            {   // batch 57: branch -d deletes merged, refuses unmerged
                std::string o57;
                CHECK(xfs::GitClient::Run(
                    repo.wstring(),
                    std::wstring(id) + L"branch feat57", o57));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch -d feat57", o57));
                std::string bl;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch --list feat57", bl));
                CHECK(bl.find("feat57") == std::string::npos);
                CHECK(xfs::GitClient::Run(
                    repo.wstring(),
                    std::wstring(id) + L"checkout -q -b side57 HEAD~1", o57));
                { std::ofstream(repo / "side57.txt") << "s57\n"; }
                CHECK(xfs::GitClient::Run(
                    repo.wstring(),
                    std::wstring(id) + L"add side57.txt", o57));
                CHECK(xfs::GitClient::Run(
                    repo.wstring(),
                    std::wstring(id) + L"commit -q -m side57", o57));
                CHECK(xfs::GitClient::Run(
                    repo.wstring(),
                    std::wstring(id) + L"checkout -q " + QuoteArg(head2), o57));
                std::string ref;
                CHECK(!xfs::GitClient::Run(repo.wstring(),
                                           L"branch -d side57", ref));
                std::string bl2;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch --list side57", bl2));
                CHECK(bl2.find("side57") != std::string::npos);
            }
            {   // batch 58: branch -m renames (current + other), collision refused
                std::string o58, bl;
                CHECK(xfs::GitClient::Run(
                    repo.wstring(), std::wstring(id) + L"branch coll58", o58));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch -m coll58 coll58b", o58));
                std::string old58, kept;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch --list coll58", old58));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch --list coll58b", kept));
                CHECK(old58.find("coll58") == std::string::npos);
                CHECK(kept.find("coll58b") != std::string::npos);
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch -m side57 side58", o58));
                std::string cur;
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch -m " + QuoteArg(head2) +
                                              L" main58", o58));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch --show-current", cur));
                CHECK(cur.find("main58") != std::string::npos);
                std::string clash;
                CHECK(!xfs::GitClient::Run(repo.wstring(),
                                           L"branch -m main58 coll58b", clash));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch -m main58 " + QuoteArg(head2),
                                          o58));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch --show-current", cur));
                CHECK(xfs::Utf8ToWide(cur).find(head2) != std::wstring::npos);
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch -d coll58b", o58));
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"branch -D side58", bl));
            }
            {   // batch 59: fetch surfaces fresh remote branches for the picker
                std::string o59;
                CHECK(xfs::GitClient::Run(w2.wstring(),
                                          L"checkout -qb new59", o59));
                CHECK(xfs::GitClient::Run(w2.wstring(),
                                          L"push -q -u origin new59", o59));
                std::string ba;
                CHECK(xfs::GitClient::Run(repo.wstring(), L"branch -a", ba));
                CHECK(ba.find("or52/new59") == std::string::npos);
                CHECK(xfs::GitClient::Run(repo.wstring(),
                                          L"fetch -q or52", o59));
                CHECK(xfs::GitClient::Run(repo.wstring(), L"branch -a", ba));
                CHECK(ba.find("or52/new59") != std::string::npos);
            }
            fs::remove_all(bare2, ec52);
            fs::remove_all(w2, ec52);
        }
        fs::remove_all(repo, ec48);
    }
    printf(g_fail ? "test_git: %d FAILED\n" : "test_git: all passed (%d)\n", g_fail);
    return g_fail;
}
