// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QDateTime>
#include <QDeadlineTimer>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QMutex>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QWaitCondition>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/keep_recording.hpp"

namespace seabass::gui
{

// Carries a job's progress from the worker to the controller. Lives on
// the GUI thread (made with makeMainThreadShared) and is emitted from the
// worker; the queued connection dies with the controller, so a page left
// mid-job is never called into.
class KeepRecordingProgressRelay : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

signals:
    void progressed(int job, const QString &step, double fraction);
};

// The end of a worker, waited for with a deadline: the worker marks it
// when it returns, and whoever waits wakes then, not later.
class WorkerEnd
{
public:
    void markEnded();
    // True when the worker has ended, at once or within `deadline`.
    bool waitUntil(QDeadlineTimer deadline);
    // Whether the worker has ended, now.
    bool ended();

private:
    QMutex m_mutex;
    QWaitCondition m_ended;
    bool m_done = false;
};

// The parts a job is done with, made on the worker for each job. Tests
// may hand in their own (setPartsForTesting).
struct KeepRecordingParts
{
    std::unique_ptr<application::KeepRecordingFiles> files;
    std::unique_ptr<application::RecordingEncoder> encoder;
    std::unique_ptr<application::CoverArtPreparer> covers;
    std::unique_ptr<application::RecordingTagWriter> tags;
};

