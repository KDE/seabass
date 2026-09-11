#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "infrastructure/hashing/sha256.hpp"

namespace seabass::infrastructure::stick_backup
{

inline constexpr std::string_view ManifestEntryName = "SEABASS-MANIFEST.tsv";

enum class BackupStatus
{
    Complete,
    PartialCancelled,   // user stopped it and chose "keep for later"
    PartialConflict,    // Engine DJ / rekordbox appeared mid-run; DB set skipped
    PartialDbTooLarge,  // DB set refused (> 1 GiB); everything else captured
    PartialSkipped,     // some files could not be read (open failed, changed while read, unwalkable); the rest captured
};

std::string_view toString(BackupStatus status);
std::optional<BackupStatus> backupStatusFromString(std::string_view text);

struct ManifestRow
{
    enum class Kind
    {
        File,
        Directory
    };
    Kind kind = Kind::File;
    std::string path;  // archive-relative, forward slashes, no trailing '/'
    std::uint64_t size = 0;
    std::int64_t mtimeUnix = 0;
    hashing::Sha256Digest sha256{};  // files only
    // Free-form per-row data; today the SQLite DB-set fingerprint on
    // `.db` / `-wal` / `-journal` rows, empty otherwise.
    std::string extra;
    // The entry's ZIP CRC32 (files only). Redundant with the central
    // directory on purpose: together with path, size and mtime it makes
    // the manifest a complete second copy of every CD field that matters,
    // so damage to a carried entry's CD record is caught without
    // re-reading its data.
    std::uint32_t crc32 = 0;
};

// The archive's own index of what it holds, written as the last entry
// before the central directory on every update. It is the integrity
// layer ZIP lacks: the central directory has no checksum of its own, so
// a flipped byte in a CD filename would otherwise restore a file under
// the wrong name with every byte "correct". It is also the stat-diff's
// picture of the previous backup.
//
// Format: tab-separated text, one row per line, so it stays readable via
// `unzip -p backup.zip SEABASS-MANIFEST.tsv` and needs no JSON library.
//
//   seabass-stick-manifest<TAB>1<TAB>stickIdentifier<TAB>label<TAB>status<TAB>createdAtUnix[<TAB>libraryFingerprint]
//   f<TAB>path<TAB>size<TAB>mtimeUnix<TAB>crc32hex<TAB>sha256hex<TAB>extra
//   d<TAB>path<TAB>0<TAB>mtimeUnix<TAB><TAB><TAB>
//   ...
//   #sha256<TAB>hex-of-everything-above
//
// Text fields escape tab, newline and backslash as \t \n \\ -- the only
// three characters that could break the row grammar.
struct BackupManifest
{
    static constexpr int FormatVersion = 1;

    std::string stickIdentifier;
    std::string stickLabel;
    BackupStatus status = BackupStatus::Complete;
    std::int64_t createdAtUnix = 0;
    // domain::LibraryFingerprint::serialize() of the library as backed
    // up; empty for backups written before it existed or when the library
    // could not be read. Opaque here: only the domain parses it.
    std::string libraryFingerprint;
    std::vector<ManifestRow> rows;

    std::string serialize() const;

    // Verifies the trailer hash and the grammar; any problem yields nullopt
    // with the reason in `error`. Never trust a manifest that fails this.
    static std::optional<BackupManifest> parse(std::string_view text, std::string *error = nullptr);

    const ManifestRow *findRow(std::string_view path) const;
};

std::string escapeManifestField(std::string_view raw);
std::optional<std::string> unescapeManifestField(std::string_view escaped);

}  // namespace seabass::infrastructure::stick_backup
