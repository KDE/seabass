// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/track_reanalysis_prepare.hpp"

#include <QStringList>

#include <filesystem>
#include <map>
#include <optional>

#include "application/path_key.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/mp3_lead_in.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;

// One file of the playlist, as the libraries list it.
struct PlaylistFile
{
    std::string filePath;
    std::string title;
    std::string artist;
    // The analysis files its rows name, by library.
    std::vector<ReanalyseTrackChange::AnalysisFileOf> analysisFiles;
    bool inEngine = false;
    std::string engineId;
    // The cues the review shows, from the first library that lists the
    // file (rekordbox's, OneLibrary's, Engine's: all on rekordbox's
    // timeline as read), and its tempo.
    std::optional<std::vector<domain::CuePoint>> cues;
    double bpm = 0.0;
    // The file's size as each row that records one has it (export.pdb,
    // Engine; OneLibrary's reader gives none).
    std::vector<std::uint64_t> recordedSizes;
};

// The track's grid before the new analysis, on rekordbox's timeline: the
// .DAT's where a rekordbox library names one that is there, else Engine's
// moved by the file's offset. Empty when there is none.
std::vector<double> oldGridOf(const PlaylistFile &file, const fs::path &root, const QString &enginePath, double offsetMs)
{
    for (const auto &analysisFile : file.analysisFiles) {
        const fs::path dat = root / pathFromUtf8(analysisFile.datPath.substr(1));
        std::error_code ec;
        if (!fs::is_regular_file(dat, ec)) {
            continue;
        }
        try {
            return infrastructure::rekordbox::pqtzBeatTimes(infrastructure::rekordbox::AnlzFile::readRaw(pathToUtf8(dat)));
        } catch (const std::exception &) {
            return {};
        }
    }
    if (!file.engineId.empty() && !enginePath.isEmpty()) {
        std::vector<double> beats;
        for (const domain::Beat &beat : infrastructure::engine::readBeatGrid(enginePath.toStdString(), file.engineId)) {
            beats.push_back(beat.timeMs + offsetMs);
        }
        return beats;
    }
    return {};
}

// prepareReanalysis()'s work, on its bar.
PreparedReanalysis prepareReanalysisOn(const ReanalysisRequest &request, const application::CancellationToken &cancel,
                                       ImportBar &bar);

}  // namespace

PreparedReanalysis prepareReanalysis(const ReanalysisRequest &request, const application::CancellationToken &cancel,
                                     const ImportProgressSink &progress)
{
    // One bar from the request to what is staged (ImportBar); planned
    // once the files to analyse are known.
    ImportBar bar(progress);
    bar.report({ImportProgress::Step::Stick, 0, 0, {}, {}});
    PreparedReanalysis prepared = prepareReanalysisOn(request, cancel, bar);
    bar.end(prepared.refusal.empty() ? ImportProgress::Step::Analysis : ImportProgress::Step::Stick);
    return prepared;
}

