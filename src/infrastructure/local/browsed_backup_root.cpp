// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/browsed_backup_root.hpp"

#include <fstream>
#include <string>
#include <system_error>

#include "infrastructure/durable_file_write.hpp"

namespace seabass::infrastructure::local
{

namespace fs = std::filesystem;

namespace
{

std::string trimmedLine(std::string line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

// The archive named by a marker that is absent, unreadable, or (an older
// single-line form, or a copy) does not name this directory -> nullopt.
std::optional<fs::path> archiveFromValidMarker(const fs::path &libraryRoot)
{
    std::ifstream in(libraryRoot / BrowsedBackupMarkerName);
    if (!in) {
        return std::nullopt;
    }
    std::string archiveLine;
    std::string rootLine;
    std::getline(in, archiveLine);
    std::getline(in, rootLine);
    archiveLine = trimmedLine(std::move(archiveLine));
    rootLine = trimmedLine(std::move(rootLine));
    if (archiveLine.empty() || rootLine.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path here = fs::weakly_canonical(libraryRoot, ec);
    if (ec || here != fs::path(rootLine)) {
        return std::nullopt;
    }
    return fs::path(archiveLine);
}

}  // namespace

bool isBrowsedBackupRoot(const fs::path &libraryRoot)
{
    return archiveFromValidMarker(libraryRoot).has_value();
}

std::optional<fs::path> browsedBackupArchive(const fs::path &libraryRoot)
{
    return archiveFromValidMarker(libraryRoot);
}

std::string browsedBackupRefusal(const std::string &label)
{
    return "\"" + label + "\" is a stick backup being browsed, not a stick. It cannot be written to, backed up or "
           "cloned from; restore it onto a stick first, or open the restored folder.";
}

fs::path canonicalOrAbsolute(const fs::path &path)
{
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    if (ec) {
        return fs::absolute(path);
    }
    return resolved;
}

bool writeBrowsedBackupMarker(const fs::path &markerDir, const fs::path &archivePath, const fs::path &cacheRoot)
{
    std::error_code ec;
    // weakly_canonical resolves what exists and keeps the rest, so the
    // final cache directory need not exist yet when this is written.
    const fs::path root = fs::weakly_canonical(cacheRoot, ec);
    if (ec) {
        return false;
    }
    const fs::path archive = canonicalOrAbsolute(archivePath);
    // Durably: the marker is what makes the whole directory a browsed
    // backup, and a present-but-empty one after a crash would read as a
    // plain folder with no cues. Written with the same primitive as every
    // other catalog file. The archive path is stored canonical, so the
    // open-archive cache keyed on it sees one key per file, whatever
    // spelling the user opened it by.
    return writeFileDurablyAtomic((markerDir / BrowsedBackupMarkerName).string(),
                                  archive.string() + "\n" + root.string() + "\n");
}

}  // namespace seabass::infrastructure::local
