// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/copy_analysis_change.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <memory>
#include <stdexcept>
#include <utility>

#include "application/path_key.hpp"
#include "application/sync_analysis_policy.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/local/analysis_file_facts.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/rekordbox_analysis_source.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;

constexpr const char *Format = "engine";
constexpr const char *Label = "beat-grid-copy";

// What this save's changes left on their rows, for the ledger: change id
// to (ledger key, fingerprint), no fingerprint for a line to take out (a
// row whose analysis is the player's again). For the whole save: a
// rolled-back change fails the save, and then nothing is recorded at all.
struct LedgerRecord
{
    std::map<QString, std::vector<std::pair<std::string, std::optional<std::string>>>> byChange;
    // Lines a change moves from one key to another (a relink), as the
    // ledger has them in that change's turn.
    std::map<QString, std::vector<std::pair<std::string, std::string>>> renameByChange;
};

std::string ledgerKeyFor(const std::string &stickRoot)
{
    return "beat-grid-ledger:" + application::normalizedPathKey(stickRoot);
}

std::string ledgerFileOf(const std::string &stickRoot)
{
    return pathToUtf8(infrastructure::paths::stickBeatGridLedger(pathFromUtf8(stickRoot)));
}

// An overview file's path on the stick, from its path in the Engine
// Library folder ('/'-separated, as the writer names it).
std::string overviewFileIn(const std::string &engineLibrary, const std::string &relative)
{
    return pathToUtf8(pathFromUtf8(engineLibrary) / pathFromUtf8(relative));
}

QString gridWords(application::BeatGridWriteResult::Outcome outcome)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    switch (outcome) {
    case Outcome::Written:
        return QStringLiteral("written");
    case Outcome::AlreadySame:
        return QStringLiteral("already the same");
    case Outcome::NoSourceGrid:
        return QStringLiteral("no rekordbox beat grid");
    case Outcome::RowNotAnalysed:
        return QStringLiteral("not analysed by the player");
    case Outcome::RowLocked:
        return QStringLiteral("locked on Engine");
    case Outcome::RowEditedOnEngine:
        return QStringLiteral("edited on Engine");
    case Outcome::NoSampleRate:
        return QStringLiteral("no sample rate");
    case Outcome::UnsupportedSchema:
        return QStringLiteral("Engine library too old");
    case Outcome::NotRequested:
        return QStringLiteral("not asked for");
    case Outcome::CannotKeepEngines:
        return QStringLiteral("Engine's analysis cannot be kept");
    case Outcome::RowUnreadable:
        return QStringLiteral("analysed or not cannot be read");
    case Outcome::DiffersFromEngine:
        return QStringLiteral("Engine's own differs from rekordbox's");
    }
    return QStringLiteral("unknown");
}

QString waveformWords(application::AnalysisImportResult::Waveform waveform)
{
    using Waveform = application::AnalysisImportResult::Waveform;
    switch (waveform) {
    case Waveform::Written:
        return QStringLiteral("written");
    case Waveform::AlreadySame:
        return QStringLiteral("already the same");
    case Waveform::NotRequested:
        return QStringLiteral("not asked for");
    case Waveform::NoSourceWaveform:
        return QStringLiteral("no rekordbox waveform");
    case Waveform::RowNotAnalysed:
        return QStringLiteral("not analysed by the player");
    case Waveform::NoSampleCount:
        return QStringLiteral("no length for the file");
    case Waveform::UnsupportedSchema:
        return QStringLiteral("Engine library too old");
    case Waveform::CannotKeepEngines:
        return QStringLiteral("Engine's analysis cannot be kept");
    case Waveform::RowUnreadable:
        return QStringLiteral("analysed or not cannot be read");
    case Waveform::PlayersWaveform:
        return QStringLiteral("drawn by the player");
    case Waveform::DiffersFromEngine:
        return QStringLiteral("Engine's own differs from rekordbox's");
    }
    return QStringLiteral("unknown");
}

// Registers, once per save, the ledger's update after the commit from the
// changes that landed.
void recordLedgerAfterCommit(SaveContext &ctx, const std::string &stickRoot)
{
    const std::string key = ledgerKeyFor(stickRoot);
    bool created = false;
    ctx.sharedForWholeSave<LedgerRecord>(key, [&created]() {
        created = true;
        return std::make_unique<LedgerRecord>();
    });
    if (!created) {
        return;
    }
    // One record per save, made after the commit from what landed: a save
    // stopped by a cancel still records the rows it wrote.
    ctx.onAfterCommit([&ctx, key, stickRoot](const SaveContext::AfterCommit &after) {
        const LedgerRecord *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(key);
        if (record == nullptr || after.appliedIds.isEmpty()) {
            return;
        }
        // In the order the changes were applied: where two record the same
        // track, the one applied later left the row as it is; a line moved
        // by a relink moves as the changes before it left it.
        bool anyWrite = false;
        bool anyRename = false;
        for (const QString &changeId : after.appliedIds) {
            anyWrite = anyWrite || record->byChange.count(changeId) > 0;
            anyRename = anyRename || record->renameByChange.count(changeId) > 0;
        }
        if (!anyWrite && !anyRename) {
            return;
        }
        std::string error;
        const auto root = pathFromUtf8(stickRoot);
        const infrastructure::local::BeatGridLedger previous = infrastructure::local::readBeatGridLedger(root, &error);
        // A ledger that cannot be read is replaced, as the command line
        // does: nothing in it was believed anyway.
        const bool unreadable = !error.empty();
        infrastructure::local::BeatGridLedger next = previous;
        int recorded = 0;
        int dropped = 0;
        std::vector<std::string> moved;
        for (const QString &changeId : after.appliedIds) {
            if (const auto renames = record->renameByChange.find(changeId); renames != record->renameByChange.end()) {
                for (const auto &[from, to] : renames->second) {
                    if (const auto line = next.find(from); line != next.end()) {
                        // The line of the name the file had is of the row
                        // now; one under the new name gives way.
                        moved.push_back(from + " to " + to + (next.count(to) > 0 ? ", in place of the line there" : ""));
                        next[to] = line->second;
                        next.erase(from);
                    }
                }
            }
            if (const auto entries = record->byChange.find(changeId); entries != record->byChange.end()) {
                for (const auto &[ledgerKey, fingerprint] : entries->second) {
                    if (fingerprint) {
                        next[ledgerKey] = *fingerprint;
                        ++recorded;
                    } else if (next.erase(ledgerKey) > 0) {
                        ++dropped;
                    }
                }
            }
        }
        if (next == previous && !unreadable) {
            return;
        }
        ctx.backupOnce(ledgerFileOf(stickRoot), Label);
        std::string writeError;
        if (!infrastructure::local::writeBeatGridLedger(root, next, &writeError)) {
            ctx.log().record("beat grids: the ledger could not be written: " + writeError);
            if (!anyWrite) {
                // Only lines carried by a relink: the save's log says so,
                // and the relink itself landed.
                return;
            }
            throw std::runtime_error("The beat grids were copied, but Seabass could not record which ones it wrote ("
                                     + writeError + "), so the next copy takes them for grids set on the player "
                                       "and leaves them alone.");
        }
        std::string movedWords;
        for (const std::string &one : moved) {
            movedWords += "; moved the line of " + one;
        }
        ctx.log().record("beat grids: recorded " + std::to_string(recorded) + " grid(s) in the ledger, took "
                         + std::to_string(dropped) + " out"
                         + (unreadable ? ", replacing one that could not be read (" + error + ")" : std::string())
                         + movedWords);
    });
}

