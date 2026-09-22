// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/cleanup/pending_deletion_applier.hpp"

#include "infrastructure/cleanup/stick_containment.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/long_paths.hpp"

#include <filesystem>
#include <system_error>

namespace seabass::infrastructure::cleanup
{

namespace fs = std::filesystem;

std::vector<PendingDeletionOutcome> applyPendingDeletions(const std::vector<PendingDeletion> &safeToDelete,
                                                            const std::string &stickRoot,
                                                            PendingDeletionManifest &manifest,
                                                            const application::CancellationToken &cancel,
                                                            const std::function<void(size_t)> &onFileProcessed)
{
    std::vector<PendingDeletionOutcome> outcomes;
    std::set<std::string> processed;

    for (const auto &entry : safeToDelete) {
        // Between two files, never inside one: a file is either still
        // there or gone, and the manifest below only ever forgets the
        // ones that are gone.
        if (cancel.cancelled()) {
            break;
        }
        PendingDeletionOutcome outcome;
        outcome.entry = entry;

        // The last line of defence, independent of whoever built the
        // list: this applier only ever deletes under the stick it was
        // given. An entry pointing elsewhere (a moved mount point, a
        // swapped drive letter) fails and stays in the manifest.
        if (!isUnderStickRoot(entry.filePath, stickRoot)) {
            outcome.status = PendingDeletionOutcome::Status::Failed;
            outcome.failureReason = "the file is not on this stick (" + stickRoot + "); nothing deleted";
            outcomes.push_back(std::move(outcome));
            if (onFileProcessed) {
                onFileProcessed(outcomes.size());
            }
            continue;
        }

        // Prefixed: a track under a long artist/album path can sit past
        // MAX_PATH, and there the unprefixed calls answer "not there" and
        // "could not remove" about a file that is present and removable.
        const fs::path path = longPathSafe(entry.filePath);
        std::string failure;
        // exists() answers false for BOTH "it is gone" and "I could not
        // look", and only the error code tells them apart. Untested, the
        // second became AlreadyAbsent -- which this file's own header
        // defines as "gone already ... still cleared from the manifest".
        // So a file Seabass could not examine, on a stick with a failing
        // cell or a directory it cannot search, was reported to the
        // person as already dealt with, left on the stick taking the
        // space they were trying to reclaim, and dropped from the
        // manifest so no later pass would ever retry it. The same
        // distinction removeEntry() exists to make one line below, and
        // the same one audio_file_walk.cpp already makes with "|| ec".
        std::error_code existsEc;
        const bool present = fs::exists(path, existsEc);
        if (existsEc) {
            outcome.status = PendingDeletionOutcome::Status::Failed;
            outcome.failureReason = "could not tell whether the file is still there: " + existsEc.message();
        } else if (!present) {
            outcome.status = PendingDeletionOutcome::Status::AlreadyAbsent;
            processed.insert(entry.filePath);
        } else if (removeEntry(entry.filePath, failure)) {
            // removeEntry, not fs::remove: on macOS a track whose name
            // carries an accent comes back from a directory read
            // decomposed, and unlink takes only the composed spelling --
            // so the file survived and the person was told it had gone.
            // It prefixes the path itself and reports what it did, not
            // merely whether it unlinked something.
            outcome.status = PendingDeletionOutcome::Status::Deleted;
            processed.insert(entry.filePath);
        } else {
            outcome.status = PendingDeletionOutcome::Status::Failed;
            outcome.failureReason = failure;
        }
        outcomes.push_back(std::move(outcome));
        if (onFileProcessed) {
            onFileProcessed(outcomes.size());
        }
    }

    manifest.removeProcessed(processed);
    return outcomes;
}

}  // namespace seabass::infrastructure::cleanup
