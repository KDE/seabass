// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <optional>
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
    // What an audio file says its sample rate is, in Hz, or nothing when
    // it cannot say. Given the absolute path the reader resolved for the
    // row (Track::filePath). May throw application::OperationCancelled;
    // any other exception is taken as "cannot say".
    using SampleRateSource = std::function<std::optional<double>(const std::string &filePath)>;

    // engineLibraryPath is the directory containing "Database2/"
    // (i.e. the "Engine Library" folder itself).
    explicit LibdjinteropEngineReader(std::string engineLibraryPath);

    // readTracks() reads m.db alone: no audio file is stat'd for its size
    // (application::fillFileSizes) and no artwork file for its existence;
    // artworkPath is what the catalog names, whether or not the file is
    // still there. fillArtwork() adds the covers that take more: an image
    // file under Artwork/, looked for on the stick, and an image kept
    // inside the database, written out once to
    // paths::localEngineArtworkDir() on this computer. Only for a caller
    // that shows covers (the catalog cache's Full stage): readAll() is
    // readTracks() alone, as the checks and tools that call it want.
    //
    // The cues are in the catalog too, as sample offsets, and the row's
    // sample rate turns them into times. A row the player has not
    // analysed yet records no rate (no trackData), and a few real rows
    // record 0. For such a row with at least one cue or loop the reader
    // asks the sample rate source, when it has one; without one, or when
    // the source cannot say, it takes 44.1 kHz, which puts the cues of a
    // 48 kHz file 9 percent late (1000 ms reads 1088.4 ms). Nothing on
    // the Track says which happened. A row with a stored rate, and a row
    // with no cues, is never asked about, so the source costs nothing on
    // a library whose unanalysed rows carry no cues.
    //
    // readTracks() asks the source inline. fillCues() is for tracks a
    // reader without a source read (the catalog cache's Tracks stage,
    // which opens no audio file): with a source, it looks up the stored
    // rate of every row that has cues, asks the source for the ones with
    // none, and reads those rows' cues again at the file's rate. Without
    // a source it does nothing (the LibraryReader default).
    std::vector<domain::Track> readAll() override;
    std::vector<domain::Track> readTracks() override;
    void fillCues(std::vector<domain::Track> &tracks) override;
    void setSampleRateSource(SampleRateSource source) { m_sampleRateSource = std::move(source); }
    // count(*) of m.db's Track table over a read-only connection: no
    // journal recovery, no libdjinterop. Nothing when m.db is not there
    // or cannot be read.
    std::optional<size_t> countTracks() override;
    void fillArtwork(std::vector<domain::Track> &tracks) override;

    // The filesystem identity of the stick the library is on, when the
    // caller knows it (a UUID or volume serial). The local copies of
    // covers kept in the database are filed under it, so clones of one
    // library on two sticks keep theirs apart. Without one they are filed
    // under the library's location.
    void setVolumeIdentity(std::string identity) { m_volumeIdentity = std::move(identity); }

private:
    std::optional<double> rateFromSource(const domain::Track &track);

    std::string m_engineLibraryPath;
    std::string m_volumeIdentity;
    SampleRateSource m_sampleRateSource;
};

}  // namespace seabass::infrastructure::engine
