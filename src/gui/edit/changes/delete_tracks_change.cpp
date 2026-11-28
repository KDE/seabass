// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/delete_tracks_change.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

#include "application/path_key.hpp"
#include "domain/sync_planning.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::gui
{

namespace
{

constexpr const char *Label = "delete-unplaylisted-tracks";

std::string exportPdbOf(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

std::string engineDbOf(const std::string &enginePath)
{
    return pathToUtf8(pathFromUtf8(enginePath) / "Database2" / "m.db");
}

// normalizedPathKey of the path `readersPath` makes, as a reader spells a
// stored path; empty when it cannot be made (bytes that are not UTF-8,
// which the reader could not have listed either).
template<typename ReadersPath>
std::string keyOf(ReadersPath &&readersPath)
{
    try {
        return application::normalizedPathKey(readersPath());
    } catch (const std::exception &) {
        return {};
    }
}

// The ids `byKey` holds for `key`, none when it holds none.
template<typename ByKey>
typename ByKey::mapped_type rowsOf(const ByKey &byKey, const std::string &key)
{
    const auto found = byKey.find(key);
    return found == byKey.end() ? typename ByKey::mapped_type() : found->second;
}

}  // namespace

DeleteTracksChange::DeleteTracksChange(QString pioneerRoot, QString enginePath, std::vector<Entry> entries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_entries(std::move(entries))
{
}

std::unique_ptr<DeleteTracksChange> DeleteTracksChange::forPlaylist(QString pioneerRoot, QString enginePath, std::string playlist,
                                                                    std::vector<Entry> entries)
{
    auto change = std::make_unique<DeleteTracksChange>(std::move(pioneerRoot), std::move(enginePath), std::move(entries));
    change->m_playlist = std::move(playlist);
    return change;
}

QString DeleteTracksChange::idFor()
{
    return QStringLiteral("library-health:delete-unplaylisted");
}

QString DeleteTracksChange::idForPlaylist(const std::string &playlist)
{
    return QStringLiteral("addcue:delete-playlist-tracks:") + QString::fromStdString(playlist);
}

QString DeleteTracksChange::id() const
{
    return m_playlist.empty() ? idFor() : idForPlaylist(m_playlist);
}

QString DeleteTracksChange::owner() const
{
    return m_playlist.empty() ? QStringLiteral("library-health") : QStringLiteral("addcue");
}

QString DeleteTracksChange::description() const
{
    if (!m_playlist.empty()) {
        const QString playlist = QString::fromStdString(m_playlist);
        return m_entries.size() == 1
                   ? QStringLiteral("Delete \"%1\", the track of \"%2\" that no other playlist holds")
                         .arg(QString::fromStdString(m_entries[0].title), playlist)
                   : QStringLiteral("Delete the %1 tracks of \"%2\" that no other playlist holds").arg(m_entries.size()).arg(playlist);
    }
    return m_entries.size() == 1 ? QStringLiteral("Delete \"%1\", which is in no playlist").arg(QString::fromStdString(m_entries[0].title))
                                 : QStringLiteral("Delete %1 tracks that are in no playlist").arg(m_entries.size());
}

QString DeleteTracksChange::subject() const
{
    if (!m_playlist.empty()) {
        return QStringLiteral("The tracks of \"%1\"").arg(QString::fromStdString(m_playlist));
    }
    return m_entries.size() == 1 ? QString::fromStdString(m_entries[0].title) : QStringLiteral("Tracks not in any playlist");
}

QString DeleteTracksChange::unit() const
{
    return QStringLiteral("tracks");
}

QString DeleteTracksChange::verb() const
{
    return QStringLiteral("deleted");
}

int DeleteTracksChange::unitsWritten() const
{
    return m_deleted;
}

bool DeleteTracksChange::has(const std::string &format) const
{
    for (const auto &e : m_entries) {
        for (const auto &r : e.rows) {
            if (r.format == format) {
                return true;
            }
        }
    }
    return false;
}

QStringList DeleteTracksChange::formatsTouched() const
{
    QStringList formats;
    for (const char *f : {"rekordbox", "onelibrary", "engine"}) {
        if (has(f)) {
            formats << QString::fromLatin1(f);
        }
    }
    return formats;
}

std::vector<BackupTarget> DeleteTracksChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    const std::string pioneer = m_pioneerRoot.toStdString();
    if (has("rekordbox")) {
        targets.push_back({exportPdbOf(pioneer), Label});
    }
    if (has("onelibrary")) {
        targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label});
    }
    if (has("engine")) {
        targets.push_back({engineDbOf(m_enginePath.toStdString()), Label});
    }
    return targets;
}