// A file of the real stick about to be written or removed by this save:
// backed up first (or noted absent, so that Undo takes it away again),
// then protected for the change, so a change or save that fails puts it
// back as it was.
void protectStickFile(SaveContext &ctx, const std::string &file)
{
    BackupTarget target;
    target.file = file;
    target.label = Label;
    target.removeOnRestoreIfAbsent = true;
    ctx.backupAllNow({target});
    ctx.protectForThisChange(file);
}

using Writer = infrastructure::engine::LibdjinteropEngineCueWriter;

// The save's one writer for the stick's Engine library, with the kept
// analyses' store, both shared by every change of the page in the save.
Writer &analysisWriterFor(SaveContext &ctx, FormatWriteSession &session, const std::string &stickRoot)
{
    auto &store = ctx.shared<infrastructure::local::StickKeptEngineAnalysisStore>(
        "beat-grids:kept:" + application::normalizedPathKey(stickRoot), [&]() {
            auto made = std::make_unique<infrastructure::local::StickKeptEngineAnalysisStore>(pathFromUtf8(stickRoot));
            made->setBeforeChange([&ctx](const std::string &file) { protectStickFile(ctx, file); });
            return made;
        });
    return ctx.shared<Writer>("beat-grids:writer:" + application::normalizedPathKey(session.writeRoot()), [&]() {
        auto made = std::make_unique<Writer>(session.writeRoot());
        // The scratch copy holds the database, not the audio: the MP3
        // lead-in that moves the grid onto Engine's timeline, as it moves
        // the cues, is read from the real library (#50).
        made->setAudioRoot(session.realRoot());
        useSaveLeadIns(ctx, *made, session.realRoot());
        // Nor the overview files: they are read (to see whether one is
        // already what would be written) and written in the real library.
        made->setOverviewFileRoot(session.realRoot());
        // Each one backed up before it is written, through this save: one
        // the preview named was declared, and backupAllNow() finds it done
        // already; one it did not is backed up (or noted absent) now. Then
        // protected for the change, so a change that fails, or a save that
        // fails, takes it back out. Written durably, as the writer's own
        // default would.
        made->setOverviewFileSink([&ctx, realRoot = session.realRoot()](const std::string &relative,
                                                                         const std::string &bytes) {
            const std::string file = overviewFileIn(realRoot, relative);
            protectStickFile(ctx, file);
            fs::create_directories(pathFromUtf8(file).parent_path());
            if (!infrastructure::writeFileDurablyAtomic(file, bytes)) {
                throw std::runtime_error("could not write the overview file " + file);
            }
        });
        made->setOverviewFileRemover([&ctx, realRoot = session.realRoot()](const std::string &relative) {
            const std::string file = overviewFileIn(realRoot, relative);
            protectStickFile(ctx, file);
            std::string failure;
            if (!infrastructure::removeEntry(pathFromUtf8(file), failure)) {
                throw std::runtime_error("could not remove the overview file " + file + ": " + failure);
            }
        });
        // The player's own analysis is kept on the real stick, in this
        // save's protection, before a row it would be lost from changes.
        made->setKeptAnalysisStore(&store);
        return made;
    });
}

}  // namespace

infrastructure::engine::LibdjinteropEngineCueWriter &engineAnalysisWriterFor(SaveContext &ctx, FormatWriteSession &session,
                                                                             const std::string &stickRoot)
{
    return analysisWriterFor(ctx, session, stickRoot);
}

const std::map<std::string, std::string> &saveBeatGridLedger(SaveContext &ctx, const std::string &stickRoot)
{
    return ctx.sharedForWholeSave<infrastructure::local::BeatGridLedger>(
        "beat-grid-ledger:read:" + application::normalizedPathKey(stickRoot), [&]() {
            std::string error;
            auto ledger = std::make_unique<infrastructure::local::BeatGridLedger>(
                infrastructure::local::readBeatGridLedger(pathFromUtf8(stickRoot), &error));
            if (!error.empty()) {
                ledger->clear();
            }
            return ledger;
        });
}

void recordGridInLedgerAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                                   const std::string &ledgerKey, const std::string &gridFingerprint)
{
    if (ledgerKey.empty() || gridFingerprint.empty()) {
        return;
    }
    recordLedgerAfterCommit(ctx, stickRoot);
    if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(stickRoot))) {
        record->byChange[changeId].emplace_back(ledgerKey, std::optional<std::string>(gridFingerprint));
    }
}

void renameGridInLedgerAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                                   const std::string &fromKey, const std::string &toKey)
{
    if (fromKey.empty() || toKey.empty() || fromKey == toKey) {
        return;
    }
    recordLedgerAfterCommit(ctx, stickRoot);
    if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(stickRoot))) {
        record->renameByChange[changeId].emplace_back(fromKey, toKey);
    }
}

void forgetGridInLedgerAfterCommit(SaveContext &ctx, const std::string &stickRoot, const QString &changeId,
                                   const std::string &ledgerKey)
{
    if (ledgerKey.empty()) {
        return;
    }
    recordLedgerAfterCommit(ctx, stickRoot);
    if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(stickRoot))) {
        record->byChange[changeId].emplace_back(ledgerKey, std::nullopt);
    }
}

std::string beatGridLedgerFileOf(const std::string &stickRoot)
{
    return ledgerFileOf(stickRoot);
}

std::string keptEngineAnalysisFileOf(const std::string &stickRoot, const std::string &ledgerKey)
{
    return ledgerKey.empty()
        ? std::string()
        : pathToUtf8(infrastructure::local::keptEngineAnalysisFile(pathFromUtf8(stickRoot), ledgerKey));
}

namespace
{

// The kept analysis' file of an item, on the stick; empty without a key.
std::string keptFileOf(const std::string &stickRoot, const application::BeatGridCopyItem &item)
{
    return item.ledgerKey.empty()
        ? std::string()
        : pathToUtf8(infrastructure::local::keptEngineAnalysisFile(pathFromUtf8(stickRoot), item.ledgerKey));
}

// One importAnalysis() landed on its row: the ledger told of the grid the
// row now has, the tally of what was written, the log of either, and the
// change's outcome. Shared by CopyAnalysisChange and SyncAnalysisChange,
// so a sync records exactly what the page records.
ChangeOutcome landImport(SaveContext &ctx, FormatWriteSession &session, const QString &changeId,
                         const application::BeatGridCopyItem &item, const std::string &stickRoot,
                         const application::AnalysisImportResult &result, AnalysisSaveTally *tally)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    using Waveform = application::AnalysisImportResult::Waveform;
    const bool gridWritten = result.grid.outcome == Outcome::Written;
    const bool waveformWritten = result.waveform == Waveform::Written;

    if ((gridWritten || result.grid.outcome == Outcome::AlreadySame) && !item.ledgerKey.empty()
        && !result.grid.gridFingerprint.empty()) {
        if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(stickRoot))) {
            record->byChange[changeId].emplace_back(item.ledgerKey, std::optional<std::string>(result.grid.gridFingerprint));
        }
    }
    if (tally) {
        tally->record(changeId, analysisTallyRowOf(result));
    }
    if (gridWritten || waveformWritten) {
        session.noteItemApplied();
        ctx.log().record("beat grids: copied rekordbox's " + std::string(gridWritten ? "grid" : "")
                         + (gridWritten && waveformWritten ? " and " : "") + (waveformWritten ? "waveform" : "")
                         + (result.completedRow ? (result.analysedNothingStored
                                                       ? ", completing the row Engine marked analysed with none stored,"
                                                       : ", completing the row,")
                                                : "")
                         + " to Engine track id="
                         + item.engineTrackId);
        return ChangeOutcome::success();
    }
    // Nothing written: the row changed since the preview, or a sync found
    // it so. Said so in the summary as a skip rather than counted as copied.
    ctx.log().record("beat grids: left Engine track id=" + item.engineTrackId + " alone: grid "
                     + gridWords(result.grid.outcome).toStdString() + ", waveform "
                     + waveformWords(result.waveform).toStdString());
    return ChangeOutcome::skip();
}

}  // namespace

AnalysisSaveTally::Row analysisTallyRowOf(const application::AnalysisImportResult &result)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    using Waveform = application::AnalysisImportResult::Waveform;
    AnalysisSaveTally::Row row;
    row.grid = result.grid.outcome == Outcome::Written;
    row.waveform = result.waveform == Waveform::Written;
    row.completed = result.completedRow;
    row.keptEngineAnalysis = result.keptEngineAnalysis;
    row.gridSetOnPlayer = result.grid.outcome == Outcome::RowLocked || result.grid.outcome == Outcome::RowEditedOnEngine;
    row.cannotKeep = result.grid.outcome == Outcome::CannotKeepEngines || result.waveform == Waveform::CannotKeepEngines;
    row.differs = result.grid.outcome == Outcome::DiffersFromEngine || result.waveform == Waveform::DiffersFromEngine;
    // One track, one bucket: a row the player has not analysed whose grid
    // is locked is left alone for the lock (the DJ's word), not counted
    // again as left for the player.
    // A row Engine marked analysed with nothing stored is not the
    // player's to analyse: it is left as it is, and said so.
    if (result.analysedNothingStored && !result.completedRow && !row.gridSetOnPlayer && !row.grid && !row.waveform
        && !row.cannotKeep) {
        row.otherReason = QStringLiteral("marked analysed by Engine with nothing stored, left as it is");
        return row;
    }
    row.leftForPlayer = !result.completedRow && !row.gridSetOnPlayer
        && (result.grid.outcome == Outcome::RowNotAnalysed || result.grid.outcome == Outcome::NoSampleRate
            || result.waveform == Waveform::RowNotAnalysed || result.waveform == Waveform::NoSampleCount);
    if (row.leftForPlayer) {
        // What kept it from being completed, when it is more than one part
        // asked for alone: the part that was missing.
        if (result.waveform == Waveform::NoSampleCount || result.waveform == Waveform::PlayersWaveform) {
            row.leftForPlayerBecause = waveformWords(result.waveform);
        } else if (result.grid.outcome == Outcome::NoSampleRate || result.grid.outcome == Outcome::NoSourceGrid) {
            row.leftForPlayerBecause = gridWords(result.grid.outcome);
        } else if (result.waveform == Waveform::NoSourceWaveform) {
            row.leftForPlayerBecause = waveformWords(result.waveform);
        }
    }
    if (row.grid || row.waveform || row.gridSetOnPlayer || row.leftForPlayer || row.cannotKeep || row.differs) {
        return row;
    }
    if (result.grid.outcome == Outcome::RowUnreadable || result.waveform == Waveform::RowUnreadable) {
        row.otherReason = QStringLiteral("whether the player analysed it cannot be read");
        return row;
    }
    const bool gridNone = result.grid.outcome == Outcome::NoSourceGrid || result.grid.outcome == Outcome::NotRequested;
    const bool waveformNone =
        result.waveform == Waveform::NoSourceWaveform || result.waveform == Waveform::NotRequested;
    const bool gridSame = result.grid.outcome == Outcome::AlreadySame || result.grid.outcome == Outcome::NotRequested;
    const bool waveformSame = result.waveform == Waveform::AlreadySame || result.waveform == Waveform::NotRequested;
    if (result.waveform == Waveform::PlayersWaveform && (gridNone || gridSame)) {
        row.otherReason = QStringLiteral("its waveform drawn by the player");
    } else if (gridNone && waveformNone) {
        row.noRekordboxAnalysis = true;
    } else if (gridSame && waveformSame) {
        row.alreadySame = true;
    } else {
        row.otherReason = QStringLiteral("grid %1, waveform %2")
                              .arg(gridWords(result.grid.outcome), waveformWords(result.waveform));
    }
    return row;
}

