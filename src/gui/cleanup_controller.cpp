#include "infrastructure/paths/seabass_paths.hpp"
#include "cleanup_controller.hpp"

#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <set>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

#include "application/use_cases/collapse_catalog_rows.hpp"
#include "application/use_cases/real_file_sizes.hpp"
#include "application/ports/cue_writer.hpp"
#include "application/ports/library_cleanup_writer.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/track_scope.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/stick_catalogs.hpp"
#include "gui/onelibrary_cue_writer_adapter.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/write_guard.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/cleanup/pending_deletion_applier.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/cleanup/pending_deletion_resolver.hpp"
#include "infrastructure/cleanup/stray_file_scan.hpp"
#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/logging/file_operation_log.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cleanup_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/cleanup_group_change.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

// Mirrors cli/main.cpp's humanSize() exactly, see duplicates_controller
// .cpp's identical copy for why this is duplicated per composition root
// rather than shared.
QString humanSize(std::uint64_t bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        unit++;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return QString::fromUtf8(buf);
}

// True when this copy is a stray file the planner refused to delete.
bool isHeldBackStray(const domain::DuplicateCleanupPlan &plan, const domain::Track &t)
{
    return std::any_of(plan.unreferencedFilesHeldBack.begin(), plan.unreferencedFilesHeldBack.end(),
                        [&t](const domain::Track &h) { return h.sourceId == t.sourceId; });
}

// Held-back stray files are deliberately not counted: they are listed
// in toRemove like any other non-survivor, but the planner refuses to
// delete them, so counting their bytes would promise space the page will
// never free. See DuplicateCleanupPlan::unreferencedFilesHeldBack.
std::uint64_t wastedBytes(const domain::DuplicateCleanupPlan &plan)
{
    std::uint64_t total = 0;
    for (const auto &t : plan.toRemove) {
        if (!isHeldBackStray(plan, t)) {
            total += t.fileSizeBytes;
        }
    }
    return total;
}

// side/artworkPath/cues added so this can feed TrackWaveformCard
// directly (same shape trackToMap() produces elsewhere in this app) --
// deliberately no "waveform" field, see TrackWaveformCard.qml's own
// comment: that's fetched on demand by QML, not precomputed here.
QVariantMap trackSummary(const domain::Track &t)
{
    QVariantMap m;
    m["side"] = QString::fromStdString(t.format);
    // Not derived from side == "disk" in QML: this decides whether the
    // row is about deleting a file or dropping a database row.
    m["isUnreferenced"] = t.isUnreferenced;
    m["sourceId"] = QString::fromStdString(t.sourceId);
    m["title"] = QString::fromStdString(t.title);
    m["artist"] = QString::fromStdString(t.artist);
    m["filePath"] = QString::fromStdString(t.filePath);
    m["artworkPath"] = QString::fromStdString(t.artworkPath);
    m["bitrate"] = t.bitrate;
    m["durationMs"] = t.durationSeconds * 1000.0;
    m["sizeBytes"] = static_cast<qulonglong>(t.fileSizeBytes);
    m["sizeHuman"] = humanSize(t.fileSizeBytes);

    QVariantList cues;
    for (const auto &c : t.cues) {
        QVariantMap cueMap;
        cueMap["kind"] = c.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("hot") : QStringLiteral("memory");
        cueMap["hotCueNumber"] = c.hotCueNumber;
        cueMap["positionMs"] = c.positionMs;
        cueMap["color"] = QString::fromStdString(c.color);
        cues << cueMap;
    }
    m["cues"] = cues;
    return m;
}

}  // namespace

CleanupPlanListModel::CleanupPlanListModel(QObject *parent) : QAbstractListModel(parent) {}

int CleanupPlanListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_visibleIndices.size());
}

QVariant CleanupPlanListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_visibleIndices.size()) {
        return {};
    }
    size_t realIndex = m_visibleIndices[static_cast<size_t>(index.row())];
    const auto &plan = m_plans[realIndex];
    switch (role) {
    case SurvivorRole:
        return trackSummary(plan.survivor);
    case ToRemoveRole: {
        QVariantList result;
        for (const auto &t : plan.toRemove) {
            QVariantMap summary = trackSummary(t);
            // Per copy, because the page shows one card each and the
            // three outcomes are genuinely different: a row dropped, a
            // file listed for deletion, or a file left alone.
            summary["heldBack"] = isHeldBackStray(plan, t);
            result << summary;
        }
        return result;
    }
    case DiffersRole:
        return plan.differs;
    case HasUnpreservableDataAtRiskRole:
        return plan.hasUnpreservableDataAtRisk;
    case UnreferencedCountRole:
        return static_cast<int>(plan.unreferencedFilesToDelete.size());
    case UnreferencedHeldBackCountRole:
        return static_cast<int>(plan.unreferencedFilesHeldBack.size());
    case WastedBytesHumanRole:
        return humanSize(wastedBytes(plan));
    case NewCueCountRole:
        return static_cast<int>(plan.mergedCuesForSurvivor.size()) - static_cast<int>(plan.survivor.cues.size());
    case IncludedRole:
        return m_included[realIndex];
    case StagedRole:
        return realIndex < m_stagedDescriptions.size() && !m_stagedDescriptions[realIndex].isEmpty();
    case StagedDescriptionRole:
        return realIndex < m_stagedDescriptions.size() ? m_stagedDescriptions[realIndex] : QString();
    default:
        return {};
    }
}

