// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <djinterop/database.hpp>

#include "application/ports/analysis_writer.hpp"
#include "application/ports/beat_grid_writer.hpp"
#include "application/ports/cue_writer.hpp"
#include "domain/beat_grid.hpp"
#include "domain/mp3_lead_in.hpp"

namespace seabass::infrastructure::engine
{

// The beat grid write's options and outcomes are the application's
// port (application/ports/beat_grid_writer.hpp), named here as well for
// the callers that always used them from this header.
using application::BeatGridWriteOptions;
using application::BeatGridWriteResult;
using application::UnanalysedRowGrid;
using application::AnalysisImportOptions;
using application::AnalysisImportResult;
using application::RekordboxAnalysis;

// Writes hot cues back into an existing Engine Library track using
// libdjinterop's track::set_hot_cues(). The caller (cli/) is responsible
// for backing up the library's files before constructing this -- this
// class only performs the write itself.
class LibdjinteropEngineCueWriter : public application::CueWriter,
                                    public application::BeatGridWriter,
                                    public application::AnalysisWriter
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

    // What a write to this track takes off every position: the shift
    // the reader adds for its file, by the same source and rule, 0 when
    // the correction is off or the lead-in cannot be read. For a caller
    // holding positions on another timeline (Restore Metadata, whose
    // stored Engine cues carry the shift they were read with) to move
    // them first. Throws if the track does not exist.
    double cueTimelineShiftMs(const std::string &trackSourceId);

    // Memory cues left out, over every write so far, because the lead-in
    // shift alone put them at or before Engine's 0 (#50): a cue in the
    // first 51 ms of a LAME MP3. Engine reads a main cue at 0 as none, so
    // it cannot be kept; a hot cue at the same place is clamped to 0
    // instead. Counted so a caller can say so rather than lose it quietly.
    int memoryCuesDroppedByLeadInShift() const { return m_memoryCuesDroppedByLeadInShift; }
    // Loops left out for the same reason: wholly inside the lead-in, so
    // the shift leaves [0, 0]. One that starts inside and ends after it
    // is kept, its start clamped to 0.
    int loopsDroppedByLeadInShift() const { return m_loopsDroppedByLeadInShift; }

    // Also counts both into `drops`, which outlives this writer: a GUI
    // save holds one per stick and says what was left out once the save
    // has landed (useSaveLeadIns), whichever writer did the writing.
    struct LeadInShiftDrops
    {
        int memoryCues = 0;
        int loops = 0;
    };
    void countLeadInShiftDropsInto(std::shared_ptr<LeadInShiftDrops> drops) { m_drops = std::move(drops); }

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

    // Puts a rekordbox beat grid (every beat, on rekordbox's timeline, as
    // the rekordbox reader gives it) into the row's adjusted grid, the one
    // the player plays by. Engine's own analysis, the default grid, stays
    // as it is, so the player can still go back to it. The grid is moved
    // onto Engine's timeline by the same lead-in shift as the cues, and
    // put in samples at the row's own rate. Throws if the track does not
    // exist; every refusal is an outcome, with nothing written.
    BeatGridWriteResult writeBeatGrid(const std::string &trackSourceId, const std::vector<domain::Beat> &rekordboxBeats,
                                      const BeatGridWriteOptions &options = {}) override;

    // Puts rekordbox's analysis into one row (see application/ports/analysis_writer.hpp).
    //
    // A row the player has analysed: the grid part is writeBeatGrid()'s
    // for such a row (the adjusted grid only, the same refusals and
    // fingerprint). The waveform part replaces the overview in the
    // database and the overview file with ones converted from rekordbox's
    // preview (three-band, else colour) over the row's own length. The
    // analysis record, the analysed flag and the tempo stay.
    //
    // A row the player has not analysed is completed all or nothing: a row
    // flagged analysed with a piece missing does not play on a Prime 4 (no
    // waveform drawn, the track never starts). Only when both parts are
    // asked for, the grid converts, a preview is there and the length can
    // be worked out (scrollEntries and fileSampleRateHz): then the analysis
    // record, the overview, the beat data (both grids rekordbox's), the
    // overview file, the tempo and key, and last the analysed flag, so a
    // failure part way leaves a row the player will still analyse. Missing
    // anything, nothing is written and the outcomes say why.
    //
    // Engine 2.x keeps these columns in Track, 3.x in PerformanceData; both
    // are written. Engine 1.x is UnsupportedSchema.
    AnalysisImportResult importAnalysis(const std::string &trackSourceId, const RekordboxAnalysis &analysis,
                                        const AnalysisImportOptions &options = {}) override;

    // Where the overview file of a track goes: its bytes and its path from
    // the Engine Library folder ("Database2/OverviewData/<uuid>/<id>.rgb",
    // '/'-separated). By default it is written durably under the folder
    // setOverviewFileRoot() names. A save that writes the database through
    // a scratch copy routes it through its own protection and backup
    // instead; a sink that throws fails the call. The file sits outside the
    // database, so it is written after the database's columns: a failure
    // there leaves a row whose file is stale, and players leave stale ones
    // themselves, so that is a state they already handle.
    using OverviewFileSink = std::function<void(const std::string &relativePath, const std::string &bytes)>;
    void setOverviewFileSink(OverviewFileSink sink) { m_overviewFileSink = std::move(sink); }

    // The Engine Library folder the overview files are read from, to see
    // whether one is already what would be written, and written to by the
    // default sink. The library path by default; a save writing through
    // FormatWriteSession's scratch copy passes realRoot() here, since the
    // files are not part of that copy.
    void setOverviewFileRoot(std::string root) { m_overviewFileRoot = std::move(root); }

    // Called once before each write of writeBeatGrid() or importAnalysis(),
    // after every check has passed and before a byte of the row or of an
    // overview file changes; never for a refusal or what is already
    // there, nor for a preview (previewOnly). For a caller that backs the
    // library up only when something is about to be written. If it
    // throws, the write does not happen and the exception leaves the call.
    void setBeforeBeatGridWrite(std::function<void()> hook) { m_beforeBeatGridWrite = std::move(hook); }

private:
    double shiftMsOf(djinterop::track &track);
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

    // The same open library, at the level of its tables: beat data has
    // two grids and the high-level track API writes both. Absent on
    // Engine 1.x. Opened by database(), on one connection with it.
    struct Tables;
    std::shared_ptr<Tables> m_tables;

    std::string m_engineLibraryPath;
    std::string m_audioRoot;
    int m_memoryCuesDroppedByLeadInShift = 0;
    int m_loopsDroppedByLeadInShift = 0;
    std::shared_ptr<LeadInShiftDrops> m_drops;
    std::optional<djinterop::database> m_database;
    LeadInSource m_leadInSource;
    std::function<void()> m_beforeBeatGridWrite;
    OverviewFileSink m_overviewFileSink;
    std::string m_overviewFileRoot;
};

}  // namespace seabass::infrastructure::engine
