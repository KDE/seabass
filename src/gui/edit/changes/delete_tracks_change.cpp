// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/delete_tracks_change.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
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

// "/Contents/..." as export.pdb spells a file on the stick.
std::string pathOnStick(const std::string &pioneerRoot, const std::string &filePath)
{
    std::error_code ec;
    const auto relative = std::filesystem::relative(pathFromUtf8(filePath), pathFromUtf8(pioneerRoot).parent_path(), ec);
    return ec ? filePath : "/" + pathToGenericUtf8(relative);
}

}  // namespace

DeleteTracksChange::DeleteTracksChange(QString pioneerRoot, QString enginePath, std::vector<Entry> entries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_entries(std::move(entries))
{
}

QString DeleteTracksChange::idFor()
{
    return QStringLiteral("library-health:delete-unplaylisted");
}

QString DeleteTracksChange::id() const
{
    return idFor();
}

QString DeleteTracksChange::owner() const
{
    return QStringLiteral("library-health");
}

QString DeleteTracksChange::description() const
{
    return m_entries.size() == 1 ? QStringLiteral("Delete \"%1\", which is in no playlist").arg(QString::fromStdString(m_entries[0].title))
                                 : QStringLiteral("Delete %1 tracks that are in no playlist").arg(m_entries.size());
}

QString DeleteTracksChange::subject() const
{
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

        // Every row looked up again now, by its own file path in its own
        // library: ids from the scan could name other tracks by now (a
        // re-export renumbers them), and each library spells the path its
        // own way.
        struct Resolved
        {
            const Entry *entry = nullptr;
            std::vector<uint32_t> rekordboxIds;
            std::vector<std::string> oneLibraryPaths;
            std::vector<std::int64_t> engineIds;
        };
        std::vector<Resolved> doomed;
        for (const auto &e : m_entries) {
            Resolved r;
            r.entry = &e;
            bool listed = false;
            for (const auto &row : e.rows) {
                if (row.filePath.empty()) {
                    continue;
                }
                if (row.format == "rekordbox" && pdb) {
                    for (const uint32_t id : pdb->trackIdsWithFilePath(pathOnStick(pioneer, row.filePath))) {
                        listed = listed || pdb->trackInAnyPlaylist(id);
                        r.rekordboxIds.push_back(id);
                    }
                } else if (row.format == "onelibrary" && oneLibrary) {
                    listed = listed || oneLibrary->isInAnyPlaylist(row.filePath);
                    r.oneLibraryPaths.push_back(row.filePath);
                } else if (row.format == "engine" && engineSession) {
                    for (const std::int64_t id :
                         infrastructure::engine::engineTrackIdsForFile(engineSession->writeRoot(), engineSession->realRoot(), row.filePath)) {
                        listed = listed || engineListed.count(id) > 0;
                        r.engineIds.push_back(id);
                    }
                }
            }
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
            for (const std::string &path : doomed[i].oneLibraryPaths) {
                try {
                    oneLibrary->removeTrackByPath(path);
                    removedFrom[i] = removedFrom[i].empty() ? "onelibrary" : removedFrom[i];
                } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    // Gone since the scan: nothing to remove.
                }
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
