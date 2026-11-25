// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/import_track_change.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

#include "domain/import_field_checks.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/qt_path.hpp"
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

// The two Pioneer rows of one track, from the one description.
infrastructure::rekordbox::PdbRowWriter::NewTrack deviceLibraryRow(const domain::ImportedTrack &track,
                                                                    std::uint64_t fileSizeBytes)
{
    infrastructure::rekordbox::PdbRowWriter::NewTrack row;
    row.pathOnStick = track.stickPath;
    row.title = track.title;
    row.artist = track.artist;
    row.album = track.album;
    row.albumArtist = track.albumArtist;
    row.genre = track.genre;
    row.label = track.label;
    row.remixer = track.remixer;
    row.composer = track.composer;
    row.key = track.key;
    row.comment = track.comment;
    row.bpm = track.bpm;
    row.durationSeconds = track.lengthSeconds;
    row.trackNumber = track.trackNumber;
    row.year = track.year;
    row.bitrate = track.bitrate;
    row.sampleDepth = track.bitDepth;
    row.sampleRateHz = track.sampleRateHz;
    row.fileSizeBytes = fileSizeBytes;
    row.date = track.date;
    return row;
}

infrastructure::onelibrary::OneLibraryCueWriter::NewTrack oneLibraryRow(const domain::ImportedTrack &track,
                                                                        const std::string &destination,
                                                                        std::uint64_t fileSizeBytes)
{
    infrastructure::onelibrary::OneLibraryCueWriter::NewTrack row;
    row.filePath = destination;
    row.title = track.title;
    row.artist = track.artist;
    row.album = track.album;
    row.albumArtist = track.albumArtist;
    row.genre = track.genre;
    row.label = track.label;
    row.remixer = track.remixer;
    row.composer = track.composer;
    row.key = track.key;
    row.comment = track.comment;
    row.bpm = track.bpm;
    row.lengthSeconds = track.lengthSeconds;
    row.trackNumber = track.trackNumber;
    row.year = track.year;
    row.bitrate = track.bitrate;
    row.bitDepth = track.bitDepth;
    row.sampleRateHz = track.sampleRateHz;
    row.fileSizeBytes = fileSizeBytes;
    row.date = track.date;
    return row;
}

}  // namespace

struct ImportRehearsal::Rows
{
    explicit Rows(const std::string &pdb)
        : writer(pdb)
    {
    }
    infrastructure::rekordbox::PdbRowWriter writer;
    // The playlist in use, and the rows whose entries went into it.
    std::optional<std::uint32_t> playlist;
    std::vector<std::uint32_t> entered;
};

ImportRehearsal::ImportRehearsal(const QString &pioneerRoot)
{
    if (pioneerRoot.isEmpty()) {
        return;
    }
    const std::string pdb = exportPdbUnder(pioneerRoot.toStdString());
    std::error_code ec;
    if (fs::is_regular_file(pathFromUtf8(pdb), ec)) {
        m_rows = std::make_unique<Rows>(pdb);
    }
}

ImportRehearsal::~ImportRehearsal() = default;

std::string ImportRehearsal::add(const domain::ImportedTrack &track, std::uint64_t fileSizeBytes)
{
    if (!m_rows) {
        return {};
    }
    try {
        // Never committed: the writer's buffer is the rehearsal.
        const std::uint32_t id = m_rows->writer.addTrack(deviceLibraryRow(track, fileSizeBytes));
        if (m_rows->playlist) {
            m_rows->writer.appendPlaylistEntries(*m_rows->playlist, {id});
            m_rows->entered.push_back(id);
        }
    } catch (const std::exception &e) {
        return e.what();
    }
    return {};
}

std::string ImportRehearsal::usePlaylist(const std::string &path, bool create)
{
    if (!m_rows) {
        return {};
    }
    try {
        m_rows->playlist = pdbPlaylistAt(m_rows->writer, path);
        if (!m_rows->playlist && create && path.find('/') == std::string::npos) {
            m_rows->playlist = m_rows->writer.createPlaylist(0, path, false);
        }
    } catch (const std::exception &e) {
        return e.what();
    }
    return m_rows->playlist ? std::string() : "export.pdb has no playlist \"" + path + "\"";
}

