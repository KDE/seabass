#include "stick_statistics_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>

#include "domain/disk_usage.hpp"
#include "domain/filesystem_compatibility.hpp"
#include "domain/library_statistics.hpp"
#include "gui/library_catalog_cache.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

QVariantMap toVariant(const domain::LibraryStatistics &stats)
{
    QVariantMap m;
    m["trackCount"] = stats.trackCount;
    m["playlistCount"] = stats.playlistCount;
    m["totalCuePoints"] = stats.totalCuePoints;
    m["hotCueCount"] = stats.hotCueCount;
    m["memoryCueCount"] = stats.memoryCueCount;
    m["ratedTrackCount"] = stats.ratedTrackCount;
    m["commentedTrackCount"] = stats.commentedTrackCount;
    m["streamingTrackCount"] = stats.streamingTrackCount;

    QVariantMap perKey;
    for (const auto &[key, count] : stats.tracksPerKey) {
        perKey[key.empty() ? QStringLiteral("(unknown)") : QString::fromStdString(key)] = count;
    }
    m["tracksPerKey"] = perKey;

    QVariantMap perFormat;
    for (const auto &[format, count] : stats.tracksPerFileFormat) {
        perFormat[format.empty() ? QStringLiteral("(unknown)") : QString::fromStdString(format)] = count;
    }
    m["tracksPerFileFormat"] = perFormat;

    QVariantMap byService;
    for (const auto &[service, count] : stats.streamingTracksByService) {
        byService[QString::fromStdString(service)] = count;
    }
    m["streamingTracksByService"] = byService;

    QVariantList bpmList;
    for (const auto &bucket : stats.bpmDistribution) {
        QVariantMap b;
        b["rangeStart"] = bucket.rangeStart;
        b["count"] = bucket.count;
        bpmList << b;
    }
    m["bpmDistribution"] = bpmList;

    return m;
}

QVariantMap toVariant(const domain::DiskUsageNode &node)
{
    QVariantMap m;
    m["label"] = QString::fromStdString(node.label);
    m["sizeBytes"] = QVariant::fromValue<qulonglong>(node.sizeBytes);
    QVariantList children;
    for (const auto &child : node.children) {
        children << toVariant(child);
    }
    m["children"] = children;
    return m;
}

QVariantMap toVariant(const infrastructure::system::StickHardwareInfo &hw,
                      const domain::FilesystemCompatibilityInfo &compat)
{
    QVariantMap m;
    m["filesystem"] = QString::fromStdString(hw.filesystem);
    m["displayName"] = QString::fromStdString(compat.displayName);
    m["maxFileSize"] = QString::fromStdString(compat.maxFileSize);
    m["hardwareNotes"] = QString::fromStdString(compat.hardwareNotes);
    m["recommendedForDjHardware"] = compat.recommendedForDjHardware;
    m["totalBytes"] = QVariant::fromValue<qulonglong>(hw.totalBytes);
    m["freeBytes"] = QVariant::fromValue<qulonglong>(hw.freeBytes);
    m["usbSpeedLabel"] = QString::fromStdString(hw.usbSpeedLabel);
    m["usbSpeedMbps"] = hw.usbSpeedMbps;
    m["stickIdentifier"] = QString::fromStdString(hw.stickIdentifier);
    return m;
}

// Best-effort recursive directory size, skipping anything that errors
// (permission-denied entries, a symlink loop, the stick being unplugged
// mid-walk) rather than aborting the whole statistics scan over it.
std::uint64_t directorySizeBytes(const std::string &dir, const application::CancellationToken &cancel)
{
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec) || ec) {
        return 0;
    }
    std::uint64_t total = 0;
    std::uint64_t entries = 0;
    auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
    auto end = fs::recursive_directory_iterator();
    for (; !ec && it != end; it.increment(ec)) {
        if ((++entries & 0xFF) == 0) {
            cancel.throwIfCancelled();  // a walk of a big stick is the slow part of this scan
        }
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && !fileEc) {
            auto size = it->file_size(fileEc);
            if (!fileEc) {
                total += size;
            }
        }
    }
    return total;
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

