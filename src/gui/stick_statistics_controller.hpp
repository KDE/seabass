#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include "application/ports/cancellation_token.hpp"

namespace seabass::gui
{

// Result of the background scan task, see
// StickStatisticsController::scan(). Built entirely on a worker thread,
// no access to the controller.
struct StickStatisticsScanResult
{
    QVariantMap filesystemInfo;    // filesystem/capacity/USB facts, see the .cpp for the exact keys
    QVariantMap rekordboxStats;    // empty map if rekordboxPath wasn't given
    QVariantMap engineStats;       // empty map if enginePath wasn't given
    QVariantMap oneLibraryStats;   // empty map if this stick has no OneLibrary export
    QVariantMap diskUsage;         // {totalBytes, usedBytes, freeBytes, root: {label, sizeBytes, children:[...]}}
    QString errorMessage;
    bool cancelled = false;  // stopped via cancelScan(); nothing else is set
};

// Read-only: this page never writes anything to the stick, so unlike
// LibraryConsistencyController there's no write lock, no backup, no
// rekordbox-running refusal -- the same "just reads" contract as
// ScanController (Browse Library). Speed lives on its own page now
// (StickPerformanceController); this one only counts and sizes.
class StickStatisticsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while the scan runs: it can be stopped via cancelScan(), after
    // which scanCancelled() fires instead of resultsChanged().
    Q_PROPERTY(bool scanCancellable READ scanCancellable NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QVariantMap filesystemInfo READ filesystemInfo NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap rekordboxStats READ rekordboxStats NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap engineStats READ engineStats NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap oneLibraryStats READ oneLibraryStats NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap diskUsage READ diskUsage NOTIFY resultsChanged)

public:
    explicit StickStatisticsController(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString errorMessage() const { return m_errorMessage; }
    QVariantMap filesystemInfo() const { return m_filesystemInfo; }
    QVariantMap rekordboxStats() const { return m_rekordboxStats; }
    QVariantMap engineStats() const { return m_engineStats; }
    QVariantMap oneLibraryStats() const { return m_oneLibraryStats; }
    QVariantMap diskUsage() const { return m_diskUsage; }

    // rekordboxPath/enginePath: empty for a catalog not present on this
    // stick, same convention as every other controller in this app.
    Q_INVOKABLE void scan(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath);

    bool scanCancellable() const { return m_busy; }
    Q_INVOKABLE void cancelScan();

signals:
    void scanCancelled();
    void busyChanged();
    void errorMessageChanged();
    void resultsChanged();

private:
    void onScanFinished();
    void setBusy(bool busy);
    void setErrorMessage(const QString &message);

    QFutureWatcher<StickStatisticsScanResult> m_watcher;
    application::CancellationToken m_scanCancel;  // fresh per scan()

    bool m_busy = false;
    QString m_errorMessage;
    QVariantMap m_filesystemInfo;
    QVariantMap m_rekordboxStats;
    QVariantMap m_engineStats;
    QVariantMap m_oneLibraryStats;
    QVariantMap m_diskUsage;
};

}  // namespace seabass::gui
