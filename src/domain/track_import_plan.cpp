// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/track_import_plan.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#include "domain/track_matching.hpp"

namespace seabass::domain
{

namespace
{

std::string basenameOf(const std::string &path)
{
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string asciiLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string asciiUpper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

// ".mp3" for "Song.MP3"; empty for a name without one, and for a name
// that is nothing but an extension (".hidden").
std::string extensionOf(const std::string &fileName)
{
    const size_t dot = fileName.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return {};
    }
    return asciiLower(fileName.substr(dot));
}

// One character of UTF-8 text.
struct CodePoint
{
    std::string_view bytes;
    char32_t value = 0;
    size_t utf16Units() const
    {
        return value > 0xFFFF ? 2 : 1;
    }
};

// The character at the front of `text`, or nullopt when the bytes there
// are not one: a stray continuation byte, a cut-off sequence, an overlong
// form, a surrogate. The caller then skips one byte.
std::optional<CodePoint> decodeFront(std::string_view text)
{
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(0);
    size_t length = 0;
    char32_t value = 0;
    char32_t smallest = 0;
    if (lead < 0x80) {
        return CodePoint{text.substr(0, 1), lead};
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
        smallest = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
        smallest = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
        smallest = 0x10000;
    } else {
        return std::nullopt;
    }
    if (text.size() < length) {
        return std::nullopt;
    }
    for (size_t i = 1; i < length; ++i) {
        if ((byte(i) & 0xC0) != 0x80) {
            return std::nullopt;
        }
        value = (value << 6) | (byte(i) & 0x3F);
    }
    if (value < smallest || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return std::nullopt;
    }
    return CodePoint{text.substr(0, length), value};
}

bool refusedOnStick(char32_t c)
{
    switch (c) {
    case U'\\':
    case U'/':
    case U':':
    case U'*':
    case U'?':
    case U'"':
    case U'<':
    case U'>':
    case U'|':
        return true;
    default:
        return false;
    }
}

void trimEnd(std::string &text)
{
    while (!text.empty() && (text.back() == '.' || text.back() == ' ')) {
        text.pop_back();
    }
}

// CON, PRN, AUX, NUL, CONIN$, CONOUT$, COM1 to COM9 and LPT1 to LPT9
// (the digit also as a superscript one, two or three), with or without
// an extension after them, and with spaces before the dot: Windows opens
// the device, not the file.
bool reservedOnWindows(const std::string &segment)
{
    std::string stem = asciiUpper(segment.substr(0, segment.find('.')));
    while (!stem.empty() && stem.back() == ' ') {
        stem.pop_back();
    }
    static const std::array<const char *, 6> Devices = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    if (std::find(Devices.begin(), Devices.end(), stem) != Devices.end()) {
        return true;
    }
    if (stem.compare(0, 3, "COM") != 0 && stem.compare(0, 3, "LPT") != 0) {
        return false;
    }
    const std::string digit = stem.substr(3);
    return (digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9') || digit == "\xc2\xb9" || digit == "\xc2\xb2"
        || digit == "\xc2\xb3";
}

// The last `segments` segments of a path as it is spelled, joined with
// forward slashes: a file's place under the stick's root, where the key
// says how deep it sits and the path keeps the spelling. Empty segments
// and "." are not counted, as the key does not count them.
std::string lastSegments(const std::string &path, size_t segments)
{
    std::vector<std::string> kept;
    size_t end = path.size();
    while (kept.size() < segments && end > 0) {
        const size_t cut = path.find_last_of("/\\", end - 1);
        const size_t start = cut == std::string::npos ? 0 : cut + 1;
        const std::string segment = path.substr(start, end - start);
        if (!segment.empty() && segment != ".") {
            kept.push_back(segment);
        }
        end = cut == std::string::npos ? 0 : cut;
    }
    std::string relative;
    for (auto segment = kept.rbegin(); segment != kept.rend(); ++segment) {
        relative += (relative.empty() ? "" : "/") + *segment;
    }
    return relative;
}

// "name (2).mp3" for "name.mp3".
std::string numbered(const std::string &relativePath, int number)
{
    const size_t slash = relativePath.find_last_of('/');
    const size_t dot = relativePath.find_last_of('.');
    const std::string suffix = " (" + std::to_string(number) + ")";
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash + 2)) {
        return relativePath + suffix;
    }
    return relativePath.substr(0, dot) + suffix + relativePath.substr(dot);
}

}  // namespace

