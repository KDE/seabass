// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// No plain CheckBox in the QML: every one is a SeabassCheckBox.
//
// The platform's own indicator was invisible on Linux: KDE's style paints
// an unticked box in the colour scheme's Button colours, within a shade of
// the rows these boxes sit on (common/SeabassCheckBox.qml has the numbers).
// SeabassCheckBox draws its own. A new plain CheckBox would bring the
// invisible one back on exactly the platform the author is least likely
// to be looking at, so it fails here instead.

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

    // A CheckBox opening a block, wherever it stands: at the start of a
    // line, or after a property name, as in `delegate: CheckBox {`, which
    // is how the Anonymize page's one slipped past the first version of
    // this test. The character before it has to be a colon or space, so
    // SeabassCheckBox itself does not match.
    const std::regex plain(R"((^|[:\s])CheckBox\s*\{)");
    const std::regex ours(R"((^|[:\s])SeabassCheckBox\s*\{)");

    std::vector<std::string> hits;
    std::size_t filesScanned = 0;
    std::size_t seabassCheckBoxes = 0;
    for (const auto &entry : fs::recursive_directory_iterator(qmlRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".qml") {
            continue;
        }
        // The one place a plain CheckBox belongs: the base of ours.
        if (entry.path().filename() == "SeabassCheckBox.qml") {
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
                ++seabassCheckBoxes;
            }
        }
    }

    // It has to be able to fail. No files, or not one SeabassCheckBox
    // found, means the scan looked in the wrong place.
    if (filesScanned == 0 || seabassCheckBoxes == 0) {
        std::cerr << "FAIL: scanned " << filesScanned << " QML file(s) and found " << seabassCheckBoxes
                  << " SeabassCheckBox; this test proved nothing.\n";
        return 1;
    }
    if (!hits.empty()) {
        std::cerr << "FAIL: " << hits.size() << " plain CheckBox(es); use SeabassCheckBox:\n";
        for (const auto &hit : hits) {
            std::cerr << "  " << hit << "\n";
        }
        return 1;
    }
    std::cout << "no plain CheckBox in " << filesScanned << " QML file(s); " << seabassCheckBoxes
              << " SeabassCheckBox\n";
    return 0;
}
