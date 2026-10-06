// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "stick_statistics_controller.hpp"

#include "gui/stick_path.hpp"
#include "gui/future_result.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>

#include "application/phased_progress.hpp"
#include "domain/disk_usage.hpp"
#include "domain/filesystem_compatibility.hpp"
#include "domain/library_statistics.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/main_thread_shared.hpp"
#include "gui/qt_path.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"
#include "storageprobe/walk_tree.hpp"
#include "application/use_cases/fill_file_sizes.hpp"

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

    // Local tracks only (streaming ones excluded), so withCues +
    // withoutCues is trackCount - streamingTrackCount, not trackCount.
    QVariantMap coverage;
    coverage["withCues"] = stats.cueCoverage.withCues;
    coverage["withoutCues"] = stats.cueCoverage.withoutCues;
    QVariantList perTrack;
    for (const auto &bucket : stats.cueCoverage.cuesPerTrack) {
        QVariantMap b;
        b["label"] = QString::fromStdString(domain::cueCountBucketLabel(bucket));
        b["count"] = bucket.count;
        perTrack << b;
    }
    coverage["cuesPerTrack"] = perTrack;
    m["cueCoverage"] = coverage;

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
//
// That is what this always claimed to do and did not: with
// recursive_directory_iterator, the first error ended the loop, so on any
// macOS stick the walk stopped at .Spotlight-V100 -- EPERM, which
// skip_permission_denied does not cover -- and the size came back 0.
// storageprobe::walkTree skips the directory instead of the stick.
//
// Counted on the bar in slices (storageprobe::countWalkSlices: about one
// track's analysis folder each on a rekordbox export): directoryWalkUnits()
// is the stretch's size, counted before the scan starts, and the walk
// ticks it as each slice is finished.
std::size_t directoryWalkUnits(const std::string &dir)
{
    std::error_code ec;
    if (dir.empty() || !fs::exists(pathFromUtf8(dir), ec) || ec) {
        return 0;
    }
    return storageprobe::countWalkSlices(dir) + 1;  // and one for the files above the slices
}

std::uint64_t directorySizeBytes(const std::string &dir, const application::CancellationToken &cancel,
                                 application::ProgressReporter &progress, const std::string &label,
                                 std::size_t plannedUnits)
{
    std::error_code ec;
    if (dir.empty() || !fs::exists(pathFromUtf8(dir), ec) || ec) {
        return 0;
    }
    progress.start(label, plannedUnits);
    auto walk = storageprobe::walkTree(
        dir, {},
        [&cancel](std::uint64_t) {
            cancel.throwIfCancelled();  // a walk of a big stick is the slow part of this scan
        },
        [&cancel, &progress](std::uint64_t slicesDone) {
            cancel.throwIfCancelled();
            progress.tick(slicesDone);
        });
    progress.finish();
    std::uint64_t total = 0;
    for (const auto &file : walk.files) {
        total += file.size;
    }
    return total;
}

std::string stickRootFromPaths(const QString &rekordboxPath, const QString &enginePath)
{
    if (!rekordboxPath.isEmpty()) {
        return pathToUtf8(pathFromQString(rekordboxPath).parent_path());
    }
    if (!enginePath.isEmpty()) {
        return pathToUtf8(pathFromQString(enginePath).parent_path());
    }
    return "";
}

