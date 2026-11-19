// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "application/ports/analysis_writer.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "application/ports/track_metadata_probe.hpp"
#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// What each change of a Beat Grids & Waveforms save left on its row, by
// change id, for the page to say after the save: the save summary counts
// tracks, and the page says beat grids, waveforms and tracks completed.
// Written on the save's thread, read on the GUI thread once the save has
// finished; the page counts only the ids the session reports applied, so
// a change that was rolled back, or never reached, is never counted.
class AnalysisSaveTally
{
public:
    struct Row
    {
        bool grid = false;
        bool waveform = false;
        bool completed = false;
        // EngineAnalysisChange: the player's analysis put back, or the row
        // marked for the player to analyse.
        bool restored = false;
        bool markedForPlayer = false;
        // The player's own analysis was kept on the stick first.
        bool keptEngineAnalysis = false;
        // Why a part was left alone, for a summary that says so: the grid
        // locked or moved on the player (never replaced here), a row the
        // player has not analysed that was not completed (left for the
        // player), no analysis in rekordbox, everything already the same,
        // or another reason in words.
        bool gridSetOnPlayer = false;
        bool leftForPlayer = false;
        bool noRekordboxAnalysis = false;
        bool alreadySame = false;
        // The player's analysis could not be kept first (the file is not
        // under the stick root), so it was not replaced.
        bool cannotKeep = false;
        QString otherReason;
        // With leftForPlayer, what kept the row from being completed, in
        // words ("no rekordbox waveform", "no length for the file"); empty
        // when the row was left only because one part was asked for.
        QString leftForPlayerBecause;
    };
    void record(const QString &changeId, Row row)
    {
        std::lock_guard lock(m_mutex);
        m_rows[changeId] = row;
    }
    std::optional<Row> rowFor(const QString &changeId) const
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_rows.find(changeId);
        return it == m_rows.end() ? std::nullopt : std::optional<Row>(it->second);
    }

private:
    mutable std::mutex m_mutex;
    std::map<QString, Row> m_rows;
};

// What one importAnalysis() left on its row, as the tally keeps it:
// written parts, and why a part was left alone.
AnalysisSaveTally::Row analysisTallyRowOf(const application::AnalysisImportResult &result);

// The counts behind the sentence a sync or a new library says beside its
// own count, summed from tally rows.
struct AnalysisNoteCounts
{
    int grids = 0;
    int waveforms = 0;
    int completed = 0;
    int kept = 0;
    int setOnPlayer = 0;
    int forPlayer = 0;
    int noAnalysis = 0;
    int same = 0;
    int cannotKeep = 0;
    // Left for the player, by what kept the row from being completed.
    std::map<QString, int> forPlayerBecause;
    std::map<QString, int> other;

    void add(const AnalysisSaveTally::Row &row);
};

// The sentence for a summary, from those counts: what was copied, what
// was kept first, what was left alone and why. Empty when there is
// nothing to say. Shared by every workflow that copies rekordbox's
// analysis as part of another job (Sync after Rekordbox Export, Sync Cue
// Points, Create Engine Library), so they say it alike.
QString analysisSummaryNote(const AnalysisNoteCounts &counts);