bool CleanupPlanListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_visibleIndices.size()) {
        return false;
    }
    if (role != IncludedRole) {
        return false;
    }
    m_included[m_visibleIndices[static_cast<size_t>(index.row())]] = value.toBool();
    emit dataChanged(index, index, {IncludedRole});
    return true;
}

QHash<int, QByteArray> CleanupPlanListModel::roleNames() const
{
    return {
        {SurvivorRole, "survivor"},
        {ToRemoveRole, "toRemove"},
        {DiffersRole, "differs"},
        {HasUnpreservableDataAtRiskRole, "hasUnpreservableDataAtRisk"},
        {UnreferencedCountRole, "unreferencedCount"},
        {UnreferencedHeldBackCountRole, "unreferencedHeldBackCount"},
        {WastedBytesHumanRole, "wastedBytesHuman"},
        {NewCueCountRole, "newCueCount"},
        {IncludedRole, "included"},
        {StagedRole, "staged"},
        {StagedDescriptionRole, "stagedDescription"},
    };
}

void CleanupPlanListModel::setPlans(std::vector<domain::DuplicateCleanupPlan> plans)
{
    beginResetModel();
    m_plans = std::move(plans);
    m_included.assign(m_plans.size(), true);
    m_stagedDescriptions.assign(m_plans.size(), QString());
    for (size_t i = 0; i < m_plans.size(); ++i) {
        // Groups where quality and length disagree, or where a copy
        // carries real rating/comment/play-count/last-played data that
        // would be silently lost, default to excluded -- see
        // DuplicateCleanupPlan::differs and ::hasUnpreservableDataAtRisk's
        // own doc comments (deliberately separate flags/reasons).
        m_included[i] = !m_plans[i].differs && !m_plans[i].hasUnpreservableDataAtRisk;
    }
    m_visibleIndices.resize(m_plans.size());
    for (size_t i = 0; i < m_plans.size(); ++i) {
        m_visibleIndices[i] = i;
    }
    endResetModel();
}

bool CleanupPlanListModel::included(size_t index) const
{
    return index < m_included.size() && m_included[index];
}

void CleanupPlanListModel::setAllIncluded(bool included)
{
    if (m_visibleIndices.empty()) {
        return;
    }
    for (size_t realIndex : m_visibleIndices) {
        m_included[realIndex] = included;
    }
    emit dataChanged(index(0), index(static_cast<int>(m_visibleIndices.size()) - 1), {IncludedRole});
}

int CleanupPlanListModel::includedCount() const
{
    return static_cast<int>(std::count(m_included.begin(), m_included.end(), true));
}

namespace
{
bool matchesQuery(const domain::Track &t, const QString &query)
{
    return QString::fromStdString(t.title).contains(query, Qt::CaseInsensitive) ||
           QString::fromStdString(t.artist).contains(query, Qt::CaseInsensitive);
}
}  // namespace

void CleanupPlanListModel::setFilter(const QString &query)
{
    beginResetModel();
    m_visibleIndices.clear();
    for (size_t i = 0; i < m_plans.size(); ++i) {
        if (query.isEmpty() || matchesQuery(m_plans[i].survivor, query) ||
            std::any_of(m_plans[i].toRemove.begin(), m_plans[i].toRemove.end(),
                        [&query](const domain::Track &t) { return matchesQuery(t, query); })) {
            m_visibleIndices.push_back(i);
        }
    }
    endResetModel();
}

void CleanupPlanListModel::removePlansAt(std::vector<int> indices)
{
    // Descending order: removing from m_plans/m_included at a higher
    // index first never disturbs the position of a not-yet-processed
    // lower index.
    std::sort(indices.rbegin(), indices.rend());
    for (int idx : indices) {
        if (idx < 0 || static_cast<size_t>(idx) >= m_plans.size()) {
            continue;
        }
        size_t rawIndex = static_cast<size_t>(idx);
        auto it = std::find(m_visibleIndices.begin(), m_visibleIndices.end(), rawIndex);
        bool wasVisible = it != m_visibleIndices.end();
        int visibleRow = wasVisible ? static_cast<int>(std::distance(m_visibleIndices.begin(), it)) : -1;

        if (wasVisible) {
            beginRemoveRows(QModelIndex(), visibleRow, visibleRow);
        }
        m_plans.erase(m_plans.begin() + idx);
        m_included.erase(m_included.begin() + idx);
        m_stagedDescriptions.erase(m_stagedDescriptions.begin() + idx);
        if (wasVisible) {
            m_visibleIndices.erase(m_visibleIndices.begin() + visibleRow);
        }
        // Every other visible-index entry pointing past the just-
        // removed raw index needs to shift down by one to stay valid,
        // whether or not the removed one was itself currently visible.
        for (auto &visIdx : m_visibleIndices) {
            if (visIdx > rawIndex) {
                visIdx--;
            }
        }
        if (wasVisible) {
            endRemoveRows();
        }
    }
}

int CleanupPlanListModel::rawIndexForRow(int row) const
{
    if (row < 0 || static_cast<size_t>(row) >= m_visibleIndices.size()) {
        return -1;
    }
    return static_cast<int>(m_visibleIndices[static_cast<size_t>(row)]);
}

