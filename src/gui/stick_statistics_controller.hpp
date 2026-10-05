// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include "gui/async_request.hpp"

#include "application/ports/cancellation_token.hpp"

namespace seabass::gui
{

class QtProgressReporter;

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
    // The scan's one bar (#58): the catalog reads, the facts, the artwork
    // and the folder walks, counted before the first read and announced
    // once; the count only goes up and scanLabel names the step under way.
    // scanTotal stays 0 only while the plan is being counted, or when a
    // catalog could not be counted at all.
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    Q_PROPERTY(QString scanLabel READ scanLabel NOTIFY scanProgressChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QVariantMap filesystemInfo READ filesystemInfo NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap rekordboxStats READ rekordboxStats NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap engineStats READ engineStats NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap oneLibraryStats READ oneLibraryStats NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap diskUsage READ diskUsage NOTIFY resultsChanged)

public:
    explicit StickStatisticsController(QObject *parent = nullptr);

    bool busy() const { return m_scan.busy(); }
    QString errorMessage() const { return m_errorMessage; }
    QVariantMap filesystemInfo() const { return m_filesystemInfo; }
    QVariantMap rekordboxStats() const { return m_rekordboxStats; }
    QVariantMap engineStats() const { return m_engineStats; }
    QVariantMap oneLibraryStats() const { return m_oneLibraryStats; }
    QVariantMap diskUsage() const { return m_diskUsage; }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    QString scanLabel() const { return m_scanLabel; }

    // rekordboxPath/enginePath: empty for a catalog not present on this
    // stick, same convention as every other controller in this app.
    Q_INVOKABLE void scan(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath);

    bool scanCancellable() const { return busy(); }
    Q_INVOKABLE void cancelScan();

signals:
    void scanCancelled();
    void busyChanged();
    void errorMessageChanged();
    void resultsChanged();
    void scanProgressChanged();

private:
    void onScanFinished(StickStatisticsScanResult &&result);
    void setErrorMessage(const QString &message);
    std::shared_ptr<QtProgressReporter> makeReporter();
    void setScanProgress(int current, int total, const QString &label);

    QString m_errorMessage;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    QString m_scanLabel;
    QVariantMap m_filesystemInfo;
    QVariantMap m_rekordboxStats;
    QVariantMap m_engineStats;
    QVariantMap m_oneLibraryStats;
    QVariantMap m_diskUsage;

    // Last, so it is destroyed first. See docs/async-requests.md.
    AsyncRequest<StickStatisticsScanResult> m_scan{this, [this]() { emit busyChanged(); }};
};

}  // namespace seabass::gui
