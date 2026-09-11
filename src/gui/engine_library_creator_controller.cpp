// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "engine_library_creator_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/library_catalog_cache.hpp"
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
            (fs::path(rekordboxPath.toStdString()).parent_path() / "Engine Library").string();
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
        auto creation = EngineLibraryCreator::create(engineLibraryPath, tracks, schemaFromInt(schemaGeneration),
                                                     *reporter, cancel);

        result.tracksCreated = creation.tracksCreated;
        result.tracksSkipped = creation.tracksSkipped;
        result.cuesCopied = creation.cuesCopied;
        result.tracksTotal = creation.tracksTotal;
        result.cancelled = creation.cancelled;
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
    m_watcher.setFuture(QtConcurrent::run(runCreateTask, rekordboxPath, schemaGeneration, makeReporter(), m_cancel));
}

void EngineLibraryCreatorController::onCreateFinished()
{
    EngineLibraryCreationTaskResult result = m_watcher.result();
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
        setStatusMessage(QString("Created %1 track(s) (%2 skipped, no local file), copied %3 cue(s).")
                             .arg(result.tracksCreated)
                             .arg(result.tracksSkipped)
                             .arg(result.cuesCopied));
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