void CleanupPlanListModel::setStaged(size_t rawIndex, bool staged, const QString &description)
{
    if (rawIndex >= m_plans.size()) {
        return;
    }
    m_stagedDescriptions[rawIndex] = staged ? description : QString();
    auto it = std::find(m_visibleIndices.begin(), m_visibleIndices.end(), rawIndex);
    if (it != m_visibleIndices.end()) {
        int row = static_cast<int>(std::distance(m_visibleIndices.begin(), it));
        emit dataChanged(index(row), index(row), {StagedRole, StagedDescriptionRole});
    }
}

void CleanupPlanListModel::clearStaged()
{
    std::fill(m_stagedDescriptions.begin(), m_stagedDescriptions.end(), QString());
    if (!m_visibleIndices.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_visibleIndices.size()) - 1), {StagedRole, StagedDescriptionRole});
    }
}

PendingDeletionListModel::PendingDeletionListModel(QObject *parent) : QAbstractListModel(parent) {}

int PendingDeletionListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_entries.size());
}

QVariant PendingDeletionListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_entries.size()) {
        return {};
    }
    const auto &entry = m_entries[static_cast<size_t>(index.row())];
    switch (role) {
    case FormatRole:
        return QString::fromStdString(entry.format);
    case TitleRole:
        return QString::fromStdString(entry.title);
    case ArtistRole:
        return QString::fromStdString(entry.artist);
    case FilePathRole:
        return QString::fromStdString(entry.filePath);
    case BackupIdRole:
        return QString::fromStdString(entry.backupId);
    case TimestampRole:
        return QString::fromStdString(entry.timestampUtc);
    case IncludedRole:
        return m_included[static_cast<size_t>(index.row())];
    case SizeHumanRole:
        return humanSize(entry.fileSizeBytes);
    default:
        return {};
    }
}

bool PendingDeletionListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_entries.size()) {
        return false;
    }
    if (role != IncludedRole) {
        return false;
    }
    m_included[static_cast<size_t>(index.row())] = value.toBool();
    emit dataChanged(index, index, {IncludedRole});
    return true;
}

QHash<int, QByteArray> PendingDeletionListModel::roleNames() const
{
    return {
        {FormatRole, "format"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {FilePathRole, "filePath"},
        {BackupIdRole, "backupId"},
        {TimestampRole, "timestampUtc"},
        {IncludedRole, "included"},
        {SizeHumanRole, "sizeHuman"},
    };
}

void PendingDeletionListModel::setEntries(std::vector<infrastructure::cleanup::PendingDeletion> entries)
{
    beginResetModel();
    m_entries = std::move(entries);
    m_included.assign(m_entries.size(), true);
    endResetModel();
}

std::uint64_t PendingDeletionListModel::totalBytes() const
{
    std::uint64_t total = 0;
    for (const auto &entry : m_entries) {
        total += entry.fileSizeBytes;
    }
    return total;
}

std::uint64_t PendingDeletionListModel::includedBytes() const
{
    std::uint64_t total = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (i < m_included.size() && m_included[i]) {
            total += m_entries[i].fileSizeBytes;
        }
    }
    return total;
}

std::vector<infrastructure::cleanup::PendingDeletion> PendingDeletionListModel::includedEntries() const
{
    std::vector<infrastructure::cleanup::PendingDeletion> result;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_included[i]) {
            result.push_back(m_entries[i]);
        }
    }
    return result;
}

int PendingDeletionListModel::includedCount() const
{
    return static_cast<int>(std::count(m_included.begin(), m_included.end(), true));
}

void PendingDeletionListModel::setAllIncluded(bool included)
{
    if (m_included.empty()) {
        return;
    }
    m_included.assign(m_included.size(), included);
    emit dataChanged(index(0), index(static_cast<int>(m_included.size()) - 1), {IncludedRole});
}

