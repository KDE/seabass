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

bool numeric(const std::string &id)
{
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
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

        // Checked again now: a file some library put in a playlist since
        // the scan is left alone in every library.
        std::vector<const Entry *> doomed;
        for (const auto &e : m_entries) {
            bool listed = false;
            for (const auto &r : e.rows) {
                if (r.format == "rekordbox" && pdb && numeric(r.sourceId)) {
                    listed = listed || pdb->trackInAnyPlaylist(static_cast<uint32_t>(std::stoul(r.sourceId)));
                } else if (r.format == "onelibrary" && oneLibrary) {
                    listed = listed || oneLibrary->isInAnyPlaylist(e.filePath);
                } else if (r.format == "engine" && numeric(r.sourceId)) {
                    listed = listed || engineListed.count(std::stoll(r.sourceId)) > 0;
                }
            }
            if (listed) {
                ctx.log().record(std::string(Label) + ": \"" + e.filePath + "\" is in a playlist now, left alone");
            } else {
                doomed.push_back(&e);
            }
        }
        if (doomed.empty()) {
            return ChangeOutcome::skip();
        }

        if (pdb) {
            bool removed = false;
            for (const Entry *e : doomed) {
                for (const auto &r : e->rows) {
                    if (r.format == "rekordbox" && numeric(r.sourceId)) {
                        removed = pdb->removeTrack(static_cast<uint32_t>(std::stoul(r.sourceId))) || removed;
                    }
                }
            }
            if (removed) {
                if (!pdb->commit()) {
                    return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                }
                pdbSession->noteItemApplied();
            }
        }
        if (oneLibrary) {
            for (const Entry *e : doomed) {
                const bool listedThere = std::any_of(e->rows.begin(), e->rows.end(), [](const auto &r) { return r.format == "onelibrary"; });
                if (!listedThere) {
                    continue;
                }
                try {
                    oneLibrary->removeTrackByPath(e->filePath);
                } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    // Gone since the scan: nothing to remove.
                }
            }
        }
        if (engineSession) {
            std::vector<std::int64_t> ids;
            for (const Entry *e : doomed) {
                for (const auto &r : e->rows) {
                    if (r.format == "engine" && numeric(r.sourceId)) {
                        ids.push_back(std::stoll(r.sourceId));
                    }
                }
            }
            if (infrastructure::engine::removeEngineTracks(engineSession->writeRoot(), ids) > 0) {
                engineSession->noteItemApplied();
            }
        }

        // Every file on the list for Delete Orphaned Files, naming the
        // backup that still holds a removed row.
        const std::filesystem::path stickRoot = pioneer.empty() ? pathFromUtf8(engine).parent_path() : pathFromUtf8(pioneer).parent_path();
        infrastructure::cleanup::PendingDeletionManifest manifest(pathToUtf8(infrastructure::paths::stickPendingDeletions(stickRoot)));
        for (const Entry *e : doomed) {
            infrastructure::cleanup::PendingDeletion pending;
            const std::string format = e->rows.empty() ? "rekordbox" : e->rows.front().format;
            pending.format = format == "engine" ? "engine" : "rekordbox";
            pending.filePath = e->filePath;
            pending.title = e->title;
            pending.artist = e->artist;
            pending.backupId = ctx.backupIdOf(format == "engine"       ? engineDbOf(engine)
                                              : format == "onelibrary" ? infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer)
                                                                       : exportPdbOf(pioneer));
            manifest.append(pending);
            ++m_deleted;
            ctx.log().record(std::string(Label) + ": removed \"" + e->title + "\" (" + e->filePath
                             + ") from every library; the file waits on the pending-deletions list");
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