std::string ImportRehearsal::place(int toIndex)
{
    if (!m_rows || !m_rows->playlist || m_rows->entered.empty()) {
        return {};
    }
    try {
        const auto order =
            orderWithRunAt(m_rows->writer.playlistTrackIds(*m_rows->playlist), m_rows->entered, toIndex);
        if (!order) {
            return "export.pdb's playlist does not hold each of the tracks once";
        }
        (void)m_rows->writer.reorderPlaylist(*m_rows->playlist, *order);
    } catch (const std::exception &e) {
        return e.what();
    }
    return {};
}

std::string ImportTrackChange::rowRefusal(const domain::ImportedTrack &track, const QStringList &libraries,
                                          std::uint64_t fileSizeBytes)
{
    if (libraries.contains(QStringLiteral("rekordbox"))) {
        const std::string refusal =
            infrastructure::rekordbox::PdbRowWriter::newTrackRefusal(deviceLibraryRow(track, fileSizeBytes));
        if (!refusal.empty()) {
            return refusal;
        }
    }
    if (libraries.contains(QStringLiteral("onelibrary"))) {
        // The file's place is all the check reads of the path.
        const std::string refusal = infrastructure::onelibrary::OneLibraryCueWriter::newTrackRefusal(
            oneLibraryRow(track, track.stickPath, fileSizeBytes));
        if (!refusal.empty()) {
            return refusal;
        }
    }
    return {};
}