namespace
{

// Runs entirely on a background thread (see CleanupController::
// deleteSelectedPendingFiles()). Re-scans the library fresh, never
// trusts the manifest alone, see resolvePendingDeletions()'s own doc
// comment, deletes every `selected` entry resolvePendingDeletions()
// confirms is genuinely orphaned, and clears exactly those from the
// pending-deletions manifest. Entries still referenced are left
// untouched on disk and in the manifest either way.
PendingDeletionApplyResult runDeletePendingTask(QString format, QString path,
                                                 std::vector<infrastructure::cleanup::PendingDeletion> selected,
                                                 std::shared_ptr<QtProgressReporter> reporter,
                                                 application::CancellationToken cancel)
{
    PendingDeletionApplyResult result;
    QString refusal = refuseIfDjSoftwareRunning();
    if (!refusal.isEmpty()) {
        result.errorMessage = refusal;
        return result;
    }
    try {
        fs::path stickRoot = fs::path(path.toStdString()).parent_path();
        infrastructure::backup::StickWriteLock lock(
            (infrastructure::paths::stickBackupsDir(stickRoot) / ".write.lock").string());
        infrastructure::cleanup::PendingDeletionManifest manifest(
            infrastructure::paths::stickPendingDeletions(stickRoot).string());
        infrastructure::logging::FileOperationLog log(infrastructure::paths::stickOperationLog(stickRoot).string());

        // Every catalog on the stick, not just the one this page is
        // working in. The same audio file routinely lives in rekordbox,
        // Engine and OneLibrary at once, and a file orphaned by a
        // cleanup in one of them can still be played from the other two
        // -- deleting it on one catalog's say-so is unrecoverable.
        auto stickCatalogs = readAllStickCatalogs(path.toStdString(), *reporter, cancel);
        if (!stickCatalogs.failed.empty()) {
            result.errorMessage =
                QString("Can't safely delete: this stick has a %1 library that could not be read, so there is no "
                        "way to tell whether it still needs these files. Nothing was deleted.")
                    .arg(QString::fromStdString(stickCatalogs.failed.front()));
            return result;
        }

        const std::string deletionRoot = std::filesystem::path(path.toStdString()).parent_path().string();
        auto resolution = infrastructure::cleanup::resolvePendingDeletions(selected, stickCatalogs.catalogs, deletionRoot);
        for (const auto &entry : resolution.notOnThisStick) {
            log.record("cleanup: pending deletion is not on this stick, left alone -> " + entry.filePath);
        }
        result.total = static_cast<int>(resolution.safeToDelete.size());

        // The actual deletion (the one place in the app that
        // permanently destroys real audio file content) lives in
        // infrastructure/cleanup/pending_deletion_applier.cpp, Qt-free
        // and unit-tested there -- this just logs/formats its result.
        reporter->start("Deleting files", resolution.safeToDelete.size());
        auto outcomes = infrastructure::cleanup::applyPendingDeletions(
            resolution.safeToDelete, deletionRoot, manifest, cancel, [&reporter](size_t done) { reporter->tick(done); });
        reporter->finish();
        result.cancelled = cancel.cancelled() && outcomes.size() < resolution.safeToDelete.size();

        int deleted = 0;
        int failed = 0;
        using Status = infrastructure::cleanup::PendingDeletionOutcome::Status;
        for (const auto &outcome : outcomes) {
            switch (outcome.status) {
                case Status::AlreadyAbsent:
                    deleted++;
                    log.record("cleanup: pending deletion already absent from disk, clearing from manifest -> " +
                                outcome.entry.filePath);
                    break;
                case Status::Deleted:
                    deleted++;
                    log.record("cleanup: deleted orphaned duplicate file (backup " + outcome.entry.backupId +
                                ") -> " + outcome.entry.filePath);
                    break;
                case Status::Failed:
                    failed++;
                    log.record("cleanup: failed to delete orphaned duplicate file -> " + outcome.entry.filePath +
                                " (" + outcome.failureReason + ")");
                    break;
            }
        }

        result.deleted = deleted;
        QStringList parts;
        parts << QString("deleted %1 file(s) from disk").arg(deleted);
        if (!resolution.stillReferenced.empty()) {
            parts << QString("%1 file(s) still referenced by a current track, left alone, not deleted")
                         .arg(static_cast<int>(resolution.stillReferenced.size()));
        }
        if (failed > 0) {
            parts << QString("%1 failed to delete").arg(failed);
        }
        result.statusMessage = parts.join("; ");
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

CleanupTaskResult runRescanTask(QString format, QString path, QString playlistName, QString searchQuery,
                                 std::shared_ptr<QtProgressReporter> reporter,
                                application::CancellationToken cancel)
{
    CleanupTaskResult result;
    try {
        std::vector<domain::Track> tracks =
            LibraryCatalogCache::instance().tracksFor(format.toStdString(), path.toStdString(), *reporter, cancel);

        // Streaming tracks (Engine/TIDAL) have no real local file.
        // Never let duplicate detection consider one, whether as
        // survivor or doomed. See domain::Track::streamingSource's own
        // doc comment for why.
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                     [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                     tracks.end());

        // Audio files no catalog references join the same review, so a
        // stray copy of a track is deduplicated with the same survivor
        // and merge information in front of the DJ as every other copy,
        // rather than being met for the first time on the page that
        // deletes things. Which catalogs it was subtracted from is the
        // whole basis of the answer, so it comes back with it.
        //
        // Read against EVERY catalog on the stick, not the one this page
        // works in: a file rekordbox has forgotten can still be playable
        // from Engine, and on a real stick that difference was 307
        // files. See readAllStickCatalogs()'s own comment.
        auto stickCatalogs = readAllStickCatalogs(path.toStdString(), *reporter, cancel);

        // A file, not a row, is the unit of duplication: the same audio is
        // listed by rekordbox, Engine and OneLibrary at once, and removing
        // a copy means removing every catalog's row for it. Folding rows
        // into files here is what lets one save do that -- see
        // CleanupGroupChange::apply(), which writes each catalog the
        // collapsed file names.
        //
        // The decision, including the refusal to collapse when a catalog
        // could not be read, lives in collapseForCleanupScan() where it
        // can be tested. Before scoping and before the strays join, never
        // after: see collapseCatalogRows()'s header.
        {
            auto scanRows = application::collapseForCleanupScan(stickCatalogs.catalogs, stickCatalogs.failed,
                                                                 std::move(tracks));
            tracks = std::move(scanRows.rows);
            result.collapsedAcrossCatalogs = scanRows.collapsedAcrossCatalogs;
        }

        auto strays = infrastructure::cleanup::scanStrayFiles(
            fs::path(path.toStdString()).parent_path().string(), stickCatalogs.catalogs, stickCatalogs.failed, cancel);

        result.strays.filesFound = static_cast<int>(strays.filesFound);
        result.strays.bytesFound = static_cast<qulonglong>(strays.bytesFound);
        result.strays.unreadable = static_cast<int>(strays.unreadable);
        result.strays.walkIncomplete = strays.walkIncomplete;
        result.strays.probeAvailable = strays.metadataProbeAvailable;
        result.strays.usable = strays.usable;
        result.strays.refusal = QString::fromStdString(strays.refusal);
        for (const auto &name : strays.catalogsConsulted) {
            result.strays.catalogsConsulted << QString::fromStdString(name);
        }
        tracks.insert(tracks.end(), strays.tracks.begin(), strays.tracks.end());

        // Every playlist the unscoped library knows about, so the
        // picker still offers the others after one is chosen.
        {
            std::set<QString> names;
            for (const auto &track : tracks) {
                for (const auto &membership : track.playlists) {
                    names.insert(QString::fromStdString(membership.name));
                }
            }
            for (const auto &name : names) {
                result.playlistNames << name;
            }
        }

        // Scope last, once the stray files are in: a playlist or a
        // search names tracks, and a stray copy of a scoped track has to
        // be reachable by the same name, or scoping would quietly hide
        // exactly the copies this page exists to find. Applied after the
        // list is whole, never to the catalogs on the way in -- see
        // collapseCatalogRows()'s header for why narrowing before the
        // parts of a file are together is the one order that breaks.
        if (!playlistName.isEmpty()) {
            tracks = domain::filterByScope(tracks, domain::TrackScope::playlist(playlistName.toStdString()));
        }
        if (!searchQuery.isEmpty()) {
            tracks = domain::filterByScope(tracks, domain::TrackScope::search(searchQuery.toStdString()));
        }

        std::vector<domain::DuplicateCleanupPlan> plans;
        for (const auto &group : domain::DuplicateTrackFinder::find(tracks)) {
            auto plan = domain::DuplicateCleanupPlanner::plan(group);
            if (plan.toRemove.empty()) {
                continue;
            }
            // Not offered at all, rather than offered-but-unchecked like
            // `differs` and `hasUnpreservableDataAtRisk`. Those two are
            // judgement calls a DJ may overrule; this one has no correct
            // way to apply yet -- the doomed row lives in a catalog the
            // survivor has no row in, so removing it takes the recording
            // out of that catalog rather than deduplicating it. The
            // writer refuses these too, but by then the DJ has already
            // chosen them and the save fails; better never to offer.
            if (plan.wouldStrandAFormat) {
                ++result.groupsHeldBackStranding;
                continue;
            }
            plans.push_back(std::move(plan));
        }
        result.plans = std::move(plans);

        // Sizes from the stick, not from a catalog. Two of the three
        // formats record no file size at all, so every "space saved"
        // figure derived from them was structurally zero -- the same
        // library reported 0 GB scanned one way and 6.35 GB scanned
        // another. Rewrites Track::fileSizeBytes on the copies these
        // plans would remove, which is what every figure the page shows
        // is derived from, so one correction makes all of them true.
        //
        // After planning on purpose: the planner weighs file size when
        // choosing a survivor, and giving it real numbers would change
        // which copy is kept. That is very likely an improvement and it
        // is not this change.
        result.sizes = application::measureRealFileSizes(result.plans);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

// Runs entirely on a background thread (see CleanupController::
// planManualMerge()). Unlike runRescanTask(), never runs
// DuplicateTrackFinder. The two tracks are already a user-declared
// match, not something to (re-)detect. Re-scans fresh rather than
// trusting whatever ScanPage had in memory when the merge was requested,
// same "never trust stale data right before a mutating decision" stance
// every other write path in this codebase already takes.
CleanupTaskResult runManualMergeTask(QString format, QString path, QString sourceIdA, QString sourceIdB,
                                      std::shared_ptr<QtProgressReporter> reporter,
                                      application::CancellationToken cancel)
{
    CleanupTaskResult result;
    try {
        std::vector<domain::Track> tracks =
            LibraryCatalogCache::instance().tracksFor(format.toStdString(), path.toStdString(), *reporter, cancel);

        std::string idA = sourceIdA.toStdString();
        std::string idB = sourceIdB.toStdString();
        const domain::Track *trackA = nullptr;
        const domain::Track *trackB = nullptr;
        for (const auto &t : tracks) {
            if (t.sourceId == idA) {
                trackA = &t;
            } else if (t.sourceId == idB) {
                trackB = &t;
            }
        }
        if (!trackA || !trackB) {
            result.errorMessage = "One or both tracks no longer exist in this library. Rescan and try again.";
            return result;
        }
        if (!trackA->streamingSource.empty() || !trackB->streamingSource.empty()) {
            result.errorMessage = "A streaming track (no local file) can't be merged.";
            return result;
        }

        domain::DuplicateGroup group;
        group.tracks = {*trackA, *trackB};
        result.plans = {domain::DuplicateCleanupPlanner::plan(group)};
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

CleanupController::CleanupController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<CleanupTaskResult>::finished, this, &CleanupController::onRescanFinished);
    connect(&m_pendingWriteWatcher, &QFutureWatcher<PendingDeletionApplyResult>::finished, this,
            &CleanupController::onDeletePendingFinished);
}

QString CleanupController::totalWastedBytesHuman() const
{
    return humanSize(static_cast<std::uint64_t>(totalWastedBytes()));
}

qlonglong CleanupController::totalWastedBytes() const
{
    std::uint64_t total = 0;
    for (const auto &plan : m_model.plans()) {
        total += wastedBytes(plan);
    }
    return static_cast<qlonglong>(total);
}

qlonglong CleanupController::includedWastedBytes() const
{
    std::uint64_t total = 0;
    const auto &plans = m_model.plans();
    for (size_t i = 0; i < plans.size(); ++i) {
        if (m_model.included(i)) {
            total += wastedBytes(plans[i]);
        }
    }
    return static_cast<qlonglong>(total);
}

namespace
{
// fs::space() on the stick the library lives on. Reported as 0/0 when it
// cannot be read, which the page treats as "unknown" -- a stick that is
// unplugged mid-scan must not render as a disk with nothing on it.
std::pair<qlonglong, qlonglong> stickSpace(const QString &libraryPath)
{
    if (libraryPath.isEmpty()) {
        return {0, 0};
    }
    std::error_code ec;
    const auto info = fs::space(fs::path(libraryPath.toStdString()), ec);
    if (ec || info.capacity == 0 || info.capacity == static_cast<std::uintmax_t>(-1)) {
        return {0, 0};
    }
    return {static_cast<qlonglong>(info.capacity), static_cast<qlonglong>(info.available)};
}
}  // namespace

qlonglong CleanupController::stickTotalBytes() const
{
    return stickSpace(m_path).first;
}

qlonglong CleanupController::stickFreeBytes() const
{
    return stickSpace(m_path).second;
}

void CleanupController::scan(const QString &format, const QString &path, const QString &playlistName,
                              const QString &searchQuery)
{
    m_format = format;
    m_path = path;
    m_playlistName = playlistName;
    m_searchQuery = searchQuery;
    attachSession();
    rescan();
}

bool CleanupController::hasOneLibrary(const QString &pioneerRoot) const
{
    return infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot.toStdString());
}

void CleanupController::loadPendingDeletionsOnly(const QString &format, const QString &path)
{
    m_format = format;
    m_path = path;
    attachSession();
    refreshPendingDeletions();
}

void CleanupController::planManualMerge(const QString &format, const QString &path, const QString &sourceIdA,
                                         const QString &sourceIdB)
{
    if (m_busy) {
        return;
    }
    m_format = format;
    m_path = path;
    attachSession();
    setErrorMessage({});
    // Also cleared here (unlike rescan(), which never needs to): a
    // second merge in the same page session must not have the previous
    // one's leftover success message hide this new plan's own preview
    // (see ScanPage.qml's Repeater, gated on statusMessage being empty).
    setStatusMessage({});
    setScanProgress(0, 0);
    setBusy(true);

    m_scanCancel = application::CancellationToken();
    m_watcher.setFuture(
        QtConcurrent::run(runManualMergeTask, format, path, sourceIdA, sourceIdB, makeReporter(), m_scanCancel));
}

void CleanupController::rescan()
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setScanProgress(0, 0);
    setBusy(true);

    m_scanCancel = application::CancellationToken();
    m_watcher.setFuture(QtConcurrent::run(runRescanTask, m_format, m_path, m_playlistName, m_searchQuery,
                                          makeReporter(), m_scanCancel));
}

void CleanupController::cancelScan()
{
    if (scanCancellable()) {
        m_scanCancel.cancel();
    }
}

void CleanupController::setIncluded(int index, bool included)
{
    m_model.setData(m_model.index(index), included, CleanupPlanListModel::IncludedRole);
    emit includedChanged();
}

void CleanupController::setAllIncluded(bool included)
{
    m_model.setAllIncluded(included);
    emit includedChanged();
}

void CleanupController::search(const QString &query)
{
    m_model.setFilter(query);
}

std::shared_ptr<QtProgressReporter> CleanupController::makeReporter()
{
    auto reporter = std::make_shared<QtProgressReporter>();
    connect(reporter.get(), &QtProgressReporter::started, this,
            [this](const QString &, int total) { setScanProgress(0, total); });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setScanProgress(current, m_scanTotal); });
    return reporter;
}

void CleanupController::onRescanFinished()
{
    CleanupTaskResult result = m_watcher.result();

    if (result.cancelled) {
        setBusy(false);
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        setBusy(false);
        return;
    }

    m_strays = result.strays;
    m_playlistNames = result.playlistNames;
    m_model.setPlans(std::move(result.plans));
    // Groups staged before this rescan keep their mark if they are still
    // listed (the change itself lives in the session).
    for (const auto &[survivorId, info] : m_stagedBySurvivor) {
        int index = indexOfSurvivor(survivorId);
        if (index >= 0) {
            m_model.setStaged(static_cast<size_t>(index), true, info.description);
        }
    }
    setBusy(false);
    emit plansChanged();
    emit includedChanged();
    refreshPendingDeletions();
}

QVariantMap CleanupController::unreferencedFiles() const
{
    QVariantMap m;
    m["filesFound"] = m_strays.filesFound;
    m["bytesHuman"] = humanSize(m_strays.bytesFound);
    m["unreadable"] = m_strays.unreadable;
    m["catalogsConsulted"] = m_strays.catalogsConsulted;
    m["walkIncomplete"] = m_strays.walkIncomplete;
    m["probeAvailable"] = m_strays.probeAvailable;
    m["usable"] = m_strays.usable;
    m["refusal"] = m_strays.refusal;
    return m;
}

bool CleanupController::writing() const
{
    return m_writing || (m_session && m_session->writing());
}

bool CleanupController::canUndo() const
{
    return m_session && m_session->canUndo();
}

int CleanupController::indexOfSurvivor(const std::string &survivorSourceId) const
{
    const auto &plans = m_model.plans();
    for (size_t i = 0; i < plans.size(); ++i) {
        if (plans[i].survivor.sourceId == survivorSourceId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void CleanupController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(m_path));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::stateChanged, this, &CleanupController::writingChanged);
            connect(m_session, &LibraryEditSession::canUndoChanged, this, &CleanupController::canUndoChanged);
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                if (changeId == QStringLiteral("undo:last-save")) {
                    rescan();  // prior file bytes are back; the plan list is stale
                    return;
                }
                for (auto it = m_stagedBySurvivor.begin(); it != m_stagedBySurvivor.end(); ++it) {
                    if (it->second.changeId == changeId) {
                        // That group is merged and gone. DuplicateTrackFinder
                        // groups by filename/title+artist+duration only, never
                        // cues or row existence, so removing it can't change
                        // any other group's own classification.
                        int index = indexOfSurvivor(it->first);
                        m_stagedBySurvivor.erase(it);
                        if (index >= 0) {
                            m_model.removePlansAt({index});
                        }
                        emit plansChanged();
                        emit includedChanged();
                        // The doomed copies were appended to the pending-
                        // deletion manifest as a real side effect.
                        refreshPendingDeletions();
                        break;
                    }
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                m_stagedBySurvivor.clear();
                m_model.clearStaged();
                emit plansChanged();
                emit includedChanged();
            });
        }
    }
    if (m_session) {
        if (m_format == "engine") {
            m_session->setLibraryPaths(QString(), m_path);
        } else {
            m_session->setLibraryPaths(m_path, QString());
        }
    }
}