// Beat Grids & Waveforms, one Engine row: rekordbox's analysis (its beat
// grid, its waveform, and for a row the player has not analysed, all it
// needs to load without analysis) through
// LibdjinteropEngineCueWriter::importAnalysis(), in the save's Engine write
// session like every other Engine write in a save. Staged by
// BeatGridsController for each ticked track its preview said would change.
//
// One change per track, for the reason FillSampleRateChange gives: the
// unit the summary counts and the one progress bar ticks is a track, a
// cancel stops between tracks and keeps the ones that landed, and a row the
// save finds changed since the preview (locked on the player meanwhile, or
// already the same) is that one track skipped, not the whole run.
//
// The writer checks the row again when the save reaches it, with the
// options the preview ran with: a refusal then writes nothing and is a
// skip.
//
// The overview file. A row's waveform lives twice: in the database and in
// "Database2/OverviewData/<uuid>/<id>.rgb" beside it, which the scratch
// copy of the database does not hold. So the file is written to the real
// library, during apply(), which is BEFORE the database's commit when the
// save writes through a scratch copy (the copy goes back onto the stick
// when the save finishes) and, in a direct write, before the analysed flag
// of a completed row (importAnalysis() writes the file before it raises
// the flag). Either way a row is never flagged analysed in the real
// database without its file. Every file is backed up before it is written:
// the one the preview named is declared in filesToBackup(), so the save's
// up-front backup takes it (or notes it absent, so Undo removes it again);
// one the preview did not foresee is backed up the same way by the sink
// before its first byte, and protected for the change. A change that fails
// takes its file back out with its rows; a save that fails puts every file
// back as it was (SaveContext::rollBackSave), so no new overview file
// outlives a failed save. A cancelled save keeps the rows that landed, and
// their files with them, as it keeps their database rows.
//
// The player's own analysis of a row it had analysed is kept on the stick
// before the row changes (<stick>/Seabass/engine-analysis/, see
// LibdjinteropEngineCueWriter::setKeptAnalysisStore()), in this save's
// protection like the overview file: the track's entry is declared in
// filesToBackup() (absent when there is none yet, so Undo takes a new one
// away), and the store backs it up and protects it for the change before
// writing it. A failure to keep it fails the change, with the row as it
// was.
//
// The ledger (<stick>/Seabass/beat-grids.tsv, the record of the grids
// Seabass wrote) is written once per save, after the commit, with the
// fingerprint of every row whose change landed with its grid written or
// already the same; nothing when nothing landed. The first change of a
// batch declares m.db and the ledger as its backups, the ledger as absent
// when the stick has none yet, so Undo Last Save puts back the database,
// the ledger and the overview files together.
class CopyAnalysisChange : public PendingChange
{
public:
    // stickRoot: the stick's root (UTF-8), where the ledger lives.
    // rekordbox's analysis is read again from `rekordboxPath` when the save
    // reaches the change (one track's at a time, not every staged track's
    // held in memory), and written only when its digest
    // (rekordboxAnalysisDigest()) is `analysisDigest`, the preview's: a
    // track whose analysis changed in rekordbox meanwhile is skipped,
    // saying so, never written with what the preview did not show.
    // `options` the writer options the preview ran with (previewOnly is
    // cleared here). `overviewFile`: the overview file the preview said
    // would be written, from the Engine Library folder; empty for none.
    CopyAnalysisChange(QString enginePath, QString rekordboxPath, std::string stickRoot,
                       application::BeatGridCopyItem item, std::string analysisDigest,
                       application::AnalysisImportOptions options, std::string overviewFile, int itemCountHint,
                       bool declaresFiles, std::shared_ptr<AnalysisSaveTally> tally = {});

    static QString idFor(const std::string &engineTrackId);
    static QString backupLabel();

    // Ledger lines (ledger key, grid fingerprint) of other ticked rows the
    // preview found already the same, recorded in the ledger with this
    // change when it lands. The first change of a batch carries them.
    void setLedgerRiders(std::vector<std::pair<std::string, std::string>> riders)
    {
        m_ledgerRiders = std::move(riders);
    }

    QString id() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    void beforeSave(SaveContext &ctx) override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_enginePath;
    QString m_rekordboxPath;
    std::string m_stickRoot;
    application::BeatGridCopyItem m_item;
    std::string m_analysisDigest;
    application::AnalysisImportOptions m_options;
    std::string m_overviewFile;
    int m_itemCountHint = 1;
    bool m_declaresFiles = true;
    std::shared_ptr<AnalysisSaveTally> m_tally;
    std::vector<std::pair<std::string, std::string>> m_ledgerRiders;
};

