// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/copy_analysis_change.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include "application/path_key.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;

constexpr const char *Format = "engine";
constexpr const char *Label = "beat-grid-copy";

// What this save's changes left on their rows, for the ledger: change id
// to (ledger key, fingerprint). For the whole save: a rolled-back change
// fails the save, and then nothing is recorded at all.
struct LedgerRecord
{
    std::map<QString, std::pair<std::string, std::string>> byChange;
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
    }
    return QStringLiteral("unknown");
}

}  // namespace

CopyAnalysisChange::CopyAnalysisChange(QString enginePath, std::string stickRoot, application::BeatGridCopyItem item,
                                       application::RekordboxAnalysis analysis,
                                       application::AnalysisImportOptions options, std::string overviewFile,
                                       int itemCountHint, bool declaresFiles, std::shared_ptr<AnalysisSaveTally> tally)
    : m_enginePath(std::move(enginePath))
    , m_stickRoot(std::move(stickRoot))
    , m_item(std::move(item))
    , m_analysis(std::move(analysis))
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
    const std::string key = ledgerKeyFor(m_stickRoot);
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
    ctx.onAfterCommit([&ctx, key, stickRoot = m_stickRoot](const SaveContext::AfterCommit &after) {
        const LedgerRecord *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(key);
        if (record == nullptr || after.appliedIds.isEmpty()) {
            return;
        }
        std::map<std::string, std::string> landed;
        for (const auto &[changeId, entry] : record->byChange) {
            if (after.appliedIds.contains(changeId)) {
                landed[entry.first] = entry.second;
            }
        }
        if (landed.empty()) {
            return;
        }
        std::string error;
        const auto root = pathFromUtf8(stickRoot);
        const infrastructure::local::BeatGridLedger previous = infrastructure::local::readBeatGridLedger(root, &error);
        // A ledger that cannot be read is replaced, as the command line
        // does: nothing in it was believed anyway.
        const bool unreadable = !error.empty();
        infrastructure::local::BeatGridLedger next = previous;
        for (const auto &[ledgerKey, fingerprint] : landed) {
            next[ledgerKey] = fingerprint;
        }
        if (next == previous && !unreadable) {
            return;
        }
        ctx.backupOnce(ledgerFileOf(stickRoot), Label);
        std::string writeError;
        if (!infrastructure::local::writeBeatGridLedger(root, next, &writeError)) {
            throw std::runtime_error("The beat grids were copied, but Seabass could not record which ones it wrote ("
                                     + writeError + "), so the next copy takes them for grids set on the player "
                                       "and leaves them alone.");
        }
        ctx.log().record("beat grids: recorded " + std::to_string(landed.size()) + " grid(s) in the ledger"
                         + (unreadable ? ", replacing one that could not be read (" + error + ")" : std::string()));
    });
}

ChangeOutcome CopyAnalysisChange::apply(SaveContext &ctx)
{
    FormatWriteSession &session = sharedFormatWriteSession(ctx, Format, m_enginePath.toStdString(), m_itemCountHint,
                                                           Label);
    using Writer = infrastructure::engine::LibdjinteropEngineCueWriter;
    Writer &writer = ctx.shared<Writer>("beat-grids:writer:" + application::normalizedPathKey(session.writeRoot()), [&]() {
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
            BackupTarget target;
            target.file = file;
            target.label = Label;
            target.removeOnRestoreIfAbsent = true;
            ctx.backupAllNow({target});
            ctx.protectForThisChange(file);
            fs::create_directories(pathFromUtf8(file).parent_path());
            if (!infrastructure::writeFileDurablyAtomic(file, bytes)) {
                throw std::runtime_error("could not write the overview file " + file);
            }
        });
        return made;
    });

    const application::AnalysisImportResult result = writer.importAnalysis(m_item.engineTrackId, m_analysis, m_options);
    using Outcome = application::BeatGridWriteResult::Outcome;
    using Waveform = application::AnalysisImportResult::Waveform;
    const bool gridWritten = result.grid.outcome == Outcome::Written;
    const bool waveformWritten = result.waveform == Waveform::Written;

    if ((gridWritten || result.grid.outcome == Outcome::AlreadySame) && !m_item.ledgerKey.empty()
        && !result.grid.gridFingerprint.empty()) {
        if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(m_stickRoot))) {
            record->byChange[id()] = {m_item.ledgerKey, result.grid.gridFingerprint};
        }
    }
    if (m_tally) {
        m_tally->record(id(), {gridWritten, waveformWritten, result.completedRow});
    }
    if (gridWritten || waveformWritten) {
        session.noteItemApplied();
        ctx.log().record("beat grids: copied rekordbox's " + std::string(gridWritten ? "grid" : "")
                         + (gridWritten && waveformWritten ? " and " : "") + (waveformWritten ? "waveform" : "")
                         + (result.completedRow ? ", completing the row," : "") + " to Engine track id="
                         + m_item.engineTrackId);
        return ChangeOutcome::success();
    }
    // The row changed since the preview: nothing written, and said so in
    // the summary as a skip rather than counted as copied.
    ctx.log().record("beat grids: left Engine track id=" + m_item.engineTrackId + " alone: grid "
                     + gridWords(result.grid.outcome).toStdString() + ", waveform "
                     + waveformWords(result.waveform).toStdString());
    return ChangeOutcome::skip();
}

}  // namespace seabass::gui
