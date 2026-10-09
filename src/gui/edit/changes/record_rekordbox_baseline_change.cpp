// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/record_rekordbox_baseline_change.hpp"

#include <chrono>
#include <set>
#include <stdexcept>

#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/local/rekordbox_baseline_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

// What the user reads when the record could not be made. Their changes
// are on the stick; the page will merely ask about some of them again.
std::runtime_error notRecorded(const std::string &why)
{
    return std::runtime_error("Your changes are saved, but Seabass could not record how this save left the "
                              "rekordbox side (" + why + "), so Sync after Rekordbox Export may list some of them "
                              "again.");
}

}  // namespace

RecordRekordboxBaselineChange::RecordRekordboxBaselineChange(std::string stickRoot, domain::RekordboxBaseline next,
                                                             AppliedBy appliedBy)
    : m_stickRoot(std::move(stickRoot)), m_next(std::move(next)), m_appliedBy(std::move(appliedBy))
{
}

QString RecordRekordboxBaselineChange::idString()
{
    return rekordboxExportSyncOwner() + QStringLiteral(":baseline");
}

QString RecordRekordboxBaselineChange::id() const
{
    return idString();
}

QString RecordRekordboxBaselineChange::description() const
{
    return QStringLiteral("Record the rekordbox side as this save leaves it, for the next comparison");
}

QString RecordRekordboxBaselineChange::unit() const
{
    return QStringLiteral("records");
}

QString RecordRekordboxBaselineChange::verb() const
{
    return QStringLiteral("recorded");
}

std::vector<BackupTarget> RecordRekordboxBaselineChange::filesToBackup(SaveContext &ctx) const
{
    (void)ctx;
    BackupTarget target;
    target.file = pathToUtf8(infrastructure::paths::stickRekordboxBaseline(pathFromUtf8(m_stickRoot)));
    target.label = rekordboxExportSyncOwner().toStdString();
    target.removeOnRestoreIfAbsent = true;
    return {target};
}

void RecordRekordboxBaselineChange::beforeSave(SaveContext &ctx)
{
    ctx.noteRekordboxBaselineRecordedBySave();
    // The change outlives the save loop's hooks: runSaveLoop() holds every
    // change until it returns, and runs the hooks before that.
    ctx.onAfterCommit([this, &ctx](const SaveContext::AfterCommit &after) {
        if (after.appliedIds.isEmpty()) {
            // Nothing landed (or the commit did not): the stick is as it
            // was, its backups may already be discarded, and a record now
            // would describe a save that did not happen.
            ctx.log().record("rekordbox baseline: nothing landed, nothing recorded");
            return;
        }
        if (ctx.rekordboxPath().isEmpty()) {
            throw notRecorded("this save has no rekordbox library");
        }
        std::string error;
        const auto previous = infrastructure::local::readRekordboxBaseline(pathFromUtf8(m_stickRoot), &error);
        if (!error.empty()) {
            throw notRecorded("the record already on the stick cannot be read: " + error);
        }

        domain::RekordboxBaseline next = m_next;
        std::set<std::string> notLanded;
        std::size_t changesNotLanded = 0;
        for (const auto &[changeId, keys] : m_appliedBy) {
            if (!after.appliedIds.contains(QString::fromStdString(changeId))) {
                ++changesNotLanded;
                notLanded.insert(keys.begin(), keys.end());
            }
        }
        domain::keepPreviousItems(next, previous ? &*previous : nullptr, notLanded);
        const auto writes = seabassWritesFor(m_stickRoot, after.rekordboxWrites);
        const auto unlisted = domain::recordSeabassWrites(next, writes);

        // As the save left export.pdb: a restore onto rekordbox rewrites it.
        const auto pdb = infrastructure::engine::readRekordboxImportState({}, ctx.rekordboxPath().toStdString());
        if (!pdb.hasRekordboxLibrary) {
            throw notRecorded("export.pdb could not be read after the save");
        }
        next.pdbSequence = pdb.librarySequence;
        if (next.engineUuid.empty() && previous) {
            next.engineUuid = previous->engineUuid;
        }
        next.recordedAtUnix = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        next.writer = baselineWriter();

        const std::string label = rekordboxExportSyncOwner().toStdString();
        if (!infrastructure::local::writeRekordboxBaseline(
                pathFromUtf8(m_stickRoot), next, [&](const std::string &path) { ctx.backupOnce(path, label); },
                &error)) {
            throw notRecorded(error);
        }
        ctx.log().record("rekordbox baseline: recorded at sequence " + std::to_string(next.pdbSequence) + ", "
                         + std::to_string(next.tracks.size()) + " track(s), " + std::to_string(next.playlists.size())
                         + " playlist(s), " + std::to_string(next.declined.size()) + " declined; "
                         + std::to_string(notLanded.size()) + " item(s) of " + std::to_string(changesNotLanded)
                         + " change(s) that did not land kept as before; " + std::to_string(writes.size() - unlisted.size())
                         + " of Seabass's own write(s) merged");
    });
}

ChangeOutcome RecordRekordboxBaselineChange::apply(SaveContext &ctx)
{
    (void)ctx;
    // The record is made after the commit (beforeSave), from the stick as
    // the save left it; reaching here only means every change before this
    // one landed.
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
