// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/copy_beat_grid_change.hpp"

#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

#include "application/path_key.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

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

QString outcomeWords(application::BeatGridWriteResult::Outcome outcome)
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

}  // namespace

CopyBeatGridChange::CopyBeatGridChange(QString enginePath, std::string stickRoot, application::BeatGridCopyItem item,
                                       std::vector<domain::Beat> beats, std::optional<std::string> ownFingerprint,
                                       bool overwriteEngineEdits, int itemCountHint, bool declaresFiles)
    : m_enginePath(std::move(enginePath))
    , m_stickRoot(std::move(stickRoot))
    , m_item(std::move(item))
    , m_beats(std::move(beats))
    , m_ownFingerprint(std::move(ownFingerprint))
    , m_overwriteEngineEdits(overwriteEngineEdits)
    , m_itemCountHint(itemCountHint)
    , m_declaresFiles(declaresFiles)
{
}

QString CopyBeatGridChange::idFor(const std::string &engineTrackId)
{
    return QStringLiteral("beat-grid:engine:%1").arg(QString::fromStdString(engineTrackId));
}

QString CopyBeatGridChange::backupLabel()
{
    return QString::fromLatin1(Label);
}

QString CopyBeatGridChange::id() const
{
    return idFor(m_item.engineTrackId);
}

QString CopyBeatGridChange::owner() const
{
    return QStringLiteral("library-health");
}

QString CopyBeatGridChange::description() const
{
    return QStringLiteral("Copy rekordbox's beat grid to %1").arg(subject());
}

QString CopyBeatGridChange::subject() const
{
    const QString title = QString::fromStdString(m_item.title);
    return title.isEmpty() ? QStringLiteral("Engine track %1").arg(QString::fromStdString(m_item.engineTrackId))
                           : QStringLiteral("\"%1\"").arg(title);
}

QString CopyBeatGridChange::unit() const
{
    return QStringLiteral("beat grids");
}

QString CopyBeatGridChange::verb() const
{
    return QStringLiteral("copied");
}

QStringList CopyBeatGridChange::formatsTouched() const
{
    return {QString::fromLatin1(Format)};
}

std::vector<BackupTarget> CopyBeatGridChange::filesToBackup(SaveContext &) const
{
    // Once for the batch, by its first change (FillSampleRateChange's
    // reason): one copy of m.db per save, not one per track.
    if (!m_declaresFiles) {
        return {};
    }
    BackupTarget ledger;
    ledger.file = ledgerFileOf(m_stickRoot);
    ledger.label = Label;
    ledger.removeOnRestoreIfAbsent = true;
    return {{FormatWriteSession::databaseFileFor(Format, m_enginePath.toStdString()), Label}, ledger};
}

void CopyBeatGridChange::beforeSave(SaveContext &ctx)
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

ChangeOutcome CopyBeatGridChange::apply(SaveContext &ctx)
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
        return made;
    });

    application::BeatGridWriteOptions options;
    options.overwriteEngineEdits = m_overwriteEngineEdits;
    options.ownGridFingerprint = m_ownFingerprint;
    const application::BeatGridWriteResult result = writer.writeBeatGrid(m_item.engineTrackId, m_beats, options);
    using Outcome = application::BeatGridWriteResult::Outcome;

    if ((result.outcome == Outcome::Written || result.outcome == Outcome::AlreadySame) && !m_item.ledgerKey.empty()
        && !result.gridFingerprint.empty()) {
        if (auto *record = ctx.sharedForWholeSaveIfPresent<LedgerRecord>(ledgerKeyFor(m_stickRoot))) {
            record->byChange[id()] = {m_item.ledgerKey, result.gridFingerprint};
        }
    }
    if (result.outcome == Outcome::Written) {
        session.noteItemApplied();
        ctx.log().record("beat grids: copied rekordbox's grid (" + std::to_string(result.markers)
                         + " marker(s)) to Engine track id=" + m_item.engineTrackId);
        return ChangeOutcome::success();
    }
    // The row changed since the preview: nothing written, and said so in
    // the summary as a skip rather than counted as copied.
    ctx.log().record("beat grids: left Engine track id=" + m_item.engineTrackId + " alone: "
                     + outcomeWords(result.outcome).toStdString());
    return ChangeOutcome::skip();
}

}  // namespace seabass::gui