ImportTrackChange::ImportTrackChange(std::string sourceFile, QString stickRoot, QString pioneerRoot, QString enginePath,
                                     domain::ImportedTrack track, QStringList libraries, int itemCountHint)
    : m_sourceFile(std::move(sourceFile)), m_stickRoot(std::move(stickRoot)), m_pioneerRoot(std::move(pioneerRoot)),
      m_enginePath(std::move(enginePath)), m_track(std::move(track)), m_libraries(std::move(libraries)),
      m_itemCountHint(itemCountHint)
{
    if (m_stickRoot.isEmpty() || m_sourceFile.empty()) {
        throw std::invalid_argument("ImportTrackChange: a source file and a stick are required");
    }
    // The one plain spelling, so the file cannot land beside the stick
    // and every library is given the same text for it.
    if (!domain::isPlainStickPath(m_track.stickPath)) {
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
    // The audio file itself: absent now, so a failed save and Undo Last
    // Save both take it away again rather than leave a file no row names.
    // Only when nothing at all is there: apply() refuses a place that is
    // taken, and "remove it on restore" said of a place where an empty
    // folder sits would have the restore remove that folder (a folder is
    // no file to the backup, so it counts as absent).
    std::error_code ec;
    const std::string destination = destinationFile();
    if (fs::symlink_status(pathFromUtf8(destination), ec).type() == fs::file_type::not_found) {
        BackupTarget audio{destination, Label};
        audio.removeOnRestoreIfAbsent = true;
        targets.push_back(std::move(audio));
    }
    return targets;
}

ChangeOutcome ImportTrackChange::apply(SaveContext &ctx)
{
    m_deviceLibraryId = 0;
    m_oneLibraryId = -1;
    m_engineId = -1;
    const std::string destination = destinationFile();
    const std::string &source = m_sourceFile;
    const std::string pioneer = m_pioneerRoot.toStdString();
    const bool toDeviceLibrary = m_libraries.contains(QStringLiteral("rekordbox"));
    const bool toOneLibrary = m_libraries.contains(QStringLiteral("onelibrary"));
    const bool toEngine = m_libraries.contains(QStringLiteral("engine"));

    // Everything that can be known before the copy is asked before it. A
    // change that fails takes the whole save back, every track imported
    // before it included, so a refusal has to come while nothing is
    // written, not after the file is on the stick.
    std::error_code ec;
    const std::uintmax_t sourceSize = fs::file_size(pathFromUtf8(source), ec);
    if (ec) {
        return ChangeOutcome::failure(QStringLiteral("%1 is not a file that can be read. Nothing was imported.").arg(pathToQString(pathFromUtf8(m_sourceFile))));
    }
    // Never over a file that is there (the copy would refuse too, after
    // the libraries were opened).
    if (fs::symlink_status(pathFromUtf8(destination), ec).type() != fs::file_type::not_found) {
        return ChangeOutcome::failure(
            QStringLiteral("%1 is on the stick already; not replaced. Nothing was imported.")
                .arg(QString::fromStdString(m_track.stickPath)));
    }
    // The libraries named are the libraries the stick has: one written
    // and another left out would have them disagree about the track.
    const bool hasDeviceLibrary = !pioneer.empty() && fs::is_regular_file(pathFromUtf8(exportPdbUnder(pioneer)), ec);
    const bool hasOneLibrary = !pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer);
    const bool hasEngine =
        !m_enginePath.isEmpty()
        && fs::is_regular_file(pathFromUtf8(FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString())), ec);
    if (hasDeviceLibrary != toDeviceLibrary || hasOneLibrary != toOneLibrary || hasEngine != toEngine) {
        return ChangeOutcome::failure(
            QStringLiteral("The libraries on the stick are not the ones this import was planned for (%1). Nothing was "
                           "imported.")
                .arg(m_libraries.join(QStringLiteral(", "))));
    }

    infrastructure::rekordbox::PdbRowWriter::NewTrack pdbRow = deviceLibraryRow(m_track, sourceSize);
    infrastructure::onelibrary::OneLibraryCueWriter::NewTrack oneRow = oneLibraryRow(m_track, destination, sourceSize);
    if (const std::string refusal = rowRefusal(m_track, m_libraries, sourceSize); !refusal.empty()) {
        return ChangeOutcome::failure(QString::fromStdString(refusal + ". Nothing was imported."));
    }
    if (toEngine) {
        if (const QString refusal = engineRowWriteRefusal(m_enginePath); !refusal.isEmpty()) {
            return ChangeOutcome::failure(refusal);
        }
        if (m_track.sampleRateHz <= 0) {
            return ChangeOutcome::failure(
                QStringLiteral("The sample rate of %1 could not be read, which an Engine row needs. Nothing was imported.")
                    .arg(pathToQString(pathFromUtf8(m_sourceFile))));
        }
    }

    try {
        // export.pdb's row is made before the copy, in the writer's own
        // copy of the file: what only the library can refuse it for (a
        // row that already names the file, a page that cannot take a row,
        // a table that cannot start a new page) is then known while
        // nothing is on the stick. It reaches the file with commit(),
        // after the copy.
        std::optional<infrastructure::rekordbox::PdbRowWriter> pdbRows;
        FormatWriteSession *pdbSession = nullptr;
        if (toDeviceLibrary) {
            pdbSession = &sharedFormatWriteSession(ctx, "rekordbox", pioneer, m_itemCountHint, Label);
            pdbRows.emplace(exportPdbUnder(pdbSession->writeRoot()));
            m_deviceLibraryId = pdbRows->addTrack(pdbRow);
        }

        // Not cancellable: the save's cancel is asked between changes
        // (runSaveLoop), and a copy stopped halfway would be a failed
        // change, which takes back every track imported before it. A
        // cancel waits for this one file.
        const infrastructure::track_import::FileCopyResult copied =
            infrastructure::track_import::copyFileVerified(pathFromUtf8(source), pathFromUtf8(destination), {});
        if (!copied.copied) {
            return ChangeOutcome::failure(QString::fromStdString(copied.error));
        }
        // The row holds the size that was measured before the copy: a
        // file that is another size now is not the file the row is for.
        if (copied.bytes != sourceSize) {
            return ChangeOutcome::failure(
                QStringLiteral("%1 changed while it was imported.").arg(pathToQString(pathFromUtf8(m_sourceFile))));
        }

        if (toDeviceLibrary) {
            if (!pdbRows->commit()) {
                return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb for %1.").arg(subject()));
            }
            pdbSession->noteItemApplied();
        }
        if (toOneLibrary) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            m_oneLibraryId = writer.addTrack(oneRow);
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
