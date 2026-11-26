// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/analysis_restore_prepare.hpp"

#include <filesystem>
#include <map>
#include <optional>

#include "application/path_key.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"

namespace seabass::gui
{

namespace
{

namespace fs = std::filesystem;

struct PlaylistFile
{
    std::string filePath;
    std::string title;
    std::string artist;
    std::vector<std::string> engineIds;
    std::optional<std::vector<domain::CuePoint>> cues;
    double bpm = 0.0;
};

struct StickPlaylist
{
    std::string refusal;
    fs::path root;
    QString pioneerRoot;
    QString enginePath;
    std::vector<PlaylistFile> files;
};

StickPlaylist readPlaylist(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel)
{
    StickPlaylist out;
    const std::string libraryPath =
        request.rekordboxPath.isEmpty() ? request.enginePath.toStdString() : request.rekordboxPath.toStdString();
    if (libraryPath.empty()) {
        out.refusal = "There is no stick to restore tracks on.";
        return out;
    }
    application::NullProgressReporter quiet;
    const StickCatalogRead read = readAllStickCatalogs(libraryPath, quiet, cancel);
    if (!read.failed.empty()) {
        std::string names;
        for (const std::string &name : read.failed) {
            names += (names.empty() ? "" : ", ") + name;
        }
        out.refusal = "This stick has a library that could not be read (" + names
                      + "). A track's analysis belongs in every library of a stick, so nothing is restored.";
        return out;
    }
    for (const std::string &format : read.catalogs.present()) {
        const QString path = QString::fromStdString(catalogPathForFormat(libraryPath, format));
        (format == "engine" ? out.enginePath : out.pioneerRoot) = path;
    }
    out.root = pathFromUtf8(infrastructure::paths::normalStickRootForCatalogPath(
        (out.pioneerRoot.isEmpty() ? out.enginePath : out.pioneerRoot).toStdString()));
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
            if (files.emplace(key, PlaylistFile{row->filePath, row->title, row->artist, {}, std::nullopt, 0.0}).second) {
                order.push_back(key);
            }
        }
    }
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
                file->second.engineIds.push_back(row.sourceId);
            }
            if (!file->second.cues) {
                file->second.cues = row.cues;
                file->second.bpm = row.bpm;
            }
        }
    }
    for (const std::string &key : order) {
        out.files.push_back(files.at(key));
    }
    return out;
}

std::string keyOf(const std::string &file, const fs::path &root)
{
    const std::string relative = application::stickRelativePathOf(file, pathToUtf8(root));
    return relative.empty() ? std::string() : application::normalizedPathKey(relative);
}

std::string stickPathOf(const std::string &file, const fs::path &root)
{
    std::error_code ec;
    const fs::path relative = fs::relative(pathFromUtf8(file), root, ec);
    const std::string stickPath = "/" + pathToGenericUtf8(relative);
    return ec || relative.empty() || stickPath.rfind("/..", 0) == 0 ? std::string() : stickPath;
}

// Whether the player's analysis of one of the file's Engine rows is kept.
bool engineKeptFor(const PlaylistFile &file, const fs::path &root, const std::string &key, std::string *damaged)
{
    if (file.engineIds.empty()) {
        return false;
    }
    infrastructure::local::StickKeptEngineAnalysisStore store(root);
    const auto found = store.find(key);
    if (found.status == application::KeptEngineAnalysisLookup::Status::Damaged) {
        *damaged = found.error;
        return false;
    }
    if (found.status != application::KeptEngineAnalysisLookup::Status::Found) {
        return false;
    }
    for (const std::string &id : file.engineIds) {
        if (id == std::to_string(found.entry.trackId)) {
            return true;
        }
    }
    return false;
}

std::vector<double> beatsOf(const std::optional<std::string> &bytes, const std::string &context)
{
    if (!bytes) {
        return {};
    }
    try {
        return infrastructure::rekordbox::pqtzBeatTimes(infrastructure::rekordbox::AnlzFile::fromBytes(*bytes, context));
    } catch (const std::exception &) {
        return {};
    }
}

}  // namespace

