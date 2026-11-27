// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/analysis_restore_prepare.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>

#include "application/path_key.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/cue_tolerance.hpp"
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
        // Only a track Analyse again kept rekordbox's analysis of: the
        // player's analysis that Beat Grids & Waveforms kept alone has its
        // own way back there (Restore Engine's Analysis), and is not
        // touched here. With rekordbox's, the player's goes back too.
        if (found.status == Status::None) {
            continue;
        }
        if (found.status == Status::Damaged) {
            prepared.leftAlone.push_back({file.filePath, "what Seabass kept of rekordbox's analysis of it cannot be "
                                                         "read (" + found.error + ")"});
            continue;
        }
        std::string engineDamaged;
        const bool engineKept = engineKeptFor(file, stick.root, key, &engineDamaged);
        std::vector<std::string> datPaths;
        for (const auto &set : found.entry.files) {
            datPaths.push_back(set.datPath);
        }
        auto change = std::make_shared<RestoreAnalysisChange>(
            file.filePath, stickPath, pathToQString(stick.root), stick.pioneerRoot, stick.enginePath, datPaths,
            engineKept, QString::fromStdString(file.title), QString::fromStdString(file.artist),
            static_cast<int>(stick.files.size()));
        {
            // The review: the cue points Analyse again moved, as the kept
            // entry records them, that are still where it put them; each
            // goes back to where it was. None recorded, none offered.
            RestoreAnalysisChange::CueReview review;
            review.cues = file.cues.value_or(std::vector<domain::CuePoint>{});
            review.oldBpm = file.bpm;
            review.plan = cueMovesBack(found.entry.cueMoves, "rekordbox", review.cues,
                                       domain::cueToleranceMsFor(file.bpm, file.bpm));
            if (review.plan.moves.empty()) {
                // Only Engine's cues moved (rekordbox's libraries held none).
                bool engineMoved = false;
                for (const auto &move : found.entry.cueMoves) {
                    engineMoved = engineMoved || move.library == "engine";
                }
                if (engineMoved) {
                    review.plan = cueMovesBack(found.entry.cueMoves, "engine", {}, 0.0);
                }
            }
            const bool onTheirOwn = application::cuesFollowOnTheirOwn(review.plan, request.cueFollowThresholdMs);
            change->setCueReview(std::move(review));
            change->setMoveCuesByThreshold(onTheirOwn, request.cueFollowThresholdMs);
        }
        changes.push_back(std::move(change));
    }
    prepared.changes = std::move(changes);
    if (prepared.changes.empty() && prepared.leftAlone.empty()) {
        prepared.refusal = "No track of \"" + request.playlist + "\" has an original analysis that Analyse again kept.";
    }
    return prepared;
}

namespace
{

struct NameCache
{
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const KeptAnalysisNames>> byStick;
    std::uint64_t generation = 0;
};

NameCache &nameCache()
{
    static NameCache cache;
    return cache;
}

std::set<std::string> fileNamesIn(const fs::path &dir)
{
    std::set<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        names.insert(pathToUtf8(it->path().filename()));
    }
    return names;
}

}  // namespace

std::shared_ptr<const KeptAnalysisNames> keptAnalysisNamesOf(const fs::path &stickRoot)
{
    NameCache &cache = nameCache();
    const std::string key = application::normalizedPathKey(pathToUtf8(stickRoot));
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(cache.mutex);
        if (const auto found = cache.byStick.find(key); found != cache.byStick.end()) {
            return found->second;
        }
        generation = cache.generation;
    }
    auto names = std::make_shared<KeptAnalysisNames>();
    names->rekordbox = fileNamesIn(infrastructure::paths::stickKeptRekordboxAnalysisDir(stickRoot));
    names->engine = fileNamesIn(infrastructure::paths::stickKeptEngineAnalysisDir(stickRoot));
    std::lock_guard lock(cache.mutex);
    if (cache.generation == generation) {
        cache.byStick[key] = names;
    }
    return names;
}

void forgetKeptAnalysisNames()
{
    NameCache &cache = nameCache();
    std::lock_guard lock(cache.mutex);
    cache.byStick.clear();
    ++cache.generation;
}

std::uint64_t keptAnalysisNamesGeneration()
{
    NameCache &cache = nameCache();
    std::lock_guard lock(cache.mutex);
    return cache.generation;
}

int countKeptOriginals(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel)
{
    const StickPlaylist stick = readPlaylist(request, cancel);
    if (!stick.refusal.empty()) {
        return -1;
    }
    const auto names = keptAnalysisNamesOf(stick.root);
    int count = 0;
    for (const PlaylistFile &file : stick.files) {
        cancel.throwIfCancelled();
        const std::string key = keyOf(file.filePath, stick.root);
        if (key.empty()) {
            continue;
        }
        // Analyse again's only: an Engine entry alone is Beat Grids &
        // Waveforms', restored there.
        const bool rekordbox =
            !stick.pioneerRoot.isEmpty()
            && names->rekordbox.count(pathToUtf8(infrastructure::local::keptRekordboxAnalysisFile(stick.root, key).filename()))
                   > 0;
        count += rekordbox ? 1 : 0;
    }
    return count;
}

}  // namespace seabass::gui