// Sync after Rekordbox Export with "Copy beat grids/waveforms from
// rekordbox to Engine when syncing" on (Preferences, application::
// SyncAnalysisPolicy): one track the save writes to or creates on the
// Engine side, given rekordbox's analysis in the same save, by exactly the
// rules of CopyAnalysisChange (the same writer, kept store, ledger and
// LibdjinteropEngineCueWriter::importAnalysis() through
// application::importAnalysisItem()): a row the player analysed gets the
// parts asked for, Engine's own analysis kept on the stick first; a row
// the player has not analysed, one this very save created included, is
// completed all or nothing, so only with both parts asked for; a grid
// locked or moved on the player is never replaced (overwriteEngineEdits
// is always off) and is counted as left alone.
//
// There is no preview behind it: rekordbox's analysis is read, and a row
// the save creates is found by its file (it has no id until
// AddEngineTrackChange made it), when the save reaches the change, on the
// save's thread. Staged after the Engine writes of the sync, linked to the
// track's rows by their keys (unitKeys) so a cancel never keeps a created
// row and drops its analysis.
//
// It carries no unit of the save's count (unitsWritten() is 0, unless it is
// staged on its own with countAsUnit()): the sync's own summary stays what
// it was, and what this did is said beside it, in
// the save's summary notes (SaveContext::addSummaryNote, one sentence for
// the batch from the tally, counting only changes that landed). The tally
// is the save's own (SaveContext::sharedForWholeSave), so changes staged
// at different times, as Sync Cue Points stages them, are counted in one
// sentence, and nothing of an earlier save is counted again.
//
// Sync Cue Points carries one inside the SyncPlanChange of each row whose
// cues go to Engine (rideIn()), so the row stays one staged change, and
// stages one on its own (countAsUnit()) for each ticked row of a track
// whose cues are already in step but whose analysis its preview found
// would change, with that preview's digest (Track::analysisDigest).
//
// Failures. Reading rekordbox's analysis writes nothing, so a track whose
// analysis cannot be read is left alone and counted, and the sync goes
// on. A failure while writing is a failure of the save, as every change's
// is: the save lands whole or not at all (runSaveLoop, SaveContext::
// rollBackSave), so a sync whose analysis could not be written goes back
// whole and stays staged, and saying so beats a half written stick.
class SyncAnalysisChange : public PendingChange
{
public:
    struct Track
    {
        // The rekordbox row (sourceId, file, title, key, tempo).
        domain::Track rekordbox;
        // The Engine row, or empty for one this save creates, which is then
        // found by `engineFilePath` (exactly one row, or the track is left
        // alone).
        std::string engineTrackId;
        std::string engineFilePath;
        // The keys of the sync's rows that write the track.
        QStringList unitKeys;
        // Staged only to be counted: the reason the track is left alone
        // (two rekordbox tracks for one Engine row or one file).
        QString leaveAloneBecause;
        // The analysis file the source row names, read instead of looking
        // the track up in export.pdb: for a OneLibrary row, whose id is
        // not export.pdb's ("/PIONEER/USBANLZ/.../ANLZ0000.DAT"). Empty
        // for a DeviceLibrary row.
        std::string analysisFile;
        // The digest (rekordboxAnalysisDigest()) of the analysis a preview
        // showed, for a track staged on that preview's word (Sync Cue
        // Points' tracks whose cues are already in step): a track whose
        // analysis changed in rekordbox since is left alone, never written
        // with what the preview did not show. Empty for none.
        std::string analysisDigest;
    };

    SyncAnalysisChange(QString owner, QString rekordboxPath, QString enginePath, std::string stickRoot, Track track,
                       application::AnalysisCopyOptions options,
                       std::shared_ptr<application::TrackMetadataProbe> probe, int itemCountHint, bool declaresFiles);

    static QString idFor(const QString &owner, const std::string &rekordboxTrackId);
    // The batch's sentence for the summary, from the tally rows of the
    // changes in `applied`; empty when nothing of it landed.
    static QString summaryNote(const AnalysisSaveTally &tally, const QStringList &applied);

    // Carried inside another change rather than staged on its own (Sync
    // Cue Points: SyncPlanChange::setAnalysisRider()): this change then
    // answers to the carrier's id, so the ledger and the summary note,
    // which count the ids that landed, count the carrier's. Its apply()
    // runs inside the carrier's, after the carrier's own write.
    void rideIn(const QString &carrierId) { m_carrierId = carrierId; }

