// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace seabass::application
{

// What browsing a stick backup produced: a directory shaped like a stick
// root, holding only the catalogs, that MediaController::openFolder() can
// then open like any other folder library.
struct OpenedStickBackup
{
    std::filesystem::path libraryRoot;  // the directory to open; empty on error
    std::string error;                  // non-empty: nothing was extracted
    std::string stickLabel;             // from the backup's manifest, for the row's name
    std::size_t filesExtracted = 0;
    std::uint64_t bytesExtracted = 0;
    // Entries left in the archive on purpose: analysis files (served
    // straight out of the ZIP as they are asked for) and everything else
    // that is not a catalog (audio, artwork). Reported so a caller can
    // say what browsing does and does not include.
    std::size_t analysisFilesLeftInArchive = 0;
    std::size_t otherEntriesSkipped = 0;
};

// Extracts only the catalog files out of a full stick backup, so the
// backup can be browsed like a stick without unpacking it.
//
// Why only those: on a real 6,000-track export the databases and settings
// come to about 8 MB against 641 MB of analysis files. The databases have
// to be real files -- SQLite, SQLCipher and the pdb parser all seek -- but
// the analysis files are wanted one track at a time, so they stay in the
// archive and are read per entry (see
// rekordbox::anlzSourceForPioneerRoot(), which the extracted directory's
// own marker file points at).
//
// Read-only in both directions: the archive is opened read-only, and
// nothing here ever writes back into it. Extracting again over the same
// cache directory is how a caller refreshes it.
class OpenStickBackup
{
public:
    // `cacheRoot` is where the catalogs are written; it is created, and
    // anything already there from an earlier open of the same archive is
    // replaced. Caller's choice of location so this use case never has to
    // know where the app keeps its data.
    static OpenedStickBackup execute(const std::filesystem::path &archivePath,
                                       const std::filesystem::path &cacheRoot);

    // True for an archive entry that must become a real file: the
    // rekordbox database/settings directory and the Engine database
    // directory. Exposed for tests -- this predicate is the whole
    // size argument above.
    static bool isCatalogEntry(const std::string &entryName);

    // True for an analysis file, which is deliberately left where it is.
    static bool isAnalysisEntry(const std::string &entryName);
};

}  // namespace seabass::application