// How many database writes this save may make (drives the scratch-copy
// decision): every listed plan's row removals, cue merges and field
// propagations, the most that could be staged. Same tally the old
// all-in-one task used, over every plan rather than the included ones.
int CleanupController::cleanupItemCountHint() const
{
    int count = 0;
    for (const auto &plan : m_model.plans()) {
        // Catalogued removals only. This hint is what
        // shouldUseWholeFileReplace() weighs a whole-database copy
        // against, so it has to mean "database writes coming" -- and a
        // stray file's removal is a line in a manifest, not a row. On a
        // real stick counting them added 632 phantom writes, which would
        // push a two-group cleanup onto the scratch-copy path.
        count += static_cast<int>(plan.toRemove.size()) - static_cast<int>(plan.unreferencedFilesToDelete.size())
            - static_cast<int>(plan.unreferencedFilesHeldBack.size());
        if (plan.mergedCuesForSurvivor.size() > plan.survivor.cues.size()) {
            count++;
        }
        if (plan.bpmForSurvivor || plan.keyForSurvivor || plan.artworkPathForSurvivor) {
            count++;
        }
    }
    return count;
}

void CleanupController::stagePlan(size_t rawIndex)
{
    const auto &plans = m_model.plans();
    if (rawIndex >= plans.size()) {
        return;
    }
    if (!m_session) {
        attachSession();
        if (!m_session) {
            setErrorMessage("This stick's library could not be identified; nothing was changed.");
            return;
        }
    }
    if (writing()) {
        setErrorMessage("A write is running -- stage more once it has finished.");
        return;
    }
    const auto &plan = plans[rawIndex];
    auto change = std::make_unique<CleanupGroupChange>(m_format, m_path, plan, cleanupItemCountHint());
    QString changeId = change->id();
    QString description = change->description();
    if (!m_session->stage(std::move(change))) {
        return;  // the session reported the lock refusal; the page shows it
    }
    m_stagedBySurvivor[plan.survivor.sourceId] = {changeId, description};
    m_model.setStaged(rawIndex, true, description);
    emit plansChanged();
}

