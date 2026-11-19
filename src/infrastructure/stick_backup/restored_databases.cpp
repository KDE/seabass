// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/restored_databases.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "application/path_key.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"

namespace seabass::infrastructure::stick_backup
{

namespace fs = std::filesystem;

namespace
{

// One line per database, then one per file the restore put beside it:
//   database<TAB>path<TAB>archive fingerprint<TAB>written fingerprint<TAB>content hash<TAB>target label
//   file<TAB>database path<TAB>file path
// Version 1 had neither the content hash nor the label; a record of it is
// read as no record (nothing before 1.0 is kept compatible), which only
// ever makes a database read as changed.
constexpr std::string_view Header = "seabass-restored-databases\t2";

fs::path recordPath(const fs::path &stickRoot)
{
    return stickRoot / pathFromUtf8(std::string(RestoredDatabasesPath));
}

std::vector<std::string> splitTabs(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) {
            return fields;
        }
        start = tab + 1;
    }
}

// Well-formed UTF-8, no overlongs, no surrogates, nothing past U+10FFFF.
// Every string the record holds becomes a path somewhere (pathFromUtf8),
// and on Windows that conversion throws on anything else.
bool isValidUtf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t point = 0;
        if (byte < 0x80) {
            ++i;
            continue;
        } else if (byte >= 0xc2 && byte <= 0xdf) {
            length = 2;
            point = byte & 0x1f;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            length = 3;
            point = byte & 0x0f;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            length = 4;
            point = byte & 0x07;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            point = (point << 6) | (next & 0x3f);
        }
        if ((length == 3 && point < 0x800) || (length == 4 && (point < 0x10000 || point > 0x10ffff))
            || (point >= 0xd800 && point <= 0xdfff)) {
            return false;
        }
        i += length;
    }
    return true;
}

// A stick-relative path as the restore writes them: '/'-separated, already
// lexically normal (no empty, "." or ".." component), not absolute, and
// nothing a Windows path would read as a drive, a stream or a separator.
bool isPlainRelativePath(const std::string &path)
{
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos
        || path.find(':') != std::string::npos || !isValidUtf8(path)) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = std::string_view(path).substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string::npos) {
            return true;
        }
        start = slash + 1;
    }
}

// The Engine library's Artwork folder for the database at `database`
// ("Engine Library/Database2/m.db" -> "Engine Library/Artwork"): the one
// place the covers a restore copies in are kept. Empty for a path too
// short to be a database inside a library.
std::string artworkFolderOf(const std::string &database)
{
    const std::size_t file = database.rfind('/');
    if (file == std::string::npos || file == 0) {
        return {};
    }
    const std::size_t folder = database.rfind('/', file - 1);
    if (folder == std::string::npos || folder == 0) {
        return {};
    }
    return database.substr(0, folder) + "/Artwork";
}

// Whether `file` is inside `folder`, by the comparison the rest of the
// restore uses (application::normalizedPathKey: case and Unicode form
// folded the same way as on the sticks it writes).
bool isUnder(const std::string &file, const std::string &folder)
{
    const std::string fileKey = application::normalizedPathKey(file);
    const std::string folderKey = application::normalizedPathKey(folder) + "/";
    return fileKey.size() > folderKey.size() && fileKey.compare(0, folderKey.size(), folderKey) == 0;
}

bool isHexDigest(const std::string &text)
{
    return hashing::digestFromHex(text).has_value();
}

}  // namespace

