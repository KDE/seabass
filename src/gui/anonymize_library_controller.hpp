// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>

#include <memory>

#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

// Result of the background anonymization task, see
// AnonymizeLibraryController::run(). Built entirely on a worker thread,
// no access to the controller.
struct AnonymizeLibraryTaskResult
{
    bool succeeded = false;
    QString errorMessage;    // empty when succeeded
    QString summaryText;     // short human summary, same shape as seabass-cli anonymize's own printed summary
    QString manifestText;    // full contents of MANIFEST.txt (which lives inside outputZipPath, not as a loose file)
    QString outputZipPath;   // the single zip file the run produced
};

// Wraps application::AnonymizeLibrary for QML -- see that use case's own
// doc comment, and infrastructure::rekordbox::anonymizeRekordboxLibrary /
// infrastructure::engine::anonymizeEngineLibrary for exactly what gets
// kept/replaced/removed. Reachable from Settings (App Settings ->
// Experimental features), not an ActionCard: this is a maintainer/
// power-user tool (regenerating the project's own test fixture, or
// submitting a library to help test hardware Sebas doesn't have), not
// a per-stick everyday action.
//
// Never sends anything anywhere itself -- see docs/testing.md and the
// page's own confirmation text -- this only ever writes a single zip
// file alongside the location the user chooses.
class AnonymizeLibraryController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progressCurrent READ progressCurrent NOTIFY progressChanged)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged)
    Q_PROPERTY(QString currentPhase READ currentPhase NOTIFY currentPhaseChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString summaryText READ summaryText NOTIFY resultChanged)
    Q_PROPERTY(QString manifestText READ manifestText NOTIFY resultChanged)
    Q_PROPERTY(QString outputZipPath READ outputZipPath NOTIFY resultChanged)

public:
    explicit AnonymizeLibraryController(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    int progressCurrent() const { return m_progressCurrent; }
    int progressTotal() const { return m_progressTotal; }
    QString currentPhase() const { return m_currentPhase; }
    QString errorMessage() const { return m_errorMessage; }
    QString summaryText() const { return m_summaryText; }
    QString manifestText() const { return m_manifestText; }
    QString outputZipPath() const { return m_outputZipPath; }

    // rekordboxPath/enginePath: empty means "skip this catalog" (same as
    // omitting --rekordbox/--engine on seabass-cli anonymize). maxTracks
    // <= 0 means "no limit, keep every real track" -- there is no
    // spinbox "null" state in QML, so 0 is the UI's own stand-in, not
    // application::AnonymizationOptions::maxTracks' literal C++ meaning
    // (an *explicit* 0 there really would mean "keep none").
    Q_INVOKABLE void run(const QString &rekordboxPath, const QString &enginePath, const QString &outPath,
                          int maxTracks, const QString &hardware, const QString &notes);

signals:
    void busyChanged();
    void progressChanged();
    void currentPhaseChanged();
    void errorMessageChanged();
    void resultChanged();

private:
    void onRunFinished();
    void setBusy(bool busy);
    void setProgress(int current, int total);
    void setCurrentPhase(const QString &phase);
    void setErrorMessage(const QString &message);
    std::shared_ptr<QtProgressReporter> makeReporter();

    QFutureWatcher<AnonymizeLibraryTaskResult> m_watcher;
    bool m_busy = false;
    int m_progressCurrent = 0;
    int m_progressTotal = 0;
    // Two phases (rekordbox, then Engine) share one continuous bar rather
    // than each restarting from 0 -- see makeReporter()'s own comment.
    // Reset to 0 at the start of run(), not per phase.
    int m_phaseBaseline = 0;
    int m_currentPhaseTotal = 0;
    QString m_currentPhase;
    QString m_errorMessage;
    QString m_summaryText;
    QString m_manifestText;
    QString m_outputZipPath;
};

}  // namespace seabass::gui
