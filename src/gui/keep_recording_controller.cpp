// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/keep_recording_controller.hpp"

#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBoundaryFinder>
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
#include "gui/seabass_settings.hpp"
#include "gui/sleep_inhibitor.hpp"
#include "infrastructure/audio/recording_temp_names.hpp"
#include "infrastructure/local/local_keep_recording_files.hpp"

#if defined(SEABASS_HAVE_RECORDING_ENCODER)
#include "infrastructure/audio/qt_cover_art_preparer.hpp"
#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
#include "infrastructure/audio/recording_file_steps.hpp"
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
    application::RecordingTagWriteResult tagInPlace(const application::VerifiedFlac &,
                                                    const application::RecordingTags &) override
    {
        return {false, "this build of Seabass cannot write tags", {}};
    }
};

std::function<KeepRecordingParts()> &partsForTesting()
{
    static std::function<KeepRecordingParts()> make;
    return make;
}

std::function<void(const char *, bool)> &stopWaitObserver()
{
    static std::function<void(const char *, bool)> observe;
    return observe;
}

// Waits for a worker's end up to KeepRecordingStopWaitMs; true when it
// ended. A test is told whether the wait ended on the worker's signal.
bool waitForWorker(WorkerEnd &end, const char *what)
{
    const QDeadlineTimer deadline(KeepRecordingStopWaitMs);
    const bool ended = end.waitUntil(deadline);
    if (stopWaitObserver()) {
        stopWaitObserver()(what, ended && !deadline.hasExpired());
    }
    return ended;
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

// Marks a worker's end when the worker returns, whatever it returns with.
class EndOnReturn
{
public:
    explicit EndOnReturn(std::shared_ptr<WorkerEnd> end) : m_end(std::move(end)) {}
    ~EndOnReturn() { m_end->markEnded(); }
    EndOnReturn(const EndOnReturn &) = delete;
    EndOnReturn &operator=(const EndOnReturn &) = delete;

private:
    std::shared_ptr<WorkerEnd> m_end;
};

// How many bytes the encoder's temporary name beside a FLAC adds to its
// name (recording_temp_names.hpp); recording_encoder_test proves the
// encoder's name is that long.
std::size_t tempNameExtraBytes()
{
    return infrastructure::audio::recordingTempNameExtraBytes();
}

// The WAV's identity in this session: its path, size and modification
// time, so that another recording that appears under the same path later
// (a stick plugged in again, another stick at the same mount point) is not
// taken for the one that was kept.
QString wavKey(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return path + QStringLiteral("|missing");
    }
    return path + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|')
        + QString::number(info.lastModified().toMSecsSinceEpoch());
}

// The longest start of a UTF-8 text, of at most `maxBytes`, that ends
// between two user-perceived characters (Unicode's grapheme clusters, as
// Qt finds them): never inside a UTF-8 sequence, a surrogate pair, a
// combining sequence or an emoji sequence.
std::size_t graphemeCut(std::string_view text, std::size_t maxBytes)
{
    const QString whole = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, whole);
    std::size_t best = 0;
    std::size_t bytes = 0;
    qsizetype last = 0;
    for (qsizetype at = finder.toNextBoundary(); at != -1; at = finder.toNextBoundary()) {
        bytes += static_cast<std::size_t>(whole.mid(last, at - last).toUtf8().size());
        if (bytes > maxBytes) {
            break;
        }
        best = bytes;
        last = at;
    }
    return best;
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

// A path as the filesystem compares it, for telling whether two jobs name
// the same file: the folder resolved where it is there (a link, "..",
// another spelling of it), the name as given, and the letters folded, as
// a filesystem that ignores case (the usual one on Windows and macOS, FAT
// and exFAT everywhere) takes them. Folded on every platform: two names
// that differ only in case are refused as one, which costs at most a
// rename in the form.
QString comparablePath(const QString &path)
{
    const QFileInfo info(QDir::cleanPath(QDir::fromNativeSeparators(path)));
    QString folder = QFileInfo(info.absolutePath()).canonicalFilePath();
    if (folder.isEmpty()) {
        folder = QDir::cleanPath(info.absolutePath());
    }
    return QDir(folder).filePath(info.fileName()).toCaseFolded();
}

