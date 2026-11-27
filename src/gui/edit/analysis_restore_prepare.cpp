// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/analysis_restore_prepare.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>

#include "application/path_key.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/cue_tolerance.hpp"
#include "gui/edit/changes/kept_analysis_save.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/local/analysis_ledger_file.hpp"
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

std::atomic<int> &catalogReads()
{
    static std::atomic<int> reads{0};
    return reads;
}

// Every catalog of the stick, at full detail for a restore (its cues) or
// at the first stage for a count (the rows and their playlists).
StickCatalogRead readCatalogs(const std::string &libraryPath, bool full, const application::CancellationToken &cancel)
{
    ++catalogReads();
    application::NullProgressReporter quiet;
    return full ? readAllStickCatalogs(libraryPath, quiet, cancel) : readAllStickCatalogTracks(libraryPath, cancel);
}

std::string libraryPathOf(const AnalysisRestoreRequest &request)
{
    return request.rekordboxPath.isEmpty() ? request.enginePath.toStdString() : request.rekordboxPath.toStdString();
}

StickPlaylist readPlaylist(const AnalysisRestoreRequest &request, bool full, const application::CancellationToken &cancel)
{
    StickPlaylist out;
    const std::string libraryPath = libraryPathOf(request);
    if (libraryPath.empty()) {
        out.refusal = "There is no stick to restore tracks on.";
        return out;
    }
    const StickCatalogRead read = readCatalogs(libraryPath, full, cancel);
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

// Whether the player's analysis of one of the file's Engine rows is kept
// by Analyse again: the record names it (`recorded`, its fingerprint).
bool engineKeptFor(const PlaylistFile &file, const fs::path &root, const std::string &key, const std::string &recorded,
                   std::string *damaged)
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
    if (found.status != application::KeptEngineAnalysisLookup::Status::Found || recorded.empty()
        || keptEngineOriginalFingerprint(found.entry) != recorded) {
        return false;
    }
    for (const std::string &id : file.engineIds) {
        if (id == std::to_string(found.entry.trackId)) {
            return true;
        }
    }
    return false;
}

// The grids the stick records as Seabass's beyond a track's own record
// (every analysis ledger entry's, every kept record's), read once for a
// prepare and only when a track's own record does not know its grid.
struct SeabassGridsOnStick
{
    std::optional<std::set<std::string>> grids;

    bool knows(const fs::path &root, const std::string &grid)
    {
        if (!grids) {
            grids.emplace();
            std::string error;
            for (const auto &[key, entry] : infrastructure::local::readAnalysisLedger(root, &error)) {
                if (!entry.rekordboxGrid.empty()) {
                    grids->insert(entry.rekordboxGrid);
                }
            }
            for (const std::string &one : infrastructure::local::keptRekordboxSeabassGrids(root, &error)) {
                grids->insert(one);
            }
        }
        return grids->count(grid) > 0;
    }
};

// Why the track's analysis cannot go back because it was changed outside
// Seabass since its original was kept: a kept .DAT whose grid on the
// stick is neither that file's kept original's nor one Seabass wrote (as
// RestoreAnalysisChange decides it at the save). Empty when every file is
// one Restore may write over, or cannot be read here (the save then says
// why).
std::string changedOutsideIn(const infrastructure::local::KeptRekordboxAnalysis &kept,
                             const infrastructure::local::KeptRekordboxRecord &record, const fs::path &root,
                             SeabassGridsOnStick &seabass)
{
    for (const auto &set : kept.files) {
        if (!set.dat || set.datPath.size() < 2) {
            continue;
        }
        std::string now;
        std::string original;
        try {
            const fs::path dat = root / pathFromUtf8(set.datPath.substr(1));
            std::error_code ec;
            if (!fs::is_regular_file(dat, ec)) {
                continue;
            }
            now = infrastructure::rekordbox::pqtzGridFingerprint(infrastructure::rekordbox::AnlzFile::readRaw(pathToUtf8(dat)));
            original = infrastructure::rekordbox::pqtzGridFingerprint(
                infrastructure::rekordbox::AnlzFile::fromBytes(*set.dat, "the kept " + set.datPath));
        } catch (const std::exception &) {
            continue;
        }
        if (now == original || std::find(record.seabassGrids.begin(), record.seabassGrids.end(), now) != record.seabassGrids.end()
            || seabass.knows(root, now)) {
            continue;
        }
        return "its beat grid in " + set.datPath
               + " was changed outside Seabass since its original analysis was kept (it is neither that original "
                 "nor one Seabass wrote), and Seabass does not write over it";
    }
    return {};
}

}  // namespace

