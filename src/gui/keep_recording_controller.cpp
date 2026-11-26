// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/keep_recording_controller.hpp"

#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "gui/detached_completion.hpp"
#include "gui/future_result.hpp"
#include "gui/main_thread_shared.hpp"
#include "gui/qt_path.hpp"
#include "gui/sleep_inhibitor.hpp"
#include "infrastructure/local/local_keep_recording_files.hpp"

#if defined(SEABASS_HAVE_RECORDING_ENCODER)
#include "infrastructure/audio/qt_cover_art_preparer.hpp"
#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
#include "infrastructure/audio/taglib_recording_tag_writer.hpp"
#endif

namespace seabass::gui
{

namespace
{

using application::KeepRecordingResult;

// For a build without the encoder's library: nothing is kept, and the
// page does not offer it (available()).
class NoCovers : public application::CoverArtPreparer
{
public:
    application::CoverArtResult prepare(const std::string &) override
    {
        return {std::nullopt, "this build of Seabass cannot embed covers"};
    }
};

class NoTags : public application::RecordingTagWriter
{
public:
    application::RecordingTagWriteResult write(const std::string &, const application::RecordingTags &) override
    {
        return {false, "this build of Seabass cannot write tags"};
    }
};

std::function<KeepRecordingParts()> &partsForTesting()
{
    static std::function<KeepRecordingParts()> make;
    return make;
}

KeepRecordingParts makeParts()
{
    if (partsForTesting()) {
        return partsForTesting()();
    }
    KeepRecordingParts parts;
    parts.files = std::make_unique<infrastructure::LocalKeepRecordingFiles>();
#if defined(SEABASS_HAVE_RECORDING_ENCODER)
    parts.encoder = std::make_unique<infrastructure::audio::QtFlacRecordingEncoder>();
    parts.covers = std::make_unique<infrastructure::audio::QtCoverArtPreparer>();
    parts.tags = std::make_unique<infrastructure::audio::TagLibRecordingTagWriter>();
#else
    parts.encoder = std::make_unique<application::NullRecordingEncoder>();
    parts.covers = std::make_unique<NoCovers>();
    parts.tags = std::make_unique<NoTags>();
#endif
    return parts;
}

QString stateKey(KeepRecordingResult::Status status)
{
    switch (status) {
        case KeepRecordingResult::Status::Kept:
            return QStringLiteral("kept");
        case KeepRecordingResult::Status::Cancelled:
            return QStringLiteral("cancelled");
        case KeepRecordingResult::Status::Failed:
            return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

QString defaultFolder()
{
    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty()) {
        music = QDir::homePath();
    }
    return QDir(music).filePath(QStringLiteral("Own Recordings"));
}

}  // namespace

void KeepRecordingController::setPartsForTesting(std::function<KeepRecordingParts()> make)
{
    partsForTesting() = std::move(make);
}

KeepRecordingController::KeepRecordingController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<KeepRecordingResult>::finished, this, &KeepRecordingController::onJobFinished);
    connect(&m_coverWatcher, &QFutureWatcher<CoverPreview>::finished, this, &KeepRecordingController::onCoverPrepared);
}

KeepRecordingController::~KeepRecordingController()
{
    m_queue.clear();
    // A cover preview only reads and scales one picture: waited for.
    awaitQuietly(m_coverWatcher);
    if (!m_running) {
        return;
    }
    m_cancel.cancel();
    QDeadlineTimer deadline(KeepRecordingStopWaitMs);
    while (!m_future.isFinished() && !deadline.hasExpired()) {
        QThread::msleep(5);
    }
    if (!m_future.isFinished()) {
        // In the tag writer's copy, which cannot be interrupted: the worker
        // owns all it uses and removes its FLAC when the copy returns. The
        // application knows a write is still out (DetachedWrites) and the
        // global pool is waited for at exit.
        whenWriteEnds(m_future, {});
    }
}

bool KeepRecordingController::available() const
{
    if (partsForTesting()) {
        return true;
    }
#if defined(SEABASS_HAVE_RECORDING_ENCODER)
    static const bool can = infrastructure::audio::QtFlacRecordingEncoder::encodingAvailable();
    return can;
#else
    return false;
#endif
}

QString KeepRecordingController::currentWavPath() const
{
    return m_running ? m_running->wavPath : QString();
}

QString KeepRecordingController::currentRecording() const
{
    return m_running ? m_running->recording : QString();
}

QString KeepRecordingController::currentFileName() const
{
    return m_running ? QString::fromStdString(m_running->request.fileName) : QString();
}

QString KeepRecordingController::currentFolder() const
{
    return m_running ? QString::fromStdString(m_running->request.folder) : QString();
}

