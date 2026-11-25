// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/import_track_change.hpp"

#include <QDir>
#include <QTemporaryFile>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "domain/import_field_checks.hpp"
#include "application/path_key.hpp"
#include "application/ports/analysis_writer.hpp"
#include "domain/mp3_lead_in.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/qt_path.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/rekordbox/anlz_builder.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/exact_sample_count.hpp"
#endif
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

// The analysis files the stick has, as rows spell them
// ("/PIONEER/USBANLZ/P01E/00009ECC/ANLZ0000.DAT"), and those this save
// has given out: where a new track's folder must not be.
struct AnalysisFoldersInUse
{
    explicit AnalysisFoldersInUse(const std::string &pioneerRoot)
    {
        const fs::path root = pathFromUtf8(pioneerRoot);
        const fs::path folder = root / "USBANLZ";
        std::error_code ec;
        if (fs::symlink_status(folder, ec).type() == fs::file_type::not_found) {
            return;  // a library without analysis files yet
        }
        fs::recursive_directory_iterator at(folder, fs::directory_options::skip_permission_denied, ec);
        for (const fs::recursive_directory_iterator end; !ec && at != end; at.increment(ec)) {
            // An error of its own each: one entry that cannot be asked
            // must not read as the end of the folder.
            std::error_code entryError;
            const bool isFile = at->is_regular_file(entryError);
            if (entryError) {
                complete = false;
                continue;
            }
            if (isFile) {
                const fs::path relative = fs::relative(at->path(), root, entryError);
                if (entryError) {
                    complete = false;
                    continue;
                }
                paths.insert("/PIONEER/" + pathToGenericUtf8(relative));
            }
        }
        complete = complete && !ec;
    }
    std::unordered_set<std::string> paths;
    // False when the folder could not be read to its end: what is free
    // is then not known, and no new analysis file is placed.
    bool complete = true;
};