namespace
{

PreparedReanalysis prepareReanalysisOn(const ReanalysisRequest &request, const application::CancellationToken &cancel,
                                       ImportBar &bar)
{
    PreparedReanalysis prepared;
    const std::string libraryPath =
        request.rekordboxPath.isEmpty() ? request.enginePath.toStdString() : request.rekordboxPath.toStdString();
    if (libraryPath.empty()) {
        prepared.refusal = "There is no stick to analyse tracks on.";
        return prepared;
    }
    const fs::path root = pathFromUtf8(libraryPath).parent_path();
    application::NullProgressReporter quiet;
    const StickCatalogRead read = readAllStickCatalogs(libraryPath, quiet, cancel);
    if (!read.failed.empty()) {
        std::string names;
        for (const std::string &name : read.failed) {
            names += (names.empty() ? "" : ", ") + name;
        }
        prepared.refusal = "This stick has a library that could not be read (" + names
                           + "). A track's analysis belongs in every library of a stick, so nothing is analysed.";
        return prepared;
    }
    QString pioneerRoot;
    QString enginePath;
    for (const std::string &format : read.catalogs.present()) {
        const QString path = QString::fromStdString(catalogPathForFormat(libraryPath, format));
        (format == "engine" ? enginePath : pioneerRoot) = path;
    }

    // The playlist's files in its order, each once: the first library
    // that lists the playlist gives the order, the others add what it
    // lacks at the end.
    const std::pair<const char *, const std::optional<std::vector<domain::Track>> *> byLibrary[] = {
        {"rekordbox", &read.catalogs.rekordbox}, {"onelibrary", &read.catalogs.oneLibrary}, {"engine", &read.catalogs.engine}};
    std::vector<std::string> order;
    std::map<std::string, PlaylistFile> files;
    for (const auto &[library, catalog] : byLibrary) {
        if (!*catalog) {
            continue;
        }
        std::multimap<int, const domain::Track *> listed;
        for (const domain::Track &row : **catalog) {
            for (const auto &membership : row.playlists) {
                if (membership.name == request.playlist && !row.filePath.empty()) {
                    listed.emplace(membership.position, &row);
                }
            }
        }
        for (const auto &[position, row] : listed) {
            const std::string key = application::normalizedPathKey(row->filePath);
            if (files.emplace(key, PlaylistFile{row->filePath, row->title, row->artist, {}, false}).second) {
                order.push_back(key);
            }
        }
    }
    prepared.tracks = static_cast<int>(order.size());
    // Every row of those files, in every library: what each has to replace.
    for (const auto &[library, catalog] : byLibrary) {
        if (!*catalog) {
            continue;
        }
        for (const domain::Track &row : **catalog) {
            const auto file = files.find(application::normalizedPathKey(row.filePath));
            if (file == files.end()) {
                continue;
            }
            if (std::string(library) == "engine") {
                file->second.inEngine = true;
                if (file->second.engineId.empty()) {
                    file->second.engineId = row.sourceId;
                }
            } else if (!row.analysisFile.empty()) {
                file->second.analysisFiles.push_back({QString::fromLatin1(library), row.analysisFile});
            }
            if (!file->second.cues) {
                file->second.cues = row.cues;
                file->second.bpm = row.bpm;
            }
            if (row.catalogFileBytes > 0) {
                file->second.recordedSizes.push_back(row.catalogFileBytes);
            }
        }
    }

    // The ones whose audio is there, as the libraries describe it.
    std::vector<std::string> toAnalyse;
    std::vector<const PlaylistFile *> analysing;
    for (const std::string &key : order) {
        cancel.throwIfCancelled();
        const PlaylistFile &file = files.at(key);
        std::error_code ec;
        const fs::path audio = infrastructure::longPathSafe(pathFromUtf8(file.filePath));
        if (!fs::is_regular_file(audio, ec)) {
            prepared.leftAlone.push_back({file.filePath, "is not on the stick"});
            continue;
        }
        // A file of another size than its rows record is not the file
        // they describe: its length, its MP3 frame count (PVBR) and its
        // size would all be the old file's beside a new analysis. Importing
        // it again is the way to give it rows of its own.
        const std::uintmax_t size = fs::file_size(audio, ec);
        bool otherFile = static_cast<bool>(ec);
        for (const std::uint64_t recorded : file.recordedSizes) {
            otherFile = otherFile || recorded != static_cast<std::uint64_t>(size);
        }
        if (otherFile) {
            prepared.leftAlone.push_back({file.filePath, "is not the file its rows describe (another size)"});
            continue;
        }
        // A row whose .DAT is not there has no analysis to replace: that
        // library is left as it is (the change asks again at the save).
        bool anyDat = false;
        for (const auto &analysisFile : file.analysisFiles) {
            anyDat = anyDat
                     || fs::is_regular_file(infrastructure::longPathSafe(root / pathFromUtf8(analysisFile.datPath.substr(1))),
                                            ec);
        }
        if (!anyDat && !file.inEngine) {
            prepared.leftAlone.push_back({file.filePath, "has no analysis in any library on the stick to replace"});
            continue;
        }
        toAnalyse.push_back(file.filePath);
        analysing.push_back(&file);
    }

    std::uint64_t bytes = 0;
    for (const std::string &file : toAnalyse) {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(infrastructure::longPathSafe(pathFromUtf8(file)), ec);
        bytes += ec ? 0 : static_cast<std::uint64_t>(size);
    }
    bar.plan({{ImportProgress::Step::Stick, ImportBar::StickUnits},
              {ImportProgress::Step::Analysis, ImportBar::analysisUnits(bytes, toAnalyse.size())}});
    const std::vector<std::shared_ptr<const ImportAnalysis>> made =
        analyseFiles(toAnalyse, request.analyser, request.analysisJobs, cancel, bar.sink());
    for (std::size_t i = 0; i < made.size(); ++i) {
        const PlaylistFile &file = *analysing[i];
        if (!made[i]) {
            prepared.leftAlone.push_back({file.filePath, "could not be decoded"});
            continue;
        }
        if (made[i]->beats.empty()) {
            prepared.leftAlone.push_back({file.filePath, "the analysis found no beat grid"});
            continue;
        }
        // An MP3 whose lead-in cannot be read: the grid has no known place
        // on rekordbox's timeline, and a grid half a frame off is worse
        // than the one the track has.
        const std::optional<double> offset = infrastructure::audio::cueTimelineOffsetMs(file.filePath);
        if (!offset) {
            prepared.leftAlone.push_back({file.filePath, "its MP3 lead-in could not be read"});
            continue;
        }
        std::error_code ec;
        const fs::path relative = fs::relative(pathFromUtf8(file.filePath), root, ec);
        const std::string stickPath = "/" + pathToGenericUtf8(relative);
        if (ec || relative.empty() || stickPath.rfind("/..", 0) == 0) {
            prepared.leftAlone.push_back({file.filePath, "is not under the stick's root"});
            continue;
        }
        const QString title = QString::fromStdString(file.title);
        auto change = std::make_shared<ReanalyseTrackChange>(
            file.filePath, stickPath, pathToQString(root), pioneerRoot, enginePath, file.analysisFiles, made[i],
            made[i]->key, title, QString::fromStdString(file.artist),
            static_cast<int>(made.size()));
        // What the cues on the old grid would do on the new one, for the
        // review: both grids as the save will see them.
        ReanalyseTrackChange::CueReview review;
        review.oldBeats = oldGridOf(file, root, enginePath, *offset);
        for (const domain::Beat &beat : rekordboxAnalysisFilesFor(*made[i], file.filePath, stickPath, 0.0).beats) {
            review.newBeats.push_back(beat.timeMs);
        }
        review.cues = file.cues.value_or(std::vector<domain::CuePoint>{});
        review.plan = application::planCueGridMoves(review.cues, review.oldBeats, review.newBeats);
        review.waveformOffsetMs = *offset;
        review.oldBpm = file.bpm;
        const bool onTheirOwn = application::cuesFollowOnTheirOwn(review.plan, request.cueFollowThresholdMs);
        change->setCueReview(std::move(review));
        change->setMoveCuesByThreshold(onTheirOwn, request.cueFollowThresholdMs);
        prepared.changes.push_back(std::move(change));
    }
    return prepared;
}

}  // namespace

}  // namespace seabass::gui
