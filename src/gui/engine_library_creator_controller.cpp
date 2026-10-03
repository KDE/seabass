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
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using infrastructure::engine::EngineLibraryCreator;
using infrastructure::engine::EngineSchemaGeneration;

namespace
{

EngineSchemaGeneration schemaFromInt(int value)
{
    switch (value) {
    case 0: return EngineSchemaGeneration::V1;
    case 2: return EngineSchemaGeneration::V3;
    default: return EngineSchemaGeneration::V2;
    }
}

// Runs entirely on a background thread (see
// EngineLibraryCreatorController::create()) -- no access to the
// controller itself.
EngineLibraryCreationTaskResult runCreateTask(QString rekordboxPath, int schemaGeneration,
                                               std::shared_ptr<QtProgressReporter> reporter,
                                               application::CancellationToken cancel)
{
    EngineLibraryCreationTaskResult result;
    // One bar (#58): the rekordbox read the cache still has to make, one
    // unit per track created, and the copy to the stick. The playlists
    // stretch is a few dozen units nobody can count before the read and
    // is left out of the plan; it runs the bar a moment early.
    auto &cache = LibraryCatalogCache::instance();
    const auto readUnits = cache.plannedUnits("rekordbox", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Full);
    const auto rows = cache.countTracks("rekordbox", rekordboxPath.toStdString());
    application::PhasedProgress progress(*reporter, "Creating the Engine library",
                                         readUnits && rows ? *readUnits + *rows + 1 : 0);
    try {
        auto tracks = cache.tracksFor("rekordbox", rekordboxPath.toStdString(), progress, cancel);

        std::string engineLibraryPath =
            pathToUtf8(pathFromQString(rekordboxPath).parent_path() / "Engine Library");
        // Invalidated before create() runs, not just on success: a
        // partially-failed create() may still have written real files at
        // this exact path, and a stale "engine" entry from a prior
        // library that once lived here (and was since deleted) must not
        // keep being served either way.
        LibraryCatalogCache::instance().invalidate("engine", engineLibraryPath);
        // The sequence of the export the tracks came from, recorded in
        // the new library so a player does not offer to import it all
        // over again on first insert (issue #42).
        const auto rekordbox = infrastructure::engine::readRekordboxImportState({}, rekordboxPath.toStdString());
        auto creation = EngineLibraryCreator::create(
            engineLibraryPath, tracks, schemaFromInt(schemaGeneration), progress, cancel,
            rekordbox.hasRekordboxLibrary ? std::optional<std::uint64_t>(rekordbox.librarySequence) : std::nullopt);

        result.tracksCreated = creation.tracksCreated;
        result.tracksSkipped = creation.tracksSkipped;
        result.cuesCopied = creation.cuesCopied;
        result.tracksTotal = creation.tracksTotal;
        result.cancelled = creation.cancelled;
        result.importNotRecorded = rekordbox.hasRekordboxLibrary && !creation.rekordboxImportRecorded;
        if (!creation.errorMessage.empty()) {
            result.errorMessage = QString::fromStdString(creation.errorMessage);
        }
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;  // during the scan; nothing was written
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

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
    m_watcher.setFuture(QtConcurrent::run(
        [keepAwake, rekordboxPath, schemaGeneration, reporter = makeReporter(), cancel = m_cancel]() {
            return runCreateTask(rekordboxPath, schemaGeneration, reporter, cancel);
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
        QString said = QString("Created %1 track(s) (%2 skipped, no local file), copied %3 cue(s).")
                           .arg(result.tracksCreated)
                           .arg(result.tracksSkipped)
                           .arg(result.cuesCopied);
        if (result.importNotRecorded) {
            // Said here rather than nowhere: the first sign of it would
            // otherwise be a player offering to overwrite this library.
            said += QStringLiteral(" The new library could not record that it came from this rekordbox export, so a "
                                   "Denon player may offer to import the rekordbox library over it. Library Health "
                                   "can mark it imported.");
        }
        setStatusMessage(said);
    }
    emit writeFinished(QVariantMap{
        {"written", result.cancelled ? 0 : result.tracksCreated},
        {"total", result.tracksTotal},
        {"unit", QStringLiteral("tracks")},
        {"verb", QStringLiteral("created")},
        {"cancelled", result.cancelled},
        {"error", result.errorMessage},
        {"detail", result.cancelled ? QStringLiteral("Stopped at your request. Nothing was created on the stick.")
                                    : QString()},
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
