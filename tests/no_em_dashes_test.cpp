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
// The ASCII "--" is covered too, since 2026-09-25. With the character
// itself banned, a spaced "--" stood in for it in 135 strings across
// src, tools and tests before anyone noticed: the same habit, the same
// text written to explain something, one byte sequence different. It
// is only a dash inside a string literal, though. Comments keep theirs,
// SQL comments start with it, command-line options start with it, and
// a bare "--" is how the pages show a value they do not have. So the
// "--" check looks at string literals alone, and at a "--" with a
// space (or the literal's edge, or a \n) on both sides of it.
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

// U+2014 EM DASH is 0xE2 0x80 0x94 in UTF-8. The character is banned
// outright, comments included: it is the same habit wherever it lands.
bool lineHasEmDash(const std::string &line)
{
    return line.find("\xE2\x80\x94") != std::string::npos;
}

// Also caught: the QML/C++ escapes for it, which render identically and
// would otherwise slip past a byte search. Only inside a string literal,
// and not one that is nothing but the escape: that is a needle for a
// test asserting this very rule against a rendered page. The escapes are
// spelled in two halves so that this file, which is under tests/ and
// therefore scanned, does not report itself.
bool literalHasEmDashEscape(const std::string &body)
{
    static const std::string backslash = "\\";
    static const std::string shortEscape = backslash + "u2014";
    static const std::string longEscape = backslash + "U00002014";
    if (body == shortEscape || body == longEscape) {
        return false;
    }
    return body.find(shortEscape) != std::string::npos || body.find(longEscape) != std::string::npos;
}

// Pulls the bodies of the string literals out of one line of source,
// leaving comments, raw strings and character literals behind. Not a
// parser, just enough of one: a line-at-a-time walk that remembers
// whether it is inside a block comment or a raw string, so that SQL
// written as R"sql(...)sql" (where "--" starts a comment) and a quoted
// phrase in a /* */ comment are never mistaken for text on screen.
class LiteralScanner
{
public:
    explicit LiteralScanner(bool cxx)
        : m_cxx(cxx)
    {
    }

    std::vector<std::string> literalsOn(const std::string &line)
    {
        std::vector<std::string> bodies;
        std::size_t i = 0;
        const std::size_t n = line.size();
        while (i < n) {
            if (!m_rawStringEnd.empty()) {
                const auto end = line.find(m_rawStringEnd, i);
                if (end == std::string::npos) {
                    return bodies;
                }
                i = end + m_rawStringEnd.size();
                m_rawStringEnd.clear();
                continue;
            }
            if (m_inBlockComment) {
                const auto end = line.find("*/", i);
                if (end == std::string::npos) {
                    return bodies;
                }
                i = end + 2;
                m_inBlockComment = false;
                continue;
            }
            const char c = line[i];
            if (c == '/' && i + 1 < n && line[i + 1] == '/') {
                return bodies;
            }
            if (c == '/' && i + 1 < n && line[i + 1] == '*') {
                m_inBlockComment = true;
                i += 2;
                continue;
            }
            if (m_cxx && c == '"' && i > 0 && line[i - 1] == 'R') {
                // R"delim( ... )delim", possibly spanning lines.
                const auto open = line.find('(', i);
                if (open == std::string::npos) {
                    return bodies;
                }
                m_rawStringEnd = ")" + line.substr(i + 1, open - i - 1) + "\"";
                i = open + 1;
                continue;
            }
            if (m_cxx && c == '\'') {
                // A character literal: 'x' or '\n' or '\''. Anything else
                // (a digit separator, an apostrophe in prose) is left alone.
                if (i + 2 < n && line[i + 1] != '\\' && line[i + 2] == '\'') {
                    i += 3;
                    continue;
                }
                if (i + 3 < n && line[i + 1] == '\\' && line[i + 3] == '\'') {
                    i += 4;
                    continue;
                }
                ++i;
                continue;
            }
            const bool opensLiteral = c == '"' || (!m_cxx && (c == '\'' || c == '`'));
            if (!opensLiteral) {
                ++i;
                continue;
            }
            std::string body;
            std::size_t j = i + 1;
            while (j < n && line[j] != c) {
                if (line[j] == '\\' && j + 1 < n) {
                    body += line.substr(j, 2);
                    j += 2;
                    continue;
                }
                body += line[j];
                ++j;
            }
            bodies.push_back(body);
            i = j + 1;
        }
        return bodies;
    }

private:
    bool m_cxx = false;
    bool m_inBlockComment = false;
    std::string m_rawStringEnd;
};