QVariantList KeepRecordingController::queued() const
{
    QVariantList out;
    for (const Job &job : m_queue) {
        out.append(QVariantMap{
            {"wavPath", job.wavPath},
            {"recording", job.recording},
            {"fileName", QString::fromStdString(job.request.fileName)},
            {"folder", QString::fromStdString(job.request.folder)},
        });
    }
    return out;
}

bool KeepRecordingController::canKeep(const QString &path) const
{
    return localPath(path).endsWith(QLatin1String(".wav"), Qt::CaseInsensitive);
}

QVariantMap KeepRecordingController::suggestionsFor(const QString &recordingFileName, const QDateTime &recorded) const
{
    const QSettings settings;
    QString folder = settings.value(QLatin1String(KeepRecordingFolderKey)).toString();
    if (folder.isEmpty()) {
        folder = defaultFolder();
    }
    return {
        {"artist", settings.value(QLatin1String(KeepRecordingArtistKey)).toString()},
        {"title", QString::fromStdString(application::suggestedRecordingTitle(recordingFileName.toStdString()))},
        {"album", QStringLiteral("Own Recordings")},
        {"genre", settings.value(QLatin1String(KeepRecordingGenreKey)).toString()},
        {"year", recorded.isValid() ? recorded.date().year() : 0},
        {"folder", QDir::toNativeSeparators(folder)},
    };
}

QString KeepRecordingController::fileNameFor(const QString &artist, const QString &title) const
{
    return QString::fromStdString(
        application::keptRecordingFileName(artist.trimmed().toStdString(), title.trimmed().toStdString()));
}

QString KeepRecordingController::localPath(const QString &pathOrUrl) const
{
    if (pathOrUrl.startsWith(QLatin1String("file:"))) {
        return QUrl(pathOrUrl).toLocalFile();
    }
    return pathOrUrl;
}

QUrl KeepRecordingController::folderUrl(const QString &folder) const
{
    return QUrl::fromLocalFile(QDir::fromNativeSeparators(localPath(folder)));
}

QString KeepRecordingController::folderProblem(const QString &folderText) const
{
    const QString folder = localPath(folderText.trimmed());
    if (folder.isEmpty()) {
        return tr("Choose a folder for the file.");
    }
    if (QDir::isRelativePath(QDir::fromNativeSeparators(folder))) {
        return tr("Choose a folder by its full path.");
    }
    // The nearest part of the path that is there: the folder itself, or
    // the one it is to be made in.
    QFileInfo info(folder);
    while (!info.exists()) {
        const QString parent = info.absolutePath();
        if (parent == info.absoluteFilePath()) {
            return tr("This folder cannot be made.");
        }
        info = QFileInfo(parent);
        if (info.isFile()) {
            return tr("A file is in the way of this folder.");
        }
    }
    if (!info.isDir()) {
        return tr("This is a file, not a folder.");
    }
    if (!info.isWritable()) {
        return tr("This folder cannot be written to.");
    }
    return {};
}

QString KeepRecordingController::targetProblem(const QString &folderText, const QString &artist,
                                               const QString &title) const
{
    const QString folder = QDir::fromNativeSeparators(localPath(folderText.trimmed()));
    const QString fileName = fileNameFor(artist, title);
    const QString target = QDir(folder).filePath(fileName);
    const auto sameTarget = [&target](const Job &job) {
        return QDir::cleanPath(targetOf(job.request)) == QDir::cleanPath(target);
    };
    if ((m_running && sameTarget(*m_running)) || std::any_of(m_queue.begin(), m_queue.end(), sameTarget)) {
        return tr("Another recording is being kept as %1 in this folder. Change the artist or the track name.")
            .arg(fileName);
    }
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
        return tr("%1 is in this folder already. Change the artist or the track name: nothing is ever replaced.")
            .arg(fileName);
    }
    return {};
}

QString KeepRecordingController::yearProblem(const QString &yearText) const
{
    const QString year = yearText.trimmed();
    if (year.isEmpty()) {
        return {};
    }
    bool ok = false;
    const int value = year.toInt(&ok);
    if (!ok || value < 1900 || value > 2100) {
        return tr("The year is a number from 1900 to 2100, or empty.");
    }
    return {};
}

QString KeepRecordingController::targetOf(const application::KeepRecordingRequest &request)
{
    return QDir(QString::fromStdString(request.folder)).filePath(QString::fromStdString(request.fileName));
}

