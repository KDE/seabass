// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace seabass::application
{

// Who asked for a backup, which decides whether Seabass may delete it on
// its own. Automatic backups are Seabass's own safety copies and Seabass
// may release them when the stick is tight; anything the user asked for
// is the user's, and only the user deletes it.
enum class BackupOrigin
{
    Automatic,
    UserRequested,
};

struct BackupRecord
{
    std::string id;         // directory name, sorts chronologically (e.g. "20260826T193000-hot-cues")
    std::string path;       // full path to the backup directory
    std::string label;
    std::string description;  // user-editable free text, empty unless set via setDescription()
    std::uint64_t sizeBytes = 0;
    // Original paths of every file this backup holds a copy of -- e.g.
    // lets a caller tell whether a given backup included OneLibrary's
    // exportLibrary.db alongside export.pdb.
    std::vector<std::string> filePaths;
    // Defaults to Automatic because that is what a record with nothing
    // recorded actually is: until this field existed, every record in
    // this store was written by a save. See readOrigin() in
    // filesystem_backup_store.cpp for the one exception.
    BackupOrigin origin = BackupOrigin::Automatic;
};

// Port for keeping "undo" copies of files before a mutating write touches
// them. Every write Seabass performs must go through here first -- see
// the plan's Backups section: manageable size (only the files actually
// touched, not a full stick copy), with a way to prune old ones.
class BackupStore
{
public:
    virtual ~BackupStore() = default;

    virtual BackupRecord backup(const std::vector<std::string> &filePaths, const std::string &label,
                                BackupOrigin origin = BackupOrigin::Automatic) = 0;
    virtual std::vector<BackupRecord> list() = 0;

    // Deletes the oldest AUTOMATIC backups so at most keepCount of them
    // remain. Returns the number of bytes freed.
    //
    // User-requested records are neither deleted nor counted towards
    // keepCount: they are not Seabass's to tidy away, and counting them
    // would let a few of the user's own backups push out every safety
    // copy Seabass still needs. Deleting one of those is remove(), which
    // takes an id and therefore only ever happens because someone named
    // it.
    virtual std::uint64_t prune(size_t keepCount) = 0;

    // Attaches/replaces a user-editable note on an existing backup (e.g.
    // "before Berlin gig"). No-op if id doesn't exist.
    virtual void setDescription(const std::string &id, const std::string &description) = 0;

    // Copies every file in the backup back to the original path it was
    // backed up from (recorded at backup() time). The current contents of
    // each target path are themselves backed up first (label
    // "pre-restore"), so a restore can itself be undone. Returns false if
    // the backup can't be found, holds nothing, or is not a shape this
    // build wrote.
    virtual bool restore(const std::string &id) = 0;

    // Permanently deletes a single backup. Returns false if id doesn't
    // exist.
    virtual bool remove(const std::string &id) = 0;
};

}  // namespace seabass::application
