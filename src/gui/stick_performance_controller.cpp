#include "stick_performance_controller.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>

#include "domain/filesystem_compatibility.hpp"
#include "domain/stick_performance.hpp"
#include "gui/future_result.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/write_guard.hpp"
#include "infrastructure/benchmark/stick_performance_probe.hpp"
#include "infrastructure/benchmark/stick_surface_check.hpp"
#include "infrastructure/benchmark/stick_write_probe.hpp"
#include "infrastructure/local/stick_performance_history.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/scratch_dir_guard.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

QVariantMap toVariant(const infrastructure::system::StickHardwareInfo &hw,
                      const domain::FilesystemCompatibilityInfo &compat)
{
    QVariantMap m;
    m["filesystem"] = QString::fromStdString(hw.filesystem);
    m["displayName"] = QString::fromStdString(compat.displayName);
    m["recommendedForDjHardware"] = compat.recommendedForDjHardware;
    m["totalBytes"] = QVariant::fromValue<qulonglong>(hw.totalBytes);
    m["freeBytes"] = QVariant::fromValue<qulonglong>(hw.freeBytes);
    m["clusterBytes"] = QVariant::fromValue<qulonglong>(hw.clusterBytes);
    m["usbSpeedLabel"] = QString::fromStdString(hw.usbSpeedLabel);
    m["usbSpeedMbps"] = hw.usbSpeedMbps;
    m["stickIdentifier"] = QString::fromStdString(hw.stickIdentifier);
    return m;
}

QVariantMap toVariant(const domain::StickPerformanceMeasurement &m)
{
    QVariantMap v;
    v["streamingBytesPerSecond"] = m.streamingBytesPerSecond;
    v["randomReadMedianMs"] = m.randomReadMedianMs;
    v["randomReadP95Ms"] = m.randomReadP95Ms;
    v["randomReads"] = m.randomReads;
    v["randomReadOutliers"] = m.randomReadOutliers;
    v["smallFileOutliers"] = m.smallFileOutliers;
    v["smallFileOpensPerSecond"] = m.smallFileOpensPerSecond;
    v["smallFileMedianMs"] = m.smallFileMedianMs;
    v["smallFilesRead"] = m.smallFilesRead;
    v["catalogBytes"] = QVariant::fromValue<qulonglong>(m.catalogBytes);
    return v;
}

