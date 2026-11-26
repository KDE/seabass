// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QDateTime>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

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
// one runs waits in a queue and starts when the one before it ends.
// `progress` is the job in hand's, 0 to 1, and never goes back within it.
//
// The form's suggestions (suggestionsFor()) come from the recording and
// from what was kept last (QSettings: artist, genre, folder). The form's
// checks are here too (fileNameFor(), folderProblem(), targetProblem()),
// and keep() asks them again before anything is queued.
//
// Leaving the page while a job runs cancels it, as the track import's
// preparing is cancelled: the destructor asks the job to stop, drops the
// queue and waits up to KeepRecordingStopWaitMs for the worker. The
// encoder answers a cancel within a fraction of a second; the tag
// writer's copy of a long recording cannot be interrupted, and a worker
// still in it then is left to finish on its own: it uses nothing of this
// object, removes the FLAC it made when the tag writer returns, and
// QCoreApplication waits for it at exit.
class KeepRecordingController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // Whether this build can encode FLAC at all.
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool cancelRequested READ cancelRequested NOTIFY busyChanged)
    // The job in hand: its recording (the WAV's path and file name), the
    // FLAC's file name and its folder.
    Q_PROPERTY(QString currentWavPath READ currentWavPath NOTIFY busyChanged)
    Q_PROPERTY(QString currentRecording READ currentRecording NOTIFY busyChanged)
    Q_PROPERTY(QString currentFileName READ currentFileName NOTIFY busyChanged)
    Q_PROPERTY(QString currentFolder READ currentFolder NOTIFY busyChanged)
    // "space", "encoding", "checking", "tags"; "" when idle.
    Q_PROPERTY(QString progressStep READ progressStep NOTIFY progressChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // The jobs that wait, in order: [{wavPath, recording, fileName, folder}].
    Q_PROPERTY(QVariantList queued READ queued NOTIFY queueChanged)
    // The job that ended last, until dismissFinished() or the next one
    // starts: {state ("kept", "failed", "cancelled"), wavPath, recording,
    // fileName, folder, flacPath, message}; empty before any.
    Q_PROPERTY(QVariantMap lastFinished READ lastFinished NOTIFY lastFinishedChanged)
    // Bumps whenever outcomeFor() may answer differently.
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
    bool cancelRequested() const { return busy() && m_cancel.cancelled(); }
    QString currentWavPath() const;
    QString currentRecording() const;
    QString currentFileName() const;
    QString currentFolder() const;
    QString progressStep() const { return m_progressStep; }
    double progress() const { return m_progress; }
    QVariantList queued() const;
    QVariantMap lastFinished() const { return m_lastFinished; }
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
    // The FLAC's file name for these fields.
    Q_INVOKABLE QString fileNameFor(const QString &artist, const QString &title) const;
    // "" when `folder` can take the FLAC (made on start when missing), or
    // why not, in words for the form.
    Q_INVOKABLE QString folderProblem(const QString &folder) const;
    // "" when the FLAC's name is free in `folder`, or why not: a file of
    // that name is there, or a job that waits or runs writes it.
    Q_INVOKABLE QString targetProblem(const QString &folder, const QString &artist, const QString &title) const;
    // "" for an empty year or one from 1900 to 2100, or why not.
    Q_INVOKABLE QString yearProblem(const QString &year) const;

    // The cover for the form: a picture file (a path or a file: URL),
    // prepared off the GUI thread; "" for no cover.
    Q_INVOKABLE void pickCover(const QString &pathOrUrl);

    // Queues the recording to be kept: {wavPath, recording (its file
    // name), artist, title, album, genre, year, folder}; the cover is the
    // one picked. Starts at once when nothing runs. False, with
    // errorMessage, when the form's checks refuse it.
    Q_INVOKABLE bool keep(const QVariantMap &form);
    // Stops the job in hand and drops the ones that wait.
    Q_INVOKABLE void cancel();
    // How the recording at `wavPath` fares in this session: {state
    // ("queued", "running", "kept", "failed", "cancelled"; "" for none),
    // fileName, folder, flacPath, message}.
    Q_INVOKABLE QVariantMap outcomeFor(const QString &wavPath) const;
    Q_INVOKABLE void dismissFinished();
    // Opens the folder of a kept FLAC in the file manager.
    Q_INVOKABLE void showInFolder(const QString &flacPath) const;
    // "file:" URLs and paths alike as a local path.
    Q_INVOKABLE QString localPath(const QString &pathOrUrl) const;
    Q_INVOKABLE QUrl folderUrl(const QString &folder) const;

    // For a test only: the parts each job is done with from now on;
    // {} for the real ones.
    static void setPartsForTesting(std::function<KeepRecordingParts()> make);

signals:
    void busyChanged();
    void progressChanged();
    void queueChanged();
    void lastFinishedChanged();
    void revisionChanged();
    void errorMessageChanged();
    void coverChanged();
    // A job ended: lastFinished's shape.
    void finished(const QVariantMap &outcome);

private:
    struct Job
    {
        int id = 0;
        QString wavPath;
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
    QVariantMap m_lastFinished;
    // Per WAV path, how its last job ended.
    QHash<QString, QVariantMap> m_outcomes;
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

// How long the destructor waits for a job asked to stop.
inline constexpr int KeepRecordingStopWaitMs = 1000;

// The settings the form remembers.
inline constexpr char KeepRecordingArtistKey[] = "ownRecordings/lastArtist";
inline constexpr char KeepRecordingGenreKey[] = "ownRecordings/lastGenre";
inline constexpr char KeepRecordingFolderKey[] = "ownRecordings/lastFolder";

}  // namespace seabass::gui
