#include "infrastructure/cleanup/pending_deletion_applier.hpp"

#include "infrastructure/cleanup/stick_containment.hpp"
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

        std::error_code ec;
        // Prefixed: a track under a long artist/album path can sit past
        // MAX_PATH, and there the unprefixed calls answer "not there" and
        // "could not remove" about a file that is present and removable.
        const fs::path path = longPathSafe(entry.filePath);
        if (!fs::exists(path, ec)) {
            outcome.status = PendingDeletionOutcome::Status::AlreadyAbsent;
            processed.insert(entry.filePath);
        } else if (fs::remove(path, ec)) {
            outcome.status = PendingDeletionOutcome::Status::Deleted;
            processed.insert(entry.filePath);
        } else {
            outcome.status = PendingDeletionOutcome::Status::Failed;
            // fs::remove returns false both for a real failure and for
            // "there was nothing to remove", and only the first sets ec.
            // Passing ec.message() through regardless put the string
            // "The operation completed successfully" in front of the user
            // as the reason their file could not be deleted.
            outcome.failureReason = ec ? ec.message() : "the file could not be removed";
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
