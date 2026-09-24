// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Nothing in the GUI reads a QFuture's result raw.
//
// QtConcurrent stores an exception thrown inside a run() body and
// rethrows it from QFutureWatcher::result() and waitForFinished(). Both
// of those are called from a slot or a destructor on the GUI thread,
// where nothing catches: Qt wraps it as QUnhandledException and the
// process calls std::terminate. Two real crashes came from exactly that
// (BackupStick::preview() throwing, and closing the Full Stick Backup
// page while its preview had thrown), which is why takeResult() and
// awaitQuietly() exist -- see src/gui/future_result.hpp.
//
// The helpers were then added controller by controller, as each crash
// was found, and eighteen slots plus three waits in two destructors
// still read the future raw. Nothing was reaching them, because every
// QtConcurrent body in src/gui happens to catch std::exception -- but
// "happens to" is the whole point: it holds only for as long as the next task body
// written also catches, and the failure is a hard crash with no message.
//
// So this is a guard on the source rather than a test of behaviour: the
// crash it prevents cannot be provoked from a test without killing the
// test, and the thing worth pinning is that no new call site appears.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;

namespace
{

struct Hit
{
    std::string file;
    int line = 0;
    std::string text;
};

bool isCommentLine(const std::string &line)
{
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return true;
    }
    const std::string rest = line.substr(first);
    if (rest.rfind("//", 0) == 0 || rest.rfind("/*", 0) == 0 || rest.rfind("*/", 0) == 0) {
        return true;
    }
    // A block comment's continuation line is "* something" or a bare
    // "*". A leading star with no space after it is code -- "*out =
    // m_watcher.result();" is a dereference, and taking it for a comment
    // would hide exactly what this scans for.
    return rest == "*" || rest.rfind("* ", 0) == 0;
}

// Every spelling that rethrows what a task stored, through a value or
// through a pointer. The first version of this listed two of them and
// was blind to the rest: a watcher held as a unique_ptr, or a read
// through results()/resultAt(), would have put the crash back with the
// guard still reporting clean. QFuture::takeResult() is the sharpest of
// them, because it reads like this project's own helper and is not one.
//
// The helper IS "takeResult(watcher, &thrown)", a free call with no dot
// or arrow in front of it, which is why the member spellings can be
// matched without catching every correct call site in the tree.
//
// QProcess has a waitForFinished() of its own and would be a false
// positive, but src/gui has no QProcess: the one place this project runs
// a program is infrastructure/process, which this does not scan. If that
// changes, this comment is the place to say so rather than quietly
// narrowing the pattern again.
bool readsAFutureRaw(const std::string &line)
{
    static const std::vector<std::string> spellings = {
        ".result()",      "->result()",      ".results()",    "->results()",
        ".resultAt(",     "->resultAt(",     ".takeResult(",  "->takeResult(",
        ".waitForFinished()", "->waitForFinished()",
    };
    for (const std::string &spelling : spellings) {
        if (line.find(spelling) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string trimmed(const std::string &line)
{
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return line;
    }
    std::string out = line.substr(first);
    if (out.size() > 96) {
        out = out.substr(0, 93) + "...";
    }
    return out;
}

}  // namespace

int main()
{
    const fs::path sourceDir = seabass::pathFromUtf8(SEABASS_SOURCE_DIR);
    const fs::path uiRoot = sourceDir / "src" / "gui";
    if (!fs::is_directory(uiRoot)) {
        std::cerr << "FAIL: " << uiRoot << " is not a directory; this test cannot check anything.\n";
        return 1;
    }

    std::vector<Hit> hits;
    std::size_t filesScanned = 0;
    bool sawTheHelpers = false;
    for (const auto &entry : fs::recursive_directory_iterator(uiRoot)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path ext = entry.path().extension();
        if (ext != ".cpp" && ext != ".hpp") {
            continue;
        }
        // The helpers themselves are the one place that may call these:
        // they are what everything else goes through.
        if (entry.path().filename() == "future_result.hpp") {
            sawTheHelpers = true;
            continue;
        }
        std::ifstream in(entry.path());
        if (!in) {
            continue;
        }
        ++filesScanned;
        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line)) {
            ++lineNumber;
            if (!isCommentLine(line) && readsAFutureRaw(line)) {
                hits.push_back({seabass::pathToGenericUtf8(fs::relative(entry.path(), sourceDir)), lineNumber, trimmed(line)});
            }
        }
    }

    // A scan that scanned nothing passes while checking nothing, which
    // is the failure this suite keeps finding in itself. Both halves are
    // pinned: files to read, and the helpers this rule points at still
    // being where it says they are.
    if (filesScanned == 0) {
        std::cerr << "FAIL: scanned no files under " << uiRoot << ", so this test proved nothing.\n";
        return 1;
    }
    if (!sawTheHelpers) {
        std::cerr << "FAIL: src/gui/future_result.hpp is gone, so the rule this checks has no home.\n";
        return 1;
    }

    if (!hits.empty()) {
        std::cerr << "FAIL: " << hits.size() << " raw future read(s):\n";
        for (const auto &hit : hits) {
            std::cerr << "  " << hit.file << ":" << hit.line << ": " << hit.text << "\n";
        }
        std::cerr << "An exception from a QtConcurrent task is rethrown here, on the GUI thread,\n"
                     "where nothing catches it: the process terminates. Use takeResult(watcher,\n"
                     "&thrown) in a finished slot and awaitQuietly(watcher) in a destructor\n"
                     "(src/gui/future_result.hpp).\n";
        return 1;
    }

    std::cout << "no_raw_future_reads_test: " << filesScanned << " file(s) scanned, every future read goes through "
                 "takeResult()/awaitQuietly()\n";
    return 0;
}
