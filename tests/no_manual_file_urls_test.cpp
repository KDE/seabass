// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// No hand-built file:// URLs in the GUI.
//
// A path that reaches QML has already been through toLocalFileUrl()
// (src/gui/local_file_url.hpp), so it arrives as "file:///...". Writing
// "file://" + path on top of that produces "file://file:///..." and the
// image or dialog silently shows nothing: no warning, no error, just a
// blank cover. It cost a round of "why is the artwork empty" in
// TrackDetailPanel, and reviews then found the same line in six more
// places (issue #22), which is what makes it a guard rather than a
// review habit.
//
// The other half of the rule is the same mistake spelled differently:
// QUrl::fromLocalFile() or Qt.resolvedUrl() over something that is
// already a URL. Only the concatenations are cheap to detect from the
// source, so those are what this scans; a path that is already a URL
// going into fromLocalFile() needs a reader.
//
// Scans the source, not the running app: a rendered screen only shows
// the strings something happened to display, the source shows every one
// that could be built.
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

// A line that is only a comment: the rule itself is written down in two
// of them (RestoreStickBackupPage.qml, app_settings_controller.hpp), and
// a guard that fails on its own documentation would be uninstallable.
bool isCommentLine(const std::string &line)
{
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return true;
    }
    const std::string rest = line.substr(first);
    return rest.rfind("//", 0) == 0 || rest.rfind("/*", 0) == 0 || rest.rfind("*", 0) == 0;
}

// "file://" + something, in the spellings the three languages here
// allow: QML and JavaScript take either quote and template literals,
// C++ takes double quotes. The trailing slashes vary because
// "file:///" + path is the same mistake with the root already in it.
bool buildsAFileUrlByHand(const std::string &line)
{
    static const std::vector<std::string> patterns = {
        "\"file://\" +", "\"file://\"+", "'file://' +", "'file://'+",
        "\"file:///\" +", "\"file:///\"+", "'file:///' +", "'file:///'+",
        "`file://${", "`file:///${",
    };
    for (const std::string &pattern : patterns) {
        if (line.find(pattern) != std::string::npos) {
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
        if (ext != ".qml" && ext != ".js" && ext != ".cpp" && ext != ".hpp") {
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
            if (!isCommentLine(line) && buildsAFileUrlByHand(line)) {
                hits.push_back({fs::relative(entry.path(), sourceDir).string(), lineNumber, trimmed(line)});
            }
        }
    }

    // The scan has to be able to fail. Finding no files would report
    // success while checking nothing, which is the failure mode this
    // suite keeps running into.
    if (filesScanned == 0) {
        std::cerr << "FAIL: scanned no files under " << uiRoot << ", so this test proved nothing.\n";
        return 1;
    }

    if (!hits.empty()) {
        std::cerr << "FAIL: " << hits.size() << " hand-built file:// URL(s):\n";
        for (const auto &hit : hits) {
            std::cerr << "  " << hit.file << ":" << hit.line << ": " << hit.text << "\n";
        }
        std::cerr << "Paths from a controller are already file:// URLs (gui/local_file_url.hpp,\n"
                     "AppSettingsController::toLocalFileUrl). Use the URL as it arrives.\n";
        return 1;
    }

    std::cout << "no hand-built file:// URLs in " << filesScanned << " GUI source file(s)\n";
    return 0;
}
