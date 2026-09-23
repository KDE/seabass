// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// KDE repositories do not carry Co-Authored-By: Claude or
// Claude-Session trailers. .githooks/commit-msg strips them and
// .githooks/pre-push refuses to publish any that got past it; this
// checks both actually do that.
//
// Worth a test rather than a read-through because the failure mode is
// silence. A hook that checks the wrong commit range, or none, exits 0
// and looks exactly like a hook that found nothing wrong -- and the
// first evidence would be a trailer on a protected branch that cannot
// be rewritten afterwards. That is how the 19 permanent ones got there
// on 2026-09-18.
//
// The pre-push range logic is the part this is really here for. Git's
// --not negates every ref after it, so `--not --remotes=origin $tip`
// silently checks nothing; `$tip --not --remotes=origin` is correct.
// Both orderings pass a human skim.
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "scratch_path.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace
{

const fs::path HooksDir = fs::path(SEABASS_SOURCE_DIR) / ".githooks";

// seabass::testing::scratchRoot() rather than $TMPDIR-or-"/tmp": every
// path built here ends up embedded in a command bash runs (see run()
// below), and on Windows a bare "/tmp" is ambiguous between the two
// runtimes that resolve it -- std::filesystem takes it as relative to
// the current drive (so this process writes the message file under,
// say, C:\tmp), while bash's own MSYS layer maps "/tmp" to its own
// mount point (typically somewhere under the Git install), a different
// directory on disk. The hook would then run against a file that does
// not exist, or an unrelated leftover one, while this process reads
// back the file it actually wrote -- unchanged, since nothing touched
// it. scratchRoot() resolves through fs::temp_directory_path(), always
// a drive-letter path, which both runtimes treat identically.
fs::path scratch(const std::string &name)
{
    fs::path base = seabass::testing::scratchRoot() / ("trailer-hook-test-" + name);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

#ifdef _WIN32
// Bare "bash" depends on the launching process's own PATH reaching a
// bash.exe, which held from an interactive MSYS/git-bash shell (this
// file's own child processes inherit that shell's PATH) and did not
// hold under ctest -j: "'bash' is not recognized as an internal or
// external command" -- ctest.exe is a native binary, and whatever
// spawns it does not necessarily hand its own children a PATH with any
// bash.exe on it, even on a machine where several exist. Git for
// Windows is the one bash this project can assume -- git itself is
// required for every other line in this file -- and its own install
// always carries bash.exe at these two fixed locations relative to
// %ProgramFiles%, checked before falling back to a bare PATH search.
std::string resolveBashCommand()
{
    char programFiles[MAX_PATH] = {};
    DWORD len = ::GetEnvironmentVariableA("ProgramFiles", programFiles, sizeof(programFiles));
    if (len > 0 && len < sizeof(programFiles)) {
        for (const char *candidate : {"\\Git\\bin\\bash.exe", "\\Git\\usr\\bin\\bash.exe"}) {
            const fs::path path = std::string(programFiles) + candidate;
            std::error_code ec;
            if (fs::is_regular_file(path, ec)) {
                return "\"" + path.generic_string() + "\"";
            }
        }
    }
    return "bash";
}
#endif

int run(const std::string &command)
{
#ifdef _WIN32
    // std::system() always launches through cmd.exe (via COMSPEC), no
    // matter which shell the calling process itself runs under -- and
    // cmd.exe cannot run .githooks/commit-msg or .githooks/pre-push
    // directly: they are extensionless shebang scripts, so it answers
    // "the system cannot find the path specified" as if the file did not
    // exist, rather than anything about the hook itself. Every command
    // here is already POSIX shell syntax (2>/dev/null, &&, single-quoted
    // pipelines), which cmd.exe would mangle differently anyway, so
    // routing the whole thing through bash -- present on any Windows
    // machine set up to build this project -- fixes both problems at
    // once rather than only the one that happened to abort first.
    //
    // Every path embedded in `command` is built by its caller with
    // fs::path::generic_string() rather than string(), so none of them
    // carry a backslash for this to trip over: operator/ appends each
    // further path segment with the native separator, which on Windows
    // is a backslash that bash's own double-quote parsing would consume
    // before the hook ever saw it -- ".githooks\commit-msg" reaching it
    // as ".githookscommit-msg", a file that does not exist. A blanket
    // replace of every backslash in `command` looked like the same fix
    // and is not one: this file also builds printf '%s\n' ..., where
    // the backslash is not a path separator at all, and turning it into
    // '%s/n' silently broke the one pre-push test that depends on the
    // newline actually landing on stdin, passing on input the hook was
    // supposed to refuse.
    //
    // A temp script rather than `bash -c "<command>"`: system() always
    // launches through cmd.exe, and cmd.exe's own quoting does not
    // survive being handed an already-quoted -c argument. Confirmed
    // directly -- the identical command run through cmd.exe reaches
    // bash as an unterminated string ("unexpected EOF while looking for
    // matching `"'"), and bash's own exit 2 for that syntax error is
    // what looked, from here, like the hook itself refusing the
    // message. A bare file path is nothing for cmd.exe to mangle.
    static int counter = 0;
    fs::path scriptPath =
        fs::temp_directory_path() / ("seabass-hook-test-" + std::to_string(::GetCurrentProcessId()) + "-"
                                     + std::to_string(++counter) + ".sh");
    {
        std::ofstream script(scriptPath, std::ios::binary);
        script << command << "\n";
    }
    const std::string status = resolveBashCommand() + " " + scriptPath.generic_string();
    const int result = std::system(status.c_str());
    std::error_code ec;
    fs::remove(scriptPath, ec);
    if (result == -1) {
        return -1;
    }
    // system() hands back the exit code directly; there is no wait status
    // to unpack, and WEXITSTATUS does not exist.
    return result;
#else
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
    return WEXITSTATUS(status);
#endif
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// ---------------------------------------------------------------- commit-msg

void testCommitMsgStripsTrailers()
{
    const fs::path dir = scratch("commit-msg");
    const fs::path message = dir / "COMMIT_EDITMSG";
    write(message,
          "Subject line\n"
          "\n"
          "A body that must survive untouched.\n"
          "\n"
          "Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>\n"
          "Claude-Session: https://claude.ai/code/session_01Abc\n");

    assert(run((HooksDir / "commit-msg").generic_string() + " " + message.generic_string() + " 2>/dev/null") == 0);

    const std::string result = readFile(message);
    assert(result.find("Claude") == std::string::npos);
    assert(result.find("A body that must survive untouched.") != std::string::npos);
    assert(result.find("Subject line") != std::string::npos);

    // The trailer block took the blank line before it with it: a message
    // must not end in blank lines.
    assert(result.size() >= 2);
    assert(result.back() == '\n');
    assert(result[result.size() - 2] != '\n');
}

void testCommitMsgLeavesHumanCoAuthorsAlone()
{
    const fs::path dir = scratch("human-coauthor");
    const fs::path message = dir / "COMMIT_EDITMSG";
    write(message,
          "Subject line\n"
          "\n"
          "Co-Authored-By: A Person <person@example.org>\n"
          "Claude-Session: https://claude.ai/code/session_01Abc\n");

    assert(run((HooksDir / "commit-msg").generic_string() + " " + message.generic_string() + " 2>/dev/null") == 0);

    const std::string result = readFile(message);
    assert(result.find("A Person <person@example.org>") != std::string::npos);
    assert(result.find("Claude-Session") == std::string::npos);
}

void testCommitMsgLeavesACleanMessageByteIdentical()
{
    const fs::path dir = scratch("clean-msg");
    const fs::path message = dir / "COMMIT_EDITMSG";
    const std::string original = "Subject line\n\nA body.\n\nSigned-off-by: A Person <person@example.org>\n";
    write(message, original);

    assert(run((HooksDir / "commit-msg").generic_string() + " " + message.generic_string() + " 2>/dev/null") == 0);
    assert(readFile(message) == original);
}

// ------------------------------------------------------------------ pre-push

// A scratch repository with an `origin` it has already pushed one clean
// commit to, so the hook's "already published" exclusion has something
// real to exclude.
struct Repo
{
    fs::path dir;
    fs::path remote;

    std::string git(const std::string &args) const
    {
        return "git -C " + dir.generic_string() + " -c user.name=Test -c user.email=test@example.org " + args;
    }

    std::string revParse(const std::string &rev) const
    {
        const fs::path out = dir / ".rev";
        [[maybe_unused]] const int status = run(git("rev-parse " + rev) + " > " + out.generic_string());
        std::string sha = readFile(out);
        while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) {
            sha.pop_back();
        }
        return sha;
    }

    void commit(const std::string &message) const
    {
        static int n = 0;
        write(dir / ("file" + std::to_string(++n) + ".txt"), "x\n");
        const fs::path msgFile = dir / ".msg";
        write(msgFile, message);
        assert(run(git("add -A")) == 0);
        assert(run(git("commit -q --no-verify -F " + msgFile.generic_string())) == 0);
    }
};

Repo makeRepo(const std::string &name)
{
    Repo repo{scratch(name), scratch(name + "-remote")};
    assert(run("git init -q -b master " + repo.dir.generic_string()) == 0);
    assert(run("git init -q --bare " + repo.remote.generic_string()) == 0);
    assert(run(repo.git("remote add origin " + repo.remote.generic_string())) == 0);
    repo.commit("Base commit, clean\n");
    assert(run(repo.git("push -q --no-verify origin master")) == 0);
    return repo;
}

// Feed the hook what git feeds it: "<local ref> <local sha> <remote ref> <remote sha>".
int runPrePush(const Repo &repo, const std::string &localSha, const std::string &remoteSha)
{
    const std::string line = "refs/heads/master " + localSha + " refs/heads/master " + remoteSha;
    return run("cd " + repo.dir.generic_string() + " && printf '%s\\n' '" + line + "' | " + (HooksDir / "pre-push").generic_string()
               + " origin " + repo.remote.generic_string() + " >/dev/null 2>&1");
}

std::string zeroSha()
{
    return std::string(40, '0');
}

void testPrePushRefusesATrailerOnAKnownBranch()
{
    const Repo repo = makeRepo("prepush-known");
    const std::string base = repo.revParse("HEAD");
    repo.commit("Adds a trailer\n\nCo-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>\n");

    assert(runPrePush(repo, repo.revParse("HEAD"), base) == 1);
}

void testPrePushAllowsCleanCommits()
{
    const Repo repo = makeRepo("prepush-clean");
    const std::string base = repo.revParse("HEAD");
    repo.commit("A perfectly ordinary commit\n");

    assert(runPrePush(repo, repo.revParse("HEAD"), base) == 0);
}

// The case the range logic gets wrong: pushing a branch the remote does
// not have yet, so git reports the remote sha as zeros. Get --not
// backwards here and the hook checks nothing while still exiting 0.
void testPrePushChecksNewBranchesToo()
{
    const Repo repo = makeRepo("prepush-newbranch");
    assert(run(repo.git("checkout -q -b side")) == 0);
    repo.commit("Adds a trailer on a new branch\n\nClaude-Session: https://claude.ai/code/session_01Abc\n");

    assert(runPrePush(repo, repo.revParse("HEAD"), zeroSha()) == 1);
}

// ...and must still not drag in the commits that were already pushed
// before the rule existed, or every push would fail forever.
void testPrePushIgnoresAlreadyPublishedHistory()
{
    const Repo repo = makeRepo("prepush-published");
    repo.commit("An old commit with a trailer\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n");
    assert(run(repo.git("push -q --no-verify origin master")) == 0);

    // A new, clean branch on top of that published history.
    assert(run(repo.git("checkout -q -b later")) == 0);
    repo.commit("Something clean, built on the old history\n");

    assert(runPrePush(repo, repo.revParse("HEAD"), zeroSha()) == 0);
}

void testPrePushIgnoresBranchDeletion()
{
    const Repo repo = makeRepo("prepush-delete");
    assert(runPrePush(repo, zeroSha(), repo.revParse("HEAD")) == 0);
}

}  // namespace

int main()
{
    testCommitMsgStripsTrailers();
    testCommitMsgLeavesHumanCoAuthorsAlone();
    testCommitMsgLeavesACleanMessageByteIdentical();
    testPrePushRefusesATrailerOnAKnownBranch();
    testPrePushAllowsCleanCommits();
    testPrePushChecksNewBranchesToo();
    testPrePushIgnoresAlreadyPublishedHistory();
    testPrePushIgnoresBranchDeletion();
    std::cout << "no_claude_trailers_test passed\n";
    return 0;
}
