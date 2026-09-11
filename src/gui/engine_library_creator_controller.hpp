// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>

#include <memory>

#include "application/ports/cancellation_token.hpp"
#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

// Result of the background creation task, see
// EngineLibraryCreatorController::create(). Built entirely on a worker
// thread, no access to the controller.
struct EngineLibraryCreationTaskResult
{
    int tracksCreated = 0;
    int tracksSkipped = 0;
    int cuesCopied = 0;
    int tracksTotal = 0;
    bool cancelled = false;  // nothing was created on the stick
    QString errorMessage;  // empty on success
};

// Wraps infrastructure::engine::EngineLibraryCreator for QML. See that
// class's own doc comment for exactly what is and isn't carried over
// from the rekordbox source, and docs/experimental-features.md for why
// this whole feature is gated as experimental: it's the first thing in
// this codebase that fabricates an entire new database from scratch
// rather than modifying an existing, already-recognized one, and real
// Denon hardware's tolerance for a library this project created (as
// opposed to one Engine DJ or a player itself created) is genuinely
// unverified until tested against real units.
class EngineLibraryCreatorController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while a cancel would still leave the stick untouched (the
    // scan and the scratch build); false once the copy to the stick has
    // started.
    Q_PROPERTY(bool cancellable READ cancellable NOTIFY cancellableChanged)
    Q_PROPERTY(QString libraryId READ libraryId NOTIFY busyChanged)
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    // "Scanning rekordbox" while reading, then "Creating Engine Library"
    // while writing -- both real, ticking phases, not a static message,
    // so a library of any real size doesn't look indistinguishable from
    // a hang (see EngineLibraryCreator::create()'s own progress-reporter
    // parameter for why the write side in particular needs this).
    Q_PROPERTY(QString currentPhase READ currentPhase NOTIFY currentPhaseChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit EngineLibraryCreatorController(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    bool cancellable() const;
    QString libraryId() const { return m_libraryId; }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    QString currentPhase() const { return m_currentPhase; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }

    // rekordboxPath: the stick's PIONEER root, scanned fresh (so the
    // library created reflects the current state of the rekordbox
    // export, not a stale earlier scan). schemaGeneration: 0=V1, 1=V2,
    // 2=V3 -- see infrastructure::engine::EngineSchemaGeneration; exposed
    // as a plain int since QML enums would need their own registration
    // for a value used nowhere else in this app.
    // stickLabel only names this instance in the lock cookie other
    // instances see.
    Q_INVOKABLE void create(const QString &rekordboxPath, int schemaGeneration, const QString &stickLabel = {});
    // Stops the build before the next track; nothing is left on the stick.
    Q_INVOKABLE void cancelWrite();

signals:
    void busyChanged();
    void cancellableChanged();
    // {written, total, unit, verb, cancelled, error, detail}, the
    // OperationSummaryDialog shape.
    void writeFinished(const QVariantMap &summary);
    // Another instance is editing this library; nothing was created.
    void lockRefused(const QVariantMap &holder);
    void scanProgressChanged();
    void currentPhaseChanged();
    void errorMessageChanged();
    void statusMessageChanged();

private:
    void onCreateFinished();
    void setBusy(bool busy);
    void setScanProgress(int current, int total);
    void setCurrentPhase(const QString &phase);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);
    std::shared_ptr<QtProgressReporter> makeReporter();

    QFutureWatcher<EngineLibraryCreationTaskResult> m_watcher;
    bool m_busy = false;
    bool m_holdsDirectWrite = false;
    QString m_libraryId;
    application::CancellationToken m_cancel;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    // Scan, then create, then copy-to-stick share one continuous bar
    // rather than each restarting from 0 -- see makeReporter()'s own
    // comment. Reset to 0 at the start of create(), not per phase.
    int m_phaseBaseline = 0;
    int m_currentPhaseTotal = 0;
    QString m_currentPhase;
    QString m_errorMessage;
    QString m_statusMessage;
};

}  // namespace seabass::gui
