// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "library_fingerprint_reader.hpp"

#include <exception>
#include <string_view>
#include <utility>
#include <vector>

#include "domain/track.hpp"
#include "gui/library_catalog_cache.hpp"

namespace seabass::gui
{

std::optional<domain::LibraryFingerprint> readLibraryFingerprint(const QString &rekordboxPath, const QString &enginePath,
                                                                 FingerprintPass pass, application::CancellationToken cancel)
{
    // The fingerprint wants titles, artists, durations, playlists and cue
    // positions: never a file size, so never the Full stage.
    const LibraryCatalogCache::Detail detail =
        pass == FingerprintPass::Tracks ? LibraryCatalogCache::Detail::Tracks : LibraryCatalogCache::Detail::Cues;
    std::vector<domain::Track> tracks;
    bool anyRead = false;
    bool rekordboxCuesMissing = false;
    for (const auto &[format, path] : {std::pair{"rekordbox", rekordboxPath}, std::pair{"engine", enginePath}}) {
        if (path.isEmpty()) {
            continue;
        }
        try {
            // The stage comes with the tracks, from one look at the entry:
            // asked for separately, a cue pass committing in between made
            // a list without cues claim its cues were known.
            LibraryCatalogCache::StagedTracks read = LibraryCatalogCache::instance().stagedTracksFor(
                format, path.toStdString(), detail, application::NullProgressReporter::instance(), cancel);
            tracks.insert(tracks.end(), read.tracks.begin(), read.tracks.end());
            anyRead = true;
            if (std::string_view(format) == "rekordbox") {
                // A Tracks answer from an entry that had already read its
                // cues carries them, and saying "checking cues" over it
                // would be a flash of nothing: the stage says which.
                rekordboxCuesMissing = read.stage < LibraryCatalogCache::Detail::Cues;
            }
        } catch (const std::exception &) {
            // A catalog that is there and could not be read (or a cancel):
            // no fingerprint at all. The other catalog alone is half the
            // library, and compared against a backup of the whole it
            // reads as a different one.
            return std::nullopt;
        }
    }
    if (!anyRead) {
        return std::nullopt;
    }
    // Engine's cues are in its catalog, so a Tracks read of an Engine-only
    // stick is already the whole fingerprint. rekordbox's are not, unless
    // the cache had read that far before this call.
    const bool cuesKnown = !rekordboxCuesMissing;
    return domain::fingerprintLibrary(tracks, cuesKnown);
}

std::optional<domain::LibraryFingerprint> fingerprintAfterCuesPass(const std::optional<domain::LibraryFingerprint> &first,
                                                                   const std::optional<domain::LibraryFingerprint> &second)
{
    if (second && second->cuesKnown) {
        return second;
    }
    return first;
}

std::optional<domain::LibraryFingerprint> readLibraryFingerprintUncached(const QString &rekordboxPath,
                                                                          const QString &enginePath)
{
    // Drops the cached read first, so what comes back describes the
    // catalogs as they are on disk rather than as whichever page last
    // opened them saw them.
    //
    // This matters for exactly one caller and it is the one that cannot
    // be wrong: a backup's manifest header. A cached answer there records
    // a library the archive does not contain, nothing afterwards compares
    // the two, and the advisor then tells someone their stick is a
    // different library from its own backup. Every other reader wants the
    // cache -- re-scanning a stick per page open is what it exists to
    // stop -- so this is a second entry point rather than a change to the
    // first.
    //
    // Invalidating rather than reading around the cache on purpose: one
    // place constructs the readers and fills in the durations neither
    // catalog records, and a second copy of that would drift from it.
    // The cost is that pages sharing these catalogs re-scan once, which a
    // backup is slow enough to absorb.
    for (const auto &[format, path] : {std::pair{"rekordbox", rekordboxPath}, std::pair{"engine", enginePath}}) {
        if (!path.isEmpty()) {
            LibraryCatalogCache::instance().invalidate(format, path.toStdString());
        }
    }
    return readLibraryFingerprint(rekordboxPath, enginePath);
}

}  // namespace seabass::gui
