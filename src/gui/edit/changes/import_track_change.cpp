// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/import_track_change.hpp"

#include <filesystem>
#include <stdexcept>
#include <vector>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/track_import/verified_file_copy.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;

constexpr const char *Label = "track-import";

std::string exportPdbUnder(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

}  // namespace

ImportTrackChange::ImportTrackChange(QString sourceFile, QString stickRoot, QString pioneerRoot, QString enginePath,
                                     domain::ImportedTrack track, QStringList libraries, int itemCountHint)
    : m_sourceFile(std::move(sourceFile)), m_stickRoot(std::move(stickRoot)), m_pioneerRoot(std::move(pioneerRoot)),
      m_enginePath(std::move(enginePath)), m_track(std::move(track)), m_libraries(std::move(libraries)),
      m_itemCountHint(itemCountHint)
{
    if (m_stickRoot.isEmpty() || m_sourceFile.isEmpty()) {
        throw std::invalid_argument("ImportTrackChange: a source file and a stick are required");
    }
    if (m_track.stickPath.size() < 2 || m_track.stickPath.front() != '/') {
        throw std::invalid_argument("ImportTrackChange: the track has no place on the stick");
    }
    for (const QString &library : m_libraries) {
        const bool known = library == QLatin1String("rekordbox") || library == QLatin1String("onelibrary")
                           || library == QLatin1String("engine");
        const bool hasRoot = library == QLatin1String("engine") ? !m_enginePath.isEmpty() : !m_pioneerRoot.isEmpty();
        if (!known || !hasRoot) {
            throw std::invalid_argument("ImportTrackChange: no library \"" + library.toStdString() + "\" to write");
        }
    }
    if (m_libraries.isEmpty()) {
        throw std::invalid_argument("ImportTrackChange: a file with no library to list it is not an import");
    }
}

QString ImportTrackChange::idFor(const std::string &stickPath)
{
    return QStringLiteral("addcue:import-track:") + QString::fromStdString(stickPath);
}

QString ImportTrackChange::unitKeyFor(const std::string &stickPath)
{
    return QStringLiteral("import-track:") + QString::fromStdString(stickPath);
}

QString ImportTrackChange::id() const
{
    return idFor(m_track.stickPath);
}

QString ImportTrackChange::owner() const
{
    // Browse stages it beside its playlist edits, which are "addcue"'s:
    // one save holds the changes of one page.
    return QStringLiteral("addcue");
}

QString ImportTrackChange::description() const
{
    return QStringLiteral("Import \"%1\"").arg(subject());
}

QString ImportTrackChange::subject() const
{
    const QString title = QString::fromStdString(m_track.title);
    return m_track.artist.empty() ? title : title + QStringLiteral(", ") + QString::fromStdString(m_track.artist);
}

QString ImportTrackChange::unit() const
{
    return QStringLiteral("tracks");
}

QString ImportTrackChange::verb() const
{
    return QStringLiteral("imported");
}

QStringList ImportTrackChange::unitKeys() const
{
    return {id(), unitKeyFor(m_track.stickPath)};
}

QStringList ImportTrackChange::formatsTouched() const
{
    return m_libraries;
}

std::string ImportTrackChange::destinationFile() const
{
    // stickPath starts with '/', which a path join would take for a root.
    return pathToUtf8(pathFromUtf8(m_stickRoot.toStdString()) / pathFromUtf8(m_track.stickPath.substr(1)));
}

std::vector<BackupTarget> ImportTrackChange::filesToBackup(SaveContext &) const
{
    std::vector<BackupTarget> targets;
    if (m_libraries.contains(QStringLiteral("rekordbox"))) {
        targets.push_back({exportPdbUnder(m_pioneerRoot.toStdString()), Label});
    }
    if (m_libraries.contains(QStringLiteral("onelibrary"))) {
        targets.push_back(
            {infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(m_pioneerRoot.toStdString()), Label});
    }
    if (m_libraries.contains(QStringLiteral("engine"))) {
        targets.push_back({FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()), Label});
    }
    // The audio file itself: absent now, so a failed change and Undo Last
    // Save both take it away again rather than leave a file no row names.
    BackupTarget audio{destinationFile(), Label};
    audio.removeOnRestoreIfAbsent = true;
    targets.push_back(std::move(audio));
    return targets;
}