QString verdictKey(domain::Verdict verdict)
{
    switch (verdict) {
    case domain::Verdict::Fine:
        return QStringLiteral("fine");
    case domain::Verdict::Slower:
        return QStringLiteral("slower");
    case domain::Verdict::Sluggish:
        return QStringLiteral("sluggish");
    case domain::Verdict::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QVariantMap toVariant(const domain::DjWorkloadScore &s)
{
    QVariantMap v;
    v["score"] = s.score;
    v["speedClass"] = QString::fromStdString(domain::speedClassLabel(s.speedClass));
    v["speedClassKey"] = [&] {
        switch (s.speedClass) {
        case domain::SpeedClass::VeryFast:
            return QStringLiteral("veryfast");
        case domain::SpeedClass::Fast:
            return QStringLiteral("fast");
        case domain::SpeedClass::Average:
            return QStringLiteral("average");
        case domain::SpeedClass::Slow:
            return QStringLiteral("slow");
        case domain::SpeedClass::VerySlow:
            return QStringLiteral("veryslow");
        case domain::SpeedClass::Unknown:
            break;
        }
        return QStringLiteral("unknown");
    }();
    v["browseScore"] = s.browseScore;
    v["trackLoadScore"] = s.trackLoadScore;
    v["mountScore"] = s.mountScore;
    v["browseActionSeconds"] = s.browseActionSeconds;
    v["trackLoadSeconds"] = s.trackLoadSeconds;
    v["mountSeconds"] = s.mountSeconds;
    v["setWaitSeconds"] = s.setWaitSeconds;
    v["referenceSetWaitSeconds"] = s.referenceSetWaitSeconds;
    v["setWaitText"] = QString::fromStdString(domain::describeSetWait(s));
    v["browseVerdict"] = verdictKey(s.browseVerdict);
    v["trackLoadVerdict"] = verdictKey(s.trackLoadVerdict);
    v["mountVerdict"] = verdictKey(s.mountVerdict);
    v["streamingVerdict"] = verdictKey(s.streamingVerdict);
    return v;
}

QVariantList toVariant(const std::vector<domain::PlayerAdvisory> &rows)
{
    QVariantList list;
    for (const auto &row : rows) {
        QVariantMap m;
        m["group"] = QString::fromStdString(row.group);
        m["players"] = QString::fromStdString(row.players);
        m["verdict"] = verdictKey(row.verdict);
        m["verdictLabel"] = QString::fromStdString(domain::verdictLabel(row.verdict));
        m["summary"] = QString::fromStdString(row.summary);
        list << m;
    }
    return list;
}

QVariantMap toVariant(const domain::StickWriteMeasurement &m)
{
    QVariantMap v;
    v["streamingWriteBytesPerSecond"] = m.streamingWriteBytesPerSecond;
    v["smallFileWritesPerSecond"] = m.smallFileWritesPerSecond;
    v["smallFileWriteMedianMs"] = m.smallFileWriteMedianMs;
    v["smallFilesWritten"] = m.smallFilesWritten;
    v["inPlaceUpdateMedianMs"] = m.inPlaceUpdateMedianMs;
    v["inPlaceUpdates"] = m.inPlaceUpdates;
    v["bytesWritten"] = QVariant::fromValue<qulonglong>(m.bytesWritten);
    return v;
}

QVariantMap toVariant(const domain::WriteWorkloadEstimate &e)
{
    QVariantMap v;
    v["cueSaveSeconds"] = e.cueSaveSeconds;
    v["cueSaveVerdict"] = verdictKey(e.cueSaveVerdict);
    v["cueSaveVerdictLabel"] = QString::fromStdString(domain::verdictLabel(e.cueSaveVerdict));
    v["exportTrackSeconds"] = e.exportTrackSeconds;
    v["exportHundredTracksSeconds"] = e.exportHundredTracksSeconds;
    v["exportVerdict"] = verdictKey(e.exportVerdict);
    v["exportVerdictLabel"] = QString::fromStdString(domain::verdictLabel(e.exportVerdict));
    return v;
}

std::string stickRootFromPaths(const QString &rekordboxPath, const QString &enginePath, const QString &mountPoint)
{
    if (!mountPoint.isEmpty()) {
        return mountPoint.toStdString();
    }
    if (!rekordboxPath.isEmpty()) {
        return fs::path(rekordboxPath.toStdString()).parent_path().string();
    }
    if (!enginePath.isEmpty()) {
        return fs::path(enginePath.toStdString()).parent_path().string();
    }
    return "";
}

// Every regular file under dir that `accept` likes, plus the folder
// count; best-effort, skipping anything that errors, like Library
// Statistics' own directory-size walk. With skipHidden, dot-folders
// (this app's own backups, the write test's scratch folder) and
// Windows' System Volume Information are left alone.
struct WalkResult
{
    std::vector<std::string> files;
    std::uint64_t folders = 0;
};

template <typename Accept>
WalkResult walk(const fs::path &dir, Accept accept, const application::CancellationToken &cancel,
                bool skipHidden = false)
{
    WalkResult result;
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec) || ec) {
        return result;
    }
    std::uint64_t entries = 0;
    auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
    auto end = fs::recursive_directory_iterator();
    for (; !ec && it != end; it.increment(ec)) {
        if ((++entries & 0xFF) == 0) {
            cancel.throwIfCancelled();
        }
        std::error_code entryEc;
        if (it->is_directory(entryEc) && !entryEc) {
            const std::string name = it->path().filename().string();
            if (skipHidden && (name.rfind('.', 0) == 0 || name == "System Volume Information")) {
                it.disable_recursion_pending();
                continue;
            }
            ++result.folders;
        } else if (it->is_regular_file(entryEc) && !entryEc && accept(*it)) {
            result.files.push_back(it->path().string());
        }
    }
    return result;
}

std::uint64_t sizeOf(const fs::directory_entry &entry)
{
    std::error_code ec;
    auto size = entry.file_size(ec);
    return ec ? 0 : size;
}

