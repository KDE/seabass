// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "engine_library_creator_controller.hpp"

#include "gui/stick_path.hpp"
#include "gui/detached_write.hpp"
#include "gui/future_result.hpp"
#include "gui/main_thread_shared.hpp"
#include "gui/stick_events.hpp"
#include "gui/async_request.hpp"
#include "gui/sleep_inhibitor.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/library_catalog_cache.hpp"
#include "application/phased_progress.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/engine_cue_timeline.hpp"
#include "application/sync_analysis_policy.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

EngineLibraryCreatorController::~EngineLibraryCreatorController()
{
    // A write still running is not abandoned and not waited for: it runs
    // to its end, watched from the application, which gives its lock back
    // then. That is safe here, unlike for a stick backup: the worker owns
    // everything it touches (the path, the schema, a shared reporter) and
    // reaches into nothing of this object. It is not cancelled either:
    // whether the worker has reached the copy to the stick, where stopping
    // is not safe, is something this thread only learns from a signal
    // still in flight. When it ends, the stick list is told to look again,
    // which the page did on writeFinished and cannot any more.
    if (m_busy) {
        const QString stickRoot = stickRootOf(m_rekordboxPath);
        finishWriteDetached(m_watcher.future(), m_writeHold.handOver(), [stickRoot] {
            LibraryCatalogCache::instance().invalidateEveryCatalogOn(stickRoot.toStdString());
            StickEvents::instance().announceStickContentsChanged(stickRoot);
        });
    }
}

EngineLibraryCreatorController::EngineLibraryCreatorController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<EngineLibraryCreationTaskResult>::finished, this,
            &EngineLibraryCreatorController::onCreateFinished);
}

std::shared_ptr<QtProgressReporter> EngineLibraryCreatorController::makeReporter()
{
    auto reporter = makeMainThreadShared<QtProgressReporter>();
    // One announcement for the whole creation (#58): the task folds the
    // scan, the create and the copy onto one bar (application::
    // PhasedProgress), names each stretch as a phase, and the count only
    // ever goes up. The copy's phase is what cancellable() keys off.
    connect(reporter.get(), &QtProgressReporter::started, this, [this](const QString &label, int total) {
        setCurrentPhase(label);
        emit cancellableChanged();
        setScanProgress(0, total);
    });
    connect(reporter.get(), &QtProgressReporter::phaseChanged, this, [this](const QString &label) {
        setCurrentPhase(label);
        emit cancellableChanged();
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setScanProgress(current, m_scanTotal); });
    return reporter;
}

bool EngineLibraryCreatorController::cancellable() const
{
    return m_busy && !m_cancel.cancelled() && m_currentPhase != QStringLiteral("Copying to stick");
}

void EngineLibraryCreatorController::cancelWrite()
{
    if (!cancellable()) {
        return;
    }
    m_cancel.cancel();
    emit cancellableChanged();
}

void EngineLibraryCreatorController::create(const QString &rekordboxPath, int schemaGeneration,
                                            const QString &stickLabel)
{
    if (m_busy) {
        return;
    }
    // A direct write on the stick's library: the new folder joins it.
    m_libraryId = EditSessionRegistry::instance()->libraryIdForPath(rekordboxPath);
    m_rekordboxPath = rekordboxPath;
    if (auto refusal = m_writeHold.acquire({m_libraryId}, stickLabel)) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder);
        }
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    m_cancel = application::CancellationToken();
    setBusy(true);
    emit cancellableChanged();
    // Awake while the library is written: see SleepInhibitor.
    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Creating an Engine library on a USB stick"));
    // Preferences' copy of rekordbox's analysis, read here on the GUI
    // thread like the syncs read it when they stage (experimental: off
    // while experimental features are, see SyncAnalysisPolicy).
    application::AnalysisCopyOptions analysis;
    analysis.grids = application::SyncAnalysisPolicy::copyBeatGrids();
    analysis.waveforms = application::SyncAnalysisPolicy::copyWaveforms();
    m_watcher.setFuture(QtConcurrent::run(
        [keepAwake, rekordboxPath, schemaGeneration, analysis, reporter = makeReporter(), cancel = m_cancel]() {
            return runEngineLibraryCreation(rekordboxPath, schemaGeneration, analysis, *reporter, cancel);
        }));
}

