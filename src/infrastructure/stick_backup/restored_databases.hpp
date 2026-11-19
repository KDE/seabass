// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace seabass::infrastructure::stick_backup
{

// What a restore (and so a clone) left on a stick when it could not leave
// a database exactly as the backup holds it.
//
// A restore onto a stick with another label makes the Engine library's
// cover links label-proof as its last write (#101): the m.db on the
// target is then the backup's, plus those covers, and no longer the
// backup's byte for byte. Judged by its DbSetFingerprint alone it read as
// a different database: the stick list said the backup was newer than a
// stick cloned a second ago, and a second clone wrote the backup's m.db
// back and copied the covers again, every time.
//
// So the restore writes down, on the target, which database state of the
// backup it wrote (the manifest's DbSetFingerprint) and what that
// database's fingerprint was once it had finished with it. A stick whose
// database still has exactly that fingerprint holds what the restore
// wrote; one written since by anything else (a player, Engine DJ,
// Seabass) has a fingerprint the record does not name, and reads as
// changed exactly as before.
//
// With it, the files that step wrote beside the database (the covers it
// copied under Engine Library/Artwork): no backup holds them, so without
// the record an exact restore would take them for extras and remove them
// from under a database that names them.
//
// The record lives on the stick it is about, and only there: it is never
// part of a backup (isExcludedFromBackup), so it cannot travel onto
// another stick, and it is not an extra a restore would remove.
inline constexpr std::string_view RestoredDatabasesPath = "Seabass/restored-databases.tsv";

struct RestoredDatabase
{
    std::string archiveFingerprint;  // DbSetFingerprint hex the backup's manifest records
    std::string writtenFingerprint;  // DbSetFingerprint hex the database had when the restore finished
    // Stick-relative, '/'-separated, UTF-8: files the database names that
    // the restore put on the stick after it, sorted, no duplicates.
    std::vector<std::string> files;
    bool operator==(const RestoredDatabase &) const = default;
};

// Archive-relative main-database path ("Engine Library/Database2/m.db")
// -> what the last restore wrote there.
using RestoredDatabases = std::map<std::string, RestoredDatabase>;

// Empty when there is no record, or one this build cannot read: no
// record is the safe answer, it only ever makes a database read as
// changed.
RestoredDatabases readRestoredDatabases(const std::filesystem::path &stickRoot);

// Replaces the record. An empty one removes the file. False when the
// write failed; the caller warns, since a stale record is never wrong in
// the dangerous direction (it names a fingerprint the stick no longer
// has) but a missing one makes the clone read as not current.
bool writeRestoredDatabases(const std::filesystem::path &stickRoot, const RestoredDatabases &record);

// Whether a database on a stick with `liveFingerprint` holds the backup
// database recorded as `archiveFingerprint`: the same fingerprint, or
// the one a restore of exactly that database left behind.
bool holdsBackupDatabase(const RestoredDatabases &record, const std::string &path, const std::string &archiveFingerprint,
                         const std::string &liveFingerprint);

}  // namespace seabass::infrastructure::stick_backup
