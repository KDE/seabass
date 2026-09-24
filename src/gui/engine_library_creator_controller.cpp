// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "engine_library_creator_controller.hpp"

#include "gui/future_result.hpp"
#include "gui/sleep_inhibitor.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/library_catalog_cache.hpp"
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
    try {
        auto tracks =
            LibraryCatalogCache::instance().tracksFor("rekordbox", rekordboxPath.toStdString(), *reporter, cancel);

        std::string engineLibraryPath =
            pathToUtf8(pathFromQString(rekordboxPath).parent_path() / "Engine Library");
        // Invalidated before create() runs, not just on success: a
        // partially-failed create() may still have written real files at
        // this exact path, and a stale "engine" entry from a prior
        // library that once lived here (and was since deleted) must not
        // keep being served either way.
        LibraryCatalogCache::instance().invalidate("engine", engineLibraryPath);
        // Same reporter as the scan above -- a second start()/tick() run
        // for this second phase, same idiom SyncController's own analyze
        // task uses, rather than leaving the bar looking stalled once the
        // scan's own 100% has already been reported.
        // The sequence of the export the tracks came from, recorded in
        // the new library so a player does not offer to import it all
        // over again on first insert (issue #42).
        const auto rekordbox = infrastructure::engine::readRekordboxImportState({}, rekordboxPath.toStdString());
        auto creation = EngineLibraryCreator::create(
            engineLibraryPath, tracks, schemaFromInt(schemaGeneration), *reporter, cancel,
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

EngineLibraryCreatorController::EngineLibraryCreatorController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<EngineLibraryCreationTaskResult>::finished, this,
            &EngineLibraryCreatorController::onCreateFinished);
}

std::shared_ptr<QtProgressReporter> EngineLibraryCreatorController::makeReporter()
{
    auto reporter = std::make_shared<QtProgressReporter>();
    // Scan, then create, then copy-to-stick each used to call
    // setScanProgress(0, total) here, so the bar visibly restarted from 0%
    // at every phase boundary -- looked like the operation kept resetting,
    // not progressing. Folding each finished phase's total into a running
    // baseline (only ever grows, reset just once in create()) keeps the
    // bar moving forward through every phase as one continuous run.
    connect(reporter.get(), &QtProgressReporter::started, this, [this](const QString &label, int total) {
        setCurrentPhase(label);
        emit cancellableChanged();
        m_phaseBaseline += m_currentPhaseTotal;
        m_currentPhaseTotal = total;
        setScanProgress(m_phaseBaseline, m_phaseBaseline + total);
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setScanProgress(m_phaseBaseline + current, m_scanTotal); });
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
    auto *registry = EditSessionRegistry::instance();
    m_libraryId = registry->libraryIdForPath(rekordboxPath);
    if (auto refusal = registry->enterDirectWrite(m_libraryId, stickLabel)) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder);
        }
        return;
    }
    m_holdsDirectWrite = true;
    setErrorMessage({});
    setStatusMessage({});
    m_phaseBaseline = 0;
    m_currentPhaseTotal = 0;
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
    if (m_holdsDirectWrite) {
        m_holdsDirectWrite = false;
        EditSessionRegistry::instance()->leaveDirectWrite(m_libraryId);
    }
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
