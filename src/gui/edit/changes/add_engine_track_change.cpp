// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/add_engine_track_change.hpp"

#include <stdexcept>
#include <vector>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"

namespace seabass::gui
{

AddEngineTrackChange::AddEngineTrackChange(QString enginePath, domain::Track track,
                                           std::shared_ptr<application::TrackMetadataProbe> probe, int itemCountHint)
    : m_enginePath(std::move(enginePath)), m_track(std::move(track)), m_probe(std::move(probe)),
      m_itemCountHint(itemCountHint)
{
    if (!m_probe) {
        throw std::invalid_argument("AddEngineTrackChange: a metadata probe is required for the sample rate");
    }
}

QString AddEngineTrackChange::idFor(const std::string &filePath)
{
    return rekordboxExportSyncOwner() + QStringLiteral(":engine-add:") + QString::fromStdString(filePath);
}

QString AddEngineTrackChange::id() const
{
    return idFor(m_track.filePath);
}

QString AddEngineTrackChange::owner() const
{
    return rekordboxExportSyncOwner();
}

QString AddEngineTrackChange::description() const
{
    return QStringLiteral("Add \"%1\" to Engine").arg(subject());
}

QString AddEngineTrackChange::subject() const
{
    const QString title = QString::fromStdString(m_track.title.empty() ? m_track.filename : m_track.title);
    return m_track.artist.empty() ? title : title + QStringLiteral(", ") + QString::fromStdString(m_track.artist);
}

QString AddEngineTrackChange::unit() const
{
    return QStringLiteral("tracks");
}

QString AddEngineTrackChange::verb() const
{
    return QStringLiteral("added");
}

QStringList AddEngineTrackChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> AddEngineTrackChange::filesToBackup(SaveContext &) const
{
    // Nothing for a library apply() refuses: there is no m.db there to
    // back up, and apply() says why.
    if (!engineRowWriteRefusal(m_enginePath).isEmpty()) {
        return {};
    }
    std::vector<BackupTarget> targets{{FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()),
                                       rekordboxExportSyncOwner().toStdString()}};
    // The cover file apply() may write under Artwork/, by the name the
    // image's content gives it: absent now, Undo removes it with m.db
    // put back, rather than leaving a file no row names. A library that
    // keeps its covers in m.db writes no file, and the target stays
    // absent. Always the stick's own Artwork/: a scratch copy of m.db
    // never redirects it.
    if (!m_track.artworkPath.empty()) {
        const std::string cover =
            infrastructure::engine::artworkFileForImage(m_enginePath.toStdString(), m_track.artworkPath);
        if (!cover.empty()) {
            BackupTarget target{cover, rekordboxExportSyncOwner().toStdString()};
            target.removeOnRestoreIfAbsent = true;
            targets.push_back(std::move(target));
        }
    }
    return targets;
}

ChangeOutcome AddEngineTrackChange::apply(SaveContext &ctx)
{
    m_createdId = -1;
    m_coverProblem.clear();
    if (const QString refusal = engineRowWriteRefusal(m_enginePath); !refusal.isEmpty()) {
        return ChangeOutcome::failure(refusal);
    }
    const QString file = QString::fromStdString(m_track.filePath);
    if (m_track.filePath.empty()) {
        return ChangeOutcome::failure(QStringLiteral("\"%1\" names no file, so there is nothing to add to Engine.")
                                          .arg(subject()));
    }
    // Before anything is opened: a refusal here writes nothing.
    const auto metadata = m_probe->read(m_track.filePath);
    if (!metadata || metadata->sampleRate <= 0) {
        return ChangeOutcome::failure(
            QStringLiteral("The sample rate of %1 could not be read, so its cues cannot be placed in Engine. Nothing "
                           "was added.")
                .arg(file));
    }

    const std::string label = rekordboxExportSyncOwner().toStdString();
    try {
        FormatWriteSession &session =
            sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, label);
        infrastructure::engine::NewEngineTrack row;
        row.source = m_track;
        row.sampleRateHz = static_cast<double>(metadata->sampleRate);
        // The real library on the stick, not the scratch copy: the row's
        // path is read on the stick, and Artwork/ is never redirected.
        row.realEngineLibraryPath = session.realRoot();
        infrastructure::engine::EngineTrackCover cover;
        cover.beforeWrite = [&ctx](const std::string &coverFile) { ctx.protectForThisChange(coverFile); };
        std::string error;
        const std::int64_t id = infrastructure::engine::createEngineTrack(session.writeRoot(), row, &cover, &error);
        if (id < 0) {
            return ChangeOutcome::failure(QString::fromStdString(error));
        }
        // What landed, read back: the file names exactly one row now, the
        // one just made. Anything else and the change fails, and the save
        // loop puts m.db back.
        const auto rows = infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(),
                                                                        m_track.filePath);
        if (rows != std::vector<std::int64_t>{id}) {
            return ChangeOutcome::failure(
                QStringLiteral("Engine has %1 rows for %2 after adding one (id %3), not the one it was given.")
                    .arg(rows.size())
                    .arg(file)
                    .arg(id));
        }
        session.noteItemApplied();
        m_createdId = id;
        if (!m_track.artworkPath.empty() && !cover.written) {
            m_coverProblem = cover.problem.empty() ? std::string("the cover was not written") : cover.problem;
            ctx.log().record(label + ": added \"" + m_track.title + "\" to Engine without its cover: " + m_coverProblem);
        }
        ctx.log().record(label + ": added \"" + m_track.title + "\" (" + m_track.filePath + ") to Engine as track id="
                         + std::to_string(id) + ", " + std::to_string(m_track.cues.size()) + " cue(s) at "
                         + std::to_string(metadata->sampleRate) + " Hz, " + std::to_string(cover.filesWritten.size())
                         + " cover file(s); left for the player to analyse");
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
