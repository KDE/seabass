// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include <djinterop/database.hpp>

#include "application/ports/cue_writer.hpp"
#include "domain/mp3_lead_in.hpp"

namespace seabass::infrastructure::engine
{

// Writes hot cues back into an existing Engine Library track using
// libdjinterop's track::set_hot_cues(). The caller (cli/) is responsible
// for backing up the library's files before constructing this -- this
// class only performs the write itself.
class LibdjinteropEngineCueWriter : public application::CueWriter
{
public:
    explicit LibdjinteropEngineCueWriter(std::string engineLibraryPath);

    // The cues every write is given are on rekordbox's timeline; an MP3's
    // are moved onto Engine's by the file's lead-in (#50; see
    // LibdjinteropEngineReader::setLeadInSource(), which adds what this
    // takes off). By default the writer reads the start of the row's
    // file itself; a source answers instead, for a test or a caller that
    // has the answer cached. Nothing when it cannot be read: no shift.
    using LeadInSource = std::function<std::optional<domain::Mp3LeadIn>(const std::string &filePath)>;
    void setLeadInSource(LeadInSource source) { m_leadInSource = std::move(source); }

    // The Engine Library folder the rows' relative paths resolve against
    // when the writer reads a file's lead-in. The library path by default.
    // A save that writes through FormatWriteSession's scratch copy opens
    // the database there, where no audio exists, and passes the real
    // library folder (FormatWriteSession::realRoot()) here; without it
    // every lead-in reads as nothing and the cues go unshifted.
    void setAudioRoot(std::string audioRoot) { m_audioRoot = std::move(audioRoot); }

    void writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues) override;

    // The same write at a sample rate the caller read from the audio file,
    // for a row that does not record one: a row waiting for the player's
    // analysis has no trackData, and writeHotCues() would then place
    // every cue at its 44.1 kHz guess, 9% early in a 48 kHz file. A rate
    // of 0 or less is refused (std::invalid_argument).
    void writeHotCuesAtSampleRate(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues,
                                  double sampleRateHz);

    // The two authored fields that are not cues. Rating in stars (0 to
    // 5, the scale domain::Track uses); Engine's own 0-to-100 scale is
    // applied here rather than by the caller. Either may be absent, and
    // an absent one is left alone rather than cleared. Throws if the
    // track doesn't exist.
    void writeAnnotation(const std::string &trackSourceId, const std::optional<int> &stars,
                          const std::optional<std::string> &comment);

    // Fills in a Clean Up survivor's missing bpm/key from another copy
    // in its duplicate group (see domain::DuplicateCleanupPlan). Only
    // bpm and key -- Engine track artwork isn't writable through
    // libdjinterop today (djinterop::track has no set_album_art()/
    // equivalent at all, and djinterop::album_art's own header is
    // explicitly marked "TODO - implement rest of album_art class"), so
    // artwork propagation isn't offered for this format. Either
    // optional being unset just skips that field; throws if the track
    // doesn't exist.
    void propagateMissingFields(const std::string &trackSourceId, std::optional<double> bpm,
                                 std::optional<std::string> key);

    // Sets a track's last-played time. Clean Up's survivor gets the latest
    // of its duplicate copies' times; Engine keeps no play count, only this.
    void setLastPlayedAt(const std::string &trackSourceId, std::chrono::system_clock::time_point when);

private:
    void writeCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues,
                   std::optional<double> knownSampleRate);

    // The open library, kept rather than reopened per call.
    //
    // load_database() is a full SQLite open plus schema detection, and
    // every method used to do one: a 200-item save opened the same
    // database 200 times, about 151 ms each against a stick (see
    // docs/write-path-performance.md). Held lazily so constructing a
    // writer stays free for the call sites that build one and never use
    // it.
    //
    // Safe to hold for a save: libdjinterop writes through its own
    // transactions, and nothing else in this process writes this file
    // while a save owns the writer.
    djinterop::database &database();

    std::string m_engineLibraryPath;
    std::string m_audioRoot;
    std::optional<djinterop::database> m_database;
    LeadInSource m_leadInSource;
};

}  // namespace seabass::infrastructure::engine
