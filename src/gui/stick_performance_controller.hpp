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
    QVariantMap facts;           // clusterBytes, analysisFiles, analysisFolders, audioFiles, sampleKind
    QVariantMap trend;           // state, summary, earlierCount, bestEarlierScore; from the local history
    // Filled only when the measurement had to write its own files first
    // (measureWithScratchFiles): the write probe ran as part of it.
    QVariantMap writeMeasurement;
    QVariantMap writeEstimate;
    // The stick has nothing to read: no library, and no other file of any
    // size. Not an error; the page offers the throwaway-file measurement.
    bool needsScratchFiles = false;
    QString errorMessage;
    bool cancelled = false;
};

struct StickWriteResult
{
    QVariantMap measurement;  // streamingWriteBytesPerSecond, smallFileWritesPerSecond, ... see the .cpp
    QVariantMap estimate;     // cueSaveSeconds, exportTrackSeconds, exportHundredTracksSeconds, verdicts
    QString errorMessage;
};

struct StickWearResult
{
    QVariantMap check;       // filesRead, bytesRead, medianBytesPerSecond, seconds, unreadable[], slow[{path, bytesPerSecond}]
    QVariantMap assessment;  // state, label, summary
    QString errorMessage;
    bool cancelled = false;
};

// USB Stick Performance: reads real files on the stick the way a player
// does and says what that means per player generation. Every measurement
// is appended to a small local history (StickPerformanceHistory, this
// computer only, never the stick) so the page can say whether the stick
// is getting slower; that is the one thing a single run cannot tell.
// measure() is
// read-only, like Library Statistics: no write lock, no backup, nothing
// written to the stick. It reads the library's own files when there is a
// library, any other files on the stick otherwise, and reports
// needsScratchFiles when there is nothing at all; then
// measureWithScratchFiles() writes the write test's throwaway files,
// reads them back, and removes them, which is the only way to measure a
// blank stick. Nothing is persisted; the result goes into
// StickPerformanceCache for this session (the backup ETA) and the local
// history file (the trend).
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
    Q_PROPERTY(bool needsScratchFiles READ needsScratchFiles NOTIFY resultsChanged)
    Q_PROPERTY(QVariantMap trend READ trend NOTIFY resultsChanged)
    Q_PROPERTY(bool writeBusy READ writeBusy NOTIFY writeBusyChanged)
    Q_PROPERTY(QString writeErrorMessage READ writeErrorMessage NOTIFY writeErrorMessageChanged)
    Q_PROPERTY(QVariantMap writeMeasurement READ writeMeasurement NOTIFY writeResultsChanged)
    Q_PROPERTY(QVariantMap writeEstimate READ writeEstimate NOTIFY writeResultsChanged)
    Q_PROPERTY(bool wearBusy READ wearBusy NOTIFY wearBusyChanged)
    Q_PROPERTY(QString wearErrorMessage READ wearErrorMessage NOTIFY wearErrorMessageChanged)
    Q_PROPERTY(qlonglong wearBytesDone READ wearBytesDone NOTIFY wearProgressChanged)
    Q_PROPERTY(qlonglong wearBytesTotal READ wearBytesTotal NOTIFY wearProgressChanged)
    Q_PROPERTY(qlonglong wearFilesDone READ wearFilesDone NOTIFY wearProgressChanged)
    Q_PROPERTY(qlonglong wearFilesTotal READ wearFilesTotal NOTIFY wearProgressChanged)
    Q_PROPERTY(QVariantMap wearCheck READ wearCheck NOTIFY wearResultsChanged)
    Q_PROPERTY(QVariantMap wearAssessment READ wearAssessment NOTIFY wearResultsChanged)

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
    bool needsScratchFiles() const { return m_needsScratchFiles; }
    QVariantMap trend() const { return m_trend; }
    bool writeBusy() const { return m_writeBusy; }
    QString writeErrorMessage() const { return m_writeErrorMessage; }
    QVariantMap writeMeasurement() const { return m_writeMeasurement; }
    QVariantMap writeEstimate() const { return m_writeEstimate; }
    bool wearBusy() const { return m_wearBusy; }
    QString wearErrorMessage() const { return m_wearErrorMessage; }
    qlonglong wearBytesDone() const { return m_wearBytesDone; }
    qlonglong wearBytesTotal() const { return m_wearBytesTotal; }
    qlonglong wearFilesDone() const { return m_wearFilesDone; }
    qlonglong wearFilesTotal() const { return m_wearFilesTotal; }
    QVariantMap wearCheck() const { return m_wearCheck; }
    QVariantMap wearAssessment() const { return m_wearAssessment; }

    // Reads every file on the stick once (see StickSurfaceCheck) and
    // judges wear from what failed or read abnormally slowly. Read-only;
    // minutes on a big stick, hence its own progress and cancel.
    Q_INVOKABLE void checkWear(const QString &rekordboxPath, const QString &enginePath, const QString &mountPoint);
    Q_INVOKABLE void cancelWearCheck();
    // Called from the wear task's worker thread through a queued call.
    Q_INVOKABLE void applyWearProgress(qlonglong bytesDone, qlonglong bytesTotal, qlonglong filesDone, qlonglong filesTotal);

    // rekordboxPath/enginePath: empty for a catalog not present on this
    // stick, same convention as every other controller in this app.
    // mountPoint: the stick root; may be empty when a catalog path is
    // given, in which case the root is the catalog's parent.
    Q_INVOKABLE void measure(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath,
                             const QString &mountPoint);
    // For a stick with nothing to read: runs the write test, keeps its
    // files long enough to read them back, removes them. Both the read
    // and the write results come out of it. Same refusal as
    // measureWrites() while DJ software runs.
    Q_INVOKABLE void measureWithScratchFiles(const QString &stickLabel, const QString &mountPoint);
    Q_INVOKABLE void cancel();

    // The optional write test (see StickWriteProbe): throwaway files in a
    // hidden folder under the stick root, removed afterwards. Refused
    // while rekordbox or Engine DJ is running, the same guard every write
    // path in this app applies, and never on a browsed backup.
    Q_INVOKABLE void measureWrites(const QString &rekordboxPath, const QString &enginePath, const QString &mountPoint);

