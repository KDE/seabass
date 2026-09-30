// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/clean_up_recordings.hpp"
#include "gui/edit/direct_write_hold.hpp"

namespace seabass::gui
{

// The recordings on a stick, one row each, with a checkbox. Nothing
// starts ticked: a deletion that cannot be undone is chosen file by file,
// or with Select All on purpose.
class RecordingListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by RecordingsController; not constructible from QML")

public:
    enum Roles
    {
        PathRole = Qt::UserRole + 1,
        FileNameRole,
        FolderNameRole,
        SourceRole,  // "engine", "pioneer", "alphatheta"
        SizeBytesRole,
        ModifiedRole,          // QDateTime, invalid when unknown
        DurationSecondsRole,   // -1 when unknown
        IncludedRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QHash<int, QByteArray> roleNames() const override;

    void setRecordings(std::vector<application::Recording> recordings);
    void setAllIncluded(bool included);
    std::vector<std::string> includedPaths() const;
    int includedCount() const;
    std::uint64_t includedBytes() const;

private:
    std::vector<application::Recording> m_rows;
    std::vector<bool> m_included;
};

// Carries a run's progress from the worker to the controller. Lives on
// the GUI thread (made with makeMainThreadShared) and is emitted from the
// worker; the queued connection to the controller dies with the
// controller, so a page left mid-run cannot be called into.
class RecordingsProgressRelay : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

signals:
    void progressed(int fileIndex, int fileCount, const QString &fileName);
};

struct RecordingsListResult
{
    application::RecordingListing listing;
    // fs::space() on the stick, as Clean Up Duplicates reads it: 0/0 when
    // it cannot tell, which SpaceReclaimBar takes as "unknown".
    qlonglong stickTotalBytes = 0;
    qlonglong stickFreeBytes = 0;
    QString error;
};

struct RecordingsRunResult
{
    application::RecordingsReport report;
    QString error;  // the run did not happen, or stopped: why
};

