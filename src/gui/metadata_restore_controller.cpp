// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "metadata_restore_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/metadata_merge.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/future_result.hpp"
#include "gui/local_file_url.hpp"
#include "gui/metadata_row_text.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/local/metadata_store.hpp"

namespace seabass::gui
{

using domain::MetadataRestoreProposal;
using infrastructure::local::MetadataStore;

namespace
{

// Runs entirely on a background thread -- no access to the controller.
MetadataRestoreTaskResult runScanTask(QString libraryPath, std::shared_ptr<QtProgressReporter> reporter,
                                       application::CancellationToken cancel)
{
    MetadataRestoreTaskResult result;
    result.libraryPath = libraryPath;
    try {
        const auto read = readAllStickCatalogs(libraryPath.toStdString(), *reporter, cancel);
        if (read.catalogs.present().empty()) {
            result.errorMessage = QStringLiteral("No rekordbox or Engine library was found on this stick.");
            return result;
        }

        // Folded into files first, so a track three catalogs list is one
        // proposal carrying all three rows -- not three proposals, each
        // of which would look like a separate track to restore.
        std::vector<domain::Track> rows;
        for (const auto *catalog : {&read.catalogs.rekordbox, &read.catalogs.oneLibrary, &read.catalogs.engine}) {
            if (*catalog) {
                rows.insert(rows.end(), (*catalog)->begin(), (*catalog)->end());
            }
        }
        const auto stickTracks = application::collapseCatalogRows(rows);
        result.stickTrackCount = static_cast<int>(stickTracks.size());

        reporter->start("Reading the metadata store", 0);
        MetadataStore store;
        const auto storedTracks = store.readAll();
        // For display and for the stick picker only, and fetched here
        // rather than carried on domain::Track so nothing in the matching
        // or merging can reach for it.
        const auto stickSources = store.stickSourcesByTrackId();
        for (const auto &[trackId, source] : stickSources) {
            if (!source.stickLabel.empty() && !source.libraryId.empty()) {
                result.recordedIdsByLabel[source.stickLabel].insert(source.libraryId);
            }
        }
        reporter->finish();
        result.storedTrackCount = static_cast<int>(storedTracks.size());

        if (cancel.cancelled()) {
            result.cancelled = true;
            return result;
        }

        // When this stick's catalogs were last written. The merge rule
        // consults it only where nothing else separates two copies, but
        // where it does the answer turns on it, so it is read from the
        // files rather than assumed.
        result.proposals =
            domain::planMetadataRestore(stickTracks, storedTracks, catalogsLastModified(libraryPath.toStdString()));

        // Tracks whose cues genuinely differ from the stored ones,
        // whichever side the rule then chose. How many of those the stick
        // kept is not counted here: the page says it of the pickers'
        // selection, so refreshScope() counts it over the scope.
        for (auto &proposal : result.proposals) {
            if (proposal.cuesConflict) {
                result.conflictCount++;
            }
            // storedId is the row id as text, which is how the store
            // spells it on a domain::Track; stoll is safe on anything
            // readAll() produced and guarded for anything that was not.
            try {
                const auto found = stickSources.find(std::stoll(proposal.storedId));
                if (found != stickSources.end()) {
                    proposal.storedFrom = found->second.stickLabel;
                    proposal.storedFromLibraryId = found->second.libraryId;
                }
            } catch (const std::exception &) {
                // No label, so the row simply does not say where it came
                // from, and the stick picker files it under "A stick with
                // no name". Not worth failing a scan over.
            }
        }
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromUtf8(e.what());
    }
    return result;
}

}  // namespace

// ---- controller -----------------------------------------------------

class MetadataRestoreController::AnalysisBatch
{
public:
    explicit AnalysisBatch(MetadataRestoreController &controller) : m_controller(controller)
    {
        m_controller.m_analysisBatchDepth++;
    }
    ~AnalysisBatch()
    {
        if (--m_controller.m_analysisBatchDepth == 0 && std::exchange(m_controller.m_analysisPending, false)) {
            emit m_controller.analysisChanged();
        }
    }
    AnalysisBatch(const AnalysisBatch &) = delete;
    AnalysisBatch &operator=(const AnalysisBatch &) = delete;

private:
    MetadataRestoreController &m_controller;
};

void MetadataRestoreController::noteAnalysisChanged()
{
    if (m_analysisBatchDepth > 0) {
        m_analysisPending = true;
        return;
    }
    emit analysisChanged();
}

MetadataRestoreController::MetadataRestoreController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<MetadataRestoreTaskResult>::finished, this,
            &MetadataRestoreController::onScanFinished);
}

