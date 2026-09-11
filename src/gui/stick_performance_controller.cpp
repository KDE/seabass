#include "stick_performance_controller.hpp"

#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <random>
#include <set>
#include <stdexcept>

#include "domain/filesystem_compatibility.hpp"
#include "domain/stick_performance.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/stick_performance_cache.hpp"
#include "infrastructure/benchmark/stick_performance_probe.hpp"
#include "infrastructure/benchmark/stick_write_probe.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"
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
    v["smallFileOpensPerSecond"] = m.smallFileOpensPerSecond;
    v["smallFileMedianMs"] = m.smallFileMedianMs;
    v["smallFilesRead"] = m.smallFilesRead;
    v["databaseBytes"] = QVariant::fromValue<qulonglong>(m.databaseBytes);
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

std::string stickRootFromPaths(const QString &rekordboxPath, const QString &enginePath)
{
    if (!rekordboxPath.isEmpty()) {
        return fs::path(rekordboxPath.toStdString()).parent_path().string();
    }
    if (!enginePath.isEmpty()) {
        return fs::path(enginePath.toStdString()).parent_path().string();
    }
    return "";
}

// Every regular file under dir whose name passes `accept`, plus the folder
// count; best-effort, skipping anything that errors, like Library
// Statistics' own directory-size walk.
struct WalkResult
{
    std::vector<std::string> files;
    std::uint64_t folders = 0;
};

template <typename Accept>
WalkResult walk(const fs::path &dir, Accept accept, const application::CancellationToken &cancel)
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
            ++result.folders;
        } else if (it->is_regular_file(entryEc) && !entryEc && accept(it->path())) {
            result.files.push_back(it->path().string());
        }
    }
    return result;
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
                                      application::CancellationToken cancel)
{
    StickPerformanceResult result;
    auto &noProgress = application::NullProgressReporter::instance();
    try {
        std::string stickRoot = stickRootFromPaths(rekordboxPath, enginePath);
        auto hwInfo = infrastructure::system::readStickHardwareInfo(stickRoot, stickLabel.toStdString());
        auto compat = domain::FilesystemCompatibility::lookup(hwInfo.filesystem);
        result.filesystemInfo = toVariant(hwInfo, compat);

        // Audio sample: 20 real files spread through the catalog(s), the
        // same choice Library Statistics' old benchmark made.
        std::vector<std::string> audioFiles;
        std::set<std::string> seen;
        std::vector<std::string> databaseFiles;
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
        }
        if (!enginePath.isEmpty()) {
            collect("engine", enginePath);
            databaseFiles.push_back((fs::path(enginePath.toStdString()) / "Database2" / "m.db").string());
        }
        const std::size_t audioCount = audioFiles.size();
        audioFiles = spread(std::move(audioFiles), 20);

        // Small files: rekordbox's per-track analysis files, or Engine's
        // overview data when there is no rekordbox export. Their count
        // and folder count are the facts line's "analysis files".
        WalkResult analysis;
        if (!rekordboxPath.isEmpty()) {
            analysis = walk(fs::path(rekordboxPath.toStdString()) / "USBANLZ",
                            [](const fs::path &p) { return p.filename() == "ANLZ0000.DAT"; }, cancel);
        }
        if (analysis.files.empty() && !enginePath.isEmpty()) {
            analysis = walk(fs::path(enginePath.toStdString()) / "Database2" / "OverviewData",
                            [](const fs::path &) { return true; }, cancel);
        }
        const std::size_t analysisCount = analysis.files.size();
        // Shuffled with a fixed seed rather than spread: analysis folders
        // are named by hash, so neighbours in sorted order say nothing,
        // and a fixed seed keeps two runs comparable.
        std::vector<std::string> smallFiles = analysis.files;
        std::shuffle(smallFiles.begin(), smallFiles.end(), std::mt19937(0x5EABA55u));
        if (smallFiles.size() > 150) {
            smallFiles.resize(150);
        }

        auto measurement = infrastructure::benchmark::StickPerformanceProbe::run(audioFiles, smallFiles, databaseFiles, cancel);
        auto score = domain::scoreDjWorkload(measurement);
        result.measurement = toVariant(measurement);
        result.score = toVariant(score);
        result.advisories = toVariant(domain::advisePlayers(measurement, score));

        QVariantMap facts;
        facts["clusterBytes"] = QVariant::fromValue<qulonglong>(hwInfo.clusterBytes);
        facts["analysisFiles"] = QVariant::fromValue<qulonglong>(analysisCount);
        facts["analysisFolders"] = QVariant::fromValue<qulonglong>(analysis.folders);
        facts["audioFiles"] = QVariant::fromValue<qulonglong>(audioCount);
        result.facts = facts;

        StickPerformanceCache::instance().store(hwInfo.stickIdentifier, measurement);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
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

StickWriteResult runWriteTask(std::string stickRoot)
{
    StickWriteResult result;
    try {
        auto measurement = infrastructure::benchmark::StickWriteProbe::run(stickRoot);
        result.measurement = toVariant(measurement);
        result.estimate = toVariant(domain::estimateWriteWorkloads(measurement));
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
}

void StickPerformanceController::measureWrites(const QString &rekordboxPath, const QString &enginePath)
{
    if (m_writeBusy) {
        return;
    }
    std::string stickRoot = stickRootFromPaths(rekordboxPath, enginePath);
    if (stickRoot.empty()) {
        setWriteErrorMessage(QStringLiteral("No stick to write a test onto."));
        return;
    }
    std::string running = infrastructure::system::conflictingDjSoftwareName();
    if (!running.empty()) {
        setWriteErrorMessage(QStringLiteral("Close %1 before running the write test; it may be writing this stick too.")
                                 .arg(QString::fromStdString(running)));
        return;
    }
    setWriteErrorMessage({});
    setWriteBusy(true);
    m_writeWatcher.setFuture(QtConcurrent::run(runWriteTask, stickRoot));
}

void StickPerformanceController::onWriteFinished()
{
    StickWriteResult result = m_writeWatcher.result();
    setWriteBusy(false);
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
                                         const QString &enginePath)
{
    if (m_busy) {
        return;
    }
    if (rekordboxPath.isEmpty() && enginePath.isEmpty()) {
        setErrorMessage(QStringLiteral("No library on this stick to measure against."));
        return;
    }
    setErrorMessage({});
    setBusy(true);
    m_cancel = application::CancellationToken();
    m_watcher.setFuture(QtConcurrent::run(runMeasureTask, stickLabel, rekordboxPath, enginePath, m_cancel));
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
    m_measurement = result.measurement;
    m_score = result.score;
    m_advisories = result.advisories;
    m_facts = result.facts;
    m_measuredAt = QDateTime::currentDateTime().toString(QStringLiteral("d MMM yyyy, HH:mm"));
    emit resultsChanged();
}

void StickPerformanceController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
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