PreparedAnalysisRestore prepareAnalysisRestore(const AnalysisRestoreRequest &request,
                                               const application::CancellationToken &cancel)
{
    PreparedAnalysisRestore prepared;
    const StickPlaylist stick = readPlaylist(request, true, cancel);
    if (!stick.refusal.empty()) {
        prepared.refusal = stick.refusal;
        return prepared;
    }
    prepared.tracks = static_cast<int>(stick.files.size());
    infrastructure::local::StickKeptRekordboxAnalysisStore rekordboxKept(stick.root);
    SeabassGridsOnStick seabassGrids;
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
        const auto recorded = rekordboxKept.findRecord(key);
        if (recorded.status != infrastructure::local::KeptRekordboxRecordLookup::Status::Found) {
            prepared.leftAlone.push_back(
                {file.filePath, recorded.status == infrastructure::local::KeptRekordboxRecordLookup::Status::None
                                    ? "Seabass cannot tell whether its beat grid is Seabass's own: the record of the "
                                      "grids Seabass wrote, kept beside the original, is missing"
                                    : "Seabass cannot tell whether its beat grid is Seabass's own: the record of the "
                                      "grids Seabass wrote, kept beside the original, cannot be read ("
                                          + recorded.error + ")"});
            continue;
        }
        // A grid changed outside Seabass since the original was kept is
        // never written over: such a track is left as it is, not staged.
        if (const std::string changed = changedOutsideIn(found.entry, recorded.record, stick.root, seabassGrids);
            !changed.empty()) {
            prepared.leftAlone.push_back({file.filePath, changed});
            continue;
        }
        std::string engineDamaged;
        const bool engineKept = engineKeptFor(file, stick.root, key, recorded.record.engineKept, &engineDamaged);
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
            review.originUnknown = !recorded.record.originKnown;
            review.plan = cueMovesBack(found.entry.cueMoves, "rekordbox", review.cues, &review.blocked);
            if (review.plan.moves.empty()) {
                // Only Engine's cues moved (rekordbox's libraries held none).
                bool engineMoved = false;
                for (const auto &move : found.entry.cueMoves) {
                    engineMoved = engineMoved || move.library == "engine";
                }
                // Only where Engine's grid goes back: one that keeps the grid
                // it has keeps its cues on it.
                if (engineMoved && engineKept) {
                    review.plan = cueMovesBack(found.entry.cueMoves, "engine", {});
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

std::string stickIdOr(const std::string &stickId, const fs::path &root)
{
    return stickId.empty() ? "root:" + application::normalizedPathKey(pathToUtf8(root)) : stickId;
}

// The kept-entry file names of the tracks of `files` on the stick.
std::vector<std::string> keptNamesOf(const std::vector<PlaylistFile> &files, const fs::path &root)
{
    std::vector<std::string> names;
    for (const PlaylistFile &file : files) {
        if (const std::string key = keyOf(file.filePath, root); !key.empty()) {
            names.push_back(pathToUtf8(infrastructure::local::keptRekordboxAnalysisFile(root, key).filename()));
        }
    }
    return names;
}

}  // namespace

int countKeptOriginals(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel)
{
    const std::string libraryPath = libraryPathOf(request);
    if (libraryPath.empty()) {
        return -1;
    }
    // Nothing kept on the stick: nothing else is read.
    const fs::path stickRoot = pathFromUtf8(infrastructure::paths::normalStickRootForCatalogPath(libraryPath));
    if (keptAnalysisNamesOf(stickIdOr(request.stickId, stickRoot), stickRoot)->rekordbox.empty()) {
        return 0;
    }
    const StickPlaylist stick = readPlaylist(request, false, cancel);
    if (!stick.refusal.empty()) {
        return -1;
    }
    const std::string id = stickIdOr(request.stickId, stick.root);
    const auto names = keptAnalysisNamesOf(id, stick.root);
    // Analyse again's only: an Engine entry alone is Beat Grids &
    // Waveforms', restored there. Without rekordbox's libraries nothing
    // is Analyse again's.
    const std::vector<std::string> mine =
        stick.pioneerRoot.isEmpty() ? std::vector<std::string>{} : keptNamesOf(stick.files, stick.root);
    noteKeptAnalysisPlaylist(id, stick.root, request.playlist, mine);
    int count = 0;
    for (const std::string &name : mine) {
        cancel.throwIfCancelled();
        count += names->rekordbox.count(name) > 0 ? 1 : 0;
    }
    return count;
}

void warmKeptCounts(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel)
{
    const std::string libraryPath = libraryPathOf(request);
    if (libraryPath.empty()) {
        return;
    }
    // The kept folders first: with nothing of rekordbox's kept, every
    // playlist's count is 0 (keptCountFromCache()), and no library is read.
    const fs::path stickRoot = pathFromUtf8(infrastructure::paths::normalStickRootForCatalogPath(libraryPath));
    if (relistKeptAnalysisNames(stickIdOr(request.stickId, stickRoot), stickRoot)->rekordbox.empty()) {
        return;
    }
    // The rows and their playlists only, as Browse has them loaded (the
    // catalog cache serves its entry at once).
    const StickCatalogRead read = readCatalogs(libraryPath, false, cancel);
    if (!read.failed.empty()) {
        return;
    }
    QString pioneerRoot;
    QString enginePath;
    for (const std::string &format : read.catalogs.present()) {
        const QString path = QString::fromStdString(catalogPathForFormat(libraryPath, format));
        (format == "engine" ? enginePath : pioneerRoot) = path;
    }
    const fs::path root = pathFromUtf8(infrastructure::paths::normalStickRootForCatalogPath(
        (pioneerRoot.isEmpty() ? enginePath : pioneerRoot).toStdString()));
    const std::string id = stickIdOr(request.stickId, root);
    // Every playlist's files, each file once, as readPlaylist() takes them.
    std::map<std::string, std::vector<PlaylistFile>> byPlaylist;
    std::map<std::string, std::set<std::string>> seen;
    for (const auto *catalog : {&read.catalogs.rekordbox, &read.catalogs.oneLibrary, &read.catalogs.engine}) {
        if (!*catalog) {
            continue;
        }
        for (const domain::Track &row : **catalog) {
            cancel.throwIfCancelled();
            for (const auto &membership : row.playlists) {
                if (!row.filePath.empty()
                    && seen[membership.name].insert(application::normalizedPathKey(row.filePath)).second) {
                    byPlaylist[membership.name].push_back(PlaylistFile{row.filePath, {}, {}, {}, std::nullopt, 0.0});
                }
            }
        }
    }
    for (const auto &[playlist, files] : byPlaylist) {
        noteKeptAnalysisPlaylist(id, root, playlist,
                     pioneerRoot.isEmpty() ? std::vector<std::string>{} : keptNamesOf(files, root));
    }
}

int keptCountCatalogReadsForTesting()
{
    return catalogReads().load();
}

}  // namespace seabass::gui