MetadataRestoreController::~MetadataRestoreController()
{
    m_cancel.cancel();
    // See MetadataBackupController's destructor, and future_result.hpp:
    // waitForFinished() rethrows in a noexcept context.
    awaitQuietly(m_watcher);
}

bool MetadataRestoreController::writing() const
{
    return m_session && m_session->state() == QStringLiteral("writing");
}

void MetadataRestoreController::scan(const QString &libraryPath)
{
    if (m_busy) {
        return;
    }
    m_libraryPath = libraryPath;
    setErrorMessage({});
    m_phaseBaseline = 0;
    m_currentPhaseTotal = 0;
    setProgress(0, 0);
    setCurrentPhase(QStringLiteral("Reading this stick"));
    m_cancel = application::CancellationToken();
    setBusy(true);
    m_watcher.setFuture(QtConcurrent::run(runScanTask, libraryPath, makeReporter(), m_cancel));
}

void MetadataRestoreController::cancelScan()
{
    m_cancel.cancel();
}

void MetadataRestoreController::onScanFinished()
{
    QString thrown;
    MetadataRestoreTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }
    setBusy(false);
    setCurrentPhase({});
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        return;
    }
    if (result.cancelled) {
        return;
    }
    applyScanResult(std::move(result));
}

void MetadataRestoreController::applyScanResult(MetadataRestoreTaskResult result)
{
    if (!result.libraryPath.isEmpty()) {
        m_libraryPath = result.libraryPath;
    }
    // A scan that found something supersedes one that failed before it.
    setErrorMessage({});
    // Once, over the whole set: which stick an unstamped row belongs to
    // depends on every stick the store recorded under its label.
    domain::resolveRestoreSources(result.proposals, result.recordedIdsByLabel);
    m_model.setProposals(std::move(result.proposals));
    m_stickTrackCount = result.stickTrackCount;
    m_storedTrackCount = result.storedTrackCount;
    m_conflictCount = result.conflictCount;
    m_hasScanned = true;
    // A scope picked before this scan keeps applying when what it names
    // is still there, and falls back to everything when it is not: a
    // playlist this stick no longer has would otherwise narrow the list
    // to nothing with no picker entry to explain why.
    domain::MetadataRestoreScope scope = m_model.scope();
    const auto &proposals = m_model.proposals();
    if (!scope.sourceKey.empty()
        && std::none_of(proposals.begin(), proposals.end(), [&scope](const MetadataRestoreProposal &proposal) {
               return domain::restoreSourceKey(proposal) == scope.sourceKey;
           })) {
        scope.sourceKey.clear();
    }
    if (!scope.playlist.empty() && domain::restorePlaylistCounts(proposals, scope.sourceKey).count(scope.playlist) == 0) {
        scope.playlist.clear();
    }
    m_model.setScope(scope);
    refreshScope();
    emit analysisChanged();
}

