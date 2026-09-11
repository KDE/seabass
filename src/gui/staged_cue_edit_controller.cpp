// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/staged_cue_edit_controller.hpp"

#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/staged_plan_model.hpp"

namespace seabass::gui
{

StagedCueEditController::StagedCueEditController(QObject *parent) : QObject(parent) {}

// Out of line: the header only forward-declares PendingChange and
// LibraryEditSession, so the implicit destructor cannot be generated there.
StagedCueEditController::~StagedCueEditController() = default;

bool StagedCueEditController::writing() const
{
    return m_session && m_session->writing();
}

bool StagedCueEditController::canUndo() const
{
    return m_session && m_session->canUndo();
}

application::CancellationToken StagedCueEditController::beginScan()
{
    setBusy(true);
    m_scanCancel = application::CancellationToken();
    return m_scanCancel;
}

void StagedCueEditController::cancelScan()
{
    if (scanCancellable()) {
        m_scanCancel.cancel();
    }
}

std::shared_ptr<QtProgressReporter> StagedCueEditController::makeReporter()
{
    auto reporter = std::make_shared<QtProgressReporter>();
    connect(reporter.get(), &QtProgressReporter::started, this, [this](const QString &label, int total) {
        setScanLabel(label);
        setScanProgress(0, total);
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setScanProgress(current, m_scanTotal); });
    return reporter;
}

void StagedCueEditController::attachSessionForPath(const QString &path)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(path));
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
    connect(m_session, &LibraryEditSession::stateChanged, this, &StagedCueEditController::writingChanged);
    connect(m_session, &LibraryEditSession::canUndoChanged, this, &StagedCueEditController::canUndoChanged);
    connect(m_session, &LibraryEditSession::changeApplied, this,
            &StagedCueEditController::onSessionChangeApplied);
    connect(m_session, &LibraryEditSession::changesDiscarded, this,
            &StagedCueEditController::onSessionChangesDiscarded);
}

void StagedCueEditController::onSessionChangeApplied(const QString &changeId)
{
    if (changeId == QStringLiteral("undo:last-save")) {
        reanalyzeAfterUndo();  // prior file bytes are back; the plan list is stale
        return;
    }
    for (auto it = m_stagedByKey.begin(); it != m_stagedByKey.end(); ++it) {
        if (it->second.changeId != changeId) {
            continue;
        }
        // That row's cues reached the stick, so it has nothing left to
        // show: both features drop a row whose change landed rather than
        // waiting for the next scan.
        int index = indexOfStagedKey(it->first);
        m_stagedByKey.erase(it);
        if (index >= 0) {
            stagedPlanModel()->removePlanAt(index);
        }
        emit stagedChanged();
        onStagedChangeApplied(index >= 0);
        break;
    }
}

void StagedCueEditController::onSessionChangesDiscarded()
{
    m_stagedByKey.clear();
    stagedPlanModel()->clearStaged();
    emit stagedChanged();
    onStagedCleared();
}

int StagedCueEditController::indexOfStagedKey(const QString &key)
{
    StagedPlanModel *model = stagedPlanModel();
    for (int i = 0; i < model->planCount(); ++i) {
        if (model->planKeyAt(i) == key) {
            return i;
        }
    }
    return -1;
}

bool StagedCueEditController::stageChange(int index, const QString &key, std::unique_ptr<PendingChange> change)
{
    if (!m_session) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return false;
    }
    if (m_session->writing()) {
        setErrorMessage("A save is running -- stage more once it has finished.");
        return false;
    }
    QString changeId = change->id();
    QString description = change->description();
    if (!m_session->stage(std::move(change))) {
        return false;  // the session reported the lock refusal; the page shows it
    }
    m_stagedByKey[key] = {changeId, description};
    stagedPlanModel()->setStaged(index, true, description);
    emit stagedChanged();
    return true;
}

void StagedCueEditController::unstage(int index)
{
    StagedPlanModel *model = stagedPlanModel();
    if (index < 0 || index >= model->planCount()) {
        return;
    }
    auto it = m_stagedByKey.find(model->planKeyAt(index));
    if (it == m_stagedByKey.end()) {
        return;
    }
    if (m_session) {
        m_session->unstage(it->second.changeId);
    }
    m_stagedByKey.erase(it);
    model->setStaged(index, false, QString());
    emit stagedChanged();
}

void StagedCueEditController::undoLastOperation()
{
    if (m_busy || !m_session) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_session->undoLastSave();
}

void StagedCueEditController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void StagedCueEditController::setScanProgress(int current, int total)
{
    if (m_scanCurrent == current && m_scanTotal == total) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    emit scanProgressChanged();
}

void StagedCueEditController::setScanLabel(const QString &label)
{
    if (m_scanLabel == label) {
        return;
    }
    m_scanLabel = label;
    emit scanProgressChanged();
}

void StagedCueEditController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void StagedCueEditController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