// Keep this recording (Recordings page, experimental): a WAV the DJ
// hardware recorded onto the stick becomes a tagged FLAC with a cover in
// a folder on this computer. See application/use_cases/keep_recording.hpp
// for the rules of a job.
//
// One job at a time, on a worker, beside the page: a job asked for while
// one runs waits in a queue and starts when the one before it ends,
// whichever stick either is on. `progress` is the job in hand's, 0 to 1,
// and never goes back within it.
//
// The app has one, and every stick's Recordings page uses it: each job,
// outcome and kept copy carries the root of the stick its recording is
// on, and what a page asks is asked of its stick (hasWorkFor(),
// queuedFor(), finishedJobsFor(), outcomeFor(), keepNotesFor(),
// cancelFor(), dismissFinishedFor()). stickRoot is the stick the form is
// for: the one a keep() is of, and whose folders are refused.
//
// The form's suggestions (suggestionsFor()) come from the recording and
// from what was kept last (the settings store: artist, genre, folder).
// The form's checks are here too (fileNameFor(), textProblem(),
// folderProblem(), targetProblem()), and keep() asks them again before
// anything is queued. The folder is never on the stick the recordings
// are on (stickRoot, which RecordingsController sets).
//
// What happened to each recording asked for stays known for the session
// (outcomeFor(), keepNotesFor()), whichever stick the form is for
// meanwhile; the jobs that ended are listed in finishedJobs until
// dismissFinishedFor() their stick. A cancel of a stick stops its job in
// hand and drops those of it that wait: each dropped one gets an outcome
// of its own ("dropped"). The jobs of other sticks go on.
//
// Nothing is kept from a stick while its recordings are being deleted
// (setDeleteRunning(), from RecordingsController), and RecordingsController
// deletes nothing from a stick while this has work for it (hasWorkFor()).
//
// The app's one lives with the window (Main.qml), not with the page:
// leaving the page leaves the work running, and the page shows it again
// when it is opened again. A stick taken out while a recording on it is
// kept or waits to be cancels that stick's work (Main.qml asks
// cancelFor()).
//
// Destroyed while a job runs (the app quits), the destructor asks the job
// to stop, drops the queue and waits up to KeepRecordingStopWaitMs for the
// worker to end (a wait on a condition the worker sets as it returns, not
// a spin). The encoder answers a cancel within a fraction of a second; the
// tag writer, which hashes the audio of a long recording twice, cannot be
// interrupted, and a worker still in it then is left to finish on its own:
// it uses nothing of this object, and QCoreApplication waits for it at
// exit. When the tag writer returns, the job sees the cancel before the
// placing and takes its temporary file away: nothing is kept. A cancel
// that comes once the FLAC is placed is too late, and the FLAC stays.
class KeepRecordingController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // Whether this build can encode FLAC at all.
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The root of the stick the form is for: what keep() keeps from, and
    // a folder on it is refused.
    Q_PROPERTY(QString stickRoot READ stickRoot WRITE setStickRoot NOTIFY stickRootChanged)
    Q_PROPERTY(bool cancelRequested READ cancelRequested NOTIFY busyChanged)
    // The job in hand: the root of its stick, its recording (the WAV's
    // path and file name), the FLAC's file name and its folder.
    Q_PROPERTY(QString currentStickRoot READ currentStickRoot NOTIFY busyChanged)
    Q_PROPERTY(QString currentWavPath READ currentWavPath NOTIFY busyChanged)
    Q_PROPERTY(QString currentRecording READ currentRecording NOTIFY busyChanged)
    Q_PROPERTY(QString currentFileName READ currentFileName NOTIFY busyChanged)
    Q_PROPERTY(QString currentFolder READ currentFolder NOTIFY busyChanged)
    // "space", "encoding", "checking", "tags", "placing"; "" when idle.
    Q_PROPERTY(QString progressStep READ progressStep NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // The jobs that wait, of every stick, in order: [{stickRoot, wavPath,
    // recording, fileName, folder}].
    Q_PROPERTY(QVariantList queued READ queued NOTIFY queueChanged)
    // The jobs that ended in this session, of every stick, oldest first,
    // until dismissFinishedFor() their stick: [{state ("kept", "failed",
    // "cancelled", "dropped"), stickRoot, wavPath, recording, fileName,
    // folder, flacPath, leftBehind, message, notes (a list of sentences),
    // unfinished, keptSeconds}]. A dropped job never started.
    Q_PROPERTY(QVariantList finishedJobs READ finishedJobs NOTIFY finishedJobsChanged)
    // Bumps whenever a question asked of a stick (hasWorkFor(),
    // queuedFor(), finishedJobsFor(), outcomeFor()) may answer differently.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    // Why the last keep() was refused; "" when it was not.
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // The cover picked in the form, as it will be embedded: a data: URL
    // of the prepared JPEG, its size in pixels, or why it cannot be used.
    Q_PROPERTY(QString coverPath READ coverPath NOTIFY coverChanged)
    Q_PROPERTY(QString coverPreview READ coverPreview NOTIFY coverChanged)
    Q_PROPERTY(int coverWidth READ coverWidth NOTIFY coverChanged)
    Q_PROPERTY(int coverHeight READ coverHeight NOTIFY coverChanged)
    Q_PROPERTY(QString coverError READ coverError NOTIFY coverChanged)
    Q_PROPERTY(bool coverPreparing READ coverPreparing NOTIFY coverChanged)

public:
    explicit KeepRecordingController(QObject *parent = nullptr);
    ~KeepRecordingController() override;

    bool available() const;
    bool busy() const { return m_running.has_value(); }
    QString stickRoot() const { return m_stickRoot; }
    void setStickRoot(const QString &root);
    // RecordingsController says when its delete on the stick at
    // `stickRoot` starts and ends.
    void setDeleteRunning(const QString &stickRoot, bool running);
    // Recordings are being deleted from the stick at `stickRoot` (by any
    // page): nothing of it is saved meanwhile (keep() refuses), so a page
    // offers no Save as FLAC. Told by `revision`.
    Q_INVOKABLE bool deletingOn(const QString &stickRoot) const;
    bool cancelRequested() const { return busy() && m_cancel.cancelled(); }
    QString currentStickRoot() const;
    QString currentWavPath() const;
    QString currentRecording() const;
    QString currentFileName() const;
    QString currentFolder() const;
    QString progressStep() const { return m_progressStep; }
    double progress() const { return m_progress; }
    QVariantList queued() const;
    QVariantList finishedJobs() const { return m_finishedJobs; }
    int revision() const { return m_revision; }
    QString errorMessage() const { return m_errorMessage; }
    QString coverPath() const { return m_coverPath; }
    QString coverPreview() const { return m_coverPreview; }
    int coverWidth() const { return m_coverWidth; }
    int coverHeight() const { return m_coverHeight; }
    QString coverError() const { return m_coverError; }
    bool coverPreparing() const { return m_coverPreparing; }

    // Whether the recording at `path` is one this can keep: a WAV, as
    // the encoder reads only those.
    Q_INVOKABLE bool canKeep(const QString &path) const;
    // What the form starts with for a recording: {artist, title, album,
    // genre, year (a number, 0 for none), folder}.
    Q_INVOKABLE QVariantMap suggestionsFor(const QString &recordingFileName, const QDateTime &recorded) const;
    // The FLAC's file name for these fields in `folder`, as it will be:
    // cut, on a character boundary, to what the folder's filesystem and
    // the temporary names beside it leave room for.
    Q_INVOKABLE QString fileNameFor(const QString &artist, const QString &title, const QString &folder) const;
    // "" when a tag's text can be used, or why not: `field` is "artist",
    // "title", "album" or "genre". Leading and trailing white space does
    // not count (it is taken away); a control character anywhere is
    // refused (hasControlCharacter()).
    Q_INVOKABLE QString textProblem(const QString &field, const QString &text) const;
    // "" when `folder` can take the FLAC (made on start when missing), or
    // why not, in words for the form: on the stick among them.
    Q_INVOKABLE QString folderProblem(const QString &folder) const;
    // "" when the FLAC's name is free in `folder`, or why not: a file of
    // that name is there, or a job that waits or runs writes it (compared
    // as files: the folder resolved, letters of either case as one).
    Q_INVOKABLE QString targetProblem(const QString &folder, const QString &artist, const QString &title) const;
    // "" for an empty year or one from 1900 to 2100, or why not.
    Q_INVOKABLE QString yearProblem(const QString &year) const;

    // The cover for the form: a picture file (a path or a file: URL),
    // prepared off the GUI thread; "" for no cover.
    Q_INVOKABLE void pickCover(const QString &pathOrUrl);

    // Queues the recording to be kept: {wavPath, recording (its file
    // name), artist, title, album, genre, year, folder, keepUnfinished
    // (the person ticked "Keep the audio the file holds" for a recording
    // the player did not finish)}; the cover is the one picked. Starts at once when nothing runs. False, with
    // errorMessage, when the form's checks refuse it, or when the
    // recording is being kept or waits to be kept already.
    Q_INVOKABLE bool keep(const QVariantMap &form);
    // A recording of the stick at `stickRoot` is being kept or waits to be.
    Q_INVOKABLE bool hasWorkFor(const QString &stickRoot) const;
    // The job in hand is of the stick at `stickRoot`; is of another stick.
    Q_INVOKABLE bool runsFor(const QString &stickRoot) const;
    Q_INVOKABLE bool runsElsewhere(const QString &stickRoot) const;
    // `queued` and `finishedJobs`, of the stick at `stickRoot` only.
    Q_INVOKABLE QVariantList queuedFor(const QString &stickRoot) const;
    Q_INVOKABLE QVariantList finishedJobsFor(const QString &stickRoot) const;
    // Stops the job in hand when it is of the stick at `stickRoot`, and
    // drops the ones of it that wait; another stick's work goes on.
    Q_INVOKABLE void cancelFor(const QString &stickRoot);
    // How the recording at `wavPath`, on the stick at `stickRoot`, fares in
    // this session: {state ("queued", "running", "kept", "failed",
    // "cancelled", "dropped"; "" for none), stickRoot, recording, fileName,
    // folder, flacPath, leftBehind, message, notes, unfinished,
    // keptSeconds}. A recording is known by its path, size and
    // modification time: another file that appears under the same path is
    // another recording.
    Q_INVOKABLE QVariantMap outcomeFor(const QString &stickRoot, const QString &wavPath) const;
    // For the delete confirmation, of the recordings at `wavPaths` on the
    // stick at `stickRoot`:
    // {notKept: [file names of those a keep was asked for in this session
    // and none ended kept], kept: [{recording, fileName, folder} of those
    // kept in this session, the last copy each, while that FLAC is still
    // there with the size and modification time it had when the job
    // ended], keptGone: [{recording, fileName, folder} of those kept whose
    // FLAC is not there like that any more]}.
    Q_INVOKABLE QVariantMap keepNotesFor(const QString &stickRoot, const QStringList &wavPaths) const;
    // The FLAC the recording at `wavPath`, on the stick at `stickRoot`, was
    // last saved as in this session, while it is there as it was saved
    // (its size and modification time, as keepNotesFor() asks); ""
    // otherwise. What Browse adds to the library for the recording: a
    // file changed since, or another one under that name, is not the
    // audio that was checked against the recording.
    Q_INVOKABLE QString keptFlacFor(const QString &stickRoot, const QString &wavPath) const;
    // The recording was saved as FLAC in this session, and a file is under
    // that FLAC's path that is no longer the one saved; false when the
    // FLAC is as saved, gone, or was never made.
    Q_INVOKABLE bool keptFlacChanged(const QString &stickRoot, const QString &wavPath) const;
    // Takes the stick's jobs off finishedJobs (the strip is closed).
    Q_INVOKABLE void dismissFinishedFor(const QString &stickRoot);
    // Opens the folder of a kept FLAC in the file manager.
    Q_INVOKABLE void showInFolder(const QString &flacPath) const;
    // "file:" URLs and paths alike as a local path.
    Q_INVOKABLE QString localPath(const QString &pathOrUrl) const;
    Q_INVOKABLE QUrl folderUrl(const QString &folder) const;

    // For a test only: the parts each job is done with from now on;
    // {} for the real ones.
    static void setPartsForTesting(std::function<KeepRecordingParts()> make);
    // For a test only: told, by the destructor, of each wait for a worker
    // ("cover" or "job") whether it ended because the worker had ended
    // (`workerEnded`, the deadline not reached) or because the deadline
    // passed. {} for none.
    static void setStopWaitObserverForTesting(std::function<void(const char *what, bool workerEnded)> observe);

signals:
    void busyChanged();
    void progressChanged();
    void queueChanged();
    void finishedJobsChanged();
    void stickRootChanged();
    void revisionChanged();
    void errorMessageChanged();
    void coverChanged();
    // A job ended: a finishedJobs entry.
    void finished(const QVariantMap &outcome);

private:
    struct Job
    {
        int id = 0;
        // stickKey() of the stick the recording is on.
        QString stickRoot;
        QString wavPath;
        // wavKey(): the path, size and modification time.
        QString wavKey;
        QString recording;
        application::KeepRecordingRequest request;
    };
    struct CoverPreview
    {
        QString path;
        QString dataUrl;
        int width = 0;
        int height = 0;
        QString error;
    };

    void startNext();
    void onJobFinished();
    void onCoverPrepared();
    void setError(const QString &message);
    void bumpRevision();
    // Drops the jobs that wait for which `drop` holds, each with an
    // outcome of its own; true when any was.
    bool dropQueued(const std::function<bool(const Job &)> &drop);
    // Every check of the form, for keep(); "" when it may start.
    QString formProblem(const QVariantMap &form) const;
    // A job that waits, as `queued` lists it.
    static QVariantMap queuedEntry(const Job &job);
    // The FLAC's path for a job.
    static QString targetOf(const application::KeepRecordingRequest &request);

    std::deque<Job> m_queue;
    std::optional<Job> m_running;
    QFutureWatcher<application::KeepRecordingResult> m_watcher;
    QFuture<application::KeepRecordingResult> m_future;
    application::CancellationToken m_cancel;
    int m_nextJob = 1;
    QString m_progressStep;
    double m_progress = 0.0;
    QVariantList m_finishedJobs;
    // Per WAV (path, size and modification time), how its last job ended,
    // and its last kept copy with the FLAC's size and modification time;
    // each with its stick ("stickRoot", a stickKey()).
    QHash<QString, QVariantMap> m_outcomes;
    QHash<QString, QVariantMap> m_keptCopies;
    QString m_stickRoot;
    // stickKey() of each stick whose recordings are being deleted.
    QSet<QString> m_deletingSticks;
    std::shared_ptr<WorkerEnd> m_jobEnd;
    std::shared_ptr<WorkerEnd> m_coverEnd;
    int m_revision = 0;
    QString m_errorMessage;

    QFutureWatcher<CoverPreview> m_coverWatcher;
    QString m_coverPath;
    QString m_coverPreview;
    int m_coverWidth = 0;
    int m_coverHeight = 0;
    QString m_coverError;
    bool m_coverPreparing = false;
};

// How long the destructor waits for a job asked to stop, and for a cover
// preview under way.
inline constexpr int KeepRecordingStopWaitMs = 1000;

// The settings the form remembers.
inline constexpr char KeepRecordingArtistKey[] = "ownRecordings/lastArtist";
inline constexpr char KeepRecordingGenreKey[] = "ownRecordings/lastGenre";
inline constexpr char KeepRecordingFolderKey[] = "ownRecordings/lastFolder";

}  // namespace seabass::gui