void KeepRecordingController::pickCover(const QString &pathOrUrl)
{
    const QString path = localPath(pathOrUrl);
    m_coverPath = path;
    m_coverPreview.clear();
    m_coverWidth = 0;
    m_coverHeight = 0;
    m_coverError.clear();
    m_coverPreparing = !path.isEmpty();
    emit coverChanged();
    if (path.isEmpty()) {
        return;  // a preview still under way is ignored when it ends
    }
    m_coverWatcher.setFuture(QtConcurrent::run([path]() {
        CoverPreview preview;
        preview.path = path;
        KeepRecordingParts parts = makeParts();
        const std::optional<std::string> bytes =
            parts.files->readFile(pathToUtf8(pathFromQString(path)), application::MaxCoverPictureBytes);
        if (!bytes) {
            preview.error = QObject::tr("This picture could not be read.");
            return preview;
        }
        const application::CoverArtResult cover = parts.covers->prepare(*bytes);
        if (!cover.cover) {
            preview.error = QObject::tr("This picture cannot be used: %1").arg(QString::fromStdString(cover.error));
            return preview;
        }
        const QByteArray jpeg(cover.cover->jpeg.data(), static_cast<qsizetype>(cover.cover->jpeg.size()));
        preview.dataUrl = QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64());
        preview.width = cover.cover->width;
        preview.height = cover.cover->height;
        return preview;
    }));
}

void KeepRecordingController::onCoverPrepared()
{
    if (m_coverWatcher.future().resultCount() == 0 && !m_coverWatcher.future().isValid()) {
        return;
    }
    QString thrown;
    const CoverPreview preview = takeResult(m_coverWatcher, &thrown);
    if (preview.path != m_coverPath) {
        return;  // a later pick is under way
    }
    m_coverPreparing = false;
    m_coverPreview = preview.dataUrl;
    m_coverWidth = preview.width;
    m_coverHeight = preview.height;
    m_coverError = thrown.isEmpty() ? preview.error : thrown;
    emit coverChanged();
}

bool KeepRecordingController::keep(const QVariantMap &form)
{
    const QString wavPath = localPath(form.value("wavPath").toString());
    const QString title = form.value("title").toString().trimmed();
    const QString artist = form.value("artist").toString().trimmed();
    const QString folder = QDir::fromNativeSeparators(localPath(form.value("folder").toString().trimmed()));
    const QString yearText = form.value("year").toString().trimmed();
    if (wavPath.isEmpty()) {
        setError(tr("No recording was chosen."));
        return false;
    }
    if (title.isEmpty()) {
        setError(tr("The track name is needed."));
        return false;
    }
    for (const QString &problem : {yearProblem(yearText), folderProblem(folder), targetProblem(folder, artist, title)}) {
        if (!problem.isEmpty()) {
            setError(problem);
            return false;
        }
    }
    if (m_coverPreparing || !m_coverError.isEmpty()) {
        setError(m_coverPreparing ? tr("The cover is still being prepared.") : m_coverError);
        return false;
    }
    if (m_running && m_running->wavPath == wavPath) {
        setError(tr("This recording is being kept right now."));
        return false;
    }
    setError({});

    Job job;
    job.id = m_nextJob++;
    job.wavPath = wavPath;
    job.recording = form.value("recording").toString();
    if (job.recording.isEmpty()) {
        job.recording = QFileInfo(wavPath).fileName();
    }
    job.request.wavPath = pathToUtf8(pathFromQString(wavPath));
    job.request.folder = pathToUtf8(pathFromQString(folder));
    job.request.fileName = fileNameFor(artist, title).toStdString();
    job.request.tags.artist = artist.toStdString();
    job.request.tags.title = title.toStdString();
    job.request.tags.album = form.value("album").toString().trimmed().toStdString();
    job.request.tags.genre = form.value("genre").toString().trimmed().toStdString();
    job.request.tags.year = yearText.isEmpty() ? 0 : yearText.toInt();
    job.request.coverPicturePath = m_coverPath.isEmpty() ? std::string() : pathToUtf8(pathFromQString(m_coverPath));

    QSettings settings;
    settings.setValue(QLatin1String(KeepRecordingArtistKey), artist);
    settings.setValue(QLatin1String(KeepRecordingGenreKey), form.value("genre").toString().trimmed());
    settings.setValue(QLatin1String(KeepRecordingFolderKey), QDir::toNativeSeparators(folder));

    m_outcomes.insert(wavPath, QVariantMap{
                                   {"state", QStringLiteral("queued")},
                                   {"fileName", QString::fromStdString(job.request.fileName)},
                                   {"folder", QDir::toNativeSeparators(folder)},
                               });
    m_queue.push_back(std::move(job));
    emit queueChanged();
    bumpRevision();
    if (!m_running) {
        startNext();
    }
    return true;
}