// Stages every currently-included group; the page's Save writes them.
// Does NOT delete any audio file, see the class comment.
void CleanupController::apply()
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    int staged = 0;
    const size_t count = m_model.plans().size();
    for (size_t i = 0; i < count; ++i) {
        if (!m_model.included(i) || m_stagedBySurvivor.count(m_model.plans()[i].survivor.sourceId)) {
            continue;
        }
        stagePlan(i);
        staged++;
        if (m_session && !m_session->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
    if (staged > 0) {
        setStatusMessage(QStringLiteral("Staged %1 group(s). Press Save to clean them up on the stick.").arg(staged));
    }
}

void CleanupController::unstageAll()
{
    if (m_stagedBySurvivor.empty()) {
        return;
    }
    // Walks the staged map rather than the model rows: a row may be
    // filtered out of view by the search box while still staged, and
    // leaving those behind would be the opposite of what Escape means.
    for (const auto &[survivorId, staged] : m_stagedBySurvivor) {
        if (m_session) {
            m_session->unstage(staged.changeId);
        }
    }
    m_stagedBySurvivor.clear();
    for (std::size_t i = 0; i < m_model.plans().size(); ++i) {
        m_model.setStaged(i, false, QString());
    }
    emit plansChanged();
}

void CleanupController::unstage(int row)
{
    int rawIndex = m_model.rawIndexForRow(row);
    if (rawIndex < 0) {
        return;
    }
    auto it = m_stagedBySurvivor.find(m_model.plans()[static_cast<size_t>(rawIndex)].survivor.sourceId);
    if (it == m_stagedBySurvivor.end()) {
        return;
    }
    if (m_session) {
        m_session->unstage(it->second.changeId);
    }
    m_stagedBySurvivor.erase(it);
    m_model.setStaged(static_cast<size_t>(rawIndex), false, QString());
    emit plansChanged();
}

void CleanupController::undoLastOperation()
{
    if (m_busy || !m_session) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_session->undoLastSave();
}

void CleanupController::refreshPendingDeletions()
{
    if (m_path.isEmpty()) {
        return;
    }
    fs::path stickRoot = fs::path(m_path.toStdString()).parent_path();
    infrastructure::cleanup::PendingDeletionManifest manifest(
        infrastructure::paths::stickPendingDeletions(stickRoot).string());

    // rekordbox and Engine each accumulate their own separate pending
    // entries (see PendingDeletion::format). This page only ever shows
    // the one currently selected via the format toggle, same as
    // everything else on it.
    std::vector<infrastructure::cleanup::PendingDeletion> filtered;
    for (auto &entry : manifest.list()) {
        if (entry.format != m_format.toStdString()) {
            continue;
        }
        // fileSizeBytes is never persisted in the manifest (see its own
        // doc comment). Stat it fresh here so "how much space would
        // deleting this free up" reflects the file's real current size,
        // not a guess. 0 if the file's already gone; still worth listing
        // (deleteSelectedPendingFiles() clears an already-absent entry
        // from the manifest instead of erroring).
        std::error_code ec;
        entry.fileSizeBytes = fs::file_size(entry.filePath, ec);
        if (ec) {
            entry.fileSizeBytes = 0;
        }
        filtered.push_back(std::move(entry));
    }
    m_pendingModel.setEntries(std::move(filtered));
    emit pendingDeletionsChanged();
}

QString CleanupController::totalPendingBytesHuman() const
{
    return humanSize(m_pendingModel.totalBytes());
}

QString CleanupController::includedPendingBytesHuman() const
{
    return humanSize(m_pendingModel.includedBytes());
}

void CleanupController::setPendingDeletionIncluded(int index, bool included)
{
    m_pendingModel.setData(m_pendingModel.index(index), included, PendingDeletionListModel::IncludedRole);
    emit pendingDeletionsChanged();
}

void CleanupController::setAllPendingDeletionIncluded(bool included)
{
    m_pendingModel.setAllIncluded(included);
    emit pendingDeletionsChanged();
}

void CleanupController::deleteSelectedPendingFiles()
{
    if (m_busy) {
        return;
    }
    auto selected = m_pendingModel.includedEntries();
    if (selected.empty()) {
        return;
    }

    // A direct write on the library: same lock the staged edits take,
    // held for exactly this run.
    auto *registry = EditSessionRegistry::instance();
    const QString libraryId = registry->libraryIdForPath(m_path);
    if (auto refusal = registry->enterDirectWrite(libraryId, QString())) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder);
        }
        return;
    }
    m_holdsDirectWrite = true;

    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    m_pendingDeleteCancel = application::CancellationToken();
    setBusy(true);
    setWriting(true);

    m_pendingWriteWatcher.setFuture(QtConcurrent::run(runDeletePendingTask, m_format, m_path, std::move(selected),
                                                      makeReporter(), m_pendingDeleteCancel));
}