void AnalysisNoteCounts::add(const AnalysisSaveTally::Row &row)
{
    grids += row.grid ? 1 : 0;
    waveforms += row.waveform ? 1 : 0;
    completed += row.completed ? 1 : 0;
    kept += row.keptEngineAnalysis ? 1 : 0;
    setOnPlayer += row.gridSetOnPlayer ? 1 : 0;
    forPlayer += row.leftForPlayer ? 1 : 0;
    if (row.leftForPlayer && !row.leftForPlayerBecause.isEmpty()) {
        ++forPlayerBecause[row.leftForPlayerBecause];
    }
    noAnalysis += row.noRekordboxAnalysis ? 1 : 0;
    same += row.alreadySame ? 1 : 0;
    cannotKeep += row.cannotKeep && !row.grid && !row.waveform ? 1 : 0;
    differs += row.differs ? 1 : 0;
    if (!row.otherReason.isEmpty()) {
        ++other[row.otherReason];
    }
}

CopyAnalysisChange::CopyAnalysisChange(QString enginePath, QString rekordboxPath, std::string stickRoot,
                                       application::BeatGridCopyItem item, std::string analysisDigest,
                                       application::AnalysisImportOptions options, std::string overviewFile,
                                       int itemCountHint, bool declaresFiles, std::shared_ptr<AnalysisSaveTally> tally)
    : m_enginePath(std::move(enginePath))
    , m_rekordboxPath(std::move(rekordboxPath))
    , m_stickRoot(std::move(stickRoot))
    , m_item(std::move(item))
    , m_analysisDigest(std::move(analysisDigest))
    , m_options(std::move(options))
    , m_overviewFile(std::move(overviewFile))
    , m_itemCountHint(itemCountHint)
    , m_declaresFiles(declaresFiles)
    , m_tally(std::move(tally))
{
    m_options.previewOnly = false;
}

QString CopyAnalysisChange::idFor(const std::string &engineTrackId)
{
    return QStringLiteral("beat-grids:engine:%1").arg(QString::fromStdString(engineTrackId));
}

QString CopyAnalysisChange::backupLabel()
{
    return QString::fromLatin1(Label);
}

QString CopyAnalysisChange::id() const
{
    return idFor(m_item.engineTrackId);
}

QString CopyAnalysisChange::description() const
{
    if (m_options.grids && m_options.waveforms) {
        return QStringLiteral("Copy rekordbox's beat grid and waveform to %1").arg(subject());
    }
    return m_options.grids ? QStringLiteral("Copy rekordbox's beat grid to %1").arg(subject())
                           : QStringLiteral("Copy rekordbox's waveform to %1").arg(subject());
}

QString CopyAnalysisChange::subject() const
{
    const QString title = QString::fromStdString(m_item.title);
    return title.isEmpty() ? QStringLiteral("Engine track %1").arg(QString::fromStdString(m_item.engineTrackId))
                           : QStringLiteral("\"%1\"").arg(title);
}

QString CopyAnalysisChange::unit() const
{
    return QStringLiteral("tracks");
}

QString CopyAnalysisChange::verb() const
{
    return QStringLiteral("given rekordbox's analysis");
}

QStringList CopyAnalysisChange::formatsTouched() const
{
    return {QString::fromLatin1(Format)};
}

std::vector<BackupTarget> CopyAnalysisChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    // The overview file this row's preview named: backed up with the rest
    // before anything is written, or noted absent, so that Undo takes a
    // file this save made away again. Small, so declaring it per change
    // (which also has it checkpointed per change) costs nothing.
    if (!m_overviewFile.empty()) {
        BackupTarget file;
        file.file = overviewFileIn(m_enginePath.toStdString(), m_overviewFile);
        file.label = Label;
        file.removeOnRestoreIfAbsent = true;
        targets.push_back(std::move(file));
    }
    // The kept analysis of the row: backed up, or noted absent, so that
    // Undo takes away an entry this save made.
    if (const std::string kept = keptFileOf(m_stickRoot, m_item); !kept.empty()) {
        targets.push_back({kept, Label, true});
    }
    // Once for the batch, by its first change (FillSampleRateChange's
    // reason): one copy of m.db per save, not one per track.
    if (m_declaresFiles) {
        BackupTarget ledger;
        ledger.file = ledgerFileOf(m_stickRoot);
        ledger.label = Label;
        ledger.removeOnRestoreIfAbsent = true;
        targets.push_back({FormatWriteSession::databaseFileFor(Format, m_enginePath.toStdString()), Label});
        targets.push_back(std::move(ledger));
    }
    return targets;
}

