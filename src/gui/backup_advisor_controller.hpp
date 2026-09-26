// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QMap>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "application/use_cases/advise_stick_backup.hpp"

namespace seabass::gui
{

// Per-stick backup advice for the stick list: for each mounted stick,
// which backup in the backup folder it relates to and what to offer
// (update it, restore it, back up fresh), and which *other* mounted
// stick or backup holds a copy of the same library to create this stick
// from or bring it up to date from. Decided by
// application::adviseStickBackup from the library's content fingerprint,
// the stick's hardware identifier, its label and its catalog mtime.
//
// The facts about a stick (fingerprints, mtime, sizes) are gathered on a
// worker thread, one stick at a time, and kept; the advice itself is
// pure and cheap, so it is recomputed for every known stick whenever any
// stick's facts change -- stick B's advice depends on stick A being
// there.
//
// Each stick is read in two steps. The first reads the catalogs alone
// (a fraction of a second) and publishes advice at once, starting the
// catalog cache's prefetch of the rest; when a rekordbox catalog's cues
// are still to come, that advice carries cuesPending, and a second step,
// queued behind every other stick's first, waits for the cue pass and
// publishes the final word. A verdict the cues cannot change (different
// tracks or playlists) is never marked pending.
class BackupAdvisorController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString backupDirectory READ backupDirectory WRITE setBackupDirectory NOTIFY backupDirectoryChanged)
    // mountPoint -> {state, matchedBy, backupPath, backupLabel,
    // backupCreatedAt, trackOverlap, cueOverlap, detail, cloneSource,
    // updateSource, diverged, cuesPending}; see StickBackupAdvice for the state and
    // matchedBy values. cloneSource / updateSource are maps {kind
    // ("none" / "disk-backup" / "stick"), label, mountPoint, backupPath,
    // modifiedAt, enoughSpace, detail, rekordboxPath, enginePath} -- the
    // last two filled for stick sources so the list can open the clone
    // page without another lookup. cuesPending: the verdict is what it
    // is if the cues have not changed, and they are still being read
    // (this stick's or a peer's); shown beside the verdict, not instead.
    Q_PROPERTY(QVariantMap advice READ advice NOTIFY adviceChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The mount points whose advice is being gathered right now or is
    // queued to be: the running one first, then the queue in order. A
    // page about one stick waits on its own entry here, not on busy,
    // which stays true until the slowest other stick has been read. Only
    // the first step counts: a stick whose advice stands and whose cues
    // are still being read is out of this list (advice.cuesPending says
    // that), so no page hides a verdict behind a spinner for the seconds
    // the cue pass takes. busy covers both steps.
    Q_PROPERTY(QStringList pending READ pending NOTIFY pendingChanged)

public:
    explicit BackupAdvisorController(QObject *parent = nullptr);
    ~BackupAdvisorController() override;

    QString backupDirectory() const { return m_backupDirectory; }
    void setBackupDirectory(const QString &directory);
    QVariantMap advice() const { return m_advice; }
    // Until the queue has drained, not merely while one stick is being
    // read: between two sticks the watcher is idle for a moment, and a
    // page waiting on the advice (BackupsHubPage's scanning overlay) must
    // not see that moment as "done".
    //
    // m_running, not m_watcher.isRunning(): the latter reads the worker's
    // live state, which goes false the moment the pass returns, before its
    // result is handled. A stick that is not there is assessed in well
    // under a millisecond, so busyChanged could announce a pass that was
    // already "not running".
    bool busy() const { return !m_running.isEmpty() || !m_queue.empty(); }
    QStringList pending() const;

    // Queues a fact-gathering pass for this stick; the result lands in
    // advice[mountPoint] and refreshes every other stick's advice too.
    Q_INVOKABLE void assess(const QString &stickLabel, const QString &mountPoint, const QString &rekordboxPath,
                            const QString &enginePath);
    // Re-runs every assessment made so far (after a backup or restore, or
    // when the stick list is shown again).
    Q_INVOKABLE void reassessAll();
    Q_INVOKABLE void forget(const QString &mountPoint);

signals:
    void backupDirectoryChanged();
    void adviceChanged();
    void busyChanged();
    void pendingChanged();

private:
    // Facts: everything about the stick, the fingerprint from the catalogs
    // alone. Cues: the fingerprint again, once the cue pass is there; the
    // rest of the facts stand.
    enum class Step
    {
        Facts,
        Cues,
    };
    struct Request
    {
        Step step = Step::Facts;
        QString stickLabel;
        QString mountPoint;
        QString rekordboxPath;
        QString enginePath;
    };
    // Everything adviseStickBackup wants to know about one stick, as it
    // was when last gathered. Also what makes that stick a peer of the
    // others.
    struct StickFacts
    {
        bool hasLibrary = false;
        std::string stickIdentifier;
        std::string stickLabel;
        std::optional<domain::LibraryFingerprint> fingerprint;
        std::map<std::string, std::string> databaseFingerprints;
        std::int64_t catalogModifiedAtUnix = 0;
        std::uint64_t usedBytes = 0;
        std::uint64_t freeBytes = 0;
    };
    struct Result;

    void startNext();
    void onFinished();
    void enqueue(const Request &request);
    void recomputeAdvice();
    QVariantMap sourceToVariant(const application::StickBackupAdvice::SourceRef &source) const;

    QString m_backupDirectory;
    QVariantMap m_advice;
    QMap<QString, Request> m_known;  // by mountPoint
    QMap<QString, StickFacts> m_facts;  // by mountPoint, once gathered
    std::vector<application::StickBackupDescription> m_backups;  // as of the last gathering pass
    std::vector<Request> m_queue;
    QString m_running;  // the mount point being read: set before setFuture(), cleared once onFinished() has handled the result
    Step m_runningStep = Step::Facts;
    QFutureWatcher<std::shared_ptr<Result>> m_watcher;
};

}  // namespace seabass::gui