void MetadataRestoreController::refreshScope()
{
    const auto &proposals = m_model.proposals();
    const auto &scope = m_model.scope();

    m_sourceSticks.clear();
    const auto sources = domain::restoreSources(proposals);
    for (const auto &source : sources) {
        QVariantMap entry;
        entry[QStringLiteral("key")] = QString::fromStdString(source.key);
        entry[QStringLiteral("label")] = QString::fromStdString(source.label);
        entry[QStringLiteral("count")] = source.proposalCount;
        entry[QStringLiteral("idNotRecorded")] = source.idNotRecorded;
        m_sourceSticks << entry;
    }

    m_sourceProposalCount = 0;
    const domain::MetadataRestoreScope wholeSource{scope.sourceKey, {}};
    for (const auto &proposal : proposals) {
        if (domain::proposalInRestoreScope(proposal, wholeSource)) {
            m_sourceProposalCount++;
        }
    }

    m_playlistNames.clear();
    m_playlistTrackCounts.clear();
    auto playlists = domain::restorePlaylistCounts(proposals, scope.sourceKey);
    // The picked playlist stays listed after a restore has emptied it,
    // at 0, so the picker still shows what the list is narrowed to.
    if (!scope.playlist.empty()) {
        playlists.emplace(scope.playlist, 0);
    }
    for (const auto &[name, count] : playlists) {
        m_playlistNames << QString::fromStdString(name);
        m_playlistTrackCounts.insert(QString::fromStdString(name), count);
    }

    // Counted over the scope, like everything else the page says about
    // the restore: a warning about comments that cannot go back is about
    // this restore, and a track outside the scope is not in it.
    m_conflictsLeftAlone = 0;
    m_commentsRekordboxCannotTake = 0;
    for (const auto &proposal : proposals) {
        if (!domain::proposalInRestoreScope(proposal, scope)) {
            continue;
        }
        if (proposal.cuesConflict && !proposal.cuesOffered) {
            m_conflictsLeftAlone++;
        }
        // A comment can only be written where a format can grow one, and
        // export.pdb cannot (tests/pdb_rating_write_test.cpp). A track
        // that rekordbox alone catalogues therefore gets its rating back
        // and not its comment, and the page has to say so.
        if (!proposal.commentOffered) {
            continue;
        }
        // "No row anywhere but rekordbox", and a track with no catalog
        // rows at all is not that: collapseCatalogRows() leaves the list
        // empty for a row with no file path and for an Engine streaming
        // track, and counting those would have the banner warn about
        // DeviceLibrary on a stick that may carry no rekordbox catalog.
        if (proposal.stickTrack.catalogRows.empty()) {
            continue;
        }
        bool elsewhere = false;
        for (const auto &row : proposal.stickTrack.catalogRows) {
            if (row.format != "rekordbox") {
                elsewhere = true;
            }
        }
        if (!elsewhere) {
            m_commentsRekordboxCannotTake++;
        }
    }
}

void MetadataRestoreController::setSourceStick(const QString &key)
{
    domain::MetadataRestoreScope scope = m_model.scope();
    scope.sourceKey = key.toStdString();
    // A playlist belongs to the stick it was picked under only as far as
    // that stick's proposals are in it; one the new stick has nothing in
    // would narrow its list to nothing.
    if (!scope.playlist.empty()
        && domain::restorePlaylistCounts(m_model.proposals(), scope.sourceKey).count(scope.playlist) == 0) {
        scope.playlist.clear();
    }
    applyScope(std::move(scope));
}

void MetadataRestoreController::setPlaylist(const QString &name)
{
    domain::MetadataRestoreScope scope = m_model.scope();
    scope.playlist = name.toStdString();
    applyScope(std::move(scope));
}

void MetadataRestoreController::applyScope(domain::MetadataRestoreScope scope)
{
    const auto &current = m_model.scope();
    if (scope.sourceKey == current.sourceKey && scope.playlist == current.playlist) {
        return;
    }
    AnalysisBatch batch(*this);
    m_model.setScope(std::move(scope));
    // What the new scope leaves out is not part of this restore any
    // more, so it comes off the save rather than being written by it
    // out of sight.
    const auto outside = m_model.stagedOutsideScope();
    unstageIndices(outside);
    refreshScope();
    noteAnalysisChanged();
    if (!outside.empty()) {
        const int n = static_cast<int>(outside.size());
        emit actionFeedback(n == 1 ? QStringLiteral("1 staged track is outside this selection and was unstaged.")
                                   : QStringLiteral("%1 staged tracks are outside this selection and were unstaged.")
                                         .arg(n),
                            false);
    }
}

QVariantMap MetadataRestoreController::waveformSourceAt(int row) const
{
    const int index = m_model.sourceIndexOfRow(row);
    if (index < 0) {
        return {};
    }
    return metadataWaveformSource(m_model.proposals()[static_cast<std::size_t>(index)].stickTrack, m_libraryPath);
}

