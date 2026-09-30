// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// No icon-only ToolButton in the QML paints its own icon: every one is an
// IconToolButton.
//
// Under KDE's desktop style a ToolButton's icon is painted by QStyle,
// which ignores icon.color, so a bundled Breeze icon came out a dim grey
// on Kelp's dark ground: the header's About and Preferences were almost
// impossible to read on Plasma while the Mac and the Basic test lane
// showed them in full ink (common/IconToolButton.qml has the story).
// IconToolButton draws the glyph itself. A new raw one would bring the
// faint icon back on exactly the platform the author is least likely to
// be looking at, so it fails here instead.
//
// The rule, for a `ToolButton {` block (also after a property name, as in
// `delegate: ToolButton {`), looking only at its own properties, not at
// those of anything declared inside it: it may not set icon.color, and
// it may not set icon.source or icon.name while display is IconOnly.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../src/infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;

namespace
{

// A button that genuinely needs the raw ToolButton, by file and
// objectName, each with the reason.
const std::set<std::pair<std::string, std::string>> kAllowed = {
    // Shows the current playlist's name beside its icon, and
    // IconToolButton draws a glyph only. Its icon is faint under KDE's
    // desktop style too; a labelled variant is a follow-up, not a reason
    // to make IconToolButton draw text.
    {"ScanPage.qml", "playlistSidebarButton"},
};

// The source with comments and the insides of string literals blanked
// to spaces, so a brace or a property name in either counts for nothing.
// Newlines are kept, so offsets still map to the same line numbers.
std::string blankCommentsAndStrings(const std::string &in)
{
    std::string out = in;
    enum { Code, LineComment, BlockComment, String } state = Code;
    char quote = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = in[i];
        const char next = i + 1 < in.size() ? in[i + 1] : '\0';
        switch (state) {
        case Code:
            if (c == '/' && next == '/') {
                state = LineComment;
                out[i] = ' ';
            } else if (c == '/' && next == '*') {
                state = BlockComment;
                out[i] = ' ';
            } else if (c == '"' || c == '\'' || c == '`') {
                state = String;
                quote = c;
            }
            break;
        case LineComment:
            if (c == '\n') {
                state = Code;
            } else {
                out[i] = ' ';
            }
            break;
        case BlockComment:
            if (c == '*' && next == '/') {
                out[i] = ' ';
                out[i + 1] = ' ';
                ++i;
                state = Code;
            } else if (c != '\n') {
                out[i] = ' ';
            }
            break;
        case String:
            if (c == '\\') {
                out[i] = ' ';
                if (i + 1 < out.size() && in[i + 1] != '\n') {
                    out[i + 1] = ' ';
                }
                ++i;
            } else if (c == quote) {
                state = Code;
            } else if (c != '\n') {
                out[i] = ' ';
            }
            break;
        }
    }
    return out;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

int lineOf(const std::string &text, std::size_t offset)
{
    int line = 1;
    for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
        }
    }
    return line;
}

} // namespace

