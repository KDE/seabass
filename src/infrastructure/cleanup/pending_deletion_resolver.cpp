#include "infrastructure/cleanup/pending_deletion_resolver.hpp"

#include "application/path_key.hpp"
#include "infrastructure/cleanup/stick_containment.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

namespace seabass::infrastructure::cleanup
{

namespace
{

namespace fs = std::filesystem;

void collect(const std::optional<std::vector<domain::Track>> &catalog, std::set<std::string> &referenced)
{
    if (!catalog) {
        return;
    }
    for (const auto &track : *catalog) {
        if (!track.filePath.empty()) {
            referenced.insert(application::normalizedPathKey(track.filePath));
        }
    }
}

}  // namespace

PendingDeletionResolution resolvePendingDeletions(const std::vector<PendingDeletion> &pending,
                                                    const application::CatalogTracks &catalogs,
                                                    const std::string &stickRoot)
{
    PendingDeletionResolution result;

    std::vector<PendingDeletion> onThisStick;
    for (const auto &entry : pending) {
        // An entry with no path at all stays in the protected bucket
        // below, as before: nothing to verify, nothing to delete.
        (entry.filePath.empty() || isUnderStickRoot(entry.filePath, stickRoot) ? onThisStick : result.notOnThisStick)
            .push_back(entry);
    }

    if (catalogs.present().empty()) {
        // Nothing was read, so nothing can be shown to be unreferenced.
        // Protect every entry rather than clearing the stick.
        result.stillReferenced = onThisStick;
        return result;
    }

    std::set<std::string> referenced;
    collect(catalogs.rekordbox, referenced);
    collect(catalogs.engine, referenced);
    collect(catalogs.oneLibrary, referenced);

    for (const auto &entry : onThisStick) {
        // No resolved path to check at all -- never guess, leave it alone.
        if (entry.filePath.empty() || referenced.contains(application::normalizedPathKey(entry.filePath))) {
            result.stillReferenced.push_back(entry);
        } else {
            result.safeToDelete.push_back(entry);
        }
    }
    return result;
}

}  // namespace seabass::infrastructure::cleanup