void CleanupController::cancelWrite()
{
    if (!writeCancellable()) {
        return;
    }
    m_pendingDeleteCancel.cancel();
    emit writingChanged();  // writeCancellable flipped
}

void CleanupController::onDeletePendingFinished()
{
    PendingDeletionApplyResult result = m_pendingWriteWatcher.result();
    if (m_holdsDirectWrite) {
        m_holdsDirectWrite = false;
        EditSessionRegistry::instance()->leaveDirectWrite(EditSessionRegistry::instance()->libraryIdForPath(m_path));
    }

    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
    } else {
        setStatusMessage(result.statusMessage);
    }
    setBusy(false);
    setWriting(false);
    refreshPendingDeletions();
    emit pendingDeletionsWriteFinished(QVariantMap{
        {"written", result.deleted},
        {"total", result.total},
        {"unit", QStringLiteral("files")},
        {"verb", QStringLiteral("deleted")},
        {"cancelled", result.cancelled},
        {"error", result.errorMessage},
    });
}

void CleanupController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void CleanupController::setWriting(bool writing)
{
    if (m_writing == writing) {
        return;
    }
    m_writing = writing;
    emit writingChanged();
}

void CleanupController::setScanProgress(int current, int total)
{
    if (m_scanCurrent == current && m_scanTotal == total) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    emit scanProgressChanged();
}

void CleanupController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void CleanupController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