int main()
{
    const fs::path sourceDir = seabass::pathFromUtf8(SEABASS_SOURCE_DIR);
    const fs::path qmlRoot = sourceDir / "src" / "gui" / "qml";
    if (!fs::is_directory(qmlRoot)) {
        std::cerr << "FAIL: " << qmlRoot << " is not a directory; this test cannot check anything.\n";
        return 1;
    }

    // The character before it has to be a colon, a space or the start, so
    // IconToolButton itself does not match.
    const std::regex rawButton(R"((^|[:\s])ToolButton\s*\{)");
    const std::regex ours(R"((^|[:\s])IconToolButton\s*\{)");
    const std::regex iconColor(R"((^|[\s;{])icon\.color\s*:)");
    const std::regex iconSource(R"((^|[\s;{])icon\.(source|name)\s*:)");
    const std::regex iconOnly(R"((^|[\s;{])display\s*:\s*(AbstractButton|Button|ToolButton)\.IconOnly\b)");
    // objectName's value is blanked with the other strings; it is read
    // from the original text at the same offsets.
    const std::regex objectName(R"re((^|[\s;{])objectName\s*:\s*"([^"]*)")re");

    std::vector<std::string> hits;
    std::size_t filesScanned = 0;
    std::size_t iconToolButtons = 0;
    std::size_t rawButtonsChecked = 0;
    std::size_t allowedSeen = 0;
    for (const auto &entry : fs::recursive_directory_iterator(qmlRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".qml") {
            continue;
        }
        // The one place a raw ToolButton belongs: the base of ours.
        if (entry.path().filename() == "IconToolButton.qml") {
            continue;
        }
        const std::string original = readFile(entry.path());
        const std::string code = blankCommentsAndStrings(original);
        ++filesScanned;

        for (auto it = std::sregex_iterator(code.begin(), code.end(), ours); it != std::sregex_iterator(); ++it) {
            ++iconToolButtons;
        }

        for (auto it = std::sregex_iterator(code.begin(), code.end(), rawButton); it != std::sregex_iterator();
             ++it) {
            ++rawButtonsChecked;
            const std::size_t open = static_cast<std::size_t>(it->position(0) + it->length(0)) - 1;
            // The button's own properties: its block with every nested
            // block cut out, so a Menu or a contentItem inside it does
            // not count as the button's.
            std::string own;
            std::string ownOriginal;
            int depth = 0;
            std::size_t i = open;
            for (; i < code.size(); ++i) {
                const char c = code[i];
                if (c == '{') {
                    ++depth;
                    if (depth == 2) {
                        own += ' ';
                        ownOriginal += ' ';
                    }
                } else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                } else if (depth == 1) {
                    own += c;
                    ownOriginal += original[i];
                }
            }
            if (depth != 0) {
                std::cerr << "FAIL: unbalanced braces after the ToolButton at "
                          << seabass::pathToGenericUtf8(fs::relative(entry.path(), sourceDir)) << ":"
                          << lineOf(code, open) << "\n";
                return 1;
            }
            const bool setsColor = std::regex_search(own, iconColor);
            const bool setsIconOnly = std::regex_search(own, iconSource) && std::regex_search(own, iconOnly);
            if (!setsColor && !setsIconOnly) {
                continue;
            }
            std::smatch name;
            const std::string buttonName =
                std::regex_search(ownOriginal, name, objectName) ? name[2].str() : std::string();
            if (kAllowed.count({seabass::pathToUtf8(entry.path().filename()), buttonName}) > 0) {
                ++allowedSeen;
                continue;
            }
            hits.push_back(seabass::pathToGenericUtf8(fs::relative(entry.path(), sourceDir)) + ":" +
                           std::to_string(lineOf(code, open)) +
                           (setsColor ? " (sets icon.color)" : " (icon-only with icon.source)"));
        }
    }

    // It has to be able to fail. No files, not one IconToolButton or not
    // one raw ToolButton looked at means the scan looked in the wrong
    // place; an allowlist entry that matches nothing is stale.
    if (filesScanned == 0 || iconToolButtons == 0 || rawButtonsChecked == 0) {
        std::cerr << "FAIL: scanned " << filesScanned << " QML file(s), found " << iconToolButtons
                  << " IconToolButton and checked " << rawButtonsChecked
                  << " ToolButton; this test proved nothing.\n";
        return 1;
    }
    if (allowedSeen != kAllowed.size()) {
        std::cerr << "FAIL: the allowlist has " << kAllowed.size() << " entr(y/ies) but only " << allowedSeen
                  << " matched a raw ToolButton; drop the stale one.\n";
        return 1;
    }
    if (!hits.empty()) {
        std::cerr << "FAIL: " << hits.size() << " icon-only ToolButton(s) painting their own icon; use IconToolButton:\n";
        for (const auto &hit : hits) {
            std::cerr << "  " << hit << "\n";
        }
        return 1;
    }
    std::cout << "no raw icon-only ToolButton in " << filesScanned << " QML file(s); " << iconToolButtons
              << " IconToolButton, " << rawButtonsChecked << " other ToolButton(s) checked\n";
    return 0;
}
