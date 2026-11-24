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

class EngineAnalysisRowAccess;

// The beat grid write's options and outcomes are the application's
// port (application/ports/beat_grid_writer.hpp), named here as well for
// the callers that always used them from this header.
using application::BeatGridWriteOptions;
using application::BeatGridWriteResult;
using application::UnanalysedRowGrid;
using application::AnalysisImportOptions;
using application::AnalysisImportResult;
using application::RekordboxAnalysis;
using application::EngineAnalysisFacts;
using application::EngineAnalysisRestoreOptions;
using application::EngineAnalysisRestoreResult;
using application::LetPlayerAnalyseResult;

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

    // The complete set of the row's cues, given on rekordbox's timeline
    // and stored on Engine's, the file's shift taken off. On a row that
    // carries the mark of Engine's rekordbox import (thirdPartySourceId,
    // see EngineAnalysisRowAccess::write()) the mark goes too, once the
    // cues are in, when that shift is known (knownShiftMsOf()): left on,
    // the player takes the cues written here for rekordbox's and moves
    // them about 52 ms earlier, for good, on the row's first load.
    // libdjinterop writes each column in a statement of its own, so a
    // failure to take the mark off comes after the cues are written; it
    // throws, saying so. Engine 1.x has no such column.
    void writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues) override;

    // The complete set of the row's cues as Engine's own stored positions:
    // nothing taken off, and the mark left exactly as it is. For a caller
    // putting back positions it holds as Engine stored them (Restore
    // Metadata, from a stored copy taken from Engine), which cannot say
    // whose timeline they were on: a copy an earlier build took from a
    // marked row holds rekordbox's positions, one taken from a played row
    // Engine's. Left with the mark it had, the row is as before the write.
    void writeEngineStoredCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues);

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

    // A rating set or cleared on purpose (#105, the Browse track details):
    // stars 1 to 5 as writeAnnotation() writes them (20 per star), and
    // nullopt for "no stars", which Engine stores as 0, the one value it
    // has for both "unrated" and "zero stars" (libdjinterop's RATING_NONE).
    // Unlike writeAnnotation(), which never clears, because a restore
    // only ever puts a rating back. Throws if the track doesn't exist or
    // stars is outside 1..5.
    void writeRating(const std::string &trackSourceId, std::optional<int> stars);

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
    // be worked out (scrollEntries and fileSampleRateHz): then the overview
    // file, and after it in one transaction the analysis record, the
    // overview, the beat data (both grids rekordbox's), the tempo and key
    // and the analysed flag, so a failure part way leaves a row the player
    // will still analyse. Missing anything, nothing is written and the
    // outcomes say why.
    //
    // A row whose analysed flag is not a number, or is up over beat data
    // that does not decode, is RowUnreadable in both parts: unsure whether
    // the player analysed it, it is neither replaced nor filled. With a
    // kept-analysis store, a row the player analysed whose track has no
    // key (keepKey empty: its file is not under the stick root) is
    // CannotKeepEngines in the parts that would be written, in a preview
    // too: the player's analysis is never replaced unkept.
    //
    // Every write of a row's columns is one transaction (writeBeatGrid()'s
    // too). Engine 2.x keeps these columns in Track, 3.x in
    // PerformanceData; both are written. Engine 1.x is UnsupportedSchema.
    AnalysisImportResult importAnalysis(const std::string &trackSourceId, const RekordboxAnalysis &analysis,
                                        const AnalysisImportOptions &options = {}) override;

    // Puts the player's kept analysis back (see AnalysisWriter). The
    // columns through writeEngineAnalysisColumns() (engine_track_rows.hpp),
    // in one transaction; the overview file through the sink when the kept
    // analysis had one, else removed through the remover when there is
    // one now. Refuses without a kept-analysis store (NothingKept). The
    // hook (setBeforeBeatGridWrite) is called before the first byte.
    EngineAnalysisRestoreResult restoreEngineAnalysis(const std::string &trackSourceId, const std::string &keepKey,
                                                      const EngineAnalysisRestoreOptions &options = {}) override;
    LetPlayerAnalyseResult letPlayerAnalyseAgain(const std::string &trackSourceId, const std::string &keepKey,
                                                 bool previewOnly = false) override;
    EngineAnalysisFacts engineAnalysisFacts(const std::string &trackSourceId, const std::string &keepKey) override;

    // Where the player's own analysis of a row is kept before Seabass first
    // replaces any of it (importAnalysis() and writeBeatGrid() on a row the
    // player has analysed; options' keepKey names the track). Not owned.
    // Without one nothing is kept, which only a test wants.
    //
    // Kept after every check and the hook, before the row's first byte
    // changes. A row with no key, or one whose kept entry cannot be read
    // (Damaged), is refused before that, in a preview as in a write
    // (CannotKeepEngines in the parts that would be written): a preview
    // never offers what the write would refuse. A failure to keep it (a
    // write to the store that fails) throws, and the row is not changed.
    //
    // A write has two parts that can come apart: the row's columns (one
    // transaction) and the overview file. So the entry records, before the
    // first byte changes, the row both as it is and as the write will leave
    // it, each part on its own (KeptEngineAnalysis::seabassColumnStates,
    // seabassColumnAnalyses, seabassFiles, with seabassState and
    // seabassAnalysis for the whole). A row whose every part is one of
    // those, or the kept original's, is Seabass's own, its write finished or
    // cut short (or a restore cut short): it is written again, and restored
    // without being taken for a change on the player.
    //
    // Which analysis is kept:
    // - nothing kept yet: the row as it is, unless every part the write
    //   replaces is provably Seabass's own: a row Seabass completed (the
    //   ledger's fingerprint is the row's grid and both grids are that one:
    //   the player never analysed it), or a write that replaces only the
    //   grid when that grid is the ledger's. A write that also replaces the
    //   waveform of a row whose grid alone is Seabass's keeps the row: its
    //   waveform is the player's. Unsure means keep;
    // - kept already: the kept one stays (first kept wins). It is replaced
    //   only on positive evidence of the player's new analysis: the
    //   analysis in the row's columns (analysis record, overview, Engine's
    //   own grid, the flag) is neither the kept one's nor one Seabass's last
    //   write found or left there. Then that newer analysis is kept instead.
    //   The overview file alone is no evidence (Seabass's write of it can be
    //   cut short), nor is a grid the DJ moved or locked on the player.
    //   A grid the DJ moved on the player since Seabass wrote the row
    //   (overwriteEngineEdits replaces it) is the newest grid the player
    //   made: the kept original takes it in place of its own adjusted grid,
    //   its analysis (Engine's own grid, rate, length, waveform) staying the
    //   player's first. The grid is the DJ's when it is neither the
    //   ledger's nor one Seabass wrote to the row
    //   (KeptEngineAnalysis::seabassGrids) and the row is not as Seabass
    //   left it. The kept beat data is then re-encoded, the same grids, not
    //   the same bytes. When the original's beat data does not decode or is
    //   at another rate or length, nothing can hold both: Refused;
    // - either way the entry records the row as this write leaves it,
    //   written only when that changes.
    // A row the player has not analysed keeps nothing; when it has a kept
    // entry (the player was asked to analyse it again), the entry records
    // the row as Seabass completes it.
    void setKeptAnalysisStore(application::KeptEngineAnalysisStore *store) { m_keptStore = store; }

    // Takes an overview file away (a restore where the kept analysis had
    // none), by its path from the Engine Library folder. By default it is
    // removed under setOverviewFileRoot()'s folder; a save routes it
    // through its own protection. A remover that throws fails the call.
    using OverviewFileRemover = std::function<void(const std::string &relativePath)>;
    void setOverviewFileRemover(OverviewFileRemover remover) { m_overviewFileRemover = std::move(remover); }

    // Where the overview file of a track goes: its bytes and its path from
    // the Engine Library folder ("Database2/OverviewData/<uuid>/<id>.rgb",
    // '/'-separated). By default it is written durably under the folder
    // setOverviewFileRoot() names. A save that writes the database through
    // a scratch copy routes it through its own protection and backup
    // instead; a sink that throws fails the call. The file sits outside the
    // database: on a row the player analysed it is written after the
    // columns, and a failure there leaves a row whose file is stale (players
    // leave stale ones themselves, and the kept entry knows the row for
    // Seabass's own); on a row being completed it is written before the
    // columns and the analysed flag.
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
    // The shift a write takes off this row's positions, when it is known:
    // 0 for a file that is not an MP3, the lead-in's for an MP3 read with
    // the correction on. Nothing for an MP3 whose lead-in cannot be read
    // or with the correction off, or a row that names no file: shiftMsOf()
    // takes 0 off those, and their cues may still be on rekordbox's
    // timeline, so a write never takes the mark off them.
    std::optional<double> knownShiftMsOf(djinterop::track &track);
    std::string databaseFile() const;
    // setOverviewFileRoot()'s folder, else the library's.
    std::string overviewFileRoot() const;
    // The row reads and writes of the analysis, over connections held for
    // the writer's life (one per call cost a full SQLite open each).
    EngineAnalysisRowAccess &rowAccess();
    // The row's analysis as stored; with its overview file when asked.
    struct RowNow;
    RowNow readRowNow(std::int64_t id, bool withOverviewFile);
    // Whether the row's overview file is on the stick; true when unsure.
    bool overviewFileThere(const RowNow &now, std::int64_t id);
    // The columns of `after` into the row in one transaction, when any
    // differs from `before`. Throws when it cannot.
    //
    // A grid written here is on Engine's timeline already, so with it the
    // row loses the mark of Engine's rekordbox import (thirdPartySourceId,
    // see EngineAnalysisRowAccess::write): left on, the player draws the
    // grid about 52 ms early on the row's first load. A marked row's cues
    // are on rekordbox's timeline (measured: every hot cue on such a row
    // equals rekordbox's position to 1 ms), so where it carries any
    // (carriesCues()) they are moved onto Engine's in the same transaction,
    // by the shift writeCues() takes off and under its rules
    // (movedOntoEngineTimeline()), and the mark goes. Where they cannot be
    // read safely, or the row's sample rate is not known, cues and mark
    // stay as they are: the player is then the one to move them. A row
    // without the mark keeps its cues byte for byte.
    // `knownShiftMs`: knownShiftMsOf() for the row, which the caller has
    // asked already for the grid; only read when a grid is written.
    void writeColumns(std::int64_t id, const application::EngineAnalysisColumns &before,
                      const application::EngineAnalysisColumns &after, std::optional<double> knownShiftMs);
    // What writeColumns() does with the mark and the cues of a row given a
    // grid: the mark taken off or not, and the cues to put in their place.
    struct GridMarkPlan;
    // `knownShift`: knownShiftMsOf() for the row, as the caller asked it.
    GridMarkPlan planMarkWithGrid(std::int64_t id, const application::EngineAnalysisColumns &after,
                                  std::optional<double> knownShift);
    // The overview file written (bytes) or taken away (none), through the
    // sink or the remover where there is one.
    void putOverviewFile(const std::string &relativePath, const std::optional<std::string> &bytes);
    // The hook, setBeforeBeatGridWrite().
    void beforeWrite();
    // See setKeptAnalysisStore(). `row` is the row as read before the
    // write; `apply` turns it into the row as the write will leave it.
    // With previewOnly nothing is kept: it says only whether the write
    // would be refused, which is what a preview reports and a write checks
    // before its hook, so that the two never disagree.
    struct KeepFacts
    {
        // The player has analysed the row (its flag up over usable beat
        // data): what is in it may be the player's.
        bool playerAnalysed = false;
        // The write replaces the overview column or the overview file.
        bool replacesWaveform = false;
        // The ledger's fingerprint of the grid Seabass last wrote to the
        // row (AnalysisImportOptions::ownGridFingerprint).
        std::optional<std::string> ledgerGrid;
        // The waveform replaced is the player's (holdsPlayersWaveform()):
        // kept whatever the ledger says of the grid.
        bool playersWaveform = false;
    };
    enum class Keep {
        // Nothing new kept (nothing to keep, or kept already).
        Nothing,
        // The player's analysis, or the grid the DJ last set on the player,
        // was kept for this write.
        Kept,
        // What the write would replace cannot be kept: the entry kept
        // before cannot be read, or the grid the DJ moved on the player
        // cannot be put beside the kept original. Not written.
        Refused,
    };
    Keep keepBeforeWrite(std::int64_t id, const std::string &keepKey, const KeepFacts &facts, const RowNow &row,
                         const std::function<void(application::EngineAnalysisColumns &)> &apply, bool previewOnly);
    // The overview column of an analysed row holds a waveform the player
    // drew (AnalysisImportOptions::overwritePlayerWaveforms): any value
    // but NULL or an overview of no points, unless it is known to be
    // Seabass's own, by the kept store (an entry for the row whose
    // original's overview is another and whose analysis is one Seabass
    // knows) or, with no entry, by the ledger (a row Seabass completed:
    // both its grids the ledger's grid, as keepBeforeWrite() reasons).
    bool holdsPlayersWaveform(std::int64_t id, const std::string &keepKey, const RowNow &row,
                              const std::optional<std::string> &ledgerGrid);
    // How the cues handed to writeCues() are placed.
    enum class CuePositions {
        // On rekordbox's timeline: the file's shift taken off, the mark
        // taken off when that shift is known.
        Rekordbox,
        // Engine's own stored positions: written as given, the mark left.
        EngineStored,
    };
    void writeCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues,
                   std::optional<double> knownSampleRate, CuePositions positions = CuePositions::Rekordbox);
    // thirdPartySourceId set to NULL where it is 1 (the mark), through the
    // library's tables (one connection with database()). Nothing on 1.x.
    void takeOffRekordboxImportMark(std::int64_t id);

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
    std::shared_ptr<EngineAnalysisRowAccess> m_rows;

    std::string m_engineLibraryPath;
    std::string m_audioRoot;
    int m_memoryCuesDroppedByLeadInShift = 0;
    int m_loopsDroppedByLeadInShift = 0;
    std::shared_ptr<LeadInShiftDrops> m_drops;
    std::optional<djinterop::database> m_database;
    LeadInSource m_leadInSource;
    std::function<void()> m_beforeBeatGridWrite;
    OverviewFileSink m_overviewFileSink;
    OverviewFileRemover m_overviewFileRemover;
    std::string m_overviewFileRoot;
    application::KeptEngineAnalysisStore *m_keptStore = nullptr;
};

}  // namespace seabass::infrastructure::engine
