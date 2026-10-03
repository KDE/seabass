// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "anonymize_library_controller.hpp"

#include "gui/future_result.hpp"
#include "gui/main_thread_shared.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <optional>
#include <system_error>

#include "application/use_cases/anonymize_library.hpp"
#include "application/phased_progress.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace seabass::gui
{

namespace
{

// Runs entirely on a background thread (see
// AnonymizeLibraryController::run()) -- no access to the controller.
AnonymizeLibraryTaskResult runAnonymizeTask(QString rekordboxPath, QString enginePath, QString outDir,
                                             QString hardware, QString notes,
                                             std::shared_ptr<QtProgressReporter> reporter)
{
    AnonymizeLibraryTaskResult result;
    try {
        std::optional<std::string> rekordboxRoot;
        if (!rekordboxPath.trimmed().isEmpty()) {
            rekordboxRoot = rekordboxPath.trimmed().toStdString();
        }
        std::optional<std::string> engineRoot;
        if (!enginePath.trimmed().isEmpty()) {
            engineRoot = enginePath.trimmed().toStdString();
        }
        if (!rekordboxRoot && !engineRoot) {
            result.errorMessage = "Enter at least one of the rekordbox or Engine paths.";
            return result;
        }

        application::AnonymizationOptions options;
        options.hardware = hardware.toStdString();
        options.notes = notes.toStdString();

        // One bar (#58): a unit per rekordbox row, then per Engine row,
        // from the catalogs' own counts. The readers are the right
        // counters here, not the catalog cache: this reads the stick
        // afresh and never through the cache.
        std::optional<size_t> planned = 0;
        const auto add = [&planned](std::optional<size_t> units) {
            if (planned && units) {
                *planned += *units;
            } else {
                planned = std::nullopt;
            }
        };
        if (rekordboxRoot) {
            add(infrastructure::rekordbox::KaitaiRekordboxReader(*rekordboxRoot).countTracks());
        }
        if (engineRoot) {
            add(infrastructure::engine::LibdjinteropEngineReader(*engineRoot).countTracks());
        }
        application::PhasedProgress progress(*reporter, "Anonymizing the library", planned.value_or(0));
        application::AnonymizeLibrary useCase;
        auto summary = useCase.execute(rekordboxRoot, engineRoot, outDir.toStdString(), options, progress);

        result.succeeded = summary.succeeded();
        result.outputZipPath = QString::fromStdString(summary.outputZipPath);
        result.manifestText = QString::fromStdString(summary.manifestText);

        if (!result.succeeded) {
            QStringList errors;
            if (!summary.outputError.empty()) {
                errors << QString::fromStdString(summary.outputError);
            }
            if (summary.rekordboxAttempted && !summary.rekordboxError.empty()) {
                errors << "rekordbox: " + QString::fromStdString(summary.rekordboxError);
            }
            if (summary.engineAttempted && !summary.engineError.empty()) {
                errors << "Engine: " + QString::fromStdString(summary.engineError);
            }
            if (summary.verificationFailed) {
                // Built, checked against the promise the manifest makes,
                // found to still hold real data, and thrown away. Say so
                // plainly: the whole point of the check is that nobody
                // sends a leaking export believing it is clean.
                errors << "This export still held real data, so it was not written. Nothing was left "
                          "on disk. Please report this."
                       << QString::fromStdString(summary.verificationReport);
            }
            result.errorMessage = errors.join("\n");
        }

        QStringList lines;
        if (summary.rekordboxAttempted && summary.rekordboxError.empty()) {
            QString line = QString("rekordbox: anonymized %1 track(s)").arg(summary.rekordboxTracksAnonymized);
            line += QString("; renamed %1 artist(s), %2 playlist(s)/folder(s)")
                        .arg(summary.rekordboxArtistsRenamed)
                        .arg(summary.rekordboxPlaylistsRenamed);
            lines << line;
        }
        if (summary.engineAttempted && summary.engineError.empty()) {
            QString line = QString("Engine: anonymized %1 track(s)").arg(summary.engineTracksAnonymized);
            line += QString("; renamed %1 playlist(s)/folder(s)").arg(summary.enginePlaylistsRenamed);
            lines << line;
        }
        // The zip is what the person actually has, so it leads. The raw
        // figure follows in brackets for context rather than being the
        // headline number, and there is no estimate any more: this runs
        // after the file exists, so the size is measured.
        const double zippedMb = static_cast<double>(summary.finalZipBytes) / (1024.0 * 1024.0);
        const double outputMb = static_cast<double>(summary.outputSizeBytes) / (1024.0 * 1024.0);
        lines << QString("%1 MB zipped, %2 file(s) (%3 MB before compression)")
                     .arg(zippedMb, 0, 'f', 1)
                     .arg(summary.filesWritten)
                     .arg(outputMb, 0, 'f', 1);
        result.summaryText = lines.join("\n");
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

AnonymizeLibraryController::AnonymizeLibraryController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<AnonymizeLibraryTaskResult>::finished, this,
            &AnonymizeLibraryController::onRunFinished);
}

std::shared_ptr<QtProgressReporter> AnonymizeLibraryController::makeReporter()
{
    auto reporter = makeMainThreadShared<QtProgressReporter>();
    // One announcement for the whole run (#58): the task folds the two
    // anonymizers onto one bar (application::PhasedProgress), names each
    // as a phase, and the count only ever goes up.
    connect(reporter.get(), &QtProgressReporter::started, this, [this](const QString &label, int total) {
        setCurrentPhase(label);
        setProgress(0, total);
    });
    connect(reporter.get(), &QtProgressReporter::phaseChanged, this, [this](const QString &label) {
        setCurrentPhase(label);
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setProgress(current, m_progressTotal); });
    return reporter;
}

void AnonymizeLibraryController::run(const QString &rekordboxPath, const QString &enginePath, const QString &outPath,
                                      const QString &hardware, const QString &notes)
{
    if (m_busy) {
        return;
    }
    // The UI asks for the zip by name. The use case stages into a
    // directory and zips that directory to <dir>.zip, so the staging
    // directory is the requested path with the suffix taken off -- which
    // keeps the zip exactly where the user pointed, rather than one
    // directory beside it.
    std::filesystem::path requested = pathFromQString(outPath);
    if (requested.extension() == ".zip") {
        requested.replace_extension();
    }
    // The proposed location is <Seabass home>/testdata, which will not
    // exist the first time. Refusing over a missing parent directory
    // would be an odd thing to make a person fix by hand.
    std::error_code dirEc;
    if (requested.has_parent_path()) {
        std::filesystem::create_directories(requested.parent_path(), dirEc);
    }
    const QString outDir = pathToQString(requested);
    setErrorMessage({});
    m_summaryText.clear();
    m_manifestText.clear();
    m_outputZipPath.clear();
    emit resultChanged();
    setProgress(0, 0);
    setBusy(true);
    m_watcher.setFuture(
        QtConcurrent::run(runAnonymizeTask, rekordboxPath, enginePath, outDir, hardware, notes, makeReporter()));
}

void AnonymizeLibraryController::onRunFinished()
{
    QString thrown;
    AnonymizeLibraryTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }
    setBusy(false);
    if (!result.succeeded) {
        setErrorMessage(result.errorMessage.isEmpty() ? "Anonymization failed." : result.errorMessage);
        return;
    }
    m_summaryText = result.summaryText;
    m_manifestText = result.manifestText;
    m_outputZipPath = result.outputZipPath;
    emit resultChanged();
}

void AnonymizeLibraryController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void AnonymizeLibraryController::setProgress(int current, int total)
{
    if (m_progressCurrent == current && m_progressTotal == total) {
        return;
    }
    m_progressCurrent = current;
    m_progressTotal = total;
    emit progressChanged();
}

void AnonymizeLibraryController::setCurrentPhase(const QString &phase)
{
    if (m_currentPhase == phase) {
        return;
    }
    m_currentPhase = phase;
    emit currentPhaseChanged();
}

void AnonymizeLibraryController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
