// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/library_cleanup_writer.hpp"
#include "domain/track.hpp"

namespace seabass::infrastructure::engine
{

// Permanently removes a track from an Engine Library, using
// libdjinterop's real, tested database::remove_track() for the track
// row itself, plus an explicit walk of every playlist to move the
// doomed track's membership onto the survivor first --
// database::remove_track()'s own claim that playlist cleanup happens
// automatically via a foreign-key cascade did not hold up against a
// real test (see the .cpp for why). The caller (cli:: or gui::) is
// responsible for backing up the library's files before constructing
// this -- this class only performs the write itself.
class LibdjinteropEngineCleanupWriter : public application::LibraryCleanupWriter
{
public:
    explicit LibdjinteropEngineCleanupWriter(std::string engineLibraryPath);

    void removeTrackReplacingWith(const std::string &doomedTrackId, const std::string &survivorTrackId) override;

    // Ensures the Engine library has a track for desc's file, returning
    // its Engine track id -- for the cross-library Clean Up case where
    // the chosen survivor is a rekordbox-only copy and this catalog
    // needs its own row for it. Looks up by relative path first
    // (database::tracks_by_relative_path()) and reuses an existing row
    // if one's already there, both to avoid ever hitting Track's
    // UNIQUE(path) constraint and to avoid a redundant row if this
    // exact file happens to already be Engine-cataloged under a track
    // duplicate-detection didn't independently match. Otherwise builds
    // a djinterop::track_snapshot from desc's title/artist/
    // relative_path/duration/bpm/key/bitrate and calls
    // database::create_track(). Does NOT write cues -- the caller does
    // that afterward via the normal CueWriter path, same separation of
    // concerns as everywhere else in this codebase.
    std::string ensureTrackForFile(const domain::Track &desc);

private:
    std::string m_engineLibraryPath;
};

}  // namespace seabass::infrastructure::engine
