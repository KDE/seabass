#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include "application/ports/cancellation_token.hpp"

namespace seabass::gui
{

// Result of the background measurement, built entirely on a worker
// thread with no access to the controller.
struct StickPerformanceResult
{
    QVariantMap filesystemInfo;  // same keys as StickStatisticsController's filesystemInfo
    QVariantMap measurement;     // streamingBytesPerSecond, randomReadMedianMs, ... see the .cpp
    QVariantMap score;           // score, speedClass, setWaitText, sub-scores, verdicts
    QVariantList advisories;     // [{group, players, verdict, summary}]
    QVariantMap facts;           // clusterBytes, analysisFiles, analysisFolders, audioFiles
    QString errorMessage;
    bool cancelled = false;
};

struct StickWriteResult
{
    QVariantMap measurement;  // streamingWriteBytesPerSecond, smallFileWritesPerSecond, ... see the .cpp
    QVariantMap estimate;     // cueSaveSeconds, exportTrackSeconds, exportHundredTracksSeconds, verdicts
    QString errorMessage;
};

// USB Stick Performance: reads real files on the stick the way a player
// does and says what that means per player generation. Read-only, like
// Library Statistics: no write lock, no backup, nothing written to the
// stick. One operation, measure(): the catalog is read for sample files,
// then the probe runs. Nothing is persisted; the result goes into
// StickPerformanceCache for this session only.
class StickPerformanceController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QVariantMap filesystemInfo READ filesystemInfo NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap measurement READ measurement NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap score READ score NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList advisories READ advisories NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap facts READ facts NOTIFY resultsChanged)
    Q_PROPERTY(QString measuredAt READ measuredAt NOTIFY resultsChanged)
    Q_PROPERTY(bool writeBusy READ writeBusy NOTIFY writeBusyChanged)
    Q_PROPERTY(QString writeErrorMessage READ writeErrorMessage NOTIFY writeErrorMessageChanged)
    Q_PROPERTY(QVariantMap writeMeasurement READ writeMeasurement NOTIFY writeResultsChanged)
    Q_PROPERTY(QVariantMap writeEstimate READ writeEstimate NOTIFY writeResultsChanged)

public:
    explicit StickPerformanceController(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString errorMessage() const { return m_errorMessage; }
    QVariantMap filesystemInfo() const { return m_filesystemInfo; }
    QVariantMap measurement() const { return m_measurement; }
    QVariantMap score() const { return m_score; }
    QVariantList advisories() const { return m_advisories; }
    QVariantMap facts() const { return m_facts; }
    QString measuredAt() const { return m_measuredAt; }
    bool writeBusy() const { return m_writeBusy; }
    QString writeErrorMessage() const { return m_writeErrorMessage; }
    QVariantMap writeMeasurement() const { return m_writeMeasurement; }
    QVariantMap writeEstimate() const { return m_writeEstimate; }

    // rekordboxPath/enginePath: empty for a catalog not present on this
    // stick, same convention as every other controller in this app.
    Q_INVOKABLE void measure(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath);
    Q_INVOKABLE void cancel();

    // The optional write test (see StickWriteProbe): throwaway files in a
    // hidden folder under the stick root, removed afterwards. Refused
    // while rekordbox or Engine DJ is running, the same guard every write
    // path in this app applies, and never on a browsed backup.
    Q_INVOKABLE void measureWrites(const QString &rekordboxPath, const QString &enginePath);

signals:
    void busyChanged();
    void errorMessageChanged();
    void resultsChanged();
    void cancelled();
    void writeBusyChanged();
    void writeErrorMessageChanged();
    void writeResultsChanged();

private:
    void onFinished();
    void onWriteFinished();
    void setBusy(bool busy);
    void setErrorMessage(const QString &message);
    void setWriteBusy(bool busy);
    void setWriteErrorMessage(const QString &message);

    QFutureWatcher<StickPerformanceResult> m_watcher;
    application::CancellationToken m_cancel;  // fresh per measure()
    QFutureWatcher<StickWriteResult> m_writeWatcher;

    bool m_busy = false;
    QString m_errorMessage;
    QVariantMap m_filesystemInfo;
    QVariantMap m_measurement;
    QVariantMap m_score;
    QVariantList m_advisories;
    QVariantMap m_facts;
    QString m_measuredAt;

    bool m_writeBusy = false;
    QString m_writeErrorMessage;
    QVariantMap m_writeMeasurement;
    QVariantMap m_writeEstimate;
};

}  // namespace seabass::gui
