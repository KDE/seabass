#include "anonymize_library_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <optional>
#include <system_error>

#include "application/use_cases/anonymize_library.hpp"

namespace seabass::gui
{

namespace
{

// Runs entirely on a background thread (see
// AnonymizeLibraryController::run()) -- no access to the controller.
AnonymizeLibraryTaskResult runAnonymizeTask(QString rekordboxPath, QString enginePath, QString outDir, int maxTracks,
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
        if (maxTracks > 0) {
            options.maxTracks = static_cast<size_t>(maxTracks);
        }
        options.hardware = hardware.toStdString();
        options.notes = notes.toStdString();

        application::AnonymizeLibrary useCase;
        auto summary = useCase.execute(rekordboxRoot, engineRoot, outDir.toStdString(), options, *reporter);

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
            QString line = QString("rekordbox: kept %1 track(s)").arg(summary.rekordboxTracksKept);
            if (summary.rekordboxTracksDropped > 0) {
                line += QString(", dropped %1").arg(summary.rekordboxTracksDropped);
            }
            line += QString("; renamed %1 artist(s), %2 playlist(s)/folder(s)")
                        .arg(summary.rekordboxArtistsRenamed)
                        .arg(summary.rekordboxPlaylistsRenamed);
            lines << line;
        }
        if (summary.engineAttempted && summary.engineError.empty()) {
            QString line = QString("Engine: kept %1 track(s)").arg(summary.engineTracksKept);
            if (summary.engineTracksDropped > 0) {
                line += QString(", dropped %1").arg(summary.engineTracksDropped);
            }
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
    auto reporter = std::make_shared<QtProgressReporter>();
    // Two sequential phases (rekordbox, then Engine) previously each called
    // setProgress(0, total) here, so the bar visibly restarted from 0% at
    // the halfway point -- looked like the whole operation had reset, not
    // like phase two beginning. Folding the previous phase's total into a
    // running baseline (only ever grows, reset just once in run()) keeps
    // the bar moving forward through both phases as one continuous run.
    connect(reporter.get(), &QtProgressReporter::started, this, [this](const QString &label, int total) {
        setCurrentPhase(label);
        m_phaseBaseline += m_currentPhaseTotal;
        m_currentPhaseTotal = total;
        setProgress(m_phaseBaseline, m_phaseBaseline + total);
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setProgress(m_phaseBaseline + current, m_progressTotal); });
    return reporter;
}

void AnonymizeLibraryController::run(const QString &rekordboxPath, const QString &enginePath, const QString &outPath,
                                      int maxTracks, const QString &hardware, const QString &notes)
{
    if (m_busy) {
        return;
    }
    // The UI asks for the zip by name. The use case stages into a
    // directory and zips that directory to <dir>.zip, so the staging
    // directory is the requested path with the suffix taken off -- which
    // keeps the zip exactly where the user pointed, rather than one
    // directory beside it.
    std::filesystem::path requested(outPath.toStdString());
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
    const QString outDir = QString::fromStdString(requested.string());
    setErrorMessage({});
    m_summaryText.clear();
    m_manifestText.clear();
    m_outputZipPath.clear();
    emit resultChanged();
    m_phaseBaseline = 0;
    m_currentPhaseTotal = 0;
    setProgress(0, 0);
    setBusy(true);
    m_watcher.setFuture(
        QtConcurrent::run(runAnonymizeTask, rekordboxPath, enginePath, outDir, maxTracks, hardware, notes, makeReporter()));
}

void AnonymizeLibraryController::onRunFinished()
{
    AnonymizeLibraryTaskResult result = m_watcher.result();
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
