// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include <djinterop/pad_color.hpp>

#include "application/ports/library_reader.hpp"
#include "domain/track.hpp"

namespace seabass::infrastructure::engine
{

// "" for alpha == 0 (djinterop::pad_color's own default-constructed
// value, and what an Engine hot cue that was never explicitly colored
// reads back as -- see the .cpp's own comment), otherwise "#RRGGBB".
// Exposed here (rather than kept anonymous-namespace-private) so it has
// direct unit test coverage.
std::string colorHex(const djinterop::pad_color &c);

// Reads an Engine Library (Database2/m.db etc.) using the vendored
// libdjinterop. This is the only place that knows about libdjinterop or the
// on-disk Engine schema.
class LibdjinteropEngineReader : public application::LibraryReader
{
public:
    // engineLibraryPath is the directory containing "Database2/"
    // (i.e. the "Engine Library" folder itself).
    explicit LibdjinteropEngineReader(std::string engineLibraryPath);

    // The cues are in the catalog, so fillCues() has nothing to add (the
    // LibraryReader default). readTracks() reads m.db alone: no audio
    // file is stat'd for its size (application::fillFileSizes) and no
    // artwork file for its existence; artworkPath is what the catalog
    // names, whether or not the file is still there. fillArtwork() adds
    // the covers that take more: an image file under Artwork/, looked for
    // on the stick, and an image kept inside the database, written out
    // once to paths::localEngineArtworkDir() on this computer. Only for a
    // caller that shows covers (the catalog cache's Full stage): readAll()
    // is readTracks() alone, as the checks and tools that call it want.
    std::vector<domain::Track> readAll() override;
    std::vector<domain::Track> readTracks() override;
    void fillArtwork(std::vector<domain::Track> &tracks) override;

    // The filesystem identity of the stick the library is on, when the
    // caller knows it (a UUID or volume serial). The local copies of
    // covers kept in the database are filed under it, so clones of one
    // library on two sticks keep theirs apart. Without one they are filed
    // under the library's location.
    void setVolumeIdentity(std::string identity) { m_volumeIdentity = std::move(identity); }

private:
    std::string m_engineLibraryPath;
    std::string m_volumeIdentity;
};

}  // namespace seabass::infrastructure::engine