QString defaultFolder()
{
    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty()) {
        music = QDir::homePath();
    }
    return QDir(music).filePath(QStringLiteral("Own Recordings"));
}

QString fieldName(const QString &field)
{
    if (field == QLatin1String("artist")) {
        return QObject::tr("The artist");
    }
    if (field == QLatin1String("title")) {
        return QObject::tr("The track name");
    }
    if (field == QLatin1String("album")) {
        return QObject::tr("The album");
    }
    return QObject::tr("The genre");
}

}  // namespace

void WorkerEnd::markEnded()
{
    const QMutexLocker lock(&m_mutex);
    m_done = true;
    m_ended.wakeAll();
}

bool WorkerEnd::ended()
{
    const QMutexLocker lock(&m_mutex);
    return m_done;
}

bool WorkerEnd::waitUntil(QDeadlineTimer deadline)
{
    QMutexLocker lock(&m_mutex);
    while (!m_done) {
        if (!m_ended.wait(&m_mutex, deadline)) {
            break;
        }
    }
    return m_done;
}

void KeepRecordingController::setPartsForTesting(std::function<KeepRecordingParts()> make)
{
    partsForTesting() = std::move(make);
}

void KeepRecordingController::setStopWaitObserverForTesting(std::function<void(const char *, bool)> observe)
{
    stopWaitObserver() = std::move(observe);
}

KeepRecordingController::KeepRecordingController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<KeepRecordingResult>::finished, this, &KeepRecordingController::onJobFinished);
    connect(&m_coverWatcher, &QFutureWatcher<CoverPreview>::finished, this, &KeepRecordingController::onCoverPrepared);
}

KeepRecordingController::~KeepRecordingController()
{
    m_queue.clear();
    // A cover preview only reads and scales one picture: waited for, as
    // long as a job is. One still under way after that is left to end on
    // its own: it uses nothing of this object, and the global pool is
    // waited for at exit.
    if (m_coverEnd && !m_coverEnd->ended()) {
        waitForWorker(*m_coverEnd, "cover");
    }
    if (!m_running) {
        return;
    }
    m_cancel.cancel();
    if (!waitForWorker(*m_jobEnd, "job")) {
        // In the tag writer, which cannot be interrupted: the worker owns
        // all it uses and removes its temporary file when it returns. The
        // application knows a write is still out (DetachedWrites) and the
        // global pool is waited for at exit.
        whenWriteEnds(m_future, {});
    }
}

void KeepRecordingController::setStickRoot(const QString &root)
{
    if (m_stickRoot == root) {
        return;
    }
    m_stickRoot = root;
    clearSessionRecords();
    emit stickRootChanged();
}

