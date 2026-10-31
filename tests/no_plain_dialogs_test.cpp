// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// No plain Dialog in the QML: every one is a SeabassDialog (or built on
// one, as MessageDialog is).
//
// A plain Dialog left its buttons flush against the frame's bottom and
// right edges, in the style's colours rather than the app's, with no
// keyboard walk across them: Browse's playlist dialogs did, and six more
// after them (2026-10-31). SeabassDialog insets the footer, draws the one
// frame, wires the keys and keeps the default button. Dialog.Ok and the
// like stay fine: they are the enum, not a block.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include "../src/infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;

int main()
{
    const fs::path sourceDir = seabass::pathFromUtf8(SEABASS_SOURCE_DIR);
    const fs::path qmlRoot = sourceDir / "src" / "gui" / "qml";
    if (!fs::is_directory(qmlRoot)) {
        std::cerr << "FAIL: " << qmlRoot << " is not a directory; this test cannot check anything.\n";
        return 1;
    }

    // A Dialog opening a block, wherever it stands: at the start of a
    // line, or after a property name. The character before it has to be
    // a colon or space, so SeabassDialog, MessageDialog and the rest of
    // ours do not match.
    const std::regex plain(R"((^|[:\s])Dialog\s*\{)");
    const std::regex ours(R"((^|[:\s])SeabassDialog\s*\{)");

    std::vector<std::string> hits;
    std::size_t filesScanned = 0;
    std::size_t seabassDialogs = 0;
    for (const auto &entry : fs::recursive_directory_iterator(qmlRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".qml") {
            continue;
        }
        // The one place a plain Dialog belongs: the base of ours.
        if (entry.path().filename() == "SeabassDialog.qml") {
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
            if (std::regex_search(line, plain)) {
                hits.push_back(seabass::pathToGenericUtf8(fs::relative(entry.path(), sourceDir)) + ":" +
                               std::to_string(lineNumber));
            }
            if (std::regex_search(line, ours)) {
                ++seabassDialogs;
            }
        }
    }

    // It has to be able to fail. No files, or not one SeabassDialog
    // found, means the scan looked in the wrong place.
    if (filesScanned == 0 || seabassDialogs == 0) {
        std::cerr << "FAIL: scanned " << filesScanned << " QML file(s) and found " << seabassDialogs
                  << " SeabassDialog; this test proved nothing.\n";
        return 1;
    }
    if (!hits.empty()) {
        std::cerr << "FAIL: " << hits.size() << " plain Dialog(s); use SeabassDialog:\n";
        for (const auto &hit : hits) {
            std::cerr << "  " << hit << "\n";
        }
        return 1;
    }
    std::cout << "no plain Dialog in " << filesScanned << " QML file(s); " << seabassDialogs
              << " SeabassDialog\n";
    return 0;
}