void EngineLibraryCreatorController::onCreateFinished()
{
    QString thrown;
    EngineLibraryCreationTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }
    m_writeHold.release();
    setBusy(false);
    emit cancellableChanged();
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
    } else if (result.cancelled) {
        setStatusMessage(QStringLiteral("Cancelled, nothing was created."));
    } else {
        const auto count = [](int n, const char *one, const char *many) {
            return QString::number(n) + QLatin1Char(' ') + QLatin1String(n == 1 ? one : many);
        };
        QString said = count(result.tracksCreated, "track", "tracks") + QStringLiteral(" and ")
                       + count(result.cuesCopied, "cue", "cues") + QStringLiteral(" copied");
        if (result.tracksSkipped > 0) {
            said += QStringLiteral("; ") + count(result.tracksSkipped, "track", "tracks")
                    + QStringLiteral(" skipped (no local file)");
        }
        said += QLatin1Char('.');
        if (result.importNotRecorded) {
            // Said here rather than nowhere: the first sign of it would
            // otherwise be a player offering to overwrite this library.
            said += QStringLiteral(" The new library could not record that it came from this rekordbox export, so a "
                                   "Denon player may offer to import the rekordbox library over it. Library Health "
                                   "can mark it imported.");
        }
        const std::string leftOut = infrastructure::engine::describeLeadInShiftDrops(
            result.memoryCuesDroppedByLeadInShift, result.loopsDroppedByLeadInShift);
        if (!leftOut.empty()) {
            said += QStringLiteral(" ") + QString::fromStdString(leftOut) + QStringLiteral(".");
        }
        for (const QString &part : {result.analysisWarning, result.analysisNote}) {
            if (!part.isEmpty()) {
                said += QLatin1Char(' ') + part;
            }
        }
        setStatusMessage(said);
    }
    // The summary dialog's lines under the counts: the import marker that
    // could not be recorded, and cues the lead-in shift left out.
    const bool created = result.errorMessage.isEmpty() && !result.cancelled;
    QStringList detailLines;
    if (created) {
        if (result.importNotRecorded) {
            detailLines << QStringLiteral("The new library could not record that it came from this rekordbox export, "
                                          "so a Denon player may offer to import the rekordbox library over it. "
                                          "Library Health can mark it imported.");
        }
        const std::string leftOut = infrastructure::engine::describeLeadInShiftDrops(
            result.memoryCuesDroppedByLeadInShift, result.loopsDroppedByLeadInShift);
        if (!leftOut.empty()) {
            detailLines << QString::fromStdString(leftOut) + QStringLiteral(".");
        }
        if (!result.analysisWarning.isEmpty()) {
            detailLines << result.analysisWarning;
        }
    }
    const QString leftOutDetail = detailLines.join(QLatin1Char(' '));
    emit writeFinished(QVariantMap{
        // What the user can do now, ahead of the counts (#91).
        {"headline", created ? QStringLiteral("Engine library created.") : QString()},
        {"guidance", created ? QStringLiteral("Denon players can use this stick as it is, no export from Engine DJ "
                                              "needed. After you change cues or playlists in rekordbox, run Sync "
                                              "after Rekordbox Export to carry them over.")
                             : QString()},
        {"written", result.cancelled ? 0 : result.tracksCreated},
        {"total", result.tracksTotal},
        {"unit", QStringLiteral("tracks")},
        {"verb", QStringLiteral("created")},
        {"cancelled", result.cancelled},
        {"error", result.errorMessage},
        {"detail", result.cancelled ? QStringLiteral("Stopped at your request. Nothing was created on the stick.")
                                    : leftOutDetail},
        // What the copy of rekordbox's analysis did (Preferences), after
        // everything else, as a sync's save says it.
        {"notes", created ? result.analysisNote : QString()},
    });
}

void EngineLibraryCreatorController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void EngineLibraryCreatorController::setScanProgress(int current, int total)
{
    if (m_scanCurrent == current && m_scanTotal == total) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    emit scanProgressChanged();
}

void EngineLibraryCreatorController::setCurrentPhase(const QString &phase)
{
    if (m_currentPhase == phase) {
        return;
    }
    m_currentPhase = phase;
    emit currentPhaseChanged();
}

void EngineLibraryCreatorController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void EngineLibraryCreatorController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