void CopyAnalysisChange::beforeSave(SaveContext &ctx)
{
    recordLedgerAfterCommit(ctx, m_stickRoot);
}

namespace
{

// What a sync's analysis changes read once per save: the ledger as the
// save found it (written only after the commit), the analysis files' index
// from one pass over export.pdb, and the files' rates. Rebuilt after a
// change that failed closes it (SaveContext::shared).
struct SyncAnalysisSources
{
    SyncAnalysisSources(const std::string &pioneer, const std::string &stickRoot,
                        std::shared_ptr<application::TrackMetadataProbe> metadataProbe)
        : probe(std::move(metadataProbe))
    {
        std::string error;
        ledger = infrastructure::local::readBeatGridLedger(pathFromUtf8(stickRoot), &error);
        // A ledger that cannot be read is no ledger: a grid Seabass wrote
        // before then reads as one set on the player and is left alone,
        // the safe side.
        ledgerError = error;
        try {
            index = std::make_unique<infrastructure::rekordbox::AnlzPathIndex>(pioneer);
        } catch (const std::exception &) {
            // Each track then finds its analysis file the slow way.
        }
        if (probe) {
            rates = std::make_unique<infrastructure::local::CachedSampleRates>(stickRoot, *probe);
        }
    }
    std::shared_ptr<application::TrackMetadataProbe> probe;
    infrastructure::local::BeatGridLedger ledger;
    std::string ledgerError;
    std::unique_ptr<infrastructure::rekordbox::AnlzPathIndex> index;
    std::unique_ptr<infrastructure::local::CachedSampleRates> rates;
};

// The save's own tally of the analysis riders of one owner: one per
// save, so the summary note counts this save's changes and nothing else.
AnalysisSaveTally &saveTallyOf(SaveContext &ctx, const QString &owner)
{
    return ctx.sharedForWholeSave<AnalysisSaveTally>("sync-analysis:tally:" + owner.toStdString(),
                                                     []() { return std::make_unique<AnalysisSaveTally>(); });
}

}  // namespace

ChangeOutcome CopyAnalysisChange::apply(SaveContext &ctx)
{
    FormatWriteSession &session = sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(), m_itemCountHint,
                                                           Label);
    // rekordbox's analysis read again, through the save's one index of the
    // export: what is written is what the preview showed, or nothing.
    // The ticked rows already the same that this change carries for the
    // ledger, recorded with it when it lands, as the command line records
    // them: a ledger that was lost learns their grids again.
    if (!m_ledgerRiders.empty()) {
        if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(m_stickRoot))) {
            for (const auto &[ledgerKey, fingerprint] : m_ledgerRiders) {
                record->byChange[id()].emplace_back(ledgerKey, std::optional<std::string>(fingerprint));
            }
        }
    }
    const std::string pioneer = m_rekordboxPath.toStdString();
    auto &sources = ctx.shared<SyncAnalysisSources>(
        "beat-grids:sources:" + application::normalizedPathKey(pioneer),
        [&]() { return std::make_unique<SyncAnalysisSources>(pioneer, m_stickRoot, nullptr); });
    const application::RekordboxAnalysis analysis =
        infrastructure::local::rekordboxAnalysisOf(pioneer, m_item, sources.index.get());
    if (infrastructure::local::rekordboxAnalysisDigest(analysis) != m_analysisDigest) {
        if (m_tally) {
            AnalysisSaveTally::Row row;
            row.otherReason = QStringLiteral("rekordbox's analysis changed since it was checked");
            m_tally->record(id(), row);
        }
        ctx.log().record("beat grids: left Engine track id=" + m_item.engineTrackId
                         + " alone: rekordbox's analysis of it changed since the preview");
        return ChangeOutcome::skip();
    }
    Writer &writer = analysisWriterFor(ctx, session, m_stickRoot);
    const application::AnalysisImportResult result = writer.importAnalysis(m_item.engineTrackId, analysis, m_options);
    return landImport(ctx, session, id(), m_item, m_stickRoot, result, m_tally.get());
}

SyncAnalysisChange::SyncAnalysisChange(QString owner, QString rekordboxPath, QString enginePath, std::string stickRoot,
                                       Track track, application::AnalysisCopyOptions options,
                                       std::shared_ptr<application::TrackMetadataProbe> probe, int itemCountHint,
                                       bool declaresFiles)
    : m_owner(std::move(owner))
    , m_rekordboxPath(std::move(rekordboxPath))
    , m_enginePath(std::move(enginePath))
    , m_stickRoot(std::move(stickRoot))
    , m_track(std::move(track))
    , m_options(std::move(options))
    , m_probe(std::move(probe))
    , m_itemCountHint(itemCountHint)
    , m_declaresFiles(declaresFiles)
{
    // Never a preview, and never over the DJ's own grid or the waveform the
    // player drew: there is no box for either in a sync.
    m_options.previewOnly = false;
    m_options.overwriteEngineEdits = false;
    m_options.overwritePlayerWaveforms = false;
    m_options.skipOverviewFile = false;
    m_options.zeroLoudness = false;
    // differingIsConflict stays as the workflow asks: Sync after Rekordbox
    // Export sets it (#136), Sync Cue Points does not, its rows being the
    // user's own choice of what to copy.
}

void SyncAnalysisChange::answerWithRekordboxs(bool lockedWhenAsked)
{
    // The user's answer for this track: rekordbox's part over Engine's own,
    // moved or drawn by the player, and locked only if it was locked when
    // the user was asked: a lock set on the player since is the DJ's newer
    // word, left as it is and said so in the note. Engine's own analysis
    // is kept on the stick first, as for every write over it.
    m_options.overwriteEngineEdits = m_options.grids;
    m_options.respectLock = !lockedWhenAsked;
    m_options.overwritePlayerWaveforms = m_options.waveforms;
    m_options.differingIsConflict = false;
    m_answer = true;
    m_countsAsUnit = true;
}