size_t ImportPlan::count(ImportVerdict verdict) const
{
    return static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
                                             [&](const ImportPlanEntry &entry) { return entry.verdict == verdict; }));
}

std::string stickSafePathSegment(std::string_view text, std::string_view fallback, size_t maxUtf16Units)
{
    std::string safe;
    size_t units = 0;
    while (!text.empty()) {
        const auto c = decodeFront(text);
        if (!c) {
            text.remove_prefix(1);  // not UTF-8: the byte goes
            continue;
        }
        text.remove_prefix(c->bytes.size());
        if (c->value < 0x20 || c->value == 0x7F) {
            continue;
        }
        // Nothing in front: a space Windows would strip, a dot that hides
        // the folder on macOS and Linux.
        if (safe.empty() && (c->value == U' ' || c->value == U'.')) {
            continue;
        }
        if (units + c->utf16Units() > maxUtf16Units) {
            break;
        }
        units += c->utf16Units();
        if (refusedOnStick(c->value)) {
            safe.push_back('_');
        } else {
            safe.append(c->bytes);
        }
    }
    trimEnd(safe);
    if (safe.empty()) {
        return std::string(fallback);
    }
    if (reservedOnWindows(safe)) {
        safe.insert(safe.begin(), '_');
    }
    return safe;
}

std::string stickSafeFileName(std::string_view fileName, size_t maxUtf16Units)
{
    // An extension is short, plain letters and digits after the last
    // dot. Anything else after a dot ("Song feat. Someone") is part of
    // the name.
    std::string_view stem = fileName;
    std::string_view extension;
    const size_t dot = fileName.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0) {
        const std::string_view tail = fileName.substr(dot + 1);
        const bool plain = !tail.empty() && tail.size() <= 8
            && std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
        if (plain) {
            stem = fileName.substr(0, dot);
            extension = tail;
        }
    }
    const size_t taken = extension.empty() ? 0 : extension.size() + 1;
    const size_t room = maxUtf16Units > taken + 1 ? maxUtf16Units - taken : 1;
    std::string name = stickSafePathSegment(stem, "track", room);
    if (!extension.empty()) {
        name += ".";
        name += extension;
    }
    return name;
}

std::string importDestination(const Track &file)
{
    const std::string name = file.filename.empty() ? basenameOf(file.filePath) : file.filename;
    // 60 units a folder and 120 the name, to stay near the 260 Windows
    // gives a whole path by default. Not a promise: the plan takes a
    // folder's spelling from the stick, which may be longer, and adds
    // " (2)" to a taken name.
    return "Contents/" + stickSafePathSegment(file.artist, "UnknownArtist", 60) + "/"
        + stickSafePathSegment(file.album, "UnknownAlbum", 60) + "/" + stickSafeFileName(name);
}

std::set<std::string> importableExtensions(const std::set<std::string> &formats)
{
    // What the Pioneer row writers have a file type number for: an .mp4
    // has none anyone measured, so a stick with a Pioneer library takes
    // none (PdbRowWriter and OneLibraryCueWriter refuse it).
    static const std::set<std::string> Pioneer = {".mp3", ".m4a", ".wav", ".aif", ".aiff", ".flac"};
    static const std::set<std::string> Engine = {".mp3", ".m4a", ".mp4", ".wav", ".aif", ".aiff", ".flac", ".ogg"};

    std::optional<std::set<std::string>> accepted;
    for (const std::string &format : formats) {
        const std::set<std::string> *takes = nullptr;
        if (format == "rekordbox" || format == "onelibrary") {
            takes = &Pioneer;
        } else if (format == "engine") {
            takes = &Engine;
        } else {
            // A library this does not know takes nothing it can vouch for.
            return {};
        }
        if (!accepted) {
            accepted = *takes;
            continue;
        }
        std::set<std::string> both;
        std::set_intersection(accepted->begin(), accepted->end(), takes->begin(), takes->end(),
                              std::inserter(both, both.begin()));
        accepted = std::move(both);
    }
    return accepted.value_or(std::set<std::string>{});
}

