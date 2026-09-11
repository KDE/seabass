// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "application/ports/backup_store.hpp"

namespace seabass::infrastructure::backup
{

// Stores backups under baseDirectory, one directory per backup:
// baseDirectory/<timestamp>-<label>/, each holding a single deflated
// `backup.zip` plus its manifest.
//
// One archive rather than loose copies, for two measured reasons. The
// loose layout cost one durable whole-file write per file -- about 118 ms
// each on Linux and 15 ms on Windows, against roughly 2 s for the same
// bytes as one file -- and it was permanent: 400 loose analysis files
// occupy 71.5 MB against 45.5 MB deflated, on sticks that were 95% and
// 97% full. See docs/write-path-performance.md.
//
// A user can still get a file back by hand without Seabass: a zip opens
// in every file manager, and the entry names carry the whole relative
// path, so 200 files all called ANLZ0000.EXT are told apart by where they
// came from rather than by a _1/_2 suffix the loose layout needed.
class FilesystemBackupStore : public application::BackupStore
{
public:
    explicit FilesystemBackupStore(std::string baseDirectory);

    application::BackupRecord backup(const std::vector<std::string> &filePaths, const std::string &label,
                                     application::BackupOrigin origin
                                     = application::BackupOrigin::Automatic) override;

    // Appends to an existing record, so a save that discovers its files
    // as it goes still ends up with one record. Each call re-writes the archive's central directory
    // and makes it durable, so the record is complete and readable after
    // every item -- the same guarantee the loose layout gives, since it
    // fsyncs every copy. The superseded central directories become dead
    // space; archive_compactor reclaims it.
    //
    // Throws if `id` is not an archive record.
    application::BackupRecord addToArchive(const std::string &id, const std::vector<std::string> &filePaths);
    std::vector<application::BackupRecord> list() override;
    std::uint64_t prune(size_t keepCount) override;

    // Deletes automatic backups, oldest first, until at least bytesWanted
    // has been freed or only the newest automatic record is left. Returns
    // the bytes actually freed.
    //
    // For the space-pressure case: a stick that has dropped below its
    // headroom after a save. Only ever called after a SUCCESSFUL save --
    // on a failure or a cancel the backups are the thing that saves you.
    //
    // This buys capacity and nothing else. Neither stick sampled supports
    // TRIM (discard_max_bytes is 0), so freeing space returns nothing to
    // the flash controller and the device does not get faster. Nothing in
    // the UI should suggest otherwise.
    std::uint64_t releaseAutomaticBackups(std::uint64_t bytesWanted);
    void setDescription(const std::string &id, const std::string &description) override;
    bool restore(const std::string &id) override;
    bool remove(const std::string &id) override;

private:
    // Paths on the stick are stored relative to it, so a backup still
    // restores after the stick comes back at a different mount point or
    // drive letter. See CurrentManifestFormatVersion in the .cpp.
    std::filesystem::path stickRoot() const;
    bool restoreFromArchive(const std::filesystem::path &dir,
                            const std::vector<std::pair<std::string, std::string>> &entries);
    // Appends `filePaths` to the archive in `dir`, returns the entry
    // name / recorded path pairs actually written and the archive's new
    // size. Shared by backup() and addToArchive().
    std::pair<std::vector<std::pair<std::string, std::string>>, std::uint64_t>
    writeArchiveEntries(const std::filesystem::path &dir, const std::vector<std::string> &filePaths);
    std::string recordedPathFor(const std::filesystem::path &source) const;
    std::filesystem::path resolveRecordedPath(const std::string &recorded) const;

    std::string m_baseDirectory;

    // Per backup directory: the names already taken in it, and the bytes
    // it holds. Both exist to keep a save linear in the number of files
    // it backs up rather than quadratic.
    //
    // A save backing up 200 rekordbox analysis files hits both. Every one
    // of them is named ANLZ0000.EXT -- only the containing directory
    // differs -- so the clash guard used to stat the destination once per
    // already-taken name, 20,000 stats on removable media for 200 files.
    // And the byte total was recomputed by walking the whole directory
    // after every single call.
    //
    // Seeded from disk the first time a directory is touched, so this
    // stays correct for a store pointed at backups an earlier run made.
    // Cached so a record's size is accumulated as it grows rather than
    // recomputed by walking the directory after every append.
    // takenNames is gone with the loose layout: a zip entry name carries
    // the whole relative path, so two files called ANLZ0000.EXT cannot
    // collide and there is nothing left to disambiguate.
    struct DirectoryState
    {
        std::uint64_t sizeBytes = 0;
    };
    DirectoryState &stateFor(const std::filesystem::path &dir);

    std::map<std::string, DirectoryState> m_directoryState;
};

}  // namespace seabass::infrastructure::backup