QString SyncAnalysisChange::idFor(const QString &owner, const std::string &rekordboxTrackId)
{
    return owner + QStringLiteral(":analysis:") + QString::fromStdString(rekordboxTrackId);
}

QString SyncAnalysisChange::id() const
{
    if (!m_carrierId.isEmpty()) {
        return m_carrierId;
    }
    // An answer is a change of its own beside the track's rider, per part.
    if (m_answer) {
        return idFor(m_owner, m_track.rekordbox.sourceId) + QStringLiteral(":answer:")
            + (m_options.grids ? QStringLiteral("grid") : QString()) + (m_options.waveforms ? QStringLiteral("waveform") : QString());
    }
    return idFor(m_owner, m_track.rekordbox.sourceId);
}

QString SyncAnalysisChange::subject() const
{
    const std::string &title = m_track.rekordbox.title.empty() ? m_track.rekordbox.filename : m_track.rekordbox.title;
    return title.empty() ? QStringLiteral("rekordbox track %1").arg(QString::fromStdString(m_track.rekordbox.sourceId))
                         : QStringLiteral("\"%1\"").arg(QString::fromStdString(title));
}

QString SyncAnalysisChange::description() const
{
    if (m_options.grids && m_options.waveforms) {
        return QStringLiteral("Copy rekordbox's beat grid and waveform to %1").arg(subject());
    }
    return m_options.grids ? QStringLiteral("Copy rekordbox's beat grid to %1").arg(subject())
                           : QStringLiteral("Copy rekordbox's waveform to %1").arg(subject());
}

QString SyncAnalysisChange::unit() const
{
    return QStringLiteral("tracks");
}

QString SyncAnalysisChange::verb() const
{
    // A row of Sync Cue Points: a track that gets its analysis is a synced
    // track, said as that page's cue rows say theirs.
    if (m_countsAsUnit && m_carrierId.isEmpty() && !m_answer) {
        return QStringLiteral("synchronised");
    }
    return QStringLiteral("given rekordbox's analysis");
}

QStringList SyncAnalysisChange::unitKeys() const
{
    return m_track.unitKeys.isEmpty() ? QStringList{id()} : m_track.unitKeys;
}

QStringList SyncAnalysisChange::formatsTouched() const
{
    return {QString::fromLatin1(Format)};
}

std::vector<BackupTarget> SyncAnalysisChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    if (!m_track.leaveAloneBecause.isEmpty()) {
        return targets;
    }
    // The kept analysis of the row, as CopyAnalysisChange declares it; the
    // overview file is not known before the save (there is no preview), so
    // the writer's sink backs it up, or notes it absent, before its first
    // byte (analysisWriterFor()).
    const application::BeatGridCopyItem item = application::beatGridCopyItemFor(m_track.rekordbox, {}, m_stickRoot);
    if (const std::string kept = keptFileOf(m_stickRoot, item); !kept.empty()) {
        targets.push_back({kept, Label, true});
    }
    if (m_declaresFiles) {
        targets.push_back({FormatWriteSession::databaseFileFor(Format, m_enginePath.toStdString()), Label});
        targets.push_back({ledgerFileOf(m_stickRoot), Label, true});
    }
    return targets;
}

void SyncAnalysisChange::beforeSave(SaveContext &ctx)
{
    recordLedgerAfterCommit(ctx, m_stickRoot);
    // One sentence for the batch, once per save, from what landed.
    bool created = false;
    ctx.sharedForWholeSave<bool>("sync-analysis:note:" + m_owner.toStdString(), [&]() {
        created = true;
        return std::make_unique<bool>(true);
    });
    if (!created) {
        return;
    }
    AnalysisSaveTally *tally = &saveTallyOf(ctx, m_owner);
    ctx.onAfterCommit([&ctx, tally](const SaveContext::AfterCommit &after) {
        const QString note = summaryNote(*tally, after.appliedIds);
        if (!note.isEmpty()) {
            ctx.addSummaryNote(note);
            ctx.log().record("beat grids: " + note.toStdString());
        }
    });
}

