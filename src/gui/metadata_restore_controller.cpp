// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "metadata_restore_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>

#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/metadata_merge.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/future_result.hpp"
#include "gui/local_file_url.hpp"
#include "gui/metadata_row_text.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/local/metadata_store.hpp"

namespace seabass::gui
{

using domain::MetadataRestoreProposal;
using infrastructure::local::MetadataStore;

namespace
{

namespace fs = std::filesystem;

// The catalog directory for one format on the stick that `libraryPath`
// belongs to.
//
// The page hands the controller a single path -- whichever catalog the
// stick list happened to open -- but a proposal carries the rows of
// every catalog that lists the track, and each row's change has to be
// given the path for ITS OWN format. Handing an Engine change the
// PIONEER folder points the Engine writer, and its backup, at a
// database that is not there. Same derivation readAllStickCatalogs()
// uses to find the catalogs in the first place.
QString catalogPathForFormat(const QString &libraryPath, const std::string &format)
{
    const fs::path stickRoot = pathFromQString(libraryPath).parent_path();
    if (format == "engine") {
        return pathToQString(infrastructure::engine::engineLibraryPath(stickRoot));
    }
    // rekordbox and onelibrary are two formats of one library, both
    // under PIONEER.
    return pathToQString(stickRoot / "PIONEER");
}

// Runs entirely on a background thread -- no access to the controller.
MetadataRestoreTaskResult runScanTask(QString libraryPath, std::shared_ptr<QtProgressReporter> reporter,
                                       application::CancellationToken cancel)
{
    MetadataRestoreTaskResult result;
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
        // For display only, and fetched here rather than carried on
        // domain::Track so nothing in the matching or merging can reach
        // for it.
        const auto stickLabels = store.stickLabelsByTrackId();
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
        // whichever side the rule then chose. The page says how many
        // there were and how many it left alone, and both numbers come
        // from this one pass.
        for (auto &proposal : result.proposals) {
            if (proposal.cuesConflict) {
                result.conflictCount++;
                if (!proposal.cuesOffered) {
                    result.conflictsLeftAlone++;
                }
            }
            // storedId is the row id as text, which is how the store
            // spells it on a domain::Track; stoll is safe on anything
            // readAll() produced and guarded for anything that was not.
            try {
                const auto found = stickLabels.find(std::stoll(proposal.storedId));
                if (found != stickLabels.end()) {
                    proposal.storedFrom = found->second;
                }
            } catch (const std::exception &) {
                // No label, so the row simply does not say where it came
                // from. Not worth failing a scan over.
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
    m_model.setProposals(result.proposals);
    m_stickTrackCount = result.stickTrackCount;
    m_storedTrackCount = result.storedTrackCount;
    m_conflictCount = result.conflictCount;
    m_conflictsLeftAlone = result.conflictsLeftAlone;
    // A comment can only be written where a format can grow one, and
    // export.pdb cannot (tests/pdb_rating_write_test.cpp). A track that
    // rekordbox alone catalogues therefore gets its rating back and not
    // its comment, and the page has to say so.
    m_commentsRekordboxCannotTake = 0;
    for (const auto &proposal : result.proposals) {
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
    m_hasScanned = true;
    emit analysisChanged();
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
    connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
        // A proposal disappears once every change it staged has landed:
        // the stick now has what the store had, so there is nothing left
        // to offer it.
        const int index = m_model.indexOfChange(changeId);
        if (index < 0) {
            return;
        }
        QStringList remaining = m_model.stagedChanges(index);
        remaining.removeAll(changeId);
        if (!remaining.isEmpty()) {
            m_model.setStagedChanges(index, remaining);
            return;
        }
        m_model.removeAt(index);
        emit analysisChanged();
    });
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
            QString::fromStdString(row.format), catalogPathForFormat(m_libraryPath, row.format),
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
    emit analysisChanged();
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
    const int count = proposalCount();
    // The real number about to be staged, which is what decides whether
    // the save copies a catalog to local scratch first. Counted before
    // staging anything, because staging removes nothing from the list
    // but does change what a later pass would count.
    int toStage = 0;
    for (int i = 0; i < count; ++i) {
        if (!m_model.isStaged(i)) {
            toStage++;
        }
    }
    if (toStage == 0) {
        return;
    }

    // No confirmation afterwards: the toolbar already says how many are
    // staged, and a popup for what the button's own name promised is one
    // more thing to dismiss.
    for (int i = 0; i < count; ++i) {
        if (m_model.isStaged(i)) {
            continue;
        }
        stageOne(i, toStage);
        if (m_session && !m_session->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
}

void MetadataRestoreController::unstageAll()
{
    // By proposal, through the same path a single row's tick box takes, so
    // the two cannot drift apart.
    for (int index = proposalCount() - 1; index >= 0; --index) {
        unstageAt(index);
    }
}

void MetadataRestoreController::unstage(int row)
{
    unstageAt(m_model.sourceIndexOfRow(row));
}

void MetadataRestoreController::unstageAt(int index)
{
    if (index < 0) {
        return;
    }
    const QStringList changes = m_model.stagedChanges(index);
    if (changes.isEmpty()) {
        return;
    }
    if (m_session) {
        for (const auto &changeId : changes) {
            m_session->unstage(changeId);
        }
    }
    m_model.setStagedChanges(index, {});
    emit analysisChanged();
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