// A "--" standing in for an em-dash: spaced on both sides, where the
// literal's own edge or a line break counts as a side. A literal that
// is nothing but "--" is a placeholder for a missing value, not a dash,
// and "--verbose" is an option.
bool literalHasDash(const std::string &body)
{
    if (body == "--") {
        return false;
    }
    for (auto at = body.find("--"); at != std::string::npos; at = body.find("--", at + 2)) {
        const bool spaceBefore = at == 0 || body[at - 1] == ' ';
        const std::string after = body.substr(at + 2, 2);
        const bool spaceAfter = after.empty() || after[0] == ' ' || after == "\\n" || after == "\\t";
        if (spaceBefore && spaceAfter) {
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
    // Everything a person can read: the GUI and the CLI under src/, the
    // rig and maintenance programs under tools/, and the tests' own
    // output under tests/. The generated rekordbox parser is vendored
    // output, not prose anyone wrote here.
    const std::vector<fs::path> roots = {sourceDir / "src", sourceDir / "tools", sourceDir / "tests"};
    const fs::path generated = sourceDir / "src" / "infrastructure" / "rekordbox" / "generated";
    for (const auto &root : roots) {
        if (!fs::is_directory(root)) {
            std::cerr << "FAIL: " << root << " is not a directory; this test cannot check anything.\n";
            return 1;
        }
    }

    std::vector<Hit> emDashes;
    std::vector<Hit> asciiDashes;
    std::size_t filesScanned = 0;
    for (const auto &root : roots) {
        for (const auto &entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const fs::path ext = entry.path().extension();
            // .qml carries almost every string a user sees; .cpp/.hpp
            // carry the rest (status messages, descriptions, tooltips
            // built controller-side, and everything the CLI prints);
            // .js is what QML pages import.
            if (ext != ".qml" && ext != ".cpp" && ext != ".hpp" && ext != ".js") {
                continue;
            }
            if (seabass::pathToGenericUtf8(entry.path()).rfind(seabass::pathToGenericUtf8(generated), 0) == 0) {
                continue;
            }
            std::ifstream in(entry.path());
            if (!in) {
                continue;
            }
            ++filesScanned;
            const std::string file = seabass::pathToGenericUtf8(fs::relative(entry.path(), sourceDir));
            LiteralScanner scanner(ext == ".cpp" || ext == ".hpp");
            std::string line;
            int lineNumber = 0;
            while (std::getline(in, line)) {
                ++lineNumber;
                bool emDash = lineHasEmDash(line);
                bool asciiDash = false;
                for (const std::string &body : scanner.literalsOn(line)) {
                    emDash = emDash || literalHasEmDashEscape(body);
                    asciiDash = asciiDash || literalHasDash(body);
                }
                if (emDash) {
                    emDashes.push_back({file, lineNumber, trimmed(line)});
                }
                if (asciiDash) {
                    asciiDashes.push_back({file, lineNumber, trimmed(line)});
                }
            }
        }
    }

    // The scan itself has to be able to fail. Finding no files would
    // report success while checking nothing, which is the failure mode
    // this whole suite keeps running into.
    if (filesScanned == 0) {
        std::cerr << "FAIL: scanned no files under " << sourceDir << ", so this test proved nothing.\n";
        return 1;
    }

    if (!emDashes.empty() || !asciiDashes.empty()) {
        if (!emDashes.empty()) {
            std::cerr << "FAIL: " << emDashes.size() << " em-dash(es) in user-visible source:\n";
            for (const auto &hit : emDashes) {
                std::cerr << "  " << hit.file << ":" << hit.line << ": " << hit.text << "\n";
            }
        }
        if (!asciiDashes.empty()) {
            std::cerr << "FAIL: " << asciiDashes.size() << " string(s) with \"--\" used as a dash:\n";
            for (const auto &hit : asciiDashes) {
                std::cerr << "  " << hit.file << ":" << hit.line << ": " << hit.text << "\n";
            }
        }
        std::cerr << "Use a colon, a comma, a full stop or parentheses instead.\n";
        return 1;
    }

    std::cout << "no em-dashes and no \"--\" dashes in " << filesScanned << " user-visible source file(s)\n";
    return 0;
}