ChangeOutcome SyncAnalysisChange::apply(SaveContext &ctx)
{
    const auto leftAlone = [&](const QString &why) {
        AnalysisSaveTally::Row row;
        row.otherReason = why;
        saveTallyOf(ctx, m_owner).record(id(), row);
        ctx.log().record("beat grids: left " + subject().toStdString() + " alone: " + why.toStdString());
        return ChangeOutcome::skip();
    };
    if (!m_track.leaveAloneBecause.isEmpty()) {
        return leftAlone(m_track.leaveAloneBecause);
    }
    // Staged while Preferences asked for these parts: a part whose switch,
    // or Experimental, was turned off since is not written, and neither is
    // the rest of the track's analysis (staged as one).
    if ((m_options.grids && !application::SyncAnalysisPolicy::copyBeatGrids())
        || (m_options.waveforms && !application::SyncAnalysisPolicy::copyWaveforms())) {
        return leftAlone(QStringLiteral("copying it from rekordbox was turned off in Preferences since it was staged"));
    }
    FormatWriteSession &session = sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(), m_itemCountHint,
                                                           Label);
    std::string engineId = m_track.engineTrackId;
    if (engineId.empty()) {
        // A row this save made: found by its file in the save's database,
        // and only when exactly one row names it.
        const auto rows =
            infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), m_track.engineFilePath);
        if (rows.size() != 1) {
            return leftAlone(rows.empty() ? QStringLiteral("Engine has no row for its file")
                                          : QStringLiteral("Engine has %1 rows for its file").arg(rows.size()));
        }
        engineId = std::to_string(rows.front());
    }
    const application::BeatGridCopyItem item =
        application::beatGridCopyItemFor(m_track.rekordbox, engineId, m_stickRoot);
    const std::string pioneer = m_rekordboxPath.toStdString();
    auto &sources = ctx.shared<SyncAnalysisSources>(
        "sync-analysis:sources:" + application::normalizedPathKey(pioneer),
        [&]() { return std::make_unique<SyncAnalysisSources>(pioneer, m_stickRoot, m_probe); });

    // Reading writes nothing: a track whose analysis cannot be read is
    // left alone, and the sync goes on.
    application::RekordboxAnalysis analysis;
    try {
        // Only the parts this copies: the other's file is not read, and a
        // preview's digest was taken of the same parts.
        analysis = m_track.analysisFile.empty()
            ? infrastructure::local::rekordboxAnalysisOf(pioneer, item, sources.index.get(), m_options.grids,
                                                         m_options.waveforms)
            : infrastructure::local::rekordboxAnalysisAt(pioneer, m_track.analysisFile, item, m_options.grids,
                                                         m_options.waveforms);
    } catch (const std::exception &e) {
        return leftAlone(QStringLiteral("rekordbox's analysis could not be read (%1)").arg(QString::fromUtf8(e.what())));
    }
    // Staged on a preview's word: what is written is what it showed, or
    // nothing.
    if (!m_track.analysisDigest.empty()
        && infrastructure::local::rekordboxAnalysisDigest(analysis) != m_track.analysisDigest) {
        return leftAlone(QStringLiteral("rekordbox's analysis changed since it was checked"));
    }
    const application::AnalysisFileFactsSource facts = [&sources](const application::BeatGridCopyItem &of) {
        if (!sources.rates) {
            return application::AnalysisFileFacts{};
        }
        return infrastructure::local::analysisFileFactsOf(of, *sources.rates);
    };
    Writer &writer = analysisWriterFor(ctx, session, m_stickRoot);
    const application::AnalysisItemOutcome outcome =
        application::importAnalysisItem(item, analysis, writer, sources.ledger, m_options, facts);
    return landImport(ctx, session, id(), item, m_stickRoot, outcome.result, &saveTallyOf(ctx, m_owner));
}

QString SyncAnalysisStaging::partsInWords() const
{
    if (options.grids && options.waveforms) {
        return QStringLiteral("beat grids and waveforms");
    }
    return options.grids ? QStringLiteral("beat grids") : QStringLiteral("waveforms");
}

SyncAnalysisStaging SyncAnalysisStaging::fromPreferences(const QString &rekordboxPath)
{
    SyncAnalysisStaging staging;
    staging.options.grids = application::SyncAnalysisPolicy::copyBeatGrids();
    staging.options.waveforms = application::SyncAnalysisPolicy::copyWaveforms();
    staging.stickRoot = infrastructure::paths::normalStickRootForCatalogPath(rekordboxPath.toStdString());
    if (staging.any()) {
        staging.probe = metadataProbe();
    }
    return staging;
}

std::shared_ptr<application::TrackMetadataProbe> SyncAnalysisStaging::metadataProbe()
{
#ifdef SEABASS_HAVE_TAGLIB
    return std::make_shared<infrastructure::audio::TagLibMetadataProbe>();
#else
    return std::make_shared<application::NullTrackMetadataProbe>();
#endif
}

QString SyncAnalysisChange::summaryNote(const AnalysisSaveTally &tally, const QStringList &applied)
{
    AnalysisNoteCounts counts;
    for (const QString &changeId : applied) {
        if (const auto row = tally.rowFor(changeId)) {
            counts.add(*row);
        }
    }
    return analysisSummaryNote(counts);
}

namespace
{

QString counted(int n, const QString &one, const QString &many)
{
    return n == 1 ? one : QStringLiteral("%1 %2").arg(n).arg(many);
}

}  // namespace

QString analysisSummaryNote(const AnalysisNoteCounts &c)
{
    QStringList sentences;
    if (c.grids > 0 || c.waveforms > 0) {
        QStringList copied;
        if (c.grids > 0) {
            copied << counted(c.grids, QStringLiteral("one beat grid"), QStringLiteral("beat grids"));
        }
        if (c.waveforms > 0) {
            copied << counted(c.waveforms, QStringLiteral("one waveform"), QStringLiteral("waveforms"));
        }
        QString text = QStringLiteral("From rekordbox, ") + copied.join(QStringLiteral(" and ")) + QStringLiteral(" copied");
        if (c.completed > 0) {
            text += QStringLiteral(", ")
                + counted(c.completed, QStringLiteral("one track now loads"), QStringLiteral("tracks now load"))
                + QStringLiteral(" without analysis");
        }
        sentences << text + QLatin1Char('.');
        if (c.kept > 0) {
            sentences << QStringLiteral("Engine's own analysis of %1 was kept on the stick first.")
                             .arg(counted(c.kept, QStringLiteral("one track"), QStringLiteral("tracks")));
        }
    }
    QStringList alone;
    if (c.setOnPlayer > 0) {
        alone << counted(c.setOnPlayer, QStringLiteral("one beat grid locked or moved on the player"),
                         QStringLiteral("beat grids locked or moved on the player"));
    }
    if (c.forPlayer > 0) {
        QString text = counted(c.forPlayer, QStringLiteral("one track left for the player to analyse"),
                               QStringLiteral("tracks left for the player to analyse"));
        // Why, where it was more than one part asked for alone.
        if (!c.forPlayerBecause.empty()) {
            QStringList why;
            for (const auto &[reason, n] : c.forPlayerBecause) {
                why << (c.forPlayerBecause.size() == 1 && n == c.forPlayer ? reason
                                                                          : QStringLiteral("%1: %2").arg(reason).arg(n));
            }
            text += QStringLiteral(" (") + why.join(QStringLiteral(", ")) + QLatin1Char(')');
        }
        alone << text;
    }
    if (c.differs > 0) {
        alone << counted(c.differs,
                         QStringLiteral("one track whose beat grid or waveform on Engine differs from rekordbox's, "
                                        "a conflict for you to answer"),
                         QStringLiteral("tracks whose beat grid or waveform on Engine differs from rekordbox's, "
                                        "conflicts for you to answer"));
    }
    if (c.noAnalysis > 0) {
        alone << counted(c.noAnalysis, QStringLiteral("one track rekordbox has no analysis of"),
                         QStringLiteral("tracks rekordbox has no analysis of"));
    }
    if (c.same > 0) {
        alone << counted(c.same, QStringLiteral("one track already the same"), QStringLiteral("tracks already the same"));
    }
    if (c.cannotKeep > 0) {
        alone << counted(c.cannotKeep,
                         QStringLiteral("one track whose own Engine analysis could not be kept first (its file "
                                        "not under the stick's root, or what was kept of it before unreadable)"),
                         QStringLiteral("tracks whose own Engine analysis could not be kept first (their files not "
                                        "under the stick's root, or what was kept of them before unreadable)"));
    }
    for (const auto &[why, n] : c.other) {
        alone << counted(n, QStringLiteral("one track"), QStringLiteral("tracks")) + QStringLiteral(" (") + why
                + QLatin1Char(')');
    }
    if (!alone.isEmpty()) {
        sentences << QStringLiteral("Analysis left alone: ") + alone.join(QStringLiteral(", ")) + QLatin1Char('.');
    }
    return sentences.join(QLatin1Char(' '));
}

