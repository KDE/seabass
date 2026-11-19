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
// backup it wrote (the manifest's DbSetFingerprint), what that database's
// fingerprint was once it had finished with it, a SHA-256 of the database
// set as it left it, and the label of the stick it wrote it for. The
// fingerprint is the cheap first test, but it reads headers and sizes
// only: most writes move it, not every one does (a WAL commit
// checkpointed into a main file of the same page count, its header
// fields as they were). Only when the fingerprint still matches is the
// set hashed, and the record vouches for the database only when that hash
// matches too. A stick written since by anything else (a player, Engine
// DJ, Seabass) reads as changed exactly as before.
//
// The label matters because the covers are made label-proof for one
// name: a stick relabelled since (to the source's own name, say), or the
// files copied onto another stick with a file manager, record and all,
// hold links that name a stick they are not. The record vouches only on
// a stick whose label is the one it names.
//
// With it, the files that step wrote beside the database (the covers it
// copied under the library's Artwork folder): no backup holds them, so
// without the record an exact restore would take them for extras and
// remove them from under a database that names them. A record listing a
// file anywhere else is not trusted at all.
//
// The record lives on the stick it is about: it is never part of a
// backup (isExcludedFromBackup), and it is not an extra a restore would
// remove.
inline constexpr std::string_view RestoredDatabasesPath = "Seabass/restored-databases.tsv";

struct RestoredDatabase
{
    std::string archiveFingerprint;  // DbSetFingerprint hex the backup's manifest records
    std::string writtenFingerprint;  // DbSetFingerprint hex the database had when the restore finished
    // Stick-relative, '/'-separated, UTF-8: files the database names that
    // the restore put on the stick after it, all under the library's
    // Artwork folder, sorted, no duplicates.
    std::vector<std::string> files;
    std::string contentHash;  // hashDbSet() hex of the set when the restore finished
    std::string targetLabel;  // the label of the stick the restore wrote it for
    bool operator==(const RestoredDatabase &) const = default;
};

// Archive-relative main-database path ("Engine Library/Database2/m.db")
// -> what the last restore wrote there.
using RestoredDatabases = std::map<std::string, RestoredDatabase>;

// The record as written, with nothing on the stick checked against it.
// Empty when there is no record, or one this build cannot read, or one
// with any line that fails a check: a string that is not UTF-8, a path
// that is not plain and relative, a file outside its database's Artwork
// folder. No record is the safe answer, it only ever makes a database
// read as changed.
RestoredDatabases readRestoredDatabases(const std::filesystem::path &stickRoot);

// Whether the record's entry for `path` vouches for the database on the
// stick at `stickRoot`, labelled `stickLabel`, right now: written for
// that label, the fingerprint still the one the restore left, and then
// the content hash too. The one test the restore planner and the stick
// list both go by (vouchedRestoredDatabases).
bool recordVouchesFor(const std::filesystem::path &stickRoot, const std::string &stickLabel, const std::string &path,
                      const RestoredDatabase &restored);

// readRestoredDatabases() with every entry recordVouchesFor() refuses
// taken out: what holdsBackupDatabase() is handed.
RestoredDatabases vouchedRestoredDatabases(const std::filesystem::path &stickRoot, const std::string &stickLabel);

// Replaces the record. An empty one removes the file. False when the
// write failed, or a field could not be written to read back as itself;
// the caller warns, since a stale record is never wrong in the dangerous
// direction (it names a state the stick no longer has) but a missing one
// makes the clone read as not current.
bool writeRestoredDatabases(const std::filesystem::path &stickRoot, const RestoredDatabases &record);

// Whether a database on a stick with `liveFingerprint` holds the backup
// database recorded as `archiveFingerprint`: the same fingerprint, or the
// one a restore of exactly that database left behind, by a record
// vouchedRestoredDatabases() returned (it has checked the label and the
// content; this compares the fingerprints it names).
bool holdsBackupDatabase(const RestoredDatabases &record, const std::string &path, const std::string &archiveFingerprint,
                         const std::string &liveFingerprint);

}  // namespace seabass::infrastructure::stick_backup
