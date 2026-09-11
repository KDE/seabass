// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>

// Every location Seabass writes to, in one place.
//
// Two roots, and the split is about portability rather than tidiness:
//
//   <stick>/Seabass/   travels with the stick. A DJ can plug it into
//                      another machine and Seabass still finds the
//                      caches, the on-stick backups and the record of
//                      what is waiting to be deleted.
//   ~/Seabass/         stays on this computer. Full stick images (far
//                      too big for the stick they came from), the local
//                      cue backups, and Seabass's own bookkeeping.
//
// These used to be scattered dotfiles at the stick root
// (.seabass-backups, .seabass.log, .seabass-pending-deletions.jsonl,
// .seabass-metadata.jsonl, .seabass-durations.jsonl) plus "~/Seabass
// Backups" and an XDG/LOCALAPPDATA directory. Five hidden files at the
// root of a DJ's stick is not a thing anyone can reason about, and a
// hidden name is exactly wrong here: this is the user's data on the
// user's stick, and they should be able to see it, copy it and delete it
// without knowing to look for dotfiles.
//
// Nothing migrates from the old locations. Seabass is pre-1.0, the
// sticks get written anew, and a migration path is code that would be
// wrong more often than it ran.
namespace seabass::infrastructure::paths
{

namespace fs = std::filesystem;

// ---- on the stick -------------------------------------------------
// A catalog path is the "PIONEER" or "Engine Library" folder; the stick
// root is its parent.
std::string stickRootForCatalogPath(const std::string &catalogPath);

// <stick>/Seabass
fs::path stickDir(const fs::path &stickRoot);
// <stick>/Seabass/backups -- one directory per backup, each holding a
// deflated backup.zip. Undo copies taken before a save writes.
fs::path stickBackupsDir(const fs::path &stickRoot);
// <stick>/Seabass/caches -- derived data that can always be rebuilt by
// re-reading the stick, so it is safe to delete and never restored.
fs::path stickCachesDir(const fs::path &stickRoot);
// <stick>/Seabass/orphaned -- files staged for deletion but not yet
// deleted, and the record of what they were.
fs::path stickOrphanedDir(const fs::path &stickRoot);

fs::path stickOperationLog(const fs::path &stickRoot);
fs::path stickPendingDeletions(const fs::path &stickRoot);
fs::path stickMetadataCache(const fs::path &stickRoot);
fs::path stickDurationCache(const fs::path &stickRoot);

// ---- on this computer ---------------------------------------------
// Where everything Seabass keeps on this computer lives.
//
// Resolved in order: an explicit override set by the application from
// the user's preference, then $SEABASS_HOME (which is what the test
// suite uses, so a test run can never write into the real tree), then
// <home>/Seabass.
fs::path localRoot();

// Sets the override above, or clears it when given an empty path. Called
// once at startup from the stored preference; never from library code,
// which has no business deciding where the user keeps their data.
void setLocalRootOverride(const fs::path &root);
// ~/Seabass/backups/full -- whole-stick images. The user-facing default
// for Backup USB Stick; changeable in settings.
fs::path localFullBackupsDir();
// ~/Seabass/backups -- also where cue backups go when the stick has no
// room for them (see StickSpace::backupGoesLocal()).
fs::path localBackupsDir();
// ~/Seabass/metadata -- Seabass's own bookkeeping: the local cue store,
// edit-lock cookies, benchmark history. Not user-facing, but kept here
// rather than under XDG/LOCALAPPDATA so everything Seabass owns is in
// one place the user can find, back up and delete.
fs::path localMetadataDir();

// ~/Seabass/metadata/browsed-backups -- the extracted catalogs of a stick
// backup being browsed, one directory per archive. Derived data with an
// obvious source: safe to delete at any time, and re-created by opening
// the backup again. Under metadata rather than backups/ because it is
// bookkeeping, not something the user put there.
fs::path localBrowsedBackupsDir();

}  // namespace seabass::infrastructure::paths