EngineAnalysisChange::EngineAnalysisChange(Kind kind, QString enginePath, std::string stickRoot,
                                           application::BeatGridCopyItem item, std::string overviewFile,
                                           int itemCountHint, bool declaresFiles,
                                           std::shared_ptr<AnalysisSaveTally> tally)
    : m_kind(kind)
    , m_enginePath(std::move(enginePath))
    , m_stickRoot(std::move(stickRoot))
    , m_item(std::move(item))
    , m_overviewFile(std::move(overviewFile))
    , m_itemCountHint(itemCountHint)
    , m_declaresFiles(declaresFiles)
    , m_tally(std::move(tally))
{
}

QString EngineAnalysisChange::id() const
{
    return CopyAnalysisChange::idFor(m_item.engineTrackId);
}

QString EngineAnalysisChange::subject() const
{
    const QString title = QString::fromStdString(m_item.title);
    return title.isEmpty() ? QStringLiteral("Engine track %1").arg(QString::fromStdString(m_item.engineTrackId))
                           : QStringLiteral("\"%1\"").arg(title);
}

QString EngineAnalysisChange::description() const
{
    return m_kind == Kind::Restore ? QStringLiteral("Restore Engine's analysis of %1").arg(subject())
                                   : QStringLiteral("Let the player analyse %1 again").arg(subject());
}

QString EngineAnalysisChange::unit() const
{
    return QStringLiteral("tracks");
}

QString EngineAnalysisChange::verb() const
{
    return m_kind == Kind::Restore ? QStringLiteral("given back Engine's analysis")
                                   : QStringLiteral("left for the player to analyse");
}

QStringList EngineAnalysisChange::formatsTouched() const
{
    return {QString::fromLatin1(Format)};
}

std::vector<BackupTarget> EngineAnalysisChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    if (!m_overviewFile.empty()) {
        targets.push_back({overviewFileIn(m_enginePath.toStdString(), m_overviewFile), Label, true});
    }
    // A restore takes the kept entry away; Undo puts it back. (Letting the
    // player analyse leaves it, unless it is damaged.)
    if (const std::string kept = keptFileOf(m_stickRoot, m_item); !kept.empty()) {
        targets.push_back({kept, Label, true});
    }
    if (m_declaresFiles) {
        targets.push_back({FormatWriteSession::databaseFileFor(Format, m_enginePath.toStdString()), Label});
        targets.push_back({ledgerFileOf(m_stickRoot), Label, true});
    }
    return targets;
}

void EngineAnalysisChange::beforeSave(SaveContext &ctx)
{
    recordLedgerAfterCommit(ctx, m_stickRoot);
}

ChangeOutcome EngineAnalysisChange::apply(SaveContext &ctx)
{
    FormatWriteSession &session = sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(), m_itemCountHint,
                                                           Label);
    Writer &writer = analysisWriterFor(ctx, session, m_stickRoot);
    bool done = false;
    std::string why;
    if (m_kind == Kind::Restore) {
        using Outcome = application::EngineAnalysisRestoreResult::Outcome;
        const auto result = writer.restoreEngineAnalysis(m_item.engineTrackId, m_item.ledgerKey, {});
        done = result.outcome == Outcome::Restored || result.outcome == Outcome::AlreadyEngines;
        why = result.reason.empty() ? std::string("nothing kept to put back") : result.reason;
    } else {
        using Outcome = application::LetPlayerAnalyseResult::Outcome;
        const auto result = writer.letPlayerAnalyseAgain(m_item.engineTrackId, m_item.ledgerKey, false);
        // A row whose flag is down: clearing what could not be read of it
        // is the work, and it is done.
        done = result.outcome == Outcome::Marked || result.clearedDamagedEntry;
        why = result.outcome == Outcome::NotAnalysed ? "the row is not analysed" : "the Engine library is too old";
    }
    const std::string what = m_kind == Kind::Restore ? "restored Engine's analysis of" : "marked for the player's analysis";
    if (!done) {
        ctx.log().record("beat grids: left Engine track id=" + m_item.engineTrackId + " alone: " + why);
        return ChangeOutcome::skip();
    }
    if (!m_item.ledgerKey.empty()) {
        if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(m_stickRoot))) {
            record->byChange[id()].emplace_back(m_item.ledgerKey, std::nullopt);
        }
    }
    if (m_tally) {
        AnalysisSaveTally::Row row;
        row.restored = m_kind == Kind::Restore;
        row.markedForPlayer = m_kind == Kind::LetPlayerAnalyse;
        m_tally->record(id(), row);
    }
    session.noteItemApplied();
    ctx.log().record("beat grids: " + what + " Engine track id=" + m_item.engineTrackId);
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