// Clean Up Recordings (Housekeeping): lists the set recordings DJ
// hardware left on a stick and deletes the ones ticked. See
// application/use_cases/clean_up_recordings.hpp for the rules.
//
// Unlike every other Housekeeping flow, nothing here takes a full stick
// backup first and nothing is added to the pending-deletions list. These
// files belong to no library, so a library backup would not hold them and
// a restore could not bring one back; the pending-deletions list tracks
// audio a library edit orphaned, which these never were. Nothing is
// copied either: they are usually long since saved and reworked
// elsewhere, and the confirmation says plainly that they will be gone for
// good.
//
// Known limitation: summarize(), the hub card's count, lists the folders on the GUI thread.
class RecordingsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(seabass::gui::RecordingListModel *recordings READ recordings CONSTANT)
    Q_PROPERTY(bool listing READ listing NOTIFY busyChanged)
    Q_PROPERTY(bool working READ working NOTIFY busyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool listed READ listed NOTIFY listingChanged)
    Q_PROPERTY(int recordingCount READ recordingCount NOTIFY listingChanged)
    Q_PROPERTY(qlonglong totalBytes READ totalBytes NOTIFY listingChanged)
    // The stick's size and free space; 0 when unknown.
    Q_PROPERTY(qlonglong stickTotalBytes READ stickTotalBytes NOTIFY listingChanged)
    Q_PROPERTY(qlonglong stickFreeBytes READ stickFreeBytes NOTIFY listingChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(qlonglong selectedBytes READ selectedBytes NOTIFY selectionChanged)
    // [{fileName, folderName, reason}] for what is in the folders and is
    // not a recording; named on the page, never touched.
    Q_PROPERTY(QVariantList leftAlone READ leftAlone NOTIFY listingChanged)
    Q_PROPERTY(QStringList unreadableFolders READ unreadableFolders NOTIFY listingChanged)
    // Progress of a run.
    Q_PROPERTY(int filesDone READ filesDone NOTIFY progressChanged)
    Q_PROPERTY(int filesTotal READ filesTotal NOTIFY progressChanged)
    Q_PROPERTY(QString currentItem READ currentItem NOTIFY progressChanged)
    Q_PROPERTY(bool cancelRequested READ cancelRequested NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY messagesChanged)

public:
    explicit RecordingsController(QObject *parent = nullptr);
    ~RecordingsController() override;

    RecordingListModel *recordings() { return &m_model; }
    bool listing() const { return m_listing; }
    bool working() const { return m_working; }
    bool busy() const { return m_listing || m_working; }
    bool listed() const { return m_listed; }
    int recordingCount() const { return m_model.rowCount(); }
    qlonglong totalBytes() const { return static_cast<qlonglong>(m_totalBytes); }
    qlonglong stickTotalBytes() const { return m_stickTotalBytes; }
    qlonglong stickFreeBytes() const { return m_stickFreeBytes; }
    int selectedCount() const { return m_model.includedCount(); }
    qlonglong selectedBytes() const { return static_cast<qlonglong>(m_model.includedBytes()); }
    QVariantList leftAlone() const { return m_leftAlone; }
    QStringList unreadableFolders() const { return m_unreadableFolders; }
    int filesDone() const { return m_filesDone; }
    int filesTotal() const { return m_filesTotal; }
    QString currentItem() const { return m_currentItem; }
    bool cancelRequested() const { return m_working && m_cancel.cancelled(); }
    QString errorMessage() const { return m_errorMessage; }

    // For the Housekeeping hub's card: directory entries and stats only,
    // on the calling thread (a few folders, a handful of files). Returns
    // {count, bytes, sources: ["engine", "pioneer", "alphatheta"],
    // unreadable: bool}.
    Q_INVOKABLE QVariantMap summarize(const QString &rekordboxPath, const QString &enginePath) const;

    // Lists the recordings on the stick holding these catalogs, with
    // durations, off the GUI thread. Nothing starts ticked.
    Q_INVOKABLE void load(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath);

    Q_INVOKABLE void setIncluded(int row, bool included);
    Q_INVOKABLE void setAllIncluded(bool included);

    // The ticked recordings' paths, as deleteSelected() would take them.
    Q_INVOKABLE QStringList selectedPaths() const;

    // Deletes the ticked recordings on a worker.
    Q_INVOKABLE void deleteSelected();
    // Stops after the file in flight.
    Q_INVOKABLE void cancel();

    // For a test only: seams handed to the use case (see
    // RecordingDeleteHooks), copied into each run when it starts, so a
    // change meanwhile cannot reach a worker already running.
    static void setDeleteHooksForTesting(application::RecordingDeleteHooks hooks);

signals:
    void busyChanged();
    void listingChanged();
    void selectionChanged();
    void progressChanged();
    void messagesChanged();
    // The OperationSummaryDialog shape: {written, total, unit, verb,
    // cancelled, error, warning}.
    void finished(const QVariantMap &summary);
    void lockRefused(const QVariantMap &holder);

private:
    void onListed();
    void onRunFinished();
    void setError(const QString &message);
    QString stickRoot() const;

    RecordingListModel m_model;
    QFutureWatcher<RecordingsListResult> m_listWatcher;
    QFutureWatcher<RecordingsRunResult> m_runWatcher;
    application::CancellationToken m_cancel;
    DirectWriteHold m_writeHold;
    QString m_stickLabel;
    QString m_rekordboxPath;
    QString m_enginePath;
    bool m_listing = false;
    bool m_working = false;
    bool m_listed = false;
    std::uint64_t m_totalBytes = 0;
    qlonglong m_stickTotalBytes = 0;
    qlonglong m_stickFreeBytes = 0;
    QVariantList m_leftAlone;
    QStringList m_unreadableFolders;
    int m_filesDone = 0;
    int m_filesTotal = 0;
    QString m_currentItem;
    QString m_errorMessage;
};

}  // namespace seabass::gui
