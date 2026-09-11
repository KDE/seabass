// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>

#include <map>
#include <memory>

#include "application/ports/cancellation_token.hpp"
#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

class LibraryEditSession;
class PendingChange;
class StagedPlanModel;

// The half of DuplicatesController and SyncController that is the same
// page mechanic rather than the same feature: a cancellable background
// scan with progress, a set of cue-copy changes staged in the library's
// LibraryEditSession until the page's Save writes them, and the
// error/status/undo surface both pages bind to.
//
// The feature half stays in each subclass -- what a plan is, how the scan
// is run, and which tracks get paired: copies of one track inside a
// single library (Match Duplicate Cues) versus the same track across two
// catalogs (Sync Cue Points). This base never learns either. It reaches
// the rows only through StagedPlanModel and builds no changes itself;
// subclasses construct the right PendingChange and hand it to
// stageChange().
//
// QML_ANONYMOUS: the two subclasses are the instantiable QML types. This
// one is registered only so their inherited properties resolve in tooling.
class StagedCueEditController : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while the read-only scan runs (never during a write): it can be
    // stopped via cancelScan(), after which scanCancelled() fires.
    Q_PROPERTY(bool scanCancellable READ scanCancellable NOTIFY busyChanged)
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    // What is actually happening right now -- e.g. "Scanning rekordbox
    // tracks" while the reader runs (scanCurrent/scanTotal move), then a
    // label for a follow-up pass with no per-item progress (scanTotal
    // resets to 0 = indeterminate). Without this the progress bar used to
    // sit frozen at 100% for several seconds with nothing to explain it.
    Q_PROPERTY(QString scanLabel READ scanLabel NOTIFY scanProgressChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    // Mirrors the session: true while a save is writing to the stick.
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY stagedChanged)

public:
    explicit StagedCueEditController(QObject *parent = nullptr);
    ~StagedCueEditController() override;

    bool busy() const { return m_busy; }
    bool scanCancellable() const { return m_busy && !writing(); }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    QString scanLabel() const { return m_scanLabel; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }
    bool writing() const;
    bool canUndo() const;
    int stagedCount() const { return static_cast<int>(m_stagedByKey.size()); }

    Q_INVOKABLE void cancelScan();

    // Drops the staged change for the row at index, leaving the row in
    // the list unstaged.
    Q_INVOKABLE void unstage(int index);

    // Reverts every file the last save touched (the session's undo).
    Q_INVOKABLE void undoLastOperation();

signals:
    void scanCancelled();
    void busyChanged();
    void scanProgressChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    void canUndoChanged();
    void writingChanged();
    void stagedChanged();

protected:
    // --- what each feature must supply -------------------------------

    // The rows this controller stages against. Never null once a scan has
    // produced a model; called only from the staging/session paths.
    virtual StagedPlanModel *stagedPlanModel() = 0;

    // An undo restored the prior file bytes, so the whole plan list is
    // stale: re-run this feature's own scan with its own current
    // arguments (playlist scope, format, ...).
    virtual void reanalyzeAfterUndo() = 0;

    // A staged change reached the stick, and `rowRemoved` says whether its
    // row was still listed (a rescan may already have dropped it). Hook for
    // whatever else the feature derives from the row set; the base has
    // already emitted stagedChanged().
    virtual void onStagedChangeApplied(bool rowRemoved) { (void)rowRemoved; }

    // The session discarded every staged change at once, so the base has
    // cleared its own map and every row's staged mark. Same kind of hook
    // as onStagedChangeApplied(), for that path.
    virtual void onStagedCleared() {}

    // --- what the base supplies --------------------------------------

    // Marks the controller busy and hands out the token the new scan task
    // must carry, replacing any previous scan's token.
    application::CancellationToken beginScan();

    // See ScanController::scan() for why the reporter is owned by the
    // task (via shared_ptr) rather than by this controller.
    std::shared_ptr<QtProgressReporter> makeReporter();

    // Finds (or re-finds) the LibraryEditSession for the library at
    // `path` and wires its state through to this controller's own
    // signals. Safe to call repeatedly; a no-op once attached.
    void attachSessionForPath(const QString &path);

    // Stages one already-built change against the row at `index`, filed
    // under `key`. Returns false without staging anything if there is no
    // session, a save is already running, or the session refused (each of
    // which has already put a message in front of the user). Staging a
    // second change for the same key replaces the first.
    bool stageChange(int index, const QString &key, std::unique_ptr<PendingChange> change);

    // -1 when no row currently carries that key -- a rescan may have
    // dropped it while its change was still staged.
    int indexOfStagedKey(const QString &key);

    void setBusy(bool busy);
    void setScanProgress(int current, int total);
    void setScanLabel(const QString &label);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);

    LibraryEditSession *session() const { return m_session; }

    struct StagedInfo
    {
        QString changeId;
        QString description;
    };
    std::map<QString, StagedInfo> m_stagedByKey;  // row key -> what is staged for it

private:
    void onSessionChangeApplied(const QString &changeId);
    void onSessionChangesDiscarded();

    QPointer<LibraryEditSession> m_session;
    application::CancellationToken m_scanCancel;  // fresh per scan
    bool m_busy = false;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    QString m_scanLabel;
    QString m_errorMessage;
    QString m_statusMessage;
};

}  // namespace seabass::gui