std::uint64_t maxFileBytesFor(const std::string &filesystem)
{
    const std::string fs = asciiLower(filesystem);
    if (fs == "vfat" || fs == "fat32" || fs == "msdos") {
        return (std::uint64_t{4} << 30) - 1;
    }
    // What Windows calls FAT16 and FAT12.
    if (fs == "fat" || fs == "fat16" || fs == "fat12") {
        return (std::uint64_t{2} << 30) - 1;
    }
    return 0;
}

ImportPlan planTrackImport(const std::vector<ImportFile> &offered, const ImportStick &stick,
                           const ImportPlanTools &tools, const ImportChoices &choices)
{
    // A plan made without these would not be a cautious plan, it would be
    // a wrong one: with no root every path on the computer reads as "on
    // the stick". The caller's mistake, said at once.
    if (!tools.pathKey || !tools.sameBytes) {
        throw std::invalid_argument("planTrackImport: pathKey and sameBytes are both required");
    }
    std::string rootKey = tools.pathKey(stick.root);
    while (!rootKey.empty() && rootKey.back() == '/') {
        rootKey.pop_back();
    }
    if (rootKey.empty()) {
        throw std::invalid_argument("planTrackImport: the stick has no root folder of its own");
    }

    ImportPlan plan;
    plan.entries.reserve(offered.size());

    const auto absolute = [&](const std::string &relative) {
        const bool slash = stick.root.back() == '/' || stick.root.back() == '\\';
        return stick.root + (slash ? "" : "/") + relative;
    };
    const auto onStick = [&](const std::string &key) {
        return key.size() > rootKey.size() + 1 && key.compare(0, rootKey.size(), rootKey) == 0
            && key[rootKey.size()] == '/';
    };

    // The stick as it is: which paths are taken, which files have a row,
    // which files have which size, and how each folder is spelled.
    std::set<std::string> takenKeys;
    std::map<std::string, const StickAudioFile *> stickFileByKey;
    std::map<std::uint64_t, std::vector<const StickAudioFile *>> stickFilesBySize;
    // Key of a folder, relative to the root, to its spelling on the
    // stick. FAT32 and exFAT ignore letter case, so "Contents/Akki (De)"
    // and "Contents/AKKI (DE)" are one folder there, and a row that
    // spells it the second way sits beside rows that spell it the first
    // (#86). A new file takes the spelling the folder has.
    std::map<std::string, std::string> folderSpelling;
    const auto noteFolders = [&](const std::string &relativePath) {
        for (size_t slash = relativePath.find('/'); slash != std::string::npos;
             slash = relativePath.find('/', slash + 1)) {
            const std::string folder = relativePath.substr(0, slash);
            folderSpelling.emplace(tools.pathKey(folder), folder);
        }
    };
    for (const StickAudioFile &file : stick.files) {
        const std::string key = tools.pathKey(file.path);
        takenKeys.insert(key);
        stickFileByKey.emplace(key, &file);
        // A size of 0 is a size nobody read: nothing to compare by.
        if (file.sizeBytes > 0) {
            stickFilesBySize[file.sizeBytes].push_back(&file);
        }
        if (onStick(key)) {
            const std::string_view under = std::string_view(key).substr(rootKey.size() + 1);
            const size_t depth = static_cast<size_t>(std::count(under.begin(), under.end(), '/')) + 1;
            noteFolders(lastSegments(file.path, depth));
        }
    }
    // One row per file for the song match, whichever library it is from:
    // a stick with three libraries lists one song three times, and to
    // matchTracks three rows under one artist and title are three songs
    // it cannot tell apart. The row that knows its length is kept.
    std::set<std::string> libraryKeys;
    std::vector<Track> songs;
    std::map<std::string, size_t> songByKey;
    for (const Track &row : stick.libraryTracks) {
        if (row.filePath.empty()) {
            // A row that names no file is still a song the library has.
            songs.push_back(row);
            continue;
        }
        const std::string key = tools.pathKey(row.filePath);
        libraryKeys.insert(key);
        // A place a row names is taken although no file is there: a file
        // copied to it would give that row, with its cues and its grid,
        // another recording to play.
        takenKeys.insert(key);
        const auto [known, isNew] = songByKey.emplace(key, songs.size());
        if (isNew) {
            songs.push_back(row);
        } else if (songs[known->second].durationSeconds <= 0.0 && row.durationSeconds > 0.0) {
            songs[known->second] = row;
        }
    }

    // What can be decided at all: a format the stick takes, a length, a
    // size. Only these are asked about as songs, so a file that is
    // refused does not make another file's artist and title ambiguous.
    std::vector<Track> files;
    files.reserve(offered.size());
    std::vector<Track> decidable;
    std::vector<size_t> decidableIndex(offered.size(), offered.size());
    for (size_t i = 0; i < offered.size(); ++i) {
        files.push_back(offered[i].file);
        if (files.back().filename.empty()) {
            files.back().filename = basenameOf(files.back().filePath);
        }
        const Track &file = files.back();
        if (stick.acceptedExtensions.count(extensionOf(file.filename)) > 0 && offered[i].readable
            && file.durationSeconds > 0.0 && file.fileSizeBytes > 0) {
            decidableIndex[i] = decidable.size();
            decidable.push_back(file);
        }
    }
    std::map<size_t, const Track *> songOnStick;
    for (const auto &[mine, theirs] : matchTracks(decidable, songs)) {
        songOnStick.emplace(static_cast<size_t>(mine - decidable.data()), theirs);
    }

    // Room, counted as the filesystem hands it out: whole clusters for a
    // file, and one for each folder that has to be made.
    const std::uint64_t cluster = std::max<std::uint64_t>(stick.clusterBytes, 1);
    const auto allocated = [&](std::uint64_t bytes) {
        const std::uint64_t clusters = bytes / cluster + (bytes % cluster != 0 ? 1 : 0);
        return clusters > UINT64_MAX / cluster ? UINT64_MAX : clusters * cluster;
    };
    std::uint64_t room = stick.freeBytes > stick.reserveBytes ? stick.freeBytes - stick.reserveBytes : 0;

    // Files of this plan that are imported or found on the stick, by
    // source and by size: the ones a later copy of the same file is
    // "offered twice" against. A file left out (a possible duplicate
    // nobody chose, one that does not fit) carries nothing, so its copy
    // is decided on its own.
    std::map<std::string, std::string> carriedBySourceKey;
    std::map<std::uint64_t, std::vector<std::string>> carriedBySize;
    // Stick files an earlier entry was found to be, by key: the file gets
    // its rows once, however many copies of it are offered.
    std::map<std::string, std::string> carriedByStickKey;

    for (size_t i = 0; i < offered.size(); ++i) {
        ImportPlanEntry entry;
        entry.offered = offered[i];
        const Track &file = files[i];
        const std::string &source = file.filePath;
        const std::string sourceKey = tools.pathKey(source);
        std::string destination;
        std::uint64_t cost = 0;

        const auto decide = [&]() {
            if (stick.acceptedExtensions.count(extensionOf(file.filename)) == 0) {
                return ImportVerdict::UnsupportedFormat;
            }
            // No length, or a size nobody could read (0 is "not known" in
            // a Track): nothing to check room or duplicates with.
            if (decidableIndex[i] == offered.size()) {
                return ImportVerdict::Unreadable;
            }

            if (const auto earlier = carriedBySourceKey.find(sourceKey); earlier != carriedBySourceKey.end()) {
                entry.existingPath = earlier->second;
                return ImportVerdict::OfferedTwice;
            }

            if (onStick(sourceKey)) {
                // Spelled as the stick's own listing spells it, where it
                // has the file: the offered path may differ in case.
                const auto listed = stickFileByKey.find(sourceKey);
                entry.existingPath = listed != stickFileByKey.end() ? listed->second->path : source;
                entry.inLibrary = libraryKeys.count(sourceKey) > 0;
                return ImportVerdict::AlreadyOnStick;
            }

            if (const auto sameSize = carriedBySize.find(file.fileSizeBytes); sameSize != carriedBySize.end()) {
                for (const std::string &earlier : sameSize->second) {
                    if (tools.sameBytes(source, earlier)) {
                        entry.existingPath = earlier;
                        return ImportVerdict::OfferedTwice;
                    }
                }
            }

            if (const auto sameSize = stickFilesBySize.find(file.fileSizeBytes); sameSize != stickFilesBySize.end()) {
                for (const StickAudioFile *onTheStick : sameSize->second) {
                    // A copy under a name the libraries do not take
                    // cannot carry the row: the file is imported under
                    // its own.
                    if (stick.acceptedExtensions.count(extensionOf(basenameOf(onTheStick->path))) == 0) {
                        continue;
                    }
                    if (tools.sameBytes(source, onTheStick->path)) {
                        entry.existingPath = onTheStick->path;
                        entry.inLibrary = libraryKeys.count(tools.pathKey(onTheStick->path)) > 0;
                        return ImportVerdict::AlreadyOnStick;
                    }
                }
            }

            if (stick.maxFileBytes > 0 && file.fileSizeBytes > stick.maxFileBytes) {
                return ImportVerdict::TooLargeForFilesystem;
            }

            if (const auto song = songOnStick.find(decidableIndex[i]); song != songOnStick.end()) {
                if (choices.importAnyway.count(source) == 0) {
                    entry.existingPath = song->second->filePath;
                    return ImportVerdict::PossibleDuplicate;
                }
                entry.importedAnyway = true;
            }

            // The folders as the stick spells them, then a name nothing
            // else has: no file on the stick, no row of a library, no
            // file planned before.
            const std::string wanted = importDestination(file);
            const size_t nameAt = wanted.find_last_of('/');
            std::string folder = wanted.substr(0, nameAt);
            for (size_t cut = folder.size(); cut != std::string::npos && cut > 0;
                 cut = folder.find_last_of('/', cut - 1)) {
                const auto spelled = folderSpelling.find(tools.pathKey(folder.substr(0, cut)));
                if (spelled != folderSpelling.end()) {
                    folder = spelled->second + folder.substr(cut);
                    break;
                }
            }
            const std::string placed = folder + wanted.substr(nameAt);
            destination = placed;
            for (int number = 2; takenKeys.count(tools.pathKey(absolute(destination))) > 0; ++number) {
                destination = numbered(placed, number);
            }

            std::uint64_t newFolders = 0;
            for (size_t slash = folder.find('/'); ; slash = folder.find('/', slash + 1)) {
                const std::string upTo = folder.substr(0, slash);
                newFolders += folderSpelling.count(tools.pathKey(upTo)) == 0 ? 1 : 0;
                if (slash == std::string::npos) {
                    break;
                }
            }
            const std::uint64_t forFile = allocated(file.fileSizeBytes);
            // No folder cost where the unit is not known: a byte each would
            // be a number made up.
            const std::uint64_t forFolders = newFolders * stick.clusterBytes;
            cost = forFile > UINT64_MAX - forFolders ? UINT64_MAX : forFile + forFolders;
            if (cost > room) {
                entry.importedAnyway = false;
                return ImportVerdict::DoesNotFit;
            }
            return ImportVerdict::Import;
        };

        entry.verdict = decide();
        if (entry.verdict == ImportVerdict::AlreadyOnStick) {
            const auto [earlier, first] = carriedByStickKey.emplace(tools.pathKey(entry.existingPath), source);
            if (!first) {
                entry.verdict = ImportVerdict::OfferedTwice;
                entry.existingPath = earlier->second;
                entry.inLibrary = false;
            }
        }
        if (entry.verdict == ImportVerdict::Import) {
            takenKeys.insert(tools.pathKey(absolute(destination)));
            noteFolders(destination);
            entry.destination = destination;
            plan.bytesToCopy += file.fileSizeBytes;
            room -= cost;
        }
        if (entry.verdict == ImportVerdict::Import || entry.verdict == ImportVerdict::AlreadyOnStick) {
            carriedBySourceKey.emplace(sourceKey, source);
            if (entry.verdict == ImportVerdict::Import) {
                carriedBySize[file.fileSizeBytes].push_back(source);
            }
        }
        plan.entries.push_back(std::move(entry));
    }
    return plan;
}

}  // namespace seabass::domain
