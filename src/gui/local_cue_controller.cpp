// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "local_cue_controller.hpp"

#include <QStringList>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <unordered_map>

#include "application/ports/cue_writer.hpp"
#include "domain/track_matching.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/onelibrary_cue_writer_adapter.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/local/local_cue_store.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/merge_cues_change.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using domain::RestoreCandidate;

namespace
{

QString describeCues(const std::vector<domain::CuePoint> &cues)
{
    int hot = 0;
    int memory = 0;
    for (const auto &cue : cues) {
        (cue.kind == domain::CuePoint::Kind::Hot ? hot : memory)++;
    }
    QString result = QString("%1 hot").arg(hot);
    if (memory > 0) {
        result += QString(", %1 memory").arg(memory);
    }
    return result;
}

// path is the PIONEER root for both "rekordbox" and "onelibrary" --
// exportLibrary.db lives alongside export.pdb (see LocalCuePage.qml's
// currentPath()), same convention LibraryCatalogCache's own callers
// elsewhere already use.
std::vector<domain::Track> scanStick(const QString &format, const QString &path,
                                      std::shared_ptr<QtProgressReporter> reporter,
                                      application::CancellationToken cancel = application::CancellationToken::none())
{
    return LibraryCatalogCache::instance().tracksFor(format.toStdString(), path.toStdString(), *reporter, cancel);
}

}  // namespace

RestoreCandidateListModel::RestoreCandidateListModel(QObject *parent) : QAbstractListModel(parent) {}

int RestoreCandidateListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_candidates.size());
}

QVariant RestoreCandidateListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_candidates.size()) {
        return {};
    }
    const auto &candidate = m_candidates[static_cast<size_t>(index.row())];
    switch (role) {
    case FilenameRole:
        return QString::fromStdString(candidate.stickTrack.filename);
    case TitleRole:
        return QString::fromStdString(candidate.stickTrack.title);
    case ArtistRole:
        return QString::fromStdString(candidate.stickTrack.artist);
    case DescriptionRole: {
        // mergeCues() appends additions after the stick's own (unchanged)
        // cues, in order -- so everything past that boundary is exactly
        // what this candidate would add.
        auto existingCount = candidate.stickTrack.cues.size();
        std::vector<domain::CuePoint> added(candidate.mergedCues.begin() +
                                                 static_cast<std::ptrdiff_t>(existingCount),
                                             candidate.mergedCues.end());
        return describeCues(added);
    }
    case StagedRole:
        return static_cast<size_t>(index.row()) < m_staged.size() && m_staged[static_cast<size_t>(index.row())];
    default:
        return {};
    }
}

QHash<int, QByteArray> RestoreCandidateListModel::roleNames() const
{
    return {
        {FilenameRole, "filename"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {DescriptionRole, "description"},
        {StagedRole, "staged"},
    };
}

void RestoreCandidateListModel::setCandidates(std::vector<domain::RestoreCandidate> candidates)
{
    beginResetModel();
    m_candidates = std::move(candidates);
    m_staged.assign(m_candidates.size(), false);
    endResetModel();
}

void RestoreCandidateListModel::removeCandidateAt(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_candidates.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), index, index);
    m_candidates.erase(m_candidates.begin() + index);
    m_staged.erase(m_staged.begin() + index);
    endRemoveRows();
}

void RestoreCandidateListModel::setStaged(int index, bool staged)
{
    if (index < 0 || static_cast<size_t>(index) >= m_candidates.size()) {
        return;
    }
    m_staged[static_cast<size_t>(index)] = staged;
    emit dataChanged(this->index(index), this->index(index), {StagedRole});
}

void RestoreCandidateListModel::clearStaged()
{
    if (m_candidates.empty()) {
        return;
    }
    std::fill(m_staged.begin(), m_staged.end(), false);
    emit dataChanged(index(0), index(static_cast<int>(m_candidates.size()) - 1), {StagedRole});
}

