// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

class AudioContentProbe;

// A file OneLibrary still lists after the rekordbox half removed it.
//
// export.pdb and exportLibrary.db are one library in two formats, so a
// file in one and not the other is a divergence -- and usually one that
// cannot say which side is right: added on one side, or removed on the
// other, look the same. Except when export.pdb still holds a DELETED row
// for the file. Clearing a presence bit leaves the row's body, path
// included, so that row says the rekordbox half listed the file and then
// removed it.
//
// That is what Seabass's own Clean Up left on every stick it ran on
// before it mirrored its removals into OneLibrary: 284 of the 290 files
// WHALESHARK2's OneLibrary had and its export.pdb did not. Finishing the
// job is removing the OneLibrary row and moving its playlist entries onto
// the copy Clean Up kept.
//
// Which copy that was is recorded nowhere a repair can read: ids do not
// line up across the two formats (309 of 1161 on WHALESHARK2), and
// playlist positions shift where Clean Up dropped an entry rather than
// moving it. So it is found the way Clean Up found it -- the same
// duplicate matching, against the tracks the rekordbox half still lists.
struct CleanupLeftover
{
    enum class Kind {
        // Exactly one live rekordbox track is a duplicate of it, and
        // OneLibrary lists that track too: the entries have somewhere to go.
        Repairable,
        // No live rekordbox track matches. Reported, never repaired:
        // removing it would drop its playlist entries with nowhere to go,
        // and it may be a file the DJ still wants.
        NoSurvivor,
        // More than one live track matches, and nothing here says which
        // one Clean Up kept. Guessing would move the entries onto the
        // wrong file.
        SeveralSurvivors,
        // The survivor is not in OneLibrary, so there is no row to move
        // the entries onto.
        SurvivorNotInOneLibrary,
    };

    Kind kind = Kind::NoSurvivor;
    // One of the OneLibrary rows at this file, for its title and artist.
    Track row;
    // Every OneLibrary row at this file: two rekordbox installations
    // exporting to one stick give the same file two rows.
    std::vector<std::string> rowIds;
    // Repairable only: the live rekordbox track the entries move onto.
    std::optional<Track> survivor;
};

class CleanupLeftoverFinder
{
public:
    // `fileKey` compares paths across the two readers; callers pass
    // application::normalizedPathKey, the domain does not depend on it.
    // `deletedRekordboxFiles` is what deletedTrackFilePaths() returns;
    // `rekordboxTracks` the live rows. Streaming entries and rows without
    // a path are never leftovers -- there is no file to have removed.
    //
    // `probe` is the same optional audio comparison Clean Up uses, for a
    // pair whose stored lengths are further apart than the exact-match
    // tolerance. Without it such a leftover comes back NoSurvivor, which
    // costs a repair and never a wrong one.
    using FileKey = std::function<std::string(const std::string &)>;
    static std::vector<CleanupLeftover> find(const std::vector<Track> &oneLibraryRows,
                                             const std::vector<Track> &rekordboxTracks,
                                             const std::vector<std::string> &deletedRekordboxFiles,
                                             const FileKey &fileKey,
                                             AudioContentProbe *probe = nullptr);
};

}  // namespace seabass::domain