RestoredDatabases readRestoredDatabases(const fs::path &stickRoot)
{
    RestoredDatabases record;
    std::ifstream in(longPathSafe(recordPath(stickRoot)), std::ios::binary);
    if (!in) {
        return record;
    }
    std::string line;
    if (!std::getline(in, line) || line != Header) {
        return record;
    }
    while (std::getline(in, line)) {
        // Every field is checked before any of it is used: anything off
        // makes the whole record untrusted, never one line of it.
        const std::vector<std::string> fields = splitTabs(line);
        for (const std::string &field : fields) {
            if (!isValidUtf8(field)) {
                return {};
            }
        }
        const bool database = fields.size() == 6 && fields[0] == "database" && isPlainRelativePath(fields[1])
                              && !fields[2].empty() && !fields[3].empty() && isHexDigest(fields[4]);
        // A file is only ever one of the covers copied into the library
        // the database belongs to: under its Artwork folder, and nowhere
        // else. An exact restore keeps what the record lists, so a record
        // that names anything outside it (by hand, by a bug, by a copy)
        // could keep any file on the stick from removal.
        const bool file = fields.size() == 3 && fields[0] == "file" && record.count(fields[1]) != 0
                          && isPlainRelativePath(fields[2]) && !artworkFolderOf(fields[1]).empty()
                          && isUnder(fields[2], artworkFolderOf(fields[1]));
        if (!database && !file) {
            return {};
        }
        if (database) {
            RestoredDatabase &restored = record[fields[1]];
            restored = {fields[2], fields[3], {}, fields[4], fields[5]};
        } else {
            record[fields[1]].files.push_back(fields[2]);
        }
    }
    for (auto &[path, restored] : record) {
        std::sort(restored.files.begin(), restored.files.end());
        restored.files.erase(std::unique(restored.files.begin(), restored.files.end()), restored.files.end());
    }
    return record;
}

bool recordVouchesFor(const fs::path &stickRoot, const std::string &stickLabel, const std::string &path,
                      const RestoredDatabase &restored)
{
    // Written for this stick under this name. A stick relabelled since (to
    // the source's own name, say) has links that name another stick, and
    // a record copied along with the files onto another stick describes
    // links to the stick it came from.
    if (!engine::sameStickLabel(restored.targetLabel, stickLabel)) {
        return false;
    }
    const fs::path main = stickRoot / pathFromUtf8(path);
    // The cheap gate first: most writes move the fingerprint, and then
    // nothing is read.
    const std::optional<DbSetFingerprint> live = fingerprintDbSet(main);
    if (!live || live->toHex() != restored.writtenFingerprint) {
        return false;
    }
    // But not every write does: a WAL commit checkpointed back into a
    // main file of the same page count, its header written back, leaves
    // every field the fingerprint reads as it was. The content is what
    // the restore left only when its hash still is.
    const std::optional<hashing::Sha256Digest> content = hashDbSet(main);
    const std::optional<hashing::Sha256Digest> recorded = hashing::digestFromHex(restored.contentHash);
    return content && recorded && *content == *recorded;
}

RestoredDatabases vouchedRestoredDatabases(const fs::path &stickRoot, const std::string &stickLabel)
{
    RestoredDatabases record = readRestoredDatabases(stickRoot);
    for (auto it = record.begin(); it != record.end();) {
        if (recordVouchesFor(stickRoot, stickLabel, it->first, it->second)) {
            ++it;
        } else {
            it = record.erase(it);
        }
    }
    return record;
}

bool writeRestoredDatabases(const fs::path &stickRoot, const RestoredDatabases &record)
{
    const fs::path path = recordPath(stickRoot);
    std::error_code ec;
    if (record.empty()) {
        fs::remove(longPathSafe(path), ec);
        return !ec;
    }
    // A field that would split a line or a column cannot be written so it
    // reads back as itself.
    const auto fits = [](const std::string &field) {
        return field.find_first_of("\t\r\n") == std::string::npos;
    };
    std::ostringstream out;
    out << Header << '\n';
    for (const auto &[database, restored] : record) {
        if (!fits(database) || !fits(restored.archiveFingerprint) || !fits(restored.writtenFingerprint)
            || !fits(restored.contentHash) || !fits(restored.targetLabel)) {
            return false;
        }
        out << "database\t" << database << '\t' << restored.archiveFingerprint << '\t' << restored.writtenFingerprint
            << '\t' << restored.contentHash << '\t' << restored.targetLabel << '\n';
        for (const std::string &file : restored.files) {
            if (!fits(file)) {
                return false;
            }
            out << "file\t" << database << '\t' << file << '\n';
        }
    }
    fs::create_directories(longPathSafe(path.parent_path()), ec);
    return writeFileDurablyAtomic(pathToUtf8(path), out.str());
}

bool holdsBackupDatabase(const RestoredDatabases &record, const std::string &path, const std::string &archiveFingerprint,
                         const std::string &liveFingerprint)
{
    if (liveFingerprint == archiveFingerprint) {
        return true;
    }
    const auto restored = record.find(path);
    return restored != record.end() && restored->second.archiveFingerprint == archiveFingerprint
           && restored->second.writtenFingerprint == liveFingerprint;
}

}  // namespace seabass::infrastructure::stick_backup