QString trendStateKey(domain::TrendState state)
{
    switch (state) {
    case domain::TrendState::Steady:
        return QStringLiteral("steady");
    case domain::TrendState::Slowing:
        return QStringLiteral("slowing");
    case domain::TrendState::Worsened:
        return QStringLiteral("worsened");
    case domain::TrendState::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

QVariantMap toVariant(const domain::TrendAssessment &trend)
{
    QVariantMap t;
    t["state"] = trendStateKey(trend.state);
    t["summary"] = QString::fromStdString(trend.summary);
    t["earlierCount"] = trend.earlierCount;
    t["bestEarlierScore"] = trend.bestEarlierScore;
    return t;
}

std::vector<domain::TrendPoint> earlierPoints(const infrastructure::local::StickPerformanceHistory &history,
                                              const std::string &stickIdentifier)
{
    std::vector<domain::TrendPoint> earlier;
    for (const auto &r : history.forStick(stickIdentifier)) {
        earlier.push_back({r.measuredAtUtc, r.score, r.randomReadMedianMs, r.outliers, r.wearState});
    }
    return earlier;
}

// Every Nth of a sorted list, so the sample spreads over the whole
// library rather than clustering on one artist.
std::vector<std::string> spread(std::vector<std::string> files, std::size_t count)
{
    std::sort(files.begin(), files.end());
    if (files.size() <= count) {
        return files;
    }
    std::vector<std::string> out;
    std::size_t step = files.size() / count;
    for (std::size_t i = 0; i < files.size() && out.size() < count; i += step) {
        out.push_back(files[i]);
    }
    return out;
}

// Runs entirely on a background thread, no access to the controller.
StickPerformanceResult runMeasureTask(QString stickLabel, QString rekordboxPath, QString enginePath,
                                      QString mountPoint, bool useScratchFiles,
                                      application::CancellationToken cancel)
{
    StickPerformanceResult result;
    auto &noProgress = application::NullProgressReporter::instance();
    try {
        std::string stickRoot = stickRootFromPaths(rekordboxPath, enginePath, mountPoint);
        auto hwInfo = infrastructure::system::readStickHardwareInfo(stickRoot, stickLabel.toStdString());
        auto compat = domain::FilesystemCompatibility::lookup(hwInfo.filesystem);
        result.filesystemInfo = toVariant(hwInfo, compat);

        std::vector<std::string> audioFiles;
        std::vector<std::string> smallFiles;
        std::vector<std::string> databaseFiles;
        std::size_t audioCount = 0;
        std::size_t smallCount = 0;
        std::uint64_t smallFolders = 0;
        QString sampleKind;
        std::optional<domain::StickWriteMeasurement> writeMeasurement;
        // Removes the scratch folder on every exit path of a throwaway-file
        // measurement, the one where the read probe threw included. The
        // shared move-only guard, not a local struct: see its header for
        // the emplace-with-a-temporary bug a copyable one caused once.
        std::optional<infrastructure::ScratchDirGuard> scratchGuard;

        if (useScratchFiles) {
            // A blank stick: the write test's own files are what gets
            // read back. Written and measured first, read second, removed
            // whatever happens in between.
            infrastructure::benchmark::ScratchFiles files;
            scratchGuard.emplace(fs::path(stickRoot) / infrastructure::benchmark::StickWriteProbe::kScratchFolderName);
            writeMeasurement = infrastructure::benchmark::StickWriteProbe::run(stickRoot, cancel, {}, &files);
            audioFiles = files.streamFiles;
            smallFiles = files.smallFiles;
            audioCount = audioFiles.size();
            smallCount = smallFiles.size();
            sampleKind = QStringLiteral("scratch");
        } else {
            // Audio sample: 20 real files spread through the catalog(s),
            // the same choice Library Statistics' old benchmark made.
            std::set<std::string> seen;
            auto &catalogCache = LibraryCatalogCache::instance();
            auto collect = [&](const char *format, const QString &path) {
                auto tracks = catalogCache.tracksFor(format, path.toStdString(), noProgress, cancel);
                for (const auto &t : tracks) {
                    if (!t.streamingSource.empty() || t.filePath.empty()) {
                        continue;
                    }
                    if (seen.insert(t.filePath).second) {
                        audioFiles.push_back(t.filePath);
                    }
                }
            };
            if (!rekordboxPath.isEmpty()) {
                collect("rekordbox", rekordboxPath);
                databaseFiles.push_back(rekordboxPath.toStdString() + "/rekordbox/export.pdb");
                // rekordbox 7 sticks carry OneLibrary's SQLite file too, and
                // the players that read it read all of it at insertion.
                if (infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString())) {
                    databaseFiles.push_back(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(rekordboxPath.toStdString()));
                }
            }
            if (!enginePath.isEmpty()) {
                collect("engine", enginePath);
                databaseFiles.push_back((fs::path(enginePath.toStdString()) / "Database2" / "m.db").string());
            }

            // Small files: rekordbox's per-track analysis files, or
            // Engine's overview data when there is no rekordbox export.
            WalkResult analysis;
            if (!rekordboxPath.isEmpty()) {
                analysis = walk(fs::path(rekordboxPath.toStdString()) / "USBANLZ",
                                [](const fs::directory_entry &e) { return e.path().filename() == "ANLZ0000.DAT"; }, cancel);
            }
            if (analysis.files.empty() && !enginePath.isEmpty()) {
                analysis = walk(fs::path(enginePath.toStdString()) / "Database2" / "OverviewData",
                                [](const fs::directory_entry &) { return true; }, cancel);
            }
            sampleKind = QStringLiteral("library");

            if (audioFiles.empty()) {
                // No library, or one with no local files: any file on the
                // stick big enough to stream from and seek in will do, and
                // any small one stands in for an analysis file.
                auto big = walk(fs::path(stickRoot),
                                [](const fs::directory_entry &e) { return sizeOf(e) >= 64 * 1024; }, cancel, true);
                audioFiles = big.files;
                if (!audioFiles.empty()) {
                    analysis = walk(fs::path(stickRoot),
                                    [](const fs::directory_entry &e) {
                                        auto size = sizeOf(e);
                                        return size >= 4 * 1024 && size < 64 * 1024;
                                    },
                                    cancel, true);
                    sampleKind = QStringLiteral("files");
                }
            }
            if (audioFiles.empty()) {
                result.needsScratchFiles = true;
                return result;
            }

            audioCount = audioFiles.size();
            audioFiles = spread(std::move(audioFiles), 20);
            smallCount = analysis.files.size();
            smallFolders = analysis.folders;
            // Shuffled with a fixed seed rather than spread: analysis
            // folders are named by hash, so neighbours in sorted order say
            // nothing, and a fixed seed keeps two runs comparable.
            smallFiles = analysis.files;
            std::shuffle(smallFiles.begin(), smallFiles.end(), std::mt19937(0x5EABA55u));
            if (smallFiles.size() > 150) {
                smallFiles.resize(150);
            }
        }

        auto measurement = infrastructure::benchmark::StickPerformanceProbe::run(audioFiles, smallFiles, databaseFiles, cancel);
        auto score = domain::scoreDjWorkload(measurement);
        result.measurement = toVariant(measurement);
        result.score = toVariant(score);
        result.advisories = toVariant(domain::advisePlayers(measurement, score));

        QVariantMap facts;
        facts["clusterBytes"] = QVariant::fromValue<qulonglong>(hwInfo.clusterBytes);
        facts["analysisFiles"] = QVariant::fromValue<qulonglong>(smallCount);
        facts["analysisFolders"] = QVariant::fromValue<qulonglong>(smallFolders);
        facts["audioFiles"] = QVariant::fromValue<qulonglong>(audioCount);
        facts["sampleKind"] = sampleKind;
        result.facts = facts;

        if (writeMeasurement) {
            result.writeMeasurement = toVariant(*writeMeasurement);
            result.writeEstimate = toVariant(domain::estimateWriteWorkloads(*writeMeasurement));
        }

        // The trend: everything measured before on this computer, then
        // this run appended. A history that cannot be read or written
        // costs the trend line, never the measurement.
        try {
            infrastructure::local::StickPerformanceHistory history;
            auto earlier = earlierPoints(history, hwInfo.stickIdentifier);
            result.trend = toVariant(domain::assessTrend(score.score, measurement.randomReadMedianMs,
                                                         measurement.randomReadOutliers + measurement.smallFileOutliers,
                                                         {}, earlier));

            infrastructure::local::StickPerformanceRecord record;
            record.measuredAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
            record.stickIdentifier = hwInfo.stickIdentifier;
            record.stickLabel = stickLabel.toStdString();
            record.score = score.score;
            record.streamingBytesPerSecond = measurement.streamingBytesPerSecond;
            record.randomReadMedianMs = measurement.randomReadMedianMs;
            record.smallFileMedianMs = measurement.smallFileMedianMs;
            record.outliers = measurement.randomReadOutliers + measurement.smallFileOutliers;
            history.append(record);
        } catch (const std::exception &) {
            // No trend this time.
        }
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

QString wearStateKey(domain::WearState state)
{
    switch (state) {
    case domain::WearState::Healthy:
        return QStringLiteral("healthy");
    case domain::WearState::Watch:
        return QStringLiteral("watch");
    case domain::WearState::Failing:
        return QStringLiteral("failing");
    case domain::WearState::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

// Runs on a worker thread; progress goes back to the controller through
// queued calls, the same way StickBackupController's backup does.
StickWearResult runWearTask(std::string stickRoot, std::string stickLabel, std::string stickIdentifier,
                            domain::StickPerformanceMeasurement probe, application::CancellationToken cancel,
                            StickPerformanceController *controller)
{
    StickWearResult result;
    try {
        // Without a completed measurement there is no identifier on the
        // controller yet; the stick can still say who it is.
        if (stickIdentifier.empty()) {
            stickIdentifier = infrastructure::system::readStickHardwareInfo(stickRoot, stickLabel).stickIdentifier;
        }
        QElapsedTimer sinceLast;
        sinceLast.start();
        auto progress = [&](std::uint64_t bytesDone, std::uint64_t bytesTotal, std::uint64_t filesDone,
                            std::uint64_t filesTotal) {
            // At most a few updates a second: a small-file walk would
            // otherwise flood the event loop.
            if (sinceLast.elapsed() < 200 && filesDone != filesTotal) {
                return;
            }
            sinceLast.restart();
            QMetaObject::invokeMethod(
                controller,
                [controller, bytesDone, bytesTotal, filesDone, filesTotal] {
                    controller->applyWearProgress(static_cast<qlonglong>(bytesDone), static_cast<qlonglong>(bytesTotal),
                                                  static_cast<qlonglong>(filesDone), static_cast<qlonglong>(filesTotal));
                },
                Qt::QueuedConnection);
        };
        auto check = infrastructure::benchmark::StickSurfaceCheck::run(stickRoot, progress, cancel);
        auto assessment = domain::assessWear(check, probe);

        QVariantMap c;
        c["filesRead"] = QVariant::fromValue<qulonglong>(check.filesRead);
        c["bytesRead"] = QVariant::fromValue<qulonglong>(check.bytesRead);
        c["medianBytesPerSecond"] = check.medianBytesPerSecond;
        c["seconds"] = check.seconds;
        QVariantList unreadable;
        for (const auto &path : check.unreadable) {
            unreadable << QString::fromStdString(path);
        }
        c["unreadable"] = unreadable;
        QVariantList slow;
        for (const auto &f : check.slow) {
            QVariantMap m;
            m["path"] = QString::fromStdString(f.path);
            m["bytesPerSecond"] = f.bytesPerSecond;
            slow << m;
        }
        c["slow"] = slow;
        result.check = c;

        QVariantMap a;
        a["state"] = wearStateKey(assessment.state);
        a["label"] = QString::fromStdString(assessment.label);
        a["summary"] = QString::fromStdString(assessment.summary);
        result.assessment = a;

        // Remembered with the latest measurement of this stick, and the
        // trend recomputed against the earlier ones, which is the only
        // way "was healthy in June, not now" can ever be said.
        if (!stickIdentifier.empty()) {
            try {
                infrastructure::local::StickPerformanceHistory history;
                auto records = history.forStick(stickIdentifier);
                if (!records.empty()) {
                    const auto &latest = records.back();
                    std::vector<domain::TrendPoint> earlier;
                    for (std::size_t i = 0; i + 1 < records.size(); ++i) {
                        earlier.push_back({records[i].measuredAtUtc, records[i].score, records[i].randomReadMedianMs,
                                           records[i].outliers, records[i].wearState});
                    }
                    result.trend = toVariant(domain::assessTrend(latest.score, latest.randomReadMedianMs, latest.outliers,
                                                                 wearStateKey(assessment.state).toStdString(), earlier));
                }
                history.setLatestWearState(stickIdentifier, wearStateKey(assessment.state).toStdString());
            } catch (const std::exception &) {
            }
        }
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

StickWriteResult runWriteTask(std::string stickRoot, application::CancellationToken cancel)
{
    StickWriteResult result;
    try {
        auto measurement = infrastructure::benchmark::StickWriteProbe::run(stickRoot, cancel);
        result.measurement = toVariant(measurement);
        result.estimate = toVariant(domain::estimateWriteWorkloads(measurement));
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

StickPerformanceController::StickPerformanceController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<StickPerformanceResult>::finished, this,
            &StickPerformanceController::onFinished);
    connect(&m_writeWatcher, &QFutureWatcher<StickWriteResult>::finished, this,
            &StickPerformanceController::onWriteFinished);
    connect(&m_wearWatcher, &QFutureWatcher<StickWearResult>::finished, this,
            &StickPerformanceController::onWearFinished);
}

void StickPerformanceController::checkWear(const QString &stickLabel, const QString &rekordboxPath,
                                           const QString &enginePath, const QString &mountPoint)
{
    if (anyBusy()) {
        return;
    }
    std::string stickRoot = stickRootFromPaths(rekordboxPath, enginePath, mountPoint);
    if (stickRoot.empty()) {
        setWearErrorMessage(QStringLiteral("No stick to check."));
        return;
    }
    // The probe's tail counts feed the assessment when they exist; an
    // unmeasured stick is judged on the surface check alone.
    domain::StickPerformanceMeasurement probe;
    probe.randomReads = m_measurement.value("randomReads").toInt();
    probe.smallFilesRead = m_measurement.value("smallFilesRead").toInt();
    probe.randomReadOutliers = m_measurement.value("randomReadOutliers").toInt();
    probe.smallFileOutliers = m_measurement.value("smallFileOutliers").toInt();
    setWearErrorMessage({});
    m_wearBytesDone = m_wearBytesTotal = m_wearFilesDone = m_wearFilesTotal = 0;
    emit wearProgressChanged();
    setWearBusy(true);
    m_wearCancel = application::CancellationToken();
    m_wearWatcher.setFuture(QtConcurrent::run(runWearTask, stickRoot, stickLabel.toStdString(),
                                              m_filesystemInfo.value("stickIdentifier").toString().toStdString(), probe,
                                              m_wearCancel, this));
}

void StickPerformanceController::cancelWearCheck()
{
    if (m_wearBusy) {
        m_wearCancel.cancel();
    }
}

void StickPerformanceController::applyWearProgress(qlonglong bytesDone, qlonglong bytesTotal, qlonglong filesDone,
                                                   qlonglong filesTotal)
{
    m_wearBytesDone = bytesDone;
    m_wearBytesTotal = bytesTotal;
    m_wearFilesDone = filesDone;
    m_wearFilesTotal = filesTotal;
    emit wearProgressChanged();
}

void StickPerformanceController::onWearFinished()
{
    StickWearResult result = m_wearWatcher.result();
    setWearBusy(false);
    if (result.cancelled) {
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setWearErrorMessage(result.errorMessage);
        return;
    }
    m_wearCheck = result.check;
    m_wearAssessment = result.assessment;
    emit wearResultsChanged();
    if (!result.trend.isEmpty()) {
        m_trend = result.trend;
        emit resultsChanged();
    }
}

void StickPerformanceController::setWearBusy(bool busy)
{
    if (m_wearBusy == busy) {
        return;
    }
    m_wearBusy = busy;
    emit wearBusyChanged();
    emit anyBusyChanged();
}

void StickPerformanceController::setWearErrorMessage(const QString &message)
{
    if (m_wearErrorMessage == message) {
        return;
    }
    m_wearErrorMessage = message;
    emit wearErrorMessageChanged();
}

StickPerformanceController::~StickPerformanceController()
{
    m_cancel.cancel();
    m_writeCancel.cancel();
    m_wearCancel.cancel();
    awaitQuietly(m_watcher);
    awaitQuietly(m_writeWatcher);
    awaitQuietly(m_wearWatcher);
}

void StickPerformanceController::measureWrites(const QString &rekordboxPath, const QString &enginePath,
                                               const QString &mountPoint)
{
    if (anyBusy()) {
        return;
    }
    std::string stickRoot = stickRootFromPaths(rekordboxPath, enginePath, mountPoint);
    if (stickRoot.empty()) {
        setWriteErrorMessage(QStringLiteral("No stick to write a test onto."));
        return;
    }
    if (QString refusal = refuseIfDjSoftwareRunning(); !refusal.isEmpty()) {
        setWriteErrorMessage(refusal);
        return;
    }
    setWriteErrorMessage({});
    setWriteBusy(true);
    m_writeCancel = application::CancellationToken();
    m_writeWatcher.setFuture(QtConcurrent::run(runWriteTask, stickRoot, m_writeCancel));
}

void StickPerformanceController::cancelWrites()
{
    if (m_writeBusy) {
        m_writeCancel.cancel();
    }
}

void StickPerformanceController::onWriteFinished()
{
    StickWriteResult result = m_writeWatcher.result();
    setWriteBusy(false);
    if (result.cancelled) {
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setWriteErrorMessage(result.errorMessage);
        return;
    }
    m_writeMeasurement = result.measurement;
    m_writeEstimate = result.estimate;
    emit writeResultsChanged();
}

void StickPerformanceController::setWriteBusy(bool busy)
{
    if (m_writeBusy == busy) {
        return;
    }
    m_writeBusy = busy;
    emit writeBusyChanged();
    emit anyBusyChanged();
}

void StickPerformanceController::setWriteErrorMessage(const QString &message)
{
    if (m_writeErrorMessage == message) {
        return;
    }
    m_writeErrorMessage = message;
    emit writeErrorMessageChanged();
}

void StickPerformanceController::measure(const QString &stickLabel, const QString &rekordboxPath,
                                         const QString &enginePath, const QString &mountPoint)
{
    if (rekordboxPath.isEmpty() && enginePath.isEmpty() && mountPoint.isEmpty()) {
        setErrorMessage(QStringLiteral("No stick to measure."));
        return;
    }
    startMeasure(stickLabel, rekordboxPath, enginePath, mountPoint, false);
}

void StickPerformanceController::measureWithScratchFiles(const QString &stickLabel, const QString &mountPoint)
{
    if (mountPoint.isEmpty()) {
        setErrorMessage(QStringLiteral("No stick to write a test onto."));
        return;
    }
    if (QString refusal = refuseIfDjSoftwareRunning(); !refusal.isEmpty()) {
        setErrorMessage(refusal);
        return;
    }
    startMeasure(stickLabel, {}, {}, mountPoint, true);
}

void StickPerformanceController::startMeasure(const QString &stickLabel, const QString &rekordboxPath,
                                              const QString &enginePath, const QString &mountPoint,
                                              bool useScratchFiles)
{
    if (anyBusy()) {
        return;
    }
    setErrorMessage({});
    setBusy(true);
    m_cancel = application::CancellationToken();
    m_watcher.setFuture(
        QtConcurrent::run(runMeasureTask, stickLabel, rekordboxPath, enginePath, mountPoint, useScratchFiles, m_cancel));
}

void StickPerformanceController::cancel()
{
    if (m_busy) {
        m_cancel.cancel();
    }
}

void StickPerformanceController::onFinished()
{
    StickPerformanceResult result = m_watcher.result();
    setBusy(false);
    if (result.cancelled) {
        emit cancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        return;
    }
    m_filesystemInfo = result.filesystemInfo;
    m_needsScratchFiles = result.needsScratchFiles;
    if (result.needsScratchFiles) {
        emit resultsChanged();
        return;
    }
    m_measurement = result.measurement;
    m_score = result.score;
    m_advisories = result.advisories;
    m_facts = result.facts;
    m_trend = result.trend;
    m_measuredAt = QDateTime::currentDateTime().toString(QStringLiteral("d MMM yyyy, HH:mm"));
    emit resultsChanged();
    if (!result.writeEstimate.isEmpty()) {
        m_writeMeasurement = result.writeMeasurement;
        m_writeEstimate = result.writeEstimate;
        setWriteErrorMessage({});
        emit writeResultsChanged();
    }
}

void StickPerformanceController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
    emit anyBusyChanged();
}

void StickPerformanceController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