// Runs entirely on a background thread (see
// StickStatisticsController::scan()) -- no access to the controller.
StickStatisticsScanResult runScanTask(QString stickLabel, QString rekordboxPath, QString enginePath,
                                      application::CancellationToken cancel)
{
    StickStatisticsScanResult result;
    auto &noProgress = application::NullProgressReporter::instance();
    try {
        std::string stickRoot = stickRootFromPaths(rekordboxPath, enginePath);
        auto hwInfo = infrastructure::system::readStickHardwareInfo(stickRoot, stickLabel.toStdString());
        auto compat = domain::FilesystemCompatibility::lookup(hwInfo.filesystem);
        result.filesystemInfo = toVariant(hwInfo, compat);

        std::vector<domain::Track> combinedTracks;
        std::set<std::string> seenFilePaths;

        auto &catalogCache = LibraryCatalogCache::instance();

        if (!rekordboxPath.isEmpty()) {
            auto tracks = catalogCache.tracksFor("rekordbox", rekordboxPath.toStdString(), noProgress, cancel);
            result.rekordboxStats = toVariant(domain::LibraryStatisticsCalculator::calculate(tracks));
            for (auto &t : tracks) {
                if (!t.filePath.empty() && seenFilePaths.insert(t.filePath).second) {
                    combinedTracks.push_back(std::move(t));
                }
            }
        }
        if (!enginePath.isEmpty()) {
            auto tracks = catalogCache.tracksFor("engine", enginePath.toStdString(), noProgress, cancel);
            result.engineStats = toVariant(domain::LibraryStatisticsCalculator::calculate(tracks));
            for (auto &t : tracks) {
                // Streaming tracks have no local file by design (see
                // Track::streamingSource) -- never counted as disk usage.
                if (!t.streamingSource.empty()) {
                    continue;
                }
                if (!t.filePath.empty() && seenFilePaths.insert(t.filePath).second) {
                    combinedTracks.push_back(std::move(t));
                }
            }
        }
        if (!rekordboxPath.isEmpty() &&
            infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString())) {
            auto tracks = catalogCache.tracksFor("onelibrary", rekordboxPath.toStdString(), noProgress, cancel);
            result.oneLibraryStats = toVariant(domain::LibraryStatisticsCalculator::calculate(tracks));
            // OneLibrary's own tracks reference the same physical files
            // already counted via rekordbox/Engine above -- not added to
            // combinedTracks again, that would double-count disk usage.
        }

        // Disk usage breakdown. Audio Files/Artwork come from the
        // already-scanned, deduplicated track list (no extra filesystem
        // walk needed); Database & Analysis Files is a directory-size
        // walk of the catalog root(s) themselves (export.pdb, ANLZ
        // analysis files, Engine's Database2/*), with the artwork
        // subtotal subtracted back out since rekordbox stores artwork
        // inside its own PIONEER root and would otherwise be counted
        // twice.
        std::uint64_t audioBytes = 0;
        std::uint64_t artworkBytes = 0;
        std::set<std::string> distinctArtwork;
        for (const auto &t : combinedTracks) {
            audioBytes += t.fileSizeBytes;
            if (!t.artworkPath.empty() && distinctArtwork.insert(t.artworkPath).second) {
                std::error_code ec;
                auto size = fs::file_size(t.artworkPath, ec);
                if (!ec) {
                    artworkBytes += size;
                }
            }
        }

        std::uint64_t metadataBytes = 0;
        if (!rekordboxPath.isEmpty()) {
            metadataBytes += directorySizeBytes(rekordboxPath.toStdString(), cancel);
        }
        if (!enginePath.isEmpty()) {
            metadataBytes += directorySizeBytes(enginePath.toStdString(), cancel);
        }
        metadataBytes = metadataBytes > artworkBytes ? metadataBytes - artworkBytes : 0;

        std::uint64_t usedBytes = hwInfo.totalBytes > hwInfo.freeBytes ? hwInfo.totalBytes - hwInfo.freeBytes : 0;
        std::uint64_t accountedBytes = audioBytes + artworkBytes + metadataBytes;
        std::uint64_t otherBytes = usedBytes > accountedBytes ? usedBytes - accountedBytes : 0;

        domain::DiskUsageNode audioNode = domain::DiskUsageAnalyzer::byArtist(combinedTracks, 10);
        audioNode.label = "Audio Files";

        domain::DiskUsageNode root;
        root.label = stickLabel.toStdString();
        root.sizeBytes = hwInfo.totalBytes;
        root.children = {
            audioNode,
            {"Artwork", artworkBytes, {}},
            {"Database & Analysis Files", metadataBytes, {}},
            {"Other / Unaccounted", otherBytes, {}},
            {"Free Space", hwInfo.freeBytes, {}},
        };

        QVariantMap diskUsageMap;
        diskUsageMap["totalBytes"] = QVariant::fromValue<qulonglong>(hwInfo.totalBytes);
        diskUsageMap["usedBytes"] = QVariant::fromValue<qulonglong>(usedBytes);
        diskUsageMap["freeBytes"] = QVariant::fromValue<qulonglong>(hwInfo.freeBytes);
        diskUsageMap["root"] = toVariant(root);
        result.diskUsage = diskUsageMap;
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

StickStatisticsController::StickStatisticsController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<StickStatisticsScanResult>::finished, this,
            &StickStatisticsController::onScanFinished);
}

void StickStatisticsController::scan(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath)
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setBusy(true);
    m_scanCancel = application::CancellationToken();
    m_watcher.setFuture(QtConcurrent::run(runScanTask, stickLabel, rekordboxPath, enginePath, m_scanCancel));
}

void StickStatisticsController::cancelScan()
{
    if (m_busy) {
        m_scanCancel.cancel();
    }
}

void StickStatisticsController::onScanFinished()
{
    StickStatisticsScanResult result = m_watcher.result();
    setBusy(false);
    if (result.cancelled) {
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        return;
    }
    m_filesystemInfo = result.filesystemInfo;
    m_rekordboxStats = result.rekordboxStats;
    m_engineStats = result.engineStats;
    m_oneLibraryStats = result.oneLibraryStats;
    m_diskUsage = result.diskUsage;
    emit resultsChanged();
}

void StickStatisticsController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void StickStatisticsController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