void MetadataRestoreController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(m_libraryPath));
    if (session == m_session) {
        return;
    }
    if (m_session) {
        disconnect(m_session, nullptr, this, nullptr);
    }
    m_session = session;
    if (!m_session) {
        return;
    }
    connect(m_session, &LibraryEditSession::stateChanged, this, &MetadataRestoreController::writingChanged);
    connect(m_session, &LibraryEditSession::canUndoChanged, this, &MetadataRestoreController::canUndoChanged);
    // A save emits changeApplied once per landed change, back to back,
    // and then saveFinished. Each one is only noted here; the list is
    // brought up to date once, when the burst is over. Done per change,
    // each of them reset the list and recounted the whole scope, and a
    // save of fourteen hundred changes was quadratic on the UI thread.
    connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
        m_appliedChanges.insert(changeId);
        // saveFinished always follows, and takes them. Queued as well, so
        // a burst that arrived without one is still not left pending.
        if (!m_takeAppliedQueued) {
            m_takeAppliedQueued = true;
            QMetaObject::invokeMethod(this, &MetadataRestoreController::takeAppliedChanges, Qt::QueuedConnection);
        }
    });
    connect(m_session, &LibraryEditSession::saveFinished, this, &MetadataRestoreController::takeAppliedChanges);
}

void MetadataRestoreController::takeAppliedChanges()
{
    m_takeAppliedQueued = false;
    if (m_appliedChanges.isEmpty()) {
        return;
    }
    const QSet<QString> applied = std::exchange(m_appliedChanges, {});
    // A proposal disappears once every change it staged has landed: the
    // stick now has what the store had, so there is nothing left to
    // offer it. One that landed only in part keeps what is still to go.
    std::vector<int> landed;
    for (int index = 0; index < m_model.totalCount(); ++index) {
        QStringList changes = m_model.stagedChanges(index);
        const auto before = changes.size();
        if (before == 0) {
            continue;
        }
        changes.removeIf([&applied](const QString &changeId) { return applied.contains(changeId); });
        if (changes.size() == before) {
            continue;
        }
        if (changes.isEmpty()) {
            landed.push_back(index);
        } else {
            m_model.setStagedChanges(index, changes);
        }
    }
    m_model.removeAll(std::move(landed));
    refreshScope();
    noteAnalysisChanged();
}

void MetadataRestoreController::search(const QString &text)
{
    m_model.setFilter(text);
    emit analysisChanged();
}

void MetadataRestoreController::stage(int row)
{
    const int index = m_model.sourceIndexOfRow(row);
    if (index < 0) {
        return;
    }
    // One track staged on its own is one track's worth of writing, and
    // saying otherwise is not the safe direction.
    //
    // The obvious-looking alternative -- hint the whole list, since the
    // session is built by whichever change applies first and a running
    // total is always 1 there -- makes the wrong trade for SQLite. A
    // hint of 400 for a single staged Engine track has the save copy the
    // entire m.db to scratch and durably copy it back for one row
    // update, and a whole-file replace can lose the database in a way a
    // page-level write cannot. Staging four hundred tracks one at a time
    // therefore does not earn the scratch copy. Staging a selection,
    // which is how that is actually done, passes the real count and does.
    stageOne(index, 1);
}

// itemCountHint is what the save is expected to write in total, which
// decides whether the catalog is worth copying to local scratch first.
// stageAll() knows that number; a single row's tick box does not, and
// does not need to.
void MetadataRestoreController::stageOne(int index, int itemCountHint)
{
    const auto &proposals = m_model.proposals();
    if (index < 0 || static_cast<std::size_t>(index) >= proposals.size()) {
        return;
    }
    const MetadataRestoreProposal &proposal = proposals[static_cast<std::size_t>(index)];
    if (m_model.isStaged(index)) {
        return;
    }
    // Only what the stick and playlist pickers include. Every caller
    // passes an in-scope index today; this is what keeps the next one
    // from staging a track the page says it is not restoring.
    if (!m_model.inScope(index)) {
        return;
    }
    if (!proposal.offersAnything()) {
        return;
    }
    if (!m_session) {
        attachSession();
        if (!m_session) {
            setErrorMessage(QStringLiteral("This stick's library could not be identified; nothing was changed."));
            return;
        }
    }

    // One change per catalog that lists this file. OneLibrary is skipped
    // when rekordbox is present, because the rekordbox change already
    // mirrors into it -- staging both would write the same cues twice
    // and report the track as two restores.
    bool hasRekordbox = false;
    for (const auto &row : proposal.stickTrack.catalogRows) {
        if (row.format == "rekordbox") {
            hasRekordbox = true;
        }
    }

    QStringList staged;
    for (const auto &row : proposal.stickTrack.catalogRows) {
        if (row.format == "onelibrary" && hasRekordbox) {
            continue;
        }
        auto change = std::make_unique<RestoreMetadataChange>(
            QString::fromStdString(row.format), catalogQtPathForFormat(m_libraryPath, row.format),
            QString::fromStdString(row.sourceId), proposal, itemCountHint);
        const QString changeId = change->id();
        if (!m_session->stage(std::move(change))) {
            // Refused: the session reports why and the page shows it.
            // Anything already staged for this track stays staged --
            // undoing a partial batch here would be a second, silent
            // decision on top of the refusal.
            break;
        }
        staged << changeId;
    }
    if (staged.isEmpty()) {
        return;
    }
    m_model.setStagedChanges(index, staged);
    noteAnalysisChanged();
}