void KeepRecordingController::clearSessionRecords()
{
    // What the session knows of recordings is of the stick it was on; the
    // jobs that run or wait keep theirs, and record their end as usual.
    QSet<QString> inHand;
    if (m_running) {
        inHand.insert(m_running->wavKey);
    }
    for (const Job &job : m_queue) {
        inHand.insert(job.wavKey);
    }
    bool changed = !m_keptCopies.isEmpty();
    m_keptCopies.clear();
    for (auto it = m_outcomes.begin(); it != m_outcomes.end();) {
        if (inHand.contains(it.key())) {
            ++it;
        } else {
            it = m_outcomes.erase(it);
            changed = true;
        }
    }
    if (changed) {
        bumpRevision();
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
    const QSettings settings = openSeabassSettings();
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

QString KeepRecordingController::fileNameFor(const QString &artist, const QString &title, const QString &folderText) const
{
    // The room the folder's filesystem leaves; a filesystem that does not
    // say is taken as the common one.
    const QString folder = QDir::fromNativeSeparators(localPath(folderText.trimmed()));
    std::size_t longest = application::MaxFileNameBytes;
    if (!folder.isEmpty() && !QDir::isRelativePath(folder)) {
        infrastructure::LocalKeepRecordingFiles files;
        if (const std::optional<std::uint64_t> said = files.maxNameBytes(pathToUtf8(pathFromQString(folder)))) {
            longest = std::min<std::size_t>(longest, static_cast<std::size_t>(*said));
        }
    }
    const std::size_t stem = application::keptRecordingStemBytes(longest, tempNameExtraBytes());
    return QString::fromStdString(application::keptRecordingFileName(artist.trimmed().toStdString(),
                                                                     title.trimmed().toStdString(), stem, graphemeCut));
}

QString KeepRecordingController::textProblem(const QString &field, const QString &text) const
{
    if (application::hasControlCharacter(text.trimmed().toStdString())) {
        return tr("%1 cannot hold a line break, a tab or another control character.").arg(fieldName(field));
    }
    return {};
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
    // On the stick: the recordings page would list the FLAC as a
    // recording, and delete it with its WAV.
    if (m_stickRoot.isEmpty()) {
        return tr("It is not known which stick the recordings are on, so no folder can be chosen.");
    }
    infrastructure::LocalKeepRecordingFiles files;
    const std::optional<bool> onStick = files.isWithin(pathToUtf8(pathFromQString(QDir::fromNativeSeparators(folder))),
                                                       pathToUtf8(pathFromQString(m_stickRoot)));
    if (!onStick) {
        return tr("It cannot be told whether this folder is on the stick. Choose a folder on this computer.");
    }
    if (*onStick) {
        return tr("This folder is on the stick. The FLAC file goes onto this computer; the stick keeps the recording.");
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
    const QString fileName = fileNameFor(artist, title, folder);
    const QString target = QDir(folder).filePath(fileName);
    const QString key = comparablePath(target);
    const auto sameTarget = [&key](const Job &job) { return comparablePath(targetOf(job.request)) == key; };
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
    auto end = std::make_shared<WorkerEnd>();
    m_coverEnd = end;
    m_coverWatcher.setFuture(QtConcurrent::run([path, end]() {
        const EndOnReturn marks(end);
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

QString KeepRecordingController::formProblem(const QVariantMap &form) const
{
    if (m_deleteRunning) {
        return tr("Recordings are being deleted right now. Keep this one once that has ended.");
    }
    const QString wavPath = localPath(form.value("wavPath").toString());
    const QString title = form.value("title").toString().trimmed();
    const QString folder = QDir::fromNativeSeparators(localPath(form.value("folder").toString().trimmed()));
    if (wavPath.isEmpty()) {
        return tr("No recording was chosen.");
    }
    if (title.isEmpty()) {
        return tr("The track name is needed.");
    }
    for (const char *field : {"artist", "title", "album", "genre"}) {
        const QString problem = textProblem(QLatin1String(field), form.value(QLatin1String(field)).toString());
        if (!problem.isEmpty()) {
            return problem;
        }
    }
    for (const QString &problem : {yearProblem(form.value("year").toString()), folderProblem(folder),
                                   targetProblem(folder, form.value("artist").toString(), title)}) {
        if (!problem.isEmpty()) {
            return problem;
        }
    }
    if (m_coverPreparing || !m_coverError.isEmpty()) {
        return m_coverPreparing ? tr("The cover is still being prepared.") : m_coverError;
    }
    // One job per recording: a second one would encode it again beside
    // the first, under another name.
    const QString wavKey = comparablePath(wavPath);
    const auto sameWav = [&wavKey](const Job &job) { return comparablePath(job.wavPath) == wavKey; };
    if (m_running && sameWav(*m_running)) {
        return tr("This recording is being kept right now.");
    }
    if (std::any_of(m_queue.begin(), m_queue.end(), sameWav)) {
        return tr("This recording is waiting to be kept already.");
    }
    return {};
}

bool KeepRecordingController::keep(const QVariantMap &form)
{
    const QString problem = formProblem(form);
    if (!problem.isEmpty()) {
        setError(problem);
        return false;
    }
    setError({});

    const QString wavPath = localPath(form.value("wavPath").toString());
    const QString title = form.value("title").toString().trimmed();
    const QString artist = form.value("artist").toString().trimmed();
    const QString album = form.value("album").toString().trimmed();
    const QString genre = form.value("genre").toString().trimmed();
    const QString folder = QDir::fromNativeSeparators(localPath(form.value("folder").toString().trimmed()));
    const QString yearText = form.value("year").toString().trimmed();

    const bool hadWork = hasWork();
    Job job;
    job.id = m_nextJob++;
    job.wavPath = wavPath;
    job.recording = form.value("recording").toString();
    if (job.recording.isEmpty()) {
        job.recording = QFileInfo(wavPath).fileName();
    }
    job.request.wavPath = pathToUtf8(pathFromQString(wavPath));
    job.request.folder = pathToUtf8(pathFromQString(folder));
    job.request.fileName = fileNameFor(artist, title, folder).toStdString();
    job.request.tags.artist = artist.toStdString();
    job.request.tags.title = title.toStdString();
    job.request.tags.album = album.toStdString();
    job.request.tags.genre = genre.toStdString();
    job.request.tags.year = yearText.isEmpty() ? 0 : yearText.toInt();
    job.request.coverPicturePath = m_coverPath.isEmpty() ? std::string() : pathToUtf8(pathFromQString(m_coverPath));
    job.request.stickRoot = pathToUtf8(pathFromQString(m_stickRoot));
    job.request.tempNameExtraBytes = tempNameExtraBytes();
    job.request.keepUnfinished = form.value("keepUnfinished").toBool();
    job.wavKey = wavKey(wavPath);

    QSettings settings = openSeabassSettings();
    settings.setValue(QLatin1String(KeepRecordingArtistKey), artist);
    settings.setValue(QLatin1String(KeepRecordingGenreKey), genre);
    settings.setValue(QLatin1String(KeepRecordingFolderKey), QDir::toNativeSeparators(folder));

    m_outcomes.insert(job.wavKey, QVariantMap{
                                   {"state", QStringLiteral("queued")},
                                   {"recording", job.recording},
                                   {"fileName", QString::fromStdString(job.request.fileName)},
                                   {"folder", QDir::toNativeSeparators(folder)},
                               });
    m_queue.push_back(std::move(job));
    emit queueChanged();
    bumpRevision();
    if (!m_running) {
        startNext();
    }
    noteWork(hadWork);
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
    QVariantMap outcome = m_outcomes.value(m_running->wavKey);
    outcome.insert("state", QStringLiteral("running"));
    m_outcomes.insert(m_running->wavKey, outcome);

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
    auto end = std::make_shared<WorkerEnd>();
    m_jobEnd = end;
    m_future = QtConcurrent::run([keepAwake, relay, end, request = m_running->request, jobId = m_running->id,
                                  cancel = m_cancel]() {
        const EndOnReturn marks(end);
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
    const bool hadWork = hasWork();
    if (!m_queue.empty()) {
        // Each job that waited ends here, never started: its row and the
        // strip say so.
        for (const Job &job : m_queue) {
            const QVariantMap outcome{
                {"state", QStringLiteral("dropped")},
                {"wavPath", job.wavPath},
                {"recording", job.recording},
                {"fileName", QString::fromStdString(job.request.fileName)},
                {"folder", QDir::toNativeSeparators(QString::fromStdString(job.request.folder))},
                {"flacPath", QString()},
                {"leftBehind", QString()},
                {"message", QString()},
            };
            m_outcomes.insert(job.wavKey, outcome);
            m_finishedJobs.append(outcome);
        }
        m_queue.clear();
        emit queueChanged();
        emit finishedJobsChanged();
        bumpRevision();
    }
    if (m_running && !m_cancel.cancelled()) {
        m_cancel.cancel();
        emit busyChanged();  // cancelRequested
    }
    noteWork(hadWork);
}

void KeepRecordingController::onJobFinished()
{
    if (!m_running) {
        return;
    }
    const bool hadWork = hasWork();
    QString thrown;
    KeepRecordingResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.status = KeepRecordingResult::Status::Failed;
        result.message = thrown.toStdString();
    }
    const Job job = std::move(*m_running);
    m_running.reset();

    const QString folder = QDir::toNativeSeparators(QString::fromStdString(job.request.folder));
    const bool kept = result.status == KeepRecordingResult::Status::Kept;
    QStringList notes;
    for (const std::string &note : result.notes) {
        notes << QString::fromStdString(note);
    }
    const QString flacPath = kept ? QDir::toNativeSeparators(QString::fromStdString(result.flacPath)) : QString();
    const QVariantMap outcome{
        {"state", stateKey(result.status)},
        {"wavPath", job.wavPath},
        {"recording", job.recording},
        {"fileName", QString::fromStdString(job.request.fileName)},
        {"folder", folder},
        {"flacPath", flacPath},
        {"leftBehind", QDir::toNativeSeparators(QString::fromStdString(result.leftBehind))},
        {"message", QString::fromStdString(result.message)},
        {"notes", notes},
        {"unfinished", result.unfinished},
        {"keptSeconds", result.keptSeconds},
    };
    m_outcomes.insert(job.wavKey, outcome);
    if (kept) {
        // The FLAC as it was when the job ended: the delete confirmation
        // counts it as kept only while it is still there like this.
        const QFileInfo flac(QString::fromStdString(result.flacPath));
        QVariantMap copy = outcome;
        copy.insert("flacSize", flac.exists() ? flac.size() : qint64(-1));
        copy.insert("flacModified", flac.exists() ? flac.lastModified().toMSecsSinceEpoch() : qint64(-1));
        m_keptCopies.insert(job.wavKey, copy);
    }
    m_finishedJobs.append(outcome);
    m_progressStep.clear();
    if (kept) {
        m_progress = 1.0;
    }
    emit progressChanged();
    emit busyChanged();
    emit finishedJobsChanged();
    bumpRevision();
    emit finished(outcome);
    startNext();
    noteWork(hadWork);
}

QVariantMap KeepRecordingController::outcomeFor(const QString &wavPath) const
{
    return m_outcomes.value(wavKey(localPath(wavPath)));
}

QVariantMap KeepRecordingController::keepNotesFor(const QStringList &wavPaths) const
{
    QStringList notKept;
    QVariantList kept;
    QVariantList keptGone;
    for (const QString &given : wavPaths) {
        const QString path = localPath(given);
        const QString key = wavKey(path);
        if (m_keptCopies.contains(key)) {
            const QVariantMap copy = m_keptCopies.value(key);
            const QVariantMap named{
                {"recording", copy.value("recording")},
                {"fileName", copy.value("fileName")},
                {"folder", copy.value("folder")},
            };
            // Asked of the disk now: a FLAC moved, deleted, changed, or on
            // a drive that is gone is not a kept copy any more.
            const QFileInfo flac(copy.value("flacPath").toString());
            const bool stillThere = flac.exists() && flac.isFile()
                && flac.size() == copy.value("flacSize").toLongLong()
                && flac.lastModified().toMSecsSinceEpoch() == copy.value("flacModified").toLongLong();
            (stillThere ? kept : keptGone).append(named);
            continue;
        }
        const QVariantMap outcome = m_outcomes.value(key);
        if (!outcome.isEmpty()) {
            const QString name = outcome.value("recording").toString();
            notKept.append(name.isEmpty() ? QFileInfo(path).fileName() : name);
        }
    }
    return {{"notKept", notKept}, {"kept", kept}, {"keptGone", keptGone}};
}

void KeepRecordingController::dismissFinished()
{
    if (!m_finishedJobs.isEmpty()) {
        m_finishedJobs.clear();
        emit finishedJobsChanged();
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

void KeepRecordingController::noteWork(bool hadWork)
{
    if (hadWork != hasWork()) {
        emit hasWorkChanged();
    }
}

}  // namespace seabass::gui