ChangeOutcome ImportTrackChange::apply(SaveContext &ctx)
{
    m_deviceLibraryId = 0;
    m_oneLibraryId = -1;
    m_engineId = -1;
    const std::string destination = destinationFile();
    const std::string source = m_sourceFile.toStdString();
    const bool toEngine = m_libraries.contains(QStringLiteral("engine"));
    // Before anything is written: what would be refused after the copy.
    if (toEngine) {
        if (const QString refusal = engineRowWriteRefusal(m_enginePath); !refusal.isEmpty()) {
            return ChangeOutcome::failure(refusal);
        }
        if (m_track.sampleRateHz <= 0) {
            return ChangeOutcome::failure(
                QStringLiteral("The sample rate of %1 could not be read, which an Engine row needs. Nothing was imported.")
                    .arg(m_sourceFile));
        }
    }

    try {
        infrastructure::track_import::FileCopyOptions options;
        options.cancel = ctx.cancel();
        const infrastructure::track_import::FileCopyResult copied =
            infrastructure::track_import::copyFileVerified(pathFromUtf8(source), pathFromUtf8(destination), options);
        if (copied.cancelled) {
            return ChangeOutcome::failure(QStringLiteral("The copy of %1 was cancelled. Nothing was imported.").arg(m_sourceFile));
        }
        if (!copied.copied) {
            return ChangeOutcome::failure(QString::fromStdString(copied.error));
        }

        const std::string pioneer = m_pioneerRoot.toStdString();
        if (m_libraries.contains(QStringLiteral("rekordbox"))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, m_itemCountHint, Label);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbUnder(session.writeRoot()));
            infrastructure::rekordbox::PdbRowWriter::NewTrack row;
            row.pathOnStick = m_track.stickPath;
            row.title = m_track.title;
            row.artist = m_track.artist;
            row.album = m_track.album;
            row.albumArtist = m_track.albumArtist;
            row.genre = m_track.genre;
            row.label = m_track.label;
            row.remixer = m_track.remixer;
            row.composer = m_track.composer;
            row.key = m_track.key;
            row.comment = m_track.comment;
            row.bpm = m_track.bpm;
            row.durationSeconds = m_track.lengthSeconds;
            row.trackNumber = m_track.trackNumber;
            row.year = m_track.year;
            row.bitrate = m_track.bitrate;
            row.sampleDepth = m_track.bitDepth;
            row.sampleRateHz = m_track.sampleRateHz;
            row.fileSizeBytes = copied.bytes;
            row.date = m_track.date;
            m_deviceLibraryId = rows.addTrack(row);
            if (!rows.commit()) {
                return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb for %1.").arg(subject()));
            }
            session.noteItemApplied();
        }
        if (m_libraries.contains(QStringLiteral("onelibrary"))) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            infrastructure::onelibrary::OneLibraryCueWriter::NewTrack row;
            row.filePath = destination;
            row.title = m_track.title;
            row.artist = m_track.artist;
            row.album = m_track.album;
            row.albumArtist = m_track.albumArtist;
            row.genre = m_track.genre;
            row.label = m_track.label;
            row.remixer = m_track.remixer;
            row.composer = m_track.composer;
            row.key = m_track.key;
            row.comment = m_track.comment;
            row.bpm = m_track.bpm;
            row.lengthSeconds = m_track.lengthSeconds;
            row.trackNumber = m_track.trackNumber;
            row.year = m_track.year;
            row.bitrate = m_track.bitrate;
            row.bitDepth = m_track.bitDepth;
            row.sampleRateHz = m_track.sampleRateHz;
            row.fileSizeBytes = copied.bytes;
            row.date = m_track.date;
            m_oneLibraryId = writer.addTrack(row);
        }
        if (toEngine) {
            FormatWriteSession &session =
                sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, Label);
            infrastructure::engine::NewEngineTrack row;
            row.source.title = m_track.title;
            row.source.artist = m_track.artist;
            row.source.album = m_track.album;
            row.source.bpm = m_track.bpm;
            row.source.key = m_track.key;
            row.source.durationSeconds = m_track.durationSeconds;
            row.source.bitrate = m_track.bitrate;
            row.source.comment = m_track.comment;
            row.source.fileSizeBytes = copied.bytes;
            row.source.filePath = destination;
            domain::TrackTags tags;
            tags.album = m_track.album;
            tags.genre = m_track.genre;
            tags.composer = m_track.composer;
            tags.label = m_track.label;
            tags.year = m_track.year;
            tags.trackNumber = m_track.trackNumber;
            row.fileTags = tags;
            row.sampleRateHz = static_cast<double>(m_track.sampleRateHz);
            row.realEngineLibraryPath = session.realRoot();
            infrastructure::engine::EngineTrackCover cover;
            std::string error;
            m_engineId = infrastructure::engine::createEngineTrack(session.writeRoot(), row, &cover, &error);
            if (m_engineId < 0) {
                return ChangeOutcome::failure(QString::fromStdString(error));
            }
            const auto rows =
                infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), destination);
            if (rows != std::vector<std::int64_t>{m_engineId}) {
                return ChangeOutcome::failure(
                    QStringLiteral("Engine has %1 rows for %2 after adding one.").arg(rows.size()).arg(subject()));
            }
            session.noteItemApplied();
        }
        ctx.log().record(std::string(Label) + ": imported \"" + m_track.title + "\" from " + source + " to "
                         + m_track.stickPath + " (" + std::to_string(copied.bytes) + " bytes): DeviceLibrary id "
                         + std::to_string(m_deviceLibraryId) + ", OneLibrary id " + std::to_string(m_oneLibraryId)
                         + ", Engine id " + std::to_string(m_engineId) + " (0 or -1: not on this stick)");
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
