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

// USB Stick Performance: reads real files on the stick the way a player
// does and says what that means per player generation. measure() is
// read-only, like Library Statistics: no write lock, no backup, nothing
// written to the stick. It reads the library's own files when there is a
// library, any other files on the stick otherwise, and reports
// needsScratchFiles when there is nothing at all; then
// measureWithScratchFiles() writes the write test's throwaway files,
// reads them back, and removes them, which is the only way to measure a
// blank stick. Nothing is persisted; the result goes into
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
    Q_PROPERTY(bool needsScratchFiles READ needsScratchFiles NOTIFY resultsChanged)
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
    bool needsScratchFiles() const { return m_needsScratchFiles; }
    bool writeBusy() const { return m_writeBusy; }
    QString writeErrorMessage() const { return m_writeErrorMessage; }
    QVariantMap writeMeasurement() const { return m_writeMeasurement; }
    QVariantMap writeEstimate() const { return m_writeEstimate; }

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

private:
    void onFinished();
    void onWriteFinished();
    void startMeasure(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath,
                      const QString &mountPoint, bool useScratchFiles);
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
    bool m_needsScratchFiles = false;

    bool m_writeBusy = false;
    QString m_writeErrorMessage;
    QVariantMap m_writeMeasurement;
    QVariantMap m_writeEstimate;
};

}  // namespace seabass::gui