PreparedAnalysisRestore prepareAnalysisRestore(const AnalysisRestoreRequest &request,
                                               const application::CancellationToken &cancel)
{
    PreparedAnalysisRestore prepared;
    const StickPlaylist stick = readPlaylist(request, cancel);
    if (!stick.refusal.empty()) {
        prepared.refusal = stick.refusal;
        return prepared;
    }
    prepared.tracks = static_cast<int>(stick.files.size());
    infrastructure::local::StickKeptRekordboxAnalysisStore rekordboxKept(stick.root);
    std::vector<std::shared_ptr<RestoreAnalysisChange>> changes;
    for (const PlaylistFile &file : stick.files) {
        cancel.throwIfCancelled();
        const std::string key = keyOf(file.filePath, stick.root);
        const std::string stickPath = stickPathOf(file.filePath, stick.root);
        if (key.empty() || stickPath.empty()) {
            continue;
        }
        using Status = infrastructure::local::KeptRekordboxAnalysisLookup::Status;
        const auto found = stick.pioneerRoot.isEmpty() ? infrastructure::local::KeptRekordboxAnalysisLookup{}
                                                       : rekordboxKept.find(key);
        std::string engineDamaged;
        const bool engineKept = engineKeptFor(file, stick.root, key, &engineDamaged);
        if (found.status == Status::Damaged) {
            prepared.leftAlone.push_back({file.filePath, "what Seabass kept of rekordbox's analysis of it cannot be "
                                                         "read (" + found.error + ")"});
            continue;
        }
        if (!engineDamaged.empty() && found.status != Status::Found) {
            prepared.leftAlone.push_back({file.filePath, "what Seabass kept of the player's analysis of it cannot be "
                                                         "read (" + engineDamaged + ")"});
            continue;
        }
        if (found.status != Status::Found && !engineKept) {
            continue;
        }
        std::vector<std::string> datPaths;
        for (const auto &set : found.entry.files) {
            datPaths.push_back(set.datPath);
        }
        auto change = std::make_shared<RestoreAnalysisChange>(
            file.filePath, stickPath, pathToQString(stick.root), stick.pioneerRoot, stick.enginePath, datPaths,
            engineKept, QString::fromStdString(file.title), QString::fromStdString(file.artist),
            static_cast<int>(stick.files.size()));
        if (found.status == Status::Found && !found.entry.files.empty()) {
            const auto &set = found.entry.files.front();
            RestoreAnalysisChange::CueReview review;
            const fs::path dat = stick.root / pathFromUtf8(set.datPath.substr(1));
            try {
                review.oldBeats = infrastructure::rekordbox::pqtzBeatTimes(
                    infrastructure::rekordbox::AnlzFile::readRaw(pathToUtf8(dat)));
            } catch (const std::exception &) {
            }
            review.newBeats = beatsOf(set.dat, "the kept " + set.datPath);
            review.cues = file.cues.value_or(std::vector<domain::CuePoint>{});
            review.plan = application::planCueGridMoves(review.cues, review.oldBeats, review.newBeats);
            review.oldBpm = file.bpm;
            const bool onTheirOwn = application::cuesFollowOnTheirOwn(review.plan, request.cueFollowThresholdMs);
            change->setCueReview(std::move(review));
            change->setMoveCuesByThreshold(onTheirOwn, request.cueFollowThresholdMs);
        }
        changes.push_back(std::move(change));
    }
    prepared.changes = std::move(changes);
    return prepared;
}

int countKeptOriginals(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel)
{
    const StickPlaylist stick = readPlaylist(request, cancel);
    if (!stick.refusal.empty()) {
        return -1;
    }
    int count = 0;
    for (const PlaylistFile &file : stick.files) {
        cancel.throwIfCancelled();
        const std::string key = keyOf(file.filePath, stick.root);
        if (key.empty()) {
            continue;
        }
        std::error_code ec;
        const bool rekordbox = !stick.pioneerRoot.isEmpty()
                               && fs::is_regular_file(infrastructure::local::keptRekordboxAnalysisFile(stick.root, key), ec);
        std::string damaged;
        count += rekordbox || engineKeptFor(file, stick.root, key, &damaged) ? 1 : 0;
    }
    return count;
}

}  // namespace seabass::gui
