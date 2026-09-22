// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"

#include "infrastructure/cleanup/pending_deletion_manifest.hpp"

namespace seabass::infrastructure::cleanup
{

struct PendingDeletionOutcome
{
    PendingDeletion entry;
    enum class Status
    {
        Deleted,        // removed from disk this call
        AlreadyAbsent,  // gone already (e.g. removed by hand since) -- still cleared from the manifest
        Failed          // filesystem::remove() failed; left in the manifest for a future retry
    };
    Status status = Status::Failed;
    std::string failureReason;  // only set when status == Failed
};

// The one place in the app that permanently destroys real audio file
// content: for every entry in `safeToDelete` (which the caller must
// already have run through resolvePendingDeletions() against a *fresh*
// scan -- this function does no re-verification of its own, and trusts
// its input completely), deletes the file at entry.filePath from disk
// (or, if it's already gone, treats that as done rather than an error),
// then clears every successfully-processed entry from `manifest` in one
// rewrite. Entries that fail to delete stay in the manifest so a later
// pass can retry them.
//
// `cancel` is checked before every file: once it fires, the remaining
// entries are neither deleted nor cleared from the manifest, and the
// returned outcomes hold only the files reached (the caller reads the
// token to tell a cancelled run from a short one). `onFileProcessed`
// (optional) is called after each file with the running count.
// stickRoot: the mount point of the stick being processed; an entry
// outside it fails ("not on this stick") and stays in the manifest,
// whatever list it arrived in.
// manifestNotUpdated (optional): set to why the manifest could not be
// rewritten after the deletions, and left alone when it was. The files
// are gone by then, so this cannot be reported by failing; it is the
// one outcome of this function that the returned list cannot express.
// The entries stay in the manifest in that case, which a later pass
// reads as "already absent" and clears, so nothing is lost as long as
// somebody is told to look again.
std::vector<PendingDeletionOutcome> applyPendingDeletions(
    const std::vector<PendingDeletion> &safeToDelete, const std::string &stickRoot, PendingDeletionManifest &manifest,
    const application::CancellationToken &cancel = application::CancellationToken::none(),
    const std::function<void(size_t done)> &onFileProcessed = {}, std::string *manifestNotUpdated = nullptr);

}  // namespace seabass::infrastructure::cleanup
