// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

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
    // What this file was on the stick, when the archive holds less of it
    // than that: a salvage read that stopped part-way. Zero means the
    // entry is whole, which is every row a healthy stick produces.
    //
    // Both numbers are needed and neither can be dropped. `size` is what
    // the archive holds, so a restore knows how many bytes to write;
    // this is what the file was, so anything reading the backup can say
    // how much is missing instead of presenting a truncated file as
    // complete. A restore that writes 4 MB of a 9 MB track over a good
    // copy, silently, is the failure this exists to prevent.
    std::uint64_t salvagedFromSize = 0;
};

// One run's worth of "what this update did", kept so a backup can say
// how it got to be what it is.
//
// The header describes only the newest generation: an update overwrites
// createdAtUnix, status and the counts, and what the previous runs did is
// gone. For an archive that is updated for months that is most of its
// story -- when it last actually changed, whether a run came off a
// damaged stick, which one was taken before the gig. Each commit appends
// one of these and carries the older ones forward.
struct GenerationRow
{
    std::int64_t createdAtUnix = 0;
    BackupStatus status = BackupStatus::Complete;
    std::size_t added = 0;
    std::size_t changed = 0;
    std::size_t removed = 0;
    // Read off the stick during this run. Deliberately not the archive's
    // resulting size: the manifest is serialized inside commit(), so a
    // generation cannot know how big it made the file, and a field that
    // is always zero is worse than one that is not there.
    std::uint64_t bytesRead = 0;
    // What the backup was called at the time, so a rename does not
    // rewrite history.
    std::string userName;
};

// How many generations an archive remembers. A backup updated nightly
// for a year would otherwise carry 365 rows in a file that is read on
// every open; the oldest are dropped first. Nothing depends on a row
// being present, so losing the tail costs only the display.
inline constexpr std::size_t MaxGenerationRows = 50;

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
//   seabass-stick-manifest<TAB>1<TAB>stickIdentifier<TAB>label<TAB>status<TAB>createdAtUnix[<TAB>libraryFingerprint[<TAB>sourceReadOnly[<TAB>userName]]]
//   g<TAB>createdAtUnix<TAB>status<TAB>added<TAB>changed<TAB>removed<TAB>bytesRead<TAB>userName
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
    // The stick was mounted read-only when this was taken: a damaged
    // filesystem the kernel had already refused writes to. What was read
    // off it is whatever survived, so the backup is an emergency copy and
    // says so wherever it is offered -- restoring one over a working
    // library is a last resort, not an ordinary restore.
    bool sourceReadOnly = false;

    // What the person called this backup, free text, empty when they
    // never named one. "before the Berlin gig", not a filename.
    //
    // It lives here, inside the archive, rather than in the archive's
    // name, because the name is identity: <stick label>.zip is how a
    // backup is matched to its stick, and the .journal, .lock and
    // .compacting siblings are derived from it by appending a suffix.
    // Renaming the file would break all of that, and orphan the
    // .seabass-backup-source marker, which stores the absolute path.
    //
    // The cost of keeping it here is that changing it means rewriting
    // the manifest, and there is no way to replace one entry in a zip --
    // compacting builds a whole new archive and needs the free space to
    // do it. So this is set when a backup is written; a later rename
    // reads from a sibling file that wins over this one, which is the
    // only way to rename a multi-gigabyte archive for free.
    std::string userName;
    // Oldest first; at most MaxGenerationRows.
    std::vector<GenerationRow> generations;
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