ChangeOutcome DeleteTracksChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string engine = m_enginePath.toStdString();
    m_deleted = 0;
    try {
        std::unique_ptr<infrastructure::rekordbox::PdbRowWriter> pdb;
        FormatWriteSession *pdbSession = nullptr;
        if (has("rekordbox")) {
            pdbSession = &sharedFormatWriteSession(ctx, "rekordbox", pioneer, static_cast<int>(m_entries.size()), Label);
            pdb = std::make_unique<infrastructure::rekordbox::PdbRowWriter>(exportPdbOf(pdbSession->writeRoot()));
        }
        infrastructure::onelibrary::OneLibraryCueWriter *oneLibrary = nullptr;
        if (has("onelibrary")) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            oneLibrary = &sharedOneLibraryWriter(ctx, pioneer);
        }
        FormatWriteSession *engineSession = nullptr;
        std::set<std::int64_t> engineListed;
        if (has("engine")) {
            engineSession = &sharedFormatWriteSession(ctx, "engine", engine, static_cast<int>(m_entries.size()), Label);
            for (const std::int64_t id : infrastructure::engine::engineTracksInAnyPlaylist(engineSession->writeRoot())) {
                engineListed.insert(id);
            }
        }

        // Every row looked up again now: ids from the scan could name other
        // tracks by now (a re-export renumbers them). Each library's rows
        // are keyed the way the plan grouped what it read, by
        // normalizedPathKey of the path that library's reader makes of the
        // stored one, so a row is found here exactly when the plan would
        // have listed it for the file: a stored path with a doubled
        // separator, a "." or another letter case is the same file there
        // as here.
        //
        // One rule for all three libraries, before anything is removed:
        // every row of the file's key is the file, however many spellings
        // a library has of it. The file stays when any of them is in a
        // playlist, and goes from all of them otherwise. A stand-in row
        // (Browse names the file in every library on the stick, see
        // Entry::standIns) with no row of the key now has nothing to
        // remove; a library the plan read a row from that has none now
        // fails the save.
        const std::string readersStickRoot =
            pioneer.empty() ? std::string() : infrastructure::paths::normalStickRootForCatalogPath(pioneer);
        // export.pdb and OneLibrary's readers join a stored path the same way.
        const auto pioneerKeyOf = [&](const std::string &stored) {
            return stored.empty() ? std::string()
                                  : keyOf([&] { return infrastructure::rekordbox::trackFilePathOnStick(readersStickRoot, stored); });
        };
        std::map<std::string, std::vector<uint32_t>> rekordboxByKey;
        if (pdb) {
            for (const auto &row : pdb->trackFilePaths()) {
                if (const std::string key = pioneerKeyOf(row.second); !key.empty()) {
                    rekordboxByKey[key].push_back(row.first);
                }
            }
        }
        std::map<std::string, std::vector<std::int64_t>> oneLibraryByKey;
        if (oneLibrary) {
            for (const auto &row : oneLibrary->contentPaths()) {
                if (const std::string key = pioneerKeyOf(row.second); !key.empty()) {
                    oneLibraryByKey[key].push_back(row.first);
                }
            }
        }
        std::map<std::string, std::vector<std::int64_t>> engineByKey;
        if (engineSession) {
            const std::filesystem::path library = pathFromUtf8(engineSession->realRoot());
            for (const auto &row : infrastructure::engine::engineTrackPaths(engineSession->writeRoot())) {
                if (const std::string key = keyOf([&] { return pathToUtf8((library / pathFromUtf8(row.second)).lexically_normal()); });
                    !key.empty()) {
                    engineByKey[key].push_back(row.first);
                }
            }
        }
        struct Resolved
        {
            const Entry *entry = nullptr;
            std::set<uint32_t> rekordboxIds;
            std::set<std::int64_t> oneLibraryIds;
            std::set<std::int64_t> engineIds;
        };
        const auto changedSince = [](const Entry &e, const std::string &format) {
            return std::runtime_error(domain::catalogDisplayName(format) + " no longer lists \"" + e.filePath
                                      + "\" since Seabass read the stick, so nothing was saved. "
                                        "Check the stick again, then delete the tracks again.");
        };
        std::vector<Resolved> doomed;
        for (const auto &e : m_entries) {
            Resolved r;
            r.entry = &e;
            for (const auto &row : e.rows) {
                if (row.filePath.empty()) {
                    continue;
                }
                const std::string key = application::normalizedPathKey(row.filePath);
                const bool standIn = std::find(e.standIns.begin(), e.standIns.end(), row.format) != e.standIns.end();
                bool found = true;
                if (row.format == "rekordbox" && pdb) {
                    const auto ids = rowsOf(rekordboxByKey, key);
                    found = !ids.empty();
                    r.rekordboxIds.insert(ids.begin(), ids.end());
                } else if (row.format == "onelibrary" && oneLibrary) {
                    const auto ids = rowsOf(oneLibraryByKey, key);
                    found = !ids.empty();
                    r.oneLibraryIds.insert(ids.begin(), ids.end());
                } else if (row.format == "engine" && engineSession) {
                    const auto ids = rowsOf(engineByKey, key);
                    found = !ids.empty();
                    r.engineIds.insert(ids.begin(), ids.end());
                }
                if (!found && !standIn) {
                    throw changedSince(e, row.format);
                }
            }
            bool listed = std::any_of(r.rekordboxIds.begin(), r.rekordboxIds.end(),
                                      [&](uint32_t id) { return pdb->trackInAnyPlaylist(id); })
                          || std::any_of(r.engineIds.begin(), r.engineIds.end(),
                                         [&](std::int64_t id) { return engineListed.count(id) > 0; });
            listed = listed
                     || (!r.oneLibraryIds.empty()
                         && oneLibrary->isAnyInPlaylist(std::vector<std::int64_t>(r.oneLibraryIds.begin(), r.oneLibraryIds.end())));
            if (listed) {
                ctx.log().record(std::string(Label) + ": \"" + e.filePath + "\" is in a playlist now, left alone");
            } else {
                doomed.push_back(std::move(r));
            }
        }
        if (doomed.empty()) {
            return ChangeOutcome::skip();
        }

        // The pending-deletions list is put back with the databases if this
        // change fails partway.
        const std::filesystem::path stickRoot = pioneer.empty() ? pathFromUtf8(engine).parent_path() : pathFromUtf8(pioneer).parent_path();
        const std::string manifestPath = pathToUtf8(infrastructure::paths::stickPendingDeletions(stickRoot));
        ctx.protectForThisChange(manifestPath);
        infrastructure::cleanup::PendingDeletionManifest manifest(manifestPath);

        bool pdbChanged = false;
        std::vector<std::int64_t> engineIds;
        std::vector<std::string> removedFrom(doomed.size());
        for (size_t i = 0; i < doomed.size(); ++i) {
            for (const uint32_t id : doomed[i].rekordboxIds) {
                if (pdb->removeTrack(id)) {
                    pdbChanged = true;
                    removedFrom[i] = removedFrom[i].empty() ? "rekordbox" : removedFrom[i];
                }
            }
            if (!doomed[i].oneLibraryIds.empty()) {
                oneLibrary->removeTracksByContentIds(
                    std::vector<std::int64_t>(doomed[i].oneLibraryIds.begin(), doomed[i].oneLibraryIds.end()));
                removedFrom[i] = removedFrom[i].empty() ? "onelibrary" : removedFrom[i];
            }
            if (!doomed[i].engineIds.empty()) {
                engineIds.insert(engineIds.end(), doomed[i].engineIds.begin(), doomed[i].engineIds.end());
                removedFrom[i] = removedFrom[i].empty() ? "engine" : removedFrom[i];
            }
        }
        if (pdbChanged) {
            if (!pdb->commit()) {
                return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
            }
            pdbSession->noteItemApplied();
        }
        if (!engineIds.empty()) {
            if (infrastructure::engine::removeEngineTracks(engineSession->writeRoot(), engineIds) != static_cast<int>(engineIds.size())) {
                return ChangeOutcome::failure(QStringLiteral("Engine did not remove every track it was asked to"));
            }
            engineSession->noteItemApplied();
        }

        // Each file whose rows actually went, on the list for Delete
        // Orphaned Files, naming the backup of a database it came out of.
        for (size_t i = 0; i < doomed.size(); ++i) {
            if (removedFrom[i].empty()) {
                ctx.log().record(std::string(Label) + ": \"" + doomed[i].entry->filePath + "\" had no row left to remove");
                continue;
            }
            const Entry *e = doomed[i].entry;
            infrastructure::cleanup::PendingDeletion pending;
            pending.format = removedFrom[i] == "engine" ? "engine" : "rekordbox";
            pending.filePath = e->filePath;
            pending.title = e->title;
            pending.artist = e->artist;
            pending.backupId = ctx.backupIdOf(removedFrom[i] == "engine"       ? engineDbOf(engine)
                                              : removedFrom[i] == "onelibrary" ? infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer)
                                                                                : exportPdbOf(pioneer));
            manifest.append(pending);
            ++m_deleted;
            ctx.log().record(std::string(Label) + ": removed \"" + e->title + "\" (" + e->filePath
                             + ") from every library that listed it; the file waits on the pending-deletions list");
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return m_deleted > 0 ? ChangeOutcome::success() : ChangeOutcome::skip();
}

}  // namespace seabass::gui
