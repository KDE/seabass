// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// Importing audio files from the computer onto a stick as new tracks:
// what happens to each file offered, decided before a byte is copied.
// Pure: the files' facts, the stick's files and the stick's library rows
// come in, the filesystem is asked only through ImportPlanTools.

// What the plan does with one offered file.
enum class ImportVerdict {
    // Copied to `destination` and given a row in the stick's libraries.
    Import,
    // These bytes are on the stick already (`existingPath`): the very file
    // offered, or a copy of it under any name. Nothing is copied.
    // `inLibrary` says whether a library lists that file; when none does,
    // the file on the stick gets the rows.
    AlreadyOnStick,
    // The library holds another file with this artist and title (or, with
    // no tags, this file name) and the same length: `existingPath`. It
    // may be the same recording as another encode or another mix that
    // runs as long. A person decides; ImportChoices::importAnyway makes
    // it an Import.
    PossibleDuplicate,
    // The same file is imported, or found on the stick, earlier in the
    // list (`existingPath` is that earlier source), which carries it.
    OfferedTwice,
    // Not an extension the stick's libraries take.
    UnsupportedFormat,
    // No tags reader could find a playing length in it, or its size is
    // not known: a file this code cannot understand does not become a
    // track.
    Unreadable,
    // Larger than one file may be on the stick's filesystem.
    TooLargeForFilesystem,
    // The stick has no room left for it after the files before it.
    DoesNotFit,
};

// One file offered, as the computer has it.
struct ImportFile
{
    // filePath is the source, absolute; filename, fileSizeBytes, and what
    // the tags gave: title, artist, album, durationSeconds. The shape
    // application::unreferencedFilesAsTracks gives a file (format "disk").
    Track file;
    // False when the tags reader could not read the file at all.
    bool readable = false;
};

// One audio file on the stick, catalogued or not.
struct StickAudioFile
{
    std::string path;  // absolute, as the library rows spell their filePath
    std::uint64_t sizeBytes = 0;
};

struct ImportStick
{
    std::string root;  // the stick's mount point, absolute
    // Every row of every library on the stick.
    std::vector<Track> libraryTracks;
    // Every audio file on the stick.
    std::vector<StickAudioFile> files;
    std::uint64_t freeBytes = 0;
    // Kept free whatever is imported: the libraries and their undo copies
    // are written after the audio.
    std::uint64_t reserveBytes = 0;
    // The filesystem's allocation unit: a file takes whole ones, and a
    // new folder one. 0 when not known, which counts bytes as they are.
    std::uint64_t clusterBytes = 0;
    // The largest file the filesystem holds; 0 for no limit worth
    // checking. See maxFileBytesFor().
    std::uint64_t maxFileBytes = 0;
    // Lowercase, with the dot. See importableExtensions().
    std::set<std::string> acceptedExtensions;
};

// Both required: planTrackImport throws std::invalid_argument without
// them, and for a stick whose root is empty or the filesystem's own root.
struct ImportPlanTools
{
    // application::normalizedPathKey.
    std::function<std::string(const std::string &)> pathKey;
    // Whether two files hold the same bytes. Asked only for two files of
    // one size. False when it cannot tell.
    std::function<bool(const std::string &, const std::string &)> sameBytes;
};

struct ImportChoices
{
    // Source paths of PossibleDuplicate files to import all the same.
    std::set<std::string> importAnyway;
};

struct ImportPlanEntry
{
    ImportFile offered;
    ImportVerdict verdict = ImportVerdict::Unreadable;
    // Import: where the file goes, relative to the stick's root, with
    // forward slashes. See importDestination().
    std::string destination;
    // AlreadyOnStick and PossibleDuplicate: the file on the stick,
    // absolute. OfferedTwice: the earlier source.
    std::string existingPath;
    // AlreadyOnStick: a library has a row for existingPath.
    bool inLibrary = false;
    // Import: this was a PossibleDuplicate the person chose to import.
    bool importedAnyway = false;
};

struct ImportPlan
{
    // One per offered file, in the order offered.
    std::vector<ImportPlanEntry> entries;
    // The Import entries together.
    std::uint64_t bytesToCopy = 0;

    size_t count(ImportVerdict verdict) const;
};

// Decides every offered file, in the order offered. The song match is
// domain::matchTracks across libraries: artist and title, or the file
// name for a file without tags, and lengths within
// MatchingPolicy::exactMatchSeconds().
ImportPlan planTrackImport(const std::vector<ImportFile> &offered, const ImportStick &stick,
                           const ImportPlanTools &tools, const ImportChoices &choices);

// One path segment every filesystem a stick is formatted as accepts, and
// Windows can open again: the characters FAT32 and exFAT refuse
// (\ / : * ? " < > |) become '_', control characters and bytes that are
// not UTF-8 go, and so do dots and spaces at the start and at the
// end (Windows strips those and then cannot find the file). A name
// Windows reserves (CON, NUL, COM1 ...) gets a '_' in front. Cut to
// `maxUtf16Units` on a character boundary; `fallback` when nothing is
// left.
//
// This renames, it does not transliterate: the library row keeps the
// artist and title as the tags spell them, only the folder and file name
// on the stick differ, and the plan shows them.
std::string stickSafePathSegment(std::string_view text, std::string_view fallback, size_t maxUtf16Units = 120);

// The same for a file name, cutting the stem and keeping the extension.
std::string stickSafeFileName(std::string_view fileName, size_t maxUtf16Units = 120);

// Where an imported file goes, relative to the stick's root:
// "Contents/<artist>/<album>/<file name>", the layout rekordbox exports
// in, with its "UnknownArtist" and "UnknownAlbum" for a file whose tags
// name neither. The file keeps its own name. Not yet made unique: the
// plan adds " (2)" before the extension when another file has the place.
std::string importDestination(const Track &file);

// The extensions (lowercase, with the dot) a stick with these libraries
// takes: what every one of them can list, since a track belongs in all of
// a stick's libraries. `formats` are Track::format values ("rekordbox",
// "onelibrary", "engine"); none gives an empty set.
//
// The lists follow what rekordbox and Engine DJ document as importable,
// and no player was asked: an XDJ-RX2 plays no FLAC although rekordbox
// exports it to a stick. Closed on purpose, like isAudioExtension().
std::set<std::string> importableExtensions(const std::set<std::string> &formats);

// The largest file a filesystem holds, by the lowercase type name the OS
// reports ("vfat", "msdos", "fat32": 4 GiB less one byte; "fat", "fat16",
// "fat12": 2 GiB less one); 0 for the rest.
std::uint64_t maxFileBytesFor(const std::string &filesystem);

}  // namespace seabass::domain
