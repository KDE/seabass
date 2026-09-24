// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// No path crosses between std::string and std::filesystem::path through
// the ANSI code page.
//
// On Windows, path::string() and path(std::string) go through the
// process code page: the first throws for any character outside it, the
// second names a file that does not exist. Both are silent on Linux and
// macOS, so nothing here catches them until a stick with a Japanese
// artist folder meets the Windows build -- shakedown round 8 lost a
// benchmark and a restore to exactly that. The rule and the two
// functions everything goes through are in
// src/infrastructure/paths/utf8_path.hpp.
//
// A guard on the source, like no_raw_future_reads_test: the failure it
// prevents cannot be provoked on the platforms that run this suite most,
// and what is worth pinning is that no new call site appears. It is
// deliberately blunt. A spelling that is correct in its one place, such as
// LoadLibraryA on a DLL name that is ASCII by construction, carries a
// "narrow-ok:" comment with the reason, and the test skips that line.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct Hit
{
    std::string file;
    int line = 0;
    std::string why;
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
    return rest == "*" || rest.rfind("* ", 0) == 0;
}

// What a line is checked against, in order. The regexes are anchored on
// spellings, not types: a scan cannot know that `x` is a path, so it
// flags the conversions that only ever make sense on one.
struct Rule
{
    const char *why;
    std::regex pattern;
};

const std::vector<Rule> &rules()
{
    static const std::vector<Rule> all = {
        // path::string() / generic_string(): narrow through the code page.
        // u8string() and generic_u8string() do not match: the dot is in
        // front of the "u8".
        {"path::string() narrows through the ANSI code page on Windows; use pathToUtf8()",
         std::regex(R"(\.(generic_)?string\(\))")},
        // A path built from a QString's UTF-8 bytes, read as ANSI.
        {"fs::path(qstring.toStdString()) reads UTF-8 as ANSI on Windows; use pathFromQString()",
         std::regex(R"(path\s*\(\s*[^()]*\.toStdString\(\))")},
        {"path(u8\"...\") aside, path(std::string) reads ANSI on Windows; use pathFromUtf8()",
         std::regex(R"(path\s*\(\s*std::string\s*\()")},
        // A stream opened by a narrow string: MSVC reads it as ANSI. Open
        // it with the fs::path itself.
        {"a stream opened with .toStdString() reads ANSI on Windows; open it with the path",
         std::regex(R"((ifstream|ofstream|fstream)\s*\w*\s*[({][^;]*\.toStdString\(\))")},
        // The ANSI Win32 file API. The wide call takes path::c_str().
        {"ANSI Win32 call; use the W version with the path's c_str()",
         std::regex(R"(\b(CreateFile|DeleteFile|MoveFile|MoveFileEx|CopyFile|ReplaceFile|GetFileAttributes|)"
                    R"(SetFileAttributes|FindFirstFile|FindFirstFileEx|CreateDirectory|RemoveDirectory|)"
                    R"(GetVolumeInformation|GetDiskFreeSpace|GetDiskFreeSpaceEx|GetFullPathName|GetLongPathName|)"
                    R"(GetShortPathName|GetTempPath|GetVolumePathName|GetVolumeNameForVolumeMountPoint|)"
                    R"(SetCurrentDirectory|GetCurrentDirectory|ShellExecute|LoadLibrary|GetModuleFileName|)"
                    R"(CreateProcess|OpenFile|_mkdir|_rmdir|_unlink|_access|_stat|_open|fopen)A?\s*\()")},
        // Qt's "local 8-bit" is the ANSI code page on Windows.
        {"toLocal8Bit/fromLocal8Bit is the ANSI code page on Windows; paths are UTF-8",
         std::regex(R"((toLocal8Bit|fromLocal8Bit)\s*\()")},
    };
    return all;
}

// The Win32 rule above lists the narrow CRT names without the A suffix;
// the plain POSIX-looking calls (fopen, _open ...) it catches are wrong
// on Windows for the same reason, and a POSIX-only file behind #if
// defined(__linux__) says so with the marker. The wide CRT calls
// (_wfopen, _wrename, _wmkdir ...) are what those are replaced with, and
// are not matched.
bool allowedByMarker(const std::string &line)
{
    return line.find("narrow-ok:") != std::string::npos;
}

std::string trimmed(const std::string &line)
{
    const auto first = line.find_first_not_of(" \t");
    std::string out = first == std::string::npos ? line : line.substr(first);
    if (out.size() > 110) {
        out = out.substr(0, 107) + "...";
    }
    return out;
}

// What is scanned. tests/ and tools/ are held to the same rule: a test
// that builds an ANSI string for the code under test proves nothing on
// Windows, and the rig runs there.
const std::vector<std::string> &scannedRoots()
{
    static const std::vector<std::string> roots = {"src", "tools", "tests"};
    return roots;
}

bool excluded(const fs::path &relative)
{
    static const std::vector<std::string> prefixes = {
        // Generated by kaitai-struct from the .ksy; not hand-written.
        "src/infrastructure/rekordbox/generated/",
    };
    static const std::vector<std::string> files = {
        // The conversions themselves live here, in u8string terms.
        "src/infrastructure/paths/utf8_path.hpp",
        "src/storageprobe/utf8_path.hpp",
        "tests/no_narrow_path_conversions_test.cpp",
    };
    const std::string generic = relative.generic_string();
    for (const auto &prefix : prefixes) {
        if (generic.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    for (const auto &file : files) {
        if (generic == file) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main()
{
    const fs::path sourceDir = fs::path(SEABASS_SOURCE_DIR);
    std::vector<Hit> hits;
    std::size_t filesScanned = 0;
    for (const std::string &rootName : scannedRoots()) {
        const fs::path root = sourceDir / rootName;
        if (!fs::is_directory(root)) {
            std::cerr << "FAIL: " << root << " is not a directory; this test cannot check anything.\n";
            return 1;
        }
        for (const auto &entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const fs::path ext = entry.path().extension();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".mm") {
                continue;
            }
            const fs::path relative = entry.path().lexically_relative(sourceDir);
            if (excluded(relative)) {
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
                if (isCommentLine(line) || allowedByMarker(line)) {
                    continue;
                }
                for (const Rule &rule : rules()) {
                    if (std::regex_search(line, rule.pattern)) {
                        hits.push_back({relative.generic_string(), lineNumber, rule.why, trimmed(line)});
                        break;
                    }
                }
            }
        }
    }

    if (filesScanned < 100) {
        std::cerr << "FAIL: scanned only " << filesScanned << " files; the tree layout has changed under this test.\n";
        return 1;
    }
    if (!hits.empty()) {
        std::cerr << "FAIL: " << hits.size() << " path conversion(s) go through the ANSI code page on Windows "
                  << "(see src/infrastructure/paths/utf8_path.hpp; a deliberate one carries a \"narrow-ok:\" comment):\n";
        for (const Hit &hit : hits) {
            std::cerr << "  " << hit.file << ":" << hit.line << ": " << hit.text << "\n      " << hit.why << "\n";
        }
        return 1;
    }
    std::cout << "no_narrow_path_conversions_test passed (" << filesScanned << " files scanned)\n";
    return 0;
}