// "..../a7.jpg" to "..../a7_m.jpg".
std::string mediumPictureOf(const std::string &small)
{
    return small.substr(0, small.size() - 4) + "_m.jpg";
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
    std::uint32_t analysisFiles = 0;
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

std::string ImportRehearsal::add(const domain::ImportedTrack &track, std::uint64_t fileSizeBytes, bool withCover,
                                 bool withAnalysisFile)
{
    if (!m_rows) {
        return {};
    }
    try {
        // Never committed: the writer's buffer is the rehearsal.
        auto row = deviceLibraryRow(track, fileSizeBytes);
        if (withCover) {
            row.artworkPath =
                infrastructure::rekordbox::PdbRowWriter::artworkPathFor('a', m_rows->writer.nextArtworkId());
        }
        if (withAnalysisFile) {
            // A name of the length the save's will have, in a folder no
            // real file is in, another for each track.
            char name[16];
            std::snprintf(name, sizeof name, "%08X", 0xFF000000u + m_rows->analysisFiles++);
            row.analyzePath = std::string("/PIONEER/USBANLZ/P07F/") + name + "/ANLZ0000.DAT";
        }
        const std::uint32_t id = m_rows->writer.addTrack(row);
        if (m_rows->playlist) {
            m_rows->writer.appendPlaylistEntries(*m_rows->playlist, {id});
            m_rows->entered.push_back(id);
        }
    } catch (const std::exception &e) {
        return e.what();
    }
    return {};
}

std::string ImportRehearsal::enter(const QString &pioneerRoot, const std::string &file)
{
    if (!m_rows || !m_rows->playlist) {
        return {};
    }
    try {
        std::error_code ec;
        const auto relative =
            fs::relative(pathFromUtf8(file), pathFromUtf8(pioneerRoot.toStdString()).parent_path(), ec);
        const auto ids = m_rows->writer.trackIdsWithFilePath(ec ? file : "/" + pathToGenericUtf8(relative));
        if (ids.empty()) {
            return "export.pdb has no row for it";
        }
        m_rows->writer.appendPlaylistEntries(*m_rows->playlist, {ids.front()});
        m_rows->entered.push_back(ids.front());
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
                                          std::uint64_t fileSizeBytes, bool withAnalysisFile)
{
    if (libraries.contains(QStringLiteral("rekordbox"))) {
        auto row = deviceLibraryRow(track, fileSizeBytes);
        if (withAnalysisFile) {
            row.analyzePath = "/PIONEER/USBANLZ/P000/00000000/ANLZ0000.DAT";
        }
        const std::string refusal = infrastructure::rekordbox::PdbRowWriter::newTrackRefusal(row);
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

void ImportTrackChange::setAnalysis(std::shared_ptr<const ImportAnalysis> analysis)
{
    m_analysis = std::move(analysis);
    if (m_analysis && m_analysis->bpm > 0.0 && m_analysis->bpm < 1000.0) {
        m_track.bpm = m_analysis->bpm;
    }
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
    if (const std::string refusal = rowRefusal(m_track, m_libraries, sourceSize, static_cast<bool>(m_analysis));
        !refusal.empty()) {
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

    // The cover, read and scaled before anything is written. Each
    // library's files are named after the row it will make for them; a
    // name that is taken means the library's ids and files do not agree,
    // and the track then goes without a cover there rather than over
    // another track's picture.
    std::optional<CoverPictures> cover = m_coverReader ? m_coverReader(source) : std::nullopt;
    if (cover && (cover->small.empty() || cover->medium.empty() || cover->large.empty())) {
        cover.reset();
    }
    const fs::path stickRoot = pathFromUtf8(m_stickRoot.toStdString());
    const auto pictureFile = [&](const std::string &onStick) { return stickRoot / pathFromUtf8(onStick.substr(1)); };
    const auto pictureNamesFree = [&](const std::string &onStick) {
        std::error_code missing;
        return fs::symlink_status(pictureFile(onStick), missing).type() == fs::file_type::not_found
               && fs::symlink_status(pictureFile(mediumPictureOf(onStick)), missing).type() == fs::file_type::not_found;
    };
    // A cover file is named after a row made in this very save, so it
    // cannot be declared with filesToBackup(). Declared here, before it
    // is written: taken away again by a failed change
    // (protectForThisChange) and by Undo Last Save (the save's record of
    // a file that was not there).
    const auto declareCreated = [&](const std::string &file) {
        ctx.protectForThisChange(file);
        BackupTarget created{file, Label};
        created.removeOnRestoreIfAbsent = true;
        ctx.backupAllNow({created});
    };
    // Throws when a picture cannot be written: the row would name a file
    // that is not there.
    const auto writePictures = [&](const std::string &onStick) {
        const std::pair<fs::path, const std::string *> files[] = {{pictureFile(onStick), &cover->small},
                                                                 {pictureFile(mediumPictureOf(onStick)), &cover->medium}};
        for (const auto &[file, bytes] : files) {
            const std::string path = pathToUtf8(file);
            declareCreated(path);
            fs::create_directories(file.parent_path());
            if (!infrastructure::writeFileDurablyAtomic(path, *bytes)) {
                throw std::runtime_error("could not write the cover " + path);
            }
        }
    };

    // The analysis as rekordbox's files, built before anything is
    // written. The grid goes onto rekordbox's timeline; an MP3 whose
    // lead-in cannot be read has no known place on it and gets no grid.
    std::optional<infrastructure::rekordbox::NewAnlzFiles> anlz;
    std::vector<domain::Beat> rekordboxBeats;
    std::optional<std::uint64_t> sampleCount;
    const bool isMp3 = domain::hasMp3Extension(source);
    if (m_analysis && !m_analysis->waveform.entries.empty()) {
        const std::optional<double> offset = infrastructure::audio::cueTimelineOffsetMs(source);
        const double lengthMs = m_analysis->waveform.sampleRateHz > 0
                                    ? 1000.0 * static_cast<double>(m_analysis->waveform.sampleCount)
                                          / m_analysis->waveform.sampleRateHz
                                    : m_track.durationSeconds * 1000.0;
        if (offset) {
            for (domain::Beat beat : m_analysis->beats) {
                beat.timeMs += *offset;
                // Inside the track: the builder does not ask.
                if (beat.timeMs >= 0.0 && beat.timeMs <= lengthMs) {
                    rekordboxBeats.push_back(beat);
                }
            }
        }
#ifdef SEABASS_HAVE_TAGLIB
        if (const auto exact = infrastructure::audio::readExactSampleCount(source)) {
            sampleCount = exact->samples;
        }
#endif
        infrastructure::rekordbox::NewAnlzInput input;
        input.trackPath = m_track.stickPath;
        input.beats = rekordboxBeats;
        input.waveform = m_analysis->waveform;
        if (offset && *offset != 0.0 && !input.waveform.entries.empty()) {
            // The waveform with the grid: moved by as many entries as the
            // beats were moved by milliseconds, the track's length kept.
            auto &entries = input.waveform.entries;
            const auto shift = static_cast<std::ptrdiff_t>(std::llround(*offset * domain::WaveformEntriesPerSecond / 1000.0));
            const auto count = static_cast<std::ptrdiff_t>(entries.size());
            if (shift > 0 && shift < count) {
                entries.insert(entries.begin(), static_cast<std::size_t>(shift), domain::WaveformEntry{});
                entries.resize(static_cast<std::size_t>(count));
            } else if (shift < 0 && -shift < count) {
                entries.erase(entries.begin(), entries.begin() - shift);
                entries.resize(static_cast<std::size_t>(count));
            }
        }
        input.sampleRateHz = m_analysis->waveform.sampleRateHz;
        input.durationMs = input.sampleRateHz > 0
                               ? 1000.0 * static_cast<double>(m_analysis->waveform.sampleCount) / input.sampleRateHz
                               : m_track.durationSeconds * 1000.0;
        if (isMp3 && sampleCount && *sampleCount <= 0xFFFFFFFFull) {
            input.mp3SampleCount = static_cast<std::uint32_t>(*sampleCount);
        }
        try {
            anlz = infrastructure::rekordbox::buildAnlzFiles(input);
        } catch (const std::invalid_argument &e) {
            // A grid or a path the files cannot hold: the track goes in
            // unanalysed, and the log says so.
            ctx.log().record(std::string(Label) + ": no analysis files for " + m_track.stickPath + ": " + e.what());
        }
    }
    std::string analysisFile;  // "/PIONEER/USBANLZ/.../ANLZ0000.DAT"
    if (anlz && (toDeviceLibrary || toOneLibrary)) {
        auto &inUse = ctx.sharedForWholeSave<AnalysisFoldersInUse>(
            "track-import:anlz:" + application::normalizedPathKey(pioneer),
            [&]() { return std::make_unique<AnalysisFoldersInUse>(pioneer); });
        // A folder nothing is in: not in the list, and not on the stick
        // either (the list is from one pass, and never an excuse to
        // write over a file).
        for (int attempt = 0; inUse.complete && analysisFile.empty() && attempt < 8; ++attempt) {
            const std::string candidate = infrastructure::rekordbox::newAnlzPathFor(m_track.stickPath, inUse.paths);
            inUse.paths.insert(candidate);
            const std::string stem = candidate.substr(0, candidate.size() - 3);
            bool taken = false;
            for (const char *extension : {"DAT", "EXT", "2EX"}) {
                std::error_code missing;
                taken = taken
                        || fs::symlink_status(stickRoot / pathFromUtf8((stem + extension).substr(1)), missing).type()
                               != fs::file_type::not_found;
            }
            // Nor one a row names whose files are gone: either library
            // refuses a second row for an analysis file.
            if (!taken && toDeviceLibrary) {
                const auto *index = sharedAnlzPathIndex(ctx, m_pioneerRoot);
                taken = index != nullptr && index->names(candidate);
            }
            if (!taken && toOneLibrary) {
                ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
                taken = sharedOneLibraryWriter(ctx, pioneer).namesAnalysisFile(candidate);
            }
            if (!taken) {
                analysisFile = candidate;
            }
        }
        if (analysisFile.empty()) {
            ctx.log().record(std::string(Label) + ": no free analysis folder found for " + m_track.stickPath
                             + "; imported without analysis files");
        }
        if (analysisFile.empty()) {
            // Neither library names a file.
        } else {
            if (toDeviceLibrary) {
                pdbRow.analyzePath = analysisFile;
            }
            if (toOneLibrary) {
                oneRow.analysisDataFilePath = analysisFile;
            }
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
            if (cover) {
                const std::string picture =
                    infrastructure::rekordbox::PdbRowWriter::artworkPathFor('a', pdbRows->nextArtworkId());
                if (pictureNamesFree(picture)) {
                    pdbRow.artworkPath = picture;
                }
            }
            m_deviceLibraryId = pdbRows->addTrack(pdbRow);
        }

        // Not cancellable: the save's cancel is asked between changes
        // (runSaveLoop), and a copy stopped halfway would be a failed
        // change, which takes back every track imported before it. A
        // cancel waits for this one file.
        // The bar counts tracks; a large file says beside it how far its
        // own copy is, every 8 MB.
        infrastructure::track_import::FileCopyOptions copying;
        const QString megabytes = QString::number((sourceSize + (1u << 20) - 1) >> 20);
        std::uint64_t said = 0;
        ctx.status(QStringLiteral("Copying %1").arg(subject()));
        copying.progress = [&](std::uint64_t written) {
            if (written - said >= (8u << 20)) {
                said = written;
                ctx.status(QStringLiteral("Copying %1 (%2 of %3 MB)").arg(subject()).arg(written >> 20).arg(megabytes));
            }
        };
        const infrastructure::track_import::FileCopyResult copied =
            infrastructure::track_import::copyFileVerified(pathFromUtf8(source), pathFromUtf8(destination), copying);
        if (!copied.copied) {
            return ChangeOutcome::failure(QString::fromStdString(copied.error));
        }
        // The row holds the size that was measured before the copy: a
        // file that is another size now is not the file the row is for.
        if (copied.bytes != sourceSize) {
            return ChangeOutcome::failure(
                QStringLiteral("%1 changed while it was imported.").arg(pathToQString(pathFromUtf8(m_sourceFile))));
        }

        if (!analysisFile.empty()) {
            // The three analysis files, before a row that names them is
            // in a library.
            const std::string stem = analysisFile.substr(0, analysisFile.size() - 3);
            const std::pair<const char *, const std::string *> files[] = {
                {"DAT", &anlz->dat}, {"EXT", &anlz->ext}, {"2EX", &anlz->twoEx}};
            for (const auto &[extension, bytes] : files) {
                const fs::path file = pictureFile(stem + extension);
                const std::string path = pathToUtf8(file);
                declareCreated(path);
                fs::create_directories(file.parent_path());
                if (!infrastructure::writeFileDurablyAtomic(path, *bytes)) {
                    return ChangeOutcome::failure(QStringLiteral("Could not write the analysis file %1.").arg(pathToQString(file)));
                }
            }
        }
        if (toDeviceLibrary) {
            if (!pdbRow.artworkPath.empty()) {
                writePictures(pdbRow.artworkPath);
            }
            if (!pdbRows->commit()) {
                return ChangeOutcome::failure(QStringLiteral("Could not write export.pdb for %1.").arg(subject()));
            }
            pdbSession->noteItemApplied();
        }
        if (toOneLibrary) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), Label);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            if (cover) {
                const std::string picture =
                    infrastructure::rekordbox::PdbRowWriter::artworkPathFor('b', static_cast<std::uint64_t>(writer.nextImageId()));
                if (pictureNamesFree(picture)) {
                    writePictures(picture);
                    oneRow.imagePath = picture;
                }
            }
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
            // Engine takes its cover from a file: the large picture, in
            // a temporary one that goes when this block ends.
            QTemporaryFile largePicture(QDir::tempPath() + QStringLiteral("/seabass-import-cover-XXXXXX.jpg"));
            if (cover && largePicture.open()
                && largePicture.write(cover->large.data(), static_cast<qint64>(cover->large.size()))
                       == static_cast<qint64>(cover->large.size())
                && largePicture.flush()) {
                row.source.artworkPath = largePicture.fileName().toStdString();
            }
            infrastructure::engine::EngineTrackCover engineCover;
            engineCover.beforeWrite = declareCreated;
            std::string error;
            m_engineId = infrastructure::engine::createEngineTrack(session.writeRoot(), row, &engineCover, &error);
            if (m_engineId < 0) {
                return ChangeOutcome::failure(QString::fromStdString(error));
            }
            const auto rows =
                infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), destination);
            if (rows != std::vector<std::int64_t>{m_engineId}) {
                return ChangeOutcome::failure(
                    QStringLiteral("Engine has %1 rows for %2 after adding one.").arg(rows.size()).arg(subject()));
            }
            if (anlz) {
                // Grid and overview into the new row, as a copy of
                // rekordbox's analysis writes them: the writer moves the
                // grid onto Engine's timeline again and marks the row
                // analysed.
                application::RekordboxAnalysis analysis;
                analysis.beats = rekordboxBeats;
                analysis.threeBandPreview = anlz->threeBandPreview;
                analysis.colourPreview = anlz->colourPreview;
                analysis.scrollEntries = anlz->scrollEntries;
                if (m_analysis->bpm > 0.0) {
                    analysis.bpm = m_analysis->bpm;
                }
                application::AnalysisImportOptions options;
                options.fileSampleRateHz = static_cast<double>(m_track.sampleRateHz);
                options.fileIsMp3 = isMp3;
                options.fileSampleCount = sampleCount;
                options.fileCountRequired = !isMp3;
                options.grids = !rekordboxBeats.empty();
                const application::AnalysisImportResult written =
                    engineAnalysisWriterFor(ctx, session, m_stickRoot.toStdString())
                        .importAnalysis(std::to_string(m_engineId), analysis, options);
                m_engineAnalysed = written.completedRow;
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