void KeepRecordingController::startNext()
{
    if (m_running || m_queue.empty()) {
        return;
    }
    m_running = std::move(m_queue.front());
    m_queue.pop_front();
    m_cancel = application::CancellationToken();
    m_progress = 0.0;
    m_progressStep = QStringLiteral("space");
    if (!m_lastFinished.isEmpty()) {
        m_lastFinished.clear();
        emit lastFinishedChanged();
    }
    QVariantMap outcome = m_outcomes.value(m_running->wavPath);
    outcome.insert("state", QStringLiteral("running"));
    m_outcomes.insert(m_running->wavPath, outcome);

    auto relay = makeMainThreadShared<KeepRecordingProgressRelay>();
    connect(relay.get(), &KeepRecordingProgressRelay::progressed, this,
            [this](int jobId, const QString &step, double fraction) {
                if (!m_running || m_running->id != jobId) {
                    return;
                }
                const double next = std::max(m_progress, std::clamp(fraction, 0.0, 1.0));
                if (next == m_progress && step == m_progressStep) {
                    return;
                }
                m_progress = next;
                m_progressStep = step;
                emit progressChanged();
            });

    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Keeping a recording"));
    m_future = QtConcurrent::run([keepAwake, relay, request = m_running->request, jobId = m_running->id,
                                  cancel = m_cancel]() {
        KeepRecordingParts parts = makeParts();
        // Told on when the step changes or the bar moves by a step of its
        // own: the encoder reports every piece of a long recording.
        QString lastStep;
        double lastFraction = -1.0;
        return application::keepRecording(
            request, *parts.files, *parts.encoder, *parts.covers, *parts.tags, cancel,
            [&](const application::KeepRecordingProgress &p) {
                const QString step = QString::fromLatin1(application::keepRecordingStepKey(p.step));
                if (step == lastStep && p.fraction < 1.0 && p.fraction - lastFraction < 0.002) {
                    return;
                }
                lastStep = step;
                lastFraction = p.fraction;
                emit relay->progressed(jobId, step, p.fraction);
            });
    });
    m_watcher.setFuture(m_future);
    emit busyChanged();
    emit progressChanged();
    emit queueChanged();
    bumpRevision();
}

void KeepRecordingController::cancel()
{
    const bool hadQueue = !m_queue.empty();
    for (const Job &job : m_queue) {
        m_outcomes.remove(job.wavPath);
    }
    m_queue.clear();
    if (hadQueue) {
        emit queueChanged();
        bumpRevision();
    }
    if (m_running && !m_cancel.cancelled()) {
        m_cancel.cancel();
        emit busyChanged();  // cancelRequested
    }
}

void KeepRecordingController::onJobFinished()
{
    if (!m_running) {
        return;
    }
    QString thrown;
    KeepRecordingResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.status = KeepRecordingResult::Status::Failed;
        result.message = thrown.toStdString();
    }
    const Job job = std::move(*m_running);
    m_running.reset();

    const QString folder = QDir::toNativeSeparators(QString::fromStdString(job.request.folder));
    QVariantMap outcome{
        {"state", stateKey(result.status)},
        {"wavPath", job.wavPath},
        {"recording", job.recording},
        {"fileName", QString::fromStdString(job.request.fileName)},
        {"folder", folder},
        {"flacPath", result.status == KeepRecordingResult::Status::Kept
                         ? QDir::toNativeSeparators(QString::fromStdString(result.flacPath))
                         : QString()},
        {"message", QString::fromStdString(result.message)},
    };
    if (result.status == KeepRecordingResult::Status::Cancelled && result.message.empty()) {
        // Nothing to show on the row: the person asked for it.
        m_outcomes.remove(job.wavPath);
    } else {
        m_outcomes.insert(job.wavPath, outcome);
    }
    m_progressStep.clear();
    if (result.status == KeepRecordingResult::Status::Kept) {
        m_progress = 1.0;
    }
    m_lastFinished = outcome;
    emit progressChanged();
    emit busyChanged();
    emit lastFinishedChanged();
    bumpRevision();
    emit finished(outcome);
    startNext();
}

QVariantMap KeepRecordingController::outcomeFor(const QString &wavPath) const
{
    return m_outcomes.value(localPath(wavPath));
}

void KeepRecordingController::dismissFinished()
{
    if (!m_lastFinished.isEmpty()) {
        m_lastFinished.clear();
        emit lastFinishedChanged();
    }
}

void KeepRecordingController::showInFolder(const QString &flacPath) const
{
    const QString path = localPath(flacPath);
    if (path.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void KeepRecordingController::setError(const QString &message)
{
    if (m_errorMessage != message) {
        m_errorMessage = message;
        emit errorMessageChanged();
    }
}

void KeepRecordingController::bumpRevision()
{
    ++m_revision;
    emit revisionChanged();
}

}  // namespace seabass::gui