    // Staged on its own as a unit of the save (Sync Cue Points' rows of
    // tracks that need only their analysis): counted as one synced track,
    // with SyncPlanChange's verb, so a save of nothing else says it wrote
    // them and a skip says so too. A rider or a change beside a sync's own
    // (Sync after Rekordbox Export) counts nothing.
    void countAsUnit() { m_countsAsUnit = true; }

    QString id() const override;
    QString owner() const override { return m_owner; }
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    int unitsWritten() const override { return m_countsAsUnit && m_carrierId.isEmpty() ? 1 : 0; }
    QString verb() const override;
    QStringList unitKeys() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    void beforeSave(SaveContext &ctx) override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_owner;
    QString m_rekordboxPath;
    QString m_enginePath;
    std::string m_stickRoot;
    Track m_track;
    application::AnalysisCopyOptions m_options;
    std::shared_ptr<application::TrackMetadataProbe> m_probe;
    int m_itemCountHint = 1;
    bool m_declaresFiles = true;
    QString m_carrierId;
    bool m_countsAsUnit = false;
};

// What a workflow that stages SyncAnalysisChange riders reads once when it
// stages (on the GUI thread, from Preferences: application::
// SyncAnalysisPolicy), shared by Sync after Rekordbox Export and Sync Cue
// Points so both stage them alike, and the parts in words for the line
// that says so; Create Engine Library reads the same options and words.
struct SyncAnalysisStaging
{
    application::AnalysisCopyOptions options;
    // The stick root spelt as Beat Grids & Waveforms spells it, so the
    // ledger and the kept analyses are keyed as the page keys them.
    std::string stickRoot;
    // Reads a created or unanalysed row's file for its exact length.
    std::shared_ptr<application::TrackMetadataProbe> probe;

    // Off when neither switch is on: nothing is staged, the save is the
    // one it always was.
    bool any() const { return options.grids || options.waveforms; }
    // "beat grids and waveforms", "beat grids" or "waveforms".
    QString partsInWords() const;

    static SyncAnalysisStaging fromPreferences(const QString &rekordboxPath);
    // The probe a save reads audio files with: TagLib's when the build has
    // it.
    static std::shared_ptr<application::TrackMetadataProbe> metadataProbe();
};

// Beat Grids & Waveforms, one Engine row, the two ways back from a copy:
//
// - Restore: the player's own analysis kept on the stick, put back byte
//   for byte (LibdjinteropEngineCueWriter::restoreEngineAnalysis()): the
//   columns into the save's database, the overview file put back or taken
//   away in the real library, the kept entry removed, and after the commit
//   the track's ledger line taken out. A row changed on the player since
//   Seabass wrote it is left alone (a skip).
// - LetPlayerAnalyse: the row marked for the player's analysis
//   (markForDeviceAnalysis), the kept entry left so it can still be put
//   back, the ledger line taken out after the commit.
//
// One change per track, in the same save and protection as
// CopyAnalysisChange: every stick file is backed up (or noted absent)
// before it changes and protected for the change; the first change of the
// batch declares m.db and the ledger.
class EngineAnalysisChange : public PendingChange
{
public:
    enum class Kind { Restore, LetPlayerAnalyse };
    // `overviewFile`: the overview file the preview said a restore puts
    // back or takes away, from the Engine Library folder; empty for none.
    EngineAnalysisChange(Kind kind, QString enginePath, std::string stickRoot, application::BeatGridCopyItem item,
                         std::string overviewFile, int itemCountHint, bool declaresFiles,
                         std::shared_ptr<AnalysisSaveTally> tally = {});

    QString id() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    void beforeSave(SaveContext &ctx) override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    Kind m_kind;
    QString m_enginePath;
    std::string m_stickRoot;
    application::BeatGridCopyItem m_item;
    std::string m_overviewFile;
    int m_itemCountHint = 1;
    bool m_declaresFiles = true;
    std::shared_ptr<AnalysisSaveTally> m_tally;
};

}  // namespace seabass::gui
