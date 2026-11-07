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
#include "domain/mp3_lead_in.hpp"
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

    // The lead-in of an MP3 file (#50, domain::Mp3LeadIn), or nothing
    // when it cannot be read. Same contract as SampleRateSource.
    using LeadInSource = std::function<std::optional<domain::Mp3LeadIn>(const std::string &filePath)>;

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
    // a source that part does nothing. It then asks the lead-in source
    // again for every cued MP3 row (see setLeadInSource()).
    std::vector<domain::Track> readAll() override;
    std::vector<domain::Track> readTracks() override;
    void fillCues(std::vector<domain::Track> &tracks) override;
    void setSampleRateSource(SampleRateSource source) { m_sampleRateSource = std::move(source); }

    // Engine positions on an MP3 are moved onto rekordbox's timeline as
    // they are read (#50; engine_cue_timeline.hpp): every hot cue, both
    // ends of every loop and the main cue get the file's
    // engineToRekordboxOffsetMs(), and Track::cueTimelineShiftMs says
    // how much. Only for a row with cues whose file is an .mp3, and only
    // while MatchingPolicy::correctMp3CueTiming() is on.
    //
    // ALWAYS, whatever the caller: the Engine cue writer takes the shift
    // off every write, so a reader that skipped it would hand a writer
    // positions it then moves 51 ms early, once per save. Without a
    // source of its own the reader reads the file's first kilobytes
    // itself (infrastructure::audio::readMp3LeadIn); a source is a
    // cheaper answer to the same question, such as the stick's metadata
    // cache (CachedSampleRates::leadInOf). readTracks() asks per cued
    // MP3 row; fillCues() asks again and moves a row whose answer
    // changed by the difference.
    void setLeadInSource(LeadInSource source) { m_leadInSource = std::move(source); }

    // The Engine Library folder the rows' relative paths are resolved
    // against to find their audio, for the lead-in and the sample rate
    // sources; by default the library read, as on a stick. A copy of a
    // library (a stick backup, a scratch copy) has no audio beside it, so
    // its MP3s would read no lead-in and their cues stay on Engine's
    // timeline next to the live stick's shifted ones: a caller that reads
    // a copy to compare with, or restore onto, a live stick passes that
    // stick's Engine Library folder here, like the cue writer's
    // setAudioRoot(). Track::filePath still names the library read.
    // Browsing a backup with no live stick to resolve against keeps the
    // default, and its Engine cues are shown on Engine's own timeline.
    void setAudioRoot(std::string audioRoot) { m_audioRoot = std::move(audioRoot); }
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
    double shiftFromSource(const domain::Track &track);
    // The track's audio file under the audio root (setAudioRoot()).
    std::string audioFileOf(const domain::Track &track) const;
    void fillRatesFromSource(std::vector<domain::Track> &tracks);

    std::string m_engineLibraryPath;
    std::string m_audioRoot;
    std::string m_volumeIdentity;
    SampleRateSource m_sampleRateSource;
    LeadInSource m_leadInSource;
};

}  // namespace seabass::infrastructure::engine