namespace
{

// Backs up one format's side of the stick into the local store -- shared
// by both branches of runBackupTask() below.
int backupOneFormat(const QString &format, const QString &path, const QString &stickLabel,
                     const QString &description, std::shared_ptr<QtProgressReporter> reporter)
{
    auto tracks = scanStick(format, path, reporter);
    int withCues = 0;
    for (const auto &track : tracks) {
        if (!track.cues.empty()) {
            withCues++;
        }
    }

    infrastructure::local::LocalCueStore store;
    store.upsert(tracks, format.toStdString(), stickLabel.toStdString());
    store.createSnapshot(tracks, format.toStdString(), stickLabel.toStdString(), description.toStdString());
    return withCues;
}

// Runs entirely on a background thread -- no access to the controller.
// Backs up whichever of rekordboxPath/enginePath/oneLibraryPath is
// non-empty, so a stick with more than one format gets all of them
// backed up from a single "Backup Now" click rather than requiring the
// user to switch formats and click once per format. oneLibraryPath is
// only ever rekordboxPath's own PIONEER root re-passed (exportLibrary.db
// lives alongside export.pdb there) -- passed as a separate, possibly-
// empty argument rather than derived here so the caller's own
// OneLibraryCueWriter::existsFor() check (needs a real file check, not
// just "is rekordboxPath set") decides whether it's actually present.
LocalCueTaskResult runBackupTask(QString stickLabel, QString description, QString rekordboxPath, QString enginePath,
                                  QString oneLibraryPath, std::shared_ptr<QtProgressReporter> reporter)
{
    LocalCueTaskResult result;
    try {
        if (!rekordboxPath.isEmpty()) {
            result.tracksAffectedRekordbox = backupOneFormat("rekordbox", rekordboxPath, stickLabel, description, reporter);
        }
        if (!enginePath.isEmpty()) {
            result.tracksAffectedEngine = backupOneFormat("engine", enginePath, stickLabel, description, reporter);
        }
        if (!oneLibraryPath.isEmpty()) {
            result.tracksAffectedOneLibrary =
                backupOneFormat("onelibrary", oneLibraryPath, stickLabel, description, reporter);
        }
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

LocalCueTaskResult runAnalyzeRestoreTask(QString format, QString path, std::shared_ptr<QtProgressReporter> reporter,
                                         application::CancellationToken cancel)
{
    LocalCueTaskResult result;
    try {
        auto stickTracks = scanStick(format, path, reporter, cancel);

        infrastructure::local::LocalCueStore store;
        auto localTracks = store.readAll();

        auto matches = domain::matchTracks(stickTracks, localTracks);
        result.candidates = domain::LocalRestorePlanner::plan(matches);
        result.stickTrackCount = static_cast<int>(stickTracks.size());
        result.localTrackCount = static_cast<int>(localTracks.size());
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

LocalCueTaskResult runAnalyzeSnapshotRestoreTask(qint64 snapshotId, QString format, QString path,
                                                  std::shared_ptr<QtProgressReporter> reporter,
                                                  application::CancellationToken cancel)
{
    LocalCueTaskResult result;
    try {
        auto stickTracks = scanStick(format, path, reporter, cancel);

        infrastructure::local::LocalCueStore store;
        auto snapshotTracks = store.readSnapshot(snapshotId);

        auto matches = domain::matchTracks(stickTracks, snapshotTracks);
        result.candidates = domain::LocalRestorePlanner::plan(matches);
        result.stickTrackCount = static_cast<int>(stickTracks.size());
        result.localTrackCount = static_cast<int>(snapshotTracks.size());
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

LocalCueController::LocalCueController(QObject *parent) : QObject(parent)
{
    connect(&m_backupWatcher, &QFutureWatcher<LocalCueTaskResult>::finished, this,
            &LocalCueController::onBackupFinished);
    connect(&m_analyzeWatcher, &QFutureWatcher<LocalCueTaskResult>::finished, this,
            &LocalCueController::onAnalyzeFinished);
}

void LocalCueController::backupToComputer(const QString &stickLabel, const QString &description,
                                           const QString &rekordboxPath, const QString &enginePath,
                                           const QString &oneLibraryPath)
{
    if (m_busy) {
        emit actionFeedback("Still busy with another operation on this stick -- try again once it finishes.", true);
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    setBusy(true);

    // oneLibraryPath is rekordboxPath itself, re-passed only when the
    // caller has confirmed exportLibrary.db actually exists there (see
    // OneLibraryCueWriter::existsFor()) -- runBackupTask() only backs it
    // up when this is non-empty.
    m_backupWatcher.setFuture(QtConcurrent::run(runBackupTask, stickLabel, description, rekordboxPath, enginePath,
                                                 oneLibraryPath, makeReporter()));
}

void LocalCueController::onBackupFinished()
{
    LocalCueTaskResult result = m_backupWatcher.result();

    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        emit actionFeedback(result.errorMessage, true);
    } else {
        QStringList parts;
        if (result.tracksAffectedRekordbox >= 0) {
            parts << QString("%1 track(s) (Rekordbox)").arg(result.tracksAffectedRekordbox);
        }
        if (result.tracksAffectedEngine >= 0) {
            parts << QString("%1 track(s) (Engine)").arg(result.tracksAffectedEngine);
        }
        if (result.tracksAffectedOneLibrary >= 0) {
            parts << QString("%1 track(s) (OneLibrary)").arg(result.tracksAffectedOneLibrary);
        }
        QString message = QString("Backed up cues to this computer: %1").arg(parts.join(", "));
        setStatusMessage(message);
        emit actionFeedback(message, false);
    }
    setBusy(false);
}

void LocalCueController::analyzeRestore(const QString &format, const QString &path, bool reportFeedback)
{
    if (m_busy) {
        if (reportFeedback) {
            emit actionFeedback("Still busy with another operation on this stick -- try again once it finishes.",
                                 true);
        }
        return;
    }
    m_analyzeReportsFeedback = reportFeedback;
    m_format = format;
    m_path = path;
    attachSession();
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    setBusy(true);

    m_scanCancel = application::CancellationToken();
    m_analyzeWatcher.setFuture(QtConcurrent::run(runAnalyzeRestoreTask, format, path, makeReporter(), m_scanCancel));
}

void LocalCueController::cancelScan()
{
    if (scanCancellable()) {
        m_scanCancel.cancel();
    }
}

void LocalCueController::analyzeSnapshotRestore(qint64 snapshotId, const QString &format, const QString &path)
{
    if (m_busy) {
        emit actionFeedback("Still busy with another operation on this stick -- try again once it finishes.", true);
        return;
    }
    m_analyzeReportsFeedback = true;
    m_format = format;
    m_path = path;
    attachSession();
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    setBusy(true);

    m_scanCancel = application::CancellationToken();
    m_analyzeWatcher.setFuture(
        QtConcurrent::run(runAnalyzeSnapshotRestoreTask, snapshotId, format, path, makeReporter(), m_scanCancel));
}

// See ScanController::scan() for why the reporter is owned by the task
// (via shared_ptr) rather than by this controller.
std::shared_ptr<QtProgressReporter> LocalCueController::makeReporter()
{
    auto reporter = std::make_shared<QtProgressReporter>();
    connect(reporter.get(), &QtProgressReporter::started, this,
            [this](const QString &, int total) { setScanProgress(0, total); });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setScanProgress(current, m_scanTotal); });
    return reporter;
}

QVariantList LocalCueController::listSnapshots()
{
    QVariantList result;
    try {
        infrastructure::local::LocalCueStore store;
        for (const auto &summary : store.listSnapshots()) {
            QVariantMap m;
            m["id"] = static_cast<qint64>(summary.id);
            m["createdAt"] = QString::fromStdString(summary.createdAt);
            m["stickLabel"] = QString::fromStdString(summary.stickLabel);
            m["sourceFormat"] = QString::fromStdString(summary.sourceFormat);
            m["description"] = QString::fromStdString(summary.description);
            m["trackCount"] = summary.trackCount;
            m["cueCount"] = summary.cueCount;
            m["uncompressedSizeBytes"] = static_cast<qulonglong>(summary.uncompressedSizeBytes);
            m["compressedSizeBytes"] = static_cast<qulonglong>(summary.compressedSizeBytes);
            m["schemaVersion"] = summary.schemaVersion;
            result << m;
        }
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromStdString(e.what()));
        emit actionFeedback(QString::fromStdString(e.what()), true);
    }
    return result;
}

void LocalCueController::setSnapshotDescription(qint64 id, const QString &description)
{
    try {
        infrastructure::local::LocalCueStore store;
        store.setSnapshotDescription(id, description.toStdString());
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromStdString(e.what()));
        emit actionFeedback(QString::fromStdString(e.what()), true);
    }
}

bool LocalCueController::deleteSnapshot(qint64 id)
{
    try {
        infrastructure::local::LocalCueStore store;
        return store.deleteSnapshot(id);
    } catch (const std::exception &e) {
        setErrorMessage(QString::fromStdString(e.what()));
        emit actionFeedback(QString::fromStdString(e.what()), true);
        return false;
    }
}

bool LocalCueController::hasOneLibrary(const QString &pioneerRoot) const
{
    return infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot.toStdString());
}

void LocalCueController::onAnalyzeFinished()
{
    LocalCueTaskResult result = m_analyzeWatcher.result();
    bool reportFeedback = m_analyzeReportsFeedback;
    m_analyzeReportsFeedback = false;

    if (result.cancelled) {
        setBusy(false);
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        if (reportFeedback) {
            emit actionFeedback(result.errorMessage, true);
        }
        setBusy(false);
        return;
    }

    m_stickTrackCount = result.stickTrackCount;
    m_localTrackCount = result.localTrackCount;
    int candidateCount = static_cast<int>(result.candidates.size());
    m_model.setCandidates(std::move(result.candidates));
    // Candidates staged before this re-analyze keep their mark if they
    // are still listed (the change itself lives in the session).
    for (const auto &[sourceId, changeId] : m_stagedBySourceId) {
        int index = indexOfSourceId(sourceId);
        if (index >= 0) {
            m_model.setStaged(index, true);
        }
    }
    emit analysisChanged();
    // Otherwise the only feedback after a real, sometimes multi-second
    // scan (see BusyOverlay's "Analyzing backups...") was the restore
    // candidate list quietly changing -- easy to read as "nothing
    // happened" rather than "here's what I found", especially right
    // after clicking a specific snapshot's "Restore From Here" (whose
    // result lands in the third section down, off the part of the page
    // that button click was in). Only for a user-initiated analyze
    // (reportFeedback) -- the automatic re-scan onWriteFinished() runs
    // right after a write already gets its own actionFeedback for the
    // write itself, and a page-load/format-switch scan was never asked
    // for in the first place.
    if (reportFeedback) {
        if (candidateCount > 0) {
            emit actionFeedback(
                QString("Found %1 track(s) with cues this backup can add -- review the list below, then "
                        "\"Merge Onto %1 Track(s)\" to apply.")
                    .arg(candidateCount),
                false);
        } else {
            emit actionFeedback("Nothing to merge: either the stick already has every cue this backup offers, "
                                 "or none of its tracks match one backed up on this computer.",
                                 false);
        }
    }
    setBusy(false);
}

bool LocalCueController::writing() const
{
    return m_session && m_session->writing();
}

bool LocalCueController::canUndo() const
{
    return m_session && m_session->canUndo();
}

int LocalCueController::indexOfSourceId(const std::string &sourceId) const
{
    const auto &candidates = m_model.candidates();
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].stickTrack.sourceId == sourceId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LocalCueController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(m_path));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::stateChanged, this, &LocalCueController::writingChanged);
            connect(m_session, &LibraryEditSession::canUndoChanged, this, &LocalCueController::canUndoChanged);
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                if (changeId == QStringLiteral("undo:last-save")) {
                    // Prior file bytes are back; the candidate list is stale.
                    analyzeRestore(m_format, m_path);
                    return;
                }
                for (auto it = m_stagedBySourceId.begin(); it != m_stagedBySourceId.end(); ++it) {
                    if (it->second == changeId) {
                        // Fully merged now: nothing left for the backup to
                        // offer this track, so the row goes.
                        int index = indexOfSourceId(it->first);
                        m_stagedBySourceId.erase(it);
                        if (index >= 0) {
                            m_model.removeCandidateAt(index);
                        }
                        emit analysisChanged();
                        break;
                    }
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                m_stagedBySourceId.clear();
                m_model.clearStaged();
                emit analysisChanged();
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

void LocalCueController::stageCandidate(int index)
{
    const auto &candidates = m_model.candidates();
    if (index < 0 || static_cast<size_t>(index) >= candidates.size()) {
        return;
    }
    if (!m_session) {
        attachSession();
        if (!m_session) {
            setErrorMessage("This stick's library could not be identified; nothing was changed.");
            return;
        }
    }
    const RestoreCandidate &candidate = candidates[static_cast<size_t>(index)];
    auto change = std::make_unique<MergeCuesChange>(m_format, m_path, candidate);
    QString changeId = change->id();
    if (!m_session->stage(std::move(change))) {
        return;  // the session reported the lock refusal; the page shows it
    }
    m_stagedBySourceId[candidate.stickTrack.sourceId] = changeId;
    m_model.setStaged(index, true);
    emit analysisChanged();
}

// Stages every candidate currently proposed; the page's Save writes them.
void LocalCueController::applyRestore()
{
    if (m_busy) {
        emit actionFeedback("Still busy with another operation on this stick -- try again once it finishes.", true);
        return;
    }
    if (writing()) {
        emit actionFeedback("A save is running -- stage more once it has finished.", true);
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    int staged = 0;
    const size_t count = m_model.candidates().size();
    for (size_t i = 0; i < count; ++i) {
        if (m_stagedBySourceId.count(m_model.candidates()[i].stickTrack.sourceId)) {
            continue;
        }
        stageCandidate(static_cast<int>(i));
        staged++;
        if (m_session && !m_session->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
    if (staged > 0) {
        emit actionFeedback(QStringLiteral("Staged merging cues onto %1 track(s). Press Save to write them to the stick.")
                                .arg(staged),
                            false);
    }
}

void LocalCueController::unstage(int index)
{
    const auto &candidates = m_model.candidates();
    if (index < 0 || static_cast<size_t>(index) >= candidates.size()) {
        return;
    }
    auto it = m_stagedBySourceId.find(candidates[static_cast<size_t>(index)].stickTrack.sourceId);
    if (it == m_stagedBySourceId.end()) {
        return;
    }
    if (m_session) {
        m_session->unstage(it->second);
    }
    m_stagedBySourceId.erase(it);
    m_model.setStaged(index, false);
    emit analysisChanged();
}

void LocalCueController::undoLastOperation()
{
    if (m_busy) {
        emit actionFeedback("Still busy with another operation on this stick -- try again once it finishes.", true);
        return;
    }
    if (!canUndo()) {
        emit actionFeedback("Nothing to undo.", true);
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_session->undoLastSave();
}

void LocalCueController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void LocalCueController::setScanProgress(int current, int total)
{
    if (m_scanCurrent == current && m_scanTotal == total) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    emit scanProgressChanged();
}

void LocalCueController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void LocalCueController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