QString MetadataRestoreController::mergeRuleHelp() const
{
    return QString::fromStdString(domain::mergeRuleExplanation());
}

void MetadataRestoreController::stageAll()
{
    if (writing()) {
        emit actionFeedback(QStringLiteral("A save is running. Stage more once it has finished."), true);
        return;
    }
    // The real number about to be staged, which is what decides whether
    // the save copies a catalog to local scratch first. Taken before
    // staging anything, because staging changes what a later pass would
    // count. In scope only: the stick and playlist pickers say what this
    // restore is for, and Select All must not reach past them.
    const std::vector<int> toStage = m_model.unstagedInScope();
    if (toStage.empty()) {
        return;
    }

    // No confirmation afterwards: the toolbar already says how many are
    // staged, and a popup for what the button's own name promised is one
    // more thing to dismiss.
    AnalysisBatch batch(*this);
    for (const int index : toStage) {
        stageOne(index, static_cast<int>(toStage.size()));
        if (m_session && !m_session->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
}

void MetadataRestoreController::unstageAll()
{
    // By proposal, through the same path a narrowing takes.
    std::vector<int> staged;
    for (int index = 0; index < proposalCount(); ++index) {
        if (m_model.isStaged(index)) {
            staged.push_back(index);
        }
    }
    unstageIndices(staged);
}

void MetadataRestoreController::unstage(int row)
{
    unstageAt(m_model.sourceIndexOfRow(row));
}

void MetadataRestoreController::unstageAt(int index)
{
    unstageIndices({index});
}

// Every unstage funnels here, a single row's tick box included, so a bulk
// unstage cannot drift from what unticking one row does.
void MetadataRestoreController::unstageIndices(const std::vector<int> &indices)
{
    QStringList changes;
    std::vector<int> staged;
    for (const int index : indices) {
        const QStringList mine = m_model.stagedChanges(index);
        if (!mine.isEmpty()) {
            changes << mine;
            staged.push_back(index);
        }
    }
    if (staged.empty()) {
        return;
    }
    if (m_session) {
        m_session->unstageAll(changes);
    }
    for (const int index : staged) {
        m_model.setStagedChanges(index, {});
    }
    noteAnalysisChanged();
}

std::shared_ptr<QtProgressReporter> MetadataRestoreController::makeReporter()
{
    auto reporter = std::make_shared<QtProgressReporter>();
    connect(reporter.get(), &QtProgressReporter::started, this, [this](const QString &label, int total) {
        m_phaseBaseline += m_currentPhaseTotal;
        m_currentPhaseTotal = total;
        setCurrentPhase(label);
        setProgress(m_phaseBaseline, m_phaseBaseline + total);
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setProgress(m_phaseBaseline + current, m_phaseBaseline + m_currentPhaseTotal); });
    return reporter;
}

void MetadataRestoreController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void MetadataRestoreController::setProgress(int current, int total)
{
    if (m_progressCurrent == current && m_progressTotal == total) {
        return;
    }
    m_progressCurrent = current;
    m_progressTotal = total;
    emit progressChanged();
}

void MetadataRestoreController::setCurrentPhase(const QString &phase)
{
    if (m_currentPhase == phase) {
        return;
    }
    m_currentPhase = phase;
    emit currentPhaseChanged();
}

void MetadataRestoreController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
