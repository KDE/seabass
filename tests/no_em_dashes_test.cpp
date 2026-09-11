// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// No em-dashes in anything a person reads on screen.
//
// A style rule, so a guard rather than a review habit: they came back
// three times in one session, each time in text written to explain
// something, and a reviewer cannot reliably spot one character among
// thousands of lines of QML.
//
// Scans the source rather than the running app on purpose. A rendered
// screen only shows the strings that happen to be displayed by whatever
// the test drove; the source shows every string that could be.
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct Hit
{
    std::string file;
    int line = 0;
    std::string text;
};

// U+2014 EM DASH is 0xE2 0x80 0x94 in UTF-8. Also caught: the QML/C++
// escape "—", which renders identically and would otherwise slip
// past a byte search.
bool lineHasEmDash(const std::string &line)
{
    return line.find("\xE2\x80\x94") != std::string::npos || line.find("\\u2014") != std::string::npos
           || line.find("\\U00002014") != std::string::npos;
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
    const fs::path sourceDir = fs::path(SEABASS_SOURCE_DIR);
    const fs::path uiRoot = sourceDir / "src" / "gui";
    if (!fs::is_directory(uiRoot)) {
        std::cerr << "FAIL: " << uiRoot << " is not a directory; this test cannot check anything.\n";
        return 1;
    }

    std::vector<Hit> hits;
    std::size_t filesScanned = 0;
    for (const auto &entry : fs::recursive_directory_iterator(uiRoot)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        // .qml carries almost every string a user sees; .cpp/.hpp in
        // gui/ carry the rest (status messages, descriptions, tooltips
        // built controller-side).
        if (ext != ".qml" && ext != ".cpp" && ext != ".hpp") {
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
            if (lineHasEmDash(line)) {
                hits.push_back({fs::relative(entry.path(), sourceDir).string(), lineNumber, trimmed(line)});
            }
        }
    }

    // The scan itself has to be able to fail. Finding no files would
    // report success while checking nothing, which is the failure mode
    // this whole suite keeps running into.
    if (filesScanned == 0) {
        std::cerr << "FAIL: scanned no files under " << uiRoot << ", so this test proved nothing.\n";
        return 1;
    }

    if (!hits.empty()) {
        std::cerr << "FAIL: " << hits.size() << " em-dash(es) in user-visible source:\n";
        for (const auto &hit : hits) {
            std::cerr << "  " << hit.file << ":" << hit.line << ": " << hit.text << "\n";
        }
        std::cerr << "Use a colon, a full stop, or a comma instead.\n";
        return 1;
    }

    std::cout << "no em-dashes in " << filesScanned << " user-visible source file(s)\n";
    return 0;
}