// Runs entirely on a background thread (see
// StickStatisticsController::scan()) -- no access to the controller.
// `reporter` gets one bar for the whole scan (#58): counted first, from
// the catalogs' row counts and a few directory listings, announced once,
// and every stretch below lands on it in turn.
StickStatisticsScanResult runScanTask(QString stickLabel, QString rekordboxPath, QString enginePath,
                                      std::shared_ptr<QtProgressReporter> reporter,
                                      application::CancellationToken cancel)
{
    StickStatisticsScanResult result;
    try {
        std::string stickRoot = stickRootFromPaths(rekordboxPath, enginePath);
        auto &catalogCache = LibraryCatalogCache::instance();
        const bool hasOneLibrary = !rekordboxPath.isEmpty()
            && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString());

        // The plan. One unit for the stick's facts and one for the
        // artwork total (a stat per cover the Full read has just looked
        // at, and one query on the Engine database), the reads the cache
        // still has to make, and the two folder walks in slices.
        std::optional<std::size_t> plannedTotal = 2;
        const auto plan = [&plannedTotal](std::optional<std::size_t> units) {
            plannedTotal = plannedTotal && units ? std::optional<std::size_t>(*plannedTotal + *units) : std::nullopt;
        };
        if (!rekordboxPath.isEmpty()) {
            plan(catalogCache.plannedUnits("rekordbox", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Full,
                                           cancel));
        }
        if (!enginePath.isEmpty()) {
            plan(catalogCache.plannedUnits("engine", enginePath.toStdString(), LibraryCatalogCache::Detail::Full,
                                           cancel));
        }
        if (hasOneLibrary) {
            plan(catalogCache.plannedUnits("onelibrary", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Full,
                                           cancel));
        }
        const std::size_t rekordboxWalkUnits = directoryWalkUnits(rekordboxPath.toStdString());
        const std::size_t engineWalkUnits = directoryWalkUnits(enginePath.toStdString());
        plan(rekordboxWalkUnits + engineWalkUnits);
        cancel.throwIfCancelled();

        application::PhasedProgress progress(*reporter, "Scanning stick statistics", plannedTotal.value_or(0));

        progress.start("Reading the stick's facts", 0);
        auto hwInfo = infrastructure::system::readStickHardwareInfo(stickRoot, stickLabel.toStdString());
        auto compat = domain::FilesystemCompatibility::lookup(hwInfo.filesystem);
        result.filesystemInfo = toVariant(hwInfo, compat);
        progress.finish();

        std::vector<domain::Track> combinedTracks;
        std::set<std::string> seenFilePaths;

        if (!rekordboxPath.isEmpty()) {
            auto tracks = catalogCache.tracksFor("rekordbox", rekordboxPath.toStdString(), progress, cancel);
            result.rekordboxStats = toVariant(domain::LibraryStatisticsCalculator::calculate(tracks));
            for (auto &t : tracks) {
                if (!t.filePath.empty() && seenFilePaths.insert(t.filePath).second) {
                    combinedTracks.push_back(std::move(t));
                }
            }
        }
        if (!enginePath.isEmpty()) {
            auto tracks = catalogCache.tracksFor("engine", enginePath.toStdString(), progress, cancel);
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
        if (hasOneLibrary) {
            auto tracks = catalogCache.tracksFor("onelibrary", rekordboxPath.toStdString(), progress, cancel);
            result.oneLibraryStats = toVariant(domain::LibraryStatisticsCalculator::calculate(tracks));
            // OneLibrary's own tracks reference the same physical files
            // already counted via rekordbox/Engine above -- not added to
            // combinedTracks again, that would double-count disk usage.
        }

        // Disk usage breakdown. Audio Files/Artwork come from the
        // already-scanned, deduplicated track list (no extra filesystem
        // walk needed), plus the images an older Engine library keeps in
        // its database; Database & Analysis Files is a directory-size
        // walk of the catalog root(s) themselves (export.pdb, ANLZ
        // analysis files, Engine's Database2/*), with the artwork
        // subtotal subtracted back out since rekordbox stores artwork
        // inside its own PIONEER root and would otherwise be counted
        // twice.
        std::uint64_t audioBytes = 0;
        std::uint64_t artworkBytes = 0;
        for (const auto &t : combinedTracks) {
            audioBytes += t.fileSizeBytes;
        }
        // When the Engine database could not be read, the other covers are
        // still counted and the figure says the Engine part is missing.
        progress.start("Measuring artwork", 0);
        const infrastructure::engine::ArtworkBytes measuredArtwork =
            infrastructure::engine::artworkBytesOnStick(combinedTracks, enginePath.toStdString());
        artworkBytes = measuredArtwork.bytes;
        progress.finish();

        std::uint64_t metadataBytes = 0;
        if (!rekordboxPath.isEmpty()) {
            metadataBytes += directorySizeBytes(rekordboxPath.toStdString(), cancel, progress,
                                                "Sizing the rekordbox folder", rekordboxWalkUnits);
        }
        if (!enginePath.isEmpty()) {
            metadataBytes += directorySizeBytes(enginePath.toStdString(), cancel, progress,
                                                "Sizing the Engine Library folder", engineWalkUnits);
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
            {measuredArtwork.engineUnreadable ? "Artwork (Engine covers could not be read)" : "Artwork", artworkBytes,
             {}},
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

StickStatisticsController::StickStatisticsController(QObject *parent) : QObject(parent) {}

void StickStatisticsController::scan(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath)
{
    // Answered either way (docs/async-requests.md): the same stick again
    // by the scan running, another by a new one that supersedes it.
    setErrorMessage({});
    const QString catalog = rekordboxPath.isEmpty() ? enginePath : rekordboxPath;
    const QString key = stickLabel + QLatin1Char('\n') + rekordboxPath + QLatin1Char('\n') + enginePath;
    if (m_scan.busy() && m_scan.key() == key) {
        return;  // served by the scan running, its bar left as it is
    }
    setScanProgress(0, 0, {});
    auto reporter = makeReporter();
    m_scan.start(
        key, stickRootOf(catalog),
        [stickLabel, rekordboxPath, enginePath, reporter](application::CancellationToken cancel) {
            return runScanTask(stickLabel, rekordboxPath, enginePath, reporter, cancel);
        },
        {
            [this](StickStatisticsScanResult &&result) { onScanFinished(std::move(result)); },
            [this](const QString &message) { setErrorMessage(message); },
            [this]() { emit scanCancelled(); },
        });
}

std::shared_ptr<QtProgressReporter> StickStatisticsController::makeReporter()
{
    auto reporter = makeMainThreadShared<QtProgressReporter>();
    const auto current = m_scan.speaksForNext();
    // One announcement per scan (#58), as StagedCueEditController's.
    connect(reporter.get(), &QtProgressReporter::started, this, [this, current](const QString &label, int total) {
        if (current()) {
            setScanProgress(0, total, label);
        }
    });
    connect(reporter.get(), &QtProgressReporter::phaseChanged, this, [this, current](const QString &label) {
        if (current()) {
            setScanProgress(m_scanCurrent, m_scanTotal, label);
        }
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this, [this, current](int done) {
        if (current()) {
            setScanProgress(done, m_scanTotal, m_scanLabel);
        }
    });
    return reporter;
}

void StickStatisticsController::setScanProgress(int current, int total, const QString &label)
{
    if (m_scanCurrent == current && m_scanTotal == total && m_scanLabel == label) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    m_scanLabel = label;
    emit scanProgressChanged();
}

void StickStatisticsController::cancelScan()
{
    m_scan.cancel();
}

void StickStatisticsController::onScanFinished(StickStatisticsScanResult &&result)
{
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

void StickStatisticsController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