signals:
    void busyChanged();
    void errorMessageChanged();
    void resultsChanged();
    void cancelled();
    void writeBusyChanged();
    void writeErrorMessageChanged();
    void writeResultsChanged();
    void wearBusyChanged();
    void wearErrorMessageChanged();
    void wearProgressChanged();
    void wearResultsChanged();

private:
    void onFinished();
    void onWriteFinished();
    void onWearFinished();
    void setWearBusy(bool busy);
    void setWearErrorMessage(const QString &message);
    void startMeasure(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath,
                      const QString &mountPoint, bool useScratchFiles);
    void setBusy(bool busy);
    void setErrorMessage(const QString &message);
    void setWriteBusy(bool busy);
    void setWriteErrorMessage(const QString &message);

    QFutureWatcher<StickPerformanceResult> m_watcher;
    application::CancellationToken m_cancel;  // fresh per measure()
    QFutureWatcher<StickWriteResult> m_writeWatcher;
    QFutureWatcher<StickWearResult> m_wearWatcher;
    application::CancellationToken m_wearCancel;  // fresh per checkWear()

    bool m_busy = false;
    QString m_errorMessage;
    QVariantMap m_filesystemInfo;
    QVariantMap m_measurement;
    QVariantMap m_score;
    QVariantList m_advisories;
    QVariantMap m_facts;
    QString m_measuredAt;
    bool m_needsScratchFiles = false;
    QVariantMap m_trend;

    bool m_writeBusy = false;
    QString m_writeErrorMessage;
    QVariantMap m_writeMeasurement;
    QVariantMap m_writeEstimate;

    bool m_wearBusy = false;
    QString m_wearErrorMessage;
    qlonglong m_wearBytesDone = 0;
    qlonglong m_wearBytesTotal = 0;
    qlonglong m_wearFilesDone = 0;
    qlonglong m_wearFilesTotal = 0;
    QVariantMap m_wearCheck;
    QVariantMap m_wearAssessment;
};

}  // namespace seabass::gui
