// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/record_rekordbox_baseline_change.hpp"

#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain/sync_planning.hpp"
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
                                                             AppliedBy appliedBy, std::set<std::string> reusedIds)
    : m_stickRoot(std::move(stickRoot))
    , m_next(std::move(next))
    , m_appliedBy(std::move(appliedBy))
    , m_reusedIds(std::move(reusedIds))
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
        std::set<std::string> landed;
        std::size_t changesNotLanded = 0;
        for (const auto &[changeId, keys] : m_appliedBy) {
            if (!after.appliedIds.contains(QString::fromStdString(changeId))) {
                ++changesNotLanded;
                notLanded.insert(keys.begin(), keys.end());
            } else {
                landed.insert(keys.begin(), keys.end());
            }
        }
        std::set<std::string> keys = notLanded;
        std::vector<std::string> notDeleted;
        for (const auto &reusedKey : m_reusedIds) {
            const auto reused = domain::parseReusedPlaylistItemKey(reusedKey);
            if (!reused) {
                continue;
            }
            const std::string createKey = domain::playlistItemKey(reused->id);
            const bool deleteStaged = landed.count(reusedKey) || notLanded.count(reusedKey);
            const bool deleteLanded = landed.count(reusedKey) > 0;
            const bool createLanded = landed.count(createKey) > 0;
            const domain::BaselinePlaylist *old = previous ? previous->findPlaylist(reused->id) : nullptr;
            if (deleteStaged && !deleteLanded && !createLanded && old && old->path == reused->recordedPath) {
                // Neither half on the stick: the record keeps the old
                // playlist whole, as before, so the next analysis finds the
                // reuse again and offers both halves. With its own declines
                // and none of this save's about the new playlist: those
                // cannot be recorded under the id while it holds the old
                // one, so they are asked again.
                keys.erase(reusedKey);
                keys.insert(createKey);
                keys.insert(domain::orderItemKey(reused->id));
                for (const auto &member : old->members) {
                    keys.insert(domain::memberItemKey(reused->id, member));
                }
                const auto underTheId = [&](const std::string &declinedKey) {
                    const auto item = domain::parseItemKey(declinedKey);
                    return item && item->playlistId == reused->id
                        && (item->kind == domain::ItemKey::Kind::Playlist || item->kind == domain::ItemKey::Kind::Member
                            || item->kind == domain::ItemKey::Kind::Order);
                };
                std::erase_if(next.declined, [&](const auto &entry) { return underTheId(entry.first); });
                for (const auto &[declinedKey, state] : previous->declined) {
                    if (underTheId(declinedKey)) {
                        next.declined.emplace(declinedKey, state);
                    }
                }
                continue;
            }
            // Puts back nothing: the rest reads the record as the plan did.
            keys.insert(reusedKey);
            if (deleteStaged && !deleteLanded) {
                // The new playlist landed and the old one's delete did not:
                // the record can hold only one of them under the id, and
                // holds the new one, so no later analysis plans the delete.
                // It stays staged, and the page keeps listing it, ticked
                // (RekordboxExportSyncController's leftover deletes), until
                // it is saved or unticked; said here, in the save's warning.
                notDeleted.push_back(reused->recordedPath);
            }
        }
        domain::keepPreviousItems(next, previous ? &*previous : nullptr, keys);
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
        if (!notDeleted.empty()) {
            // After the record is written: an after-commit hook's exception
            // is the save's warning (SaveContext::runAfterCommitHooks).
            const std::string engineName = domain::catalogDisplayName("engine");
            std::string said;
            for (const auto &path : notDeleted) {
                said += std::string(said.empty() ? "" : " ") + "The old playlist \"" + path
                    + "\" was not deleted from " + engineName + "; its delete is still staged, and saving again "
                    + "completes it.";
            }
            throw std::runtime_error(said);  // logged by runAfterCommitHooks
        }
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
