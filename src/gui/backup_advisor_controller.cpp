// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/browsed_backup_root.hpp"
#include "backup_advisor_controller.hpp"

#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <set>

#include "gui/library_fingerprint_reader.hpp"
#include "gui/future_result.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/stick_backup/library_catalog_mtime.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using application::StickBackupAdvice;
using application::StickBackupAdviceInput;
using application::StickBackupDescription;

struct BackupAdvisorController::Result
{
    QString mountPoint;
    StickFacts facts;
    std::vector<StickBackupDescription> backups;
};

namespace
{

QString isoTime(std::int64_t unix)
{
    return unix > 0 ? QDateTime::fromSecsSinceEpoch(unix).toString(Qt::ISODate) : QString();
}

}  // namespace

BackupAdvisorController::BackupAdvisorController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<Result>>::finished, this, &BackupAdvisorController::onFinished);
}

BackupAdvisorController::~BackupAdvisorController()
{
    awaitQuietly(m_watcher);
}

void BackupAdvisorController::setBackupDirectory(const QString &directory)
{
    if (m_backupDirectory == directory) {
        return;
    }
    m_backupDirectory = directory;
    emit backupDirectoryChanged();
    reassessAll();
}

void BackupAdvisorController::assess(const QString &stickLabel, const QString &mountPoint, const QString &rekordboxPath,
                                     const QString &enginePath)
{
    if (mountPoint.isEmpty()) {
        return;
    }
    const Request request{stickLabel, mountPoint, rekordboxPath, enginePath};
    m_known[mountPoint] = request;
    for (Request &queued : m_queue) {
        if (queued.mountPoint == mountPoint) {
            queued = request;
            return;
        }
    }
    m_queue.push_back(request);
    startNext();
}

void BackupAdvisorController::reassessAll()
{
    for (const Request &request : m_known) {
        assess(request.stickLabel, request.mountPoint, request.rekordboxPath, request.enginePath);
    }
}

void BackupAdvisorController::forget(const QString &mountPoint)
{
    m_known.remove(mountPoint);
    const bool hadFacts = m_facts.remove(mountPoint) > 0;
    const bool hadAdvice = m_advice.remove(mountPoint) > 0;
    if (hadFacts || hadAdvice) {
        // The others may have been cloning from or updating from it.
        recomputeAdvice();
    }
}

void BackupAdvisorController::startNext()
{
    if (m_watcher.isRunning() || m_queue.empty()) {
        return;
    }
    const Request request = m_queue.front();
    m_queue.erase(m_queue.begin());
    const fs::path directory(m_backupDirectory.toStdString());
    emit busyChanged();
    m_watcher.setFuture(QtConcurrent::run([request, directory]() {
        namespace stick_backup = infrastructure::stick_backup;
        auto result = std::make_shared<Result>();
        result->mountPoint = request.mountPoint;
        StickFacts &facts = result->facts;

        facts.hasLibrary = !request.rekordboxPath.isEmpty() || !request.enginePath.isEmpty();
        facts.stickLabel = request.stickLabel.toStdString();
        const infrastructure::system::StickHardwareInfo hardware =
            infrastructure::system::readStickHardwareInfo(request.mountPoint.toStdString(), facts.stickLabel);
        facts.stickIdentifier = hardware.stickIdentifier;
        facts.freeBytes = hardware.freeBytes;
        facts.usedBytes = hardware.totalBytes > hardware.freeBytes ? hardware.totalBytes - hardware.freeBytes : 0;
        const fs::path root(request.mountPoint.toStdString());
        if (facts.hasLibrary) {
            facts.fingerprint = readLibraryFingerprint(request.rekordboxPath, request.enginePath);
            facts.catalogModifiedAtUnix = stick_backup::libraryCatalogModifiedAt(root);
        }
        if (!directory.empty()) {
            result->backups = application::RestoreStickBackup::describeAll(directory);
        }
        // The exact "changed since" test: the same databases the backups
        // captured, fingerprinted as they are on the stick right now --
        // plus the stick's own Engine database, so two sticks can be
        // compared with each other even when no backup captured either.
        std::set<std::string> databasePaths;
        for (const StickBackupDescription &backup : result->backups) {
            for (const auto &[path, hex] : backup.databaseFingerprints) {
                databasePaths.insert(path);
            }
        }
        if (facts.hasLibrary) {
            databasePaths.insert(stick_backup::pathToUtf8(infrastructure::engine::engineMainDatabasePath(fs::path())));
        }
        for (const std::string &path : databasePaths) {
            if (const auto fingerprint = stick_backup::fingerprintDbSet(root / stick_backup::pathFromUtf8(path))) {
                facts.databaseFingerprints[path] = fingerprint->toHex();
            }
        }
        return result;
    }));
}

void BackupAdvisorController::onFinished()
{
    // No error surface here on purpose: the advisor is background
    // colour on the stick list, so a stick that could not be read this
    // pass just keeps whatever advice it had (or none) rather than
    // interrupting anything.
    const std::shared_ptr<Result> result = takeResult(m_watcher);
    if (result) {
        m_facts[result->mountPoint] = result->facts;
        m_backups = result->backups;
        recomputeAdvice();
    }
    emit busyChanged();
    startNext();
}

QVariantMap BackupAdvisorController::sourceToVariant(const StickBackupAdvice::SourceRef &source) const
{
    QVariantMap map;
    map["kind"] = QString::fromUtf8(std::string(application::toString(source.kind)).c_str());
    map["label"] = QString::fromStdString(source.label);
    map["mountPoint"] = QString::fromStdString(source.mountPoint);
    map["backupPath"] = QString::fromStdString(source.backupPath.string());
    map["modifiedAt"] = isoTime(source.modifiedAtUnix);
    map["enoughSpace"] = source.enoughSpace;
    map["detail"] = QString::fromStdString(source.detail);
    QString rekordboxPath;
    QString enginePath;
    if (source.kind == StickBackupAdvice::SourceRef::Kind::Stick) {
        const auto known = m_known.find(QString::fromStdString(source.mountPoint));
        if (known != m_known.end()) {
            rekordboxPath = known->rekordboxPath;
            enginePath = known->enginePath;
        }
    }
    map["rekordboxPath"] = rekordboxPath;
    map["enginePath"] = enginePath;
    return map;
}

void BackupAdvisorController::recomputeAdvice()
{
    QVariantMap advice;
    for (auto it = m_facts.constBegin(); it != m_facts.constEnd(); ++it) {
        const StickFacts &facts = it.value();
        StickBackupAdviceInput input;
        input.hasLibrary = facts.hasLibrary;
        input.stickIdentifier = facts.stickIdentifier;
        input.stickLabel = facts.stickLabel;
        input.liveFingerprint = facts.fingerprint;
        input.liveDatabaseFingerprints = facts.databaseFingerprints;
        input.catalogModifiedAtUnix = facts.catalogModifiedAtUnix;
        input.usedBytes = facts.usedBytes;
        input.freeBytes = facts.freeBytes;
        input.backups = m_backups;
        for (auto other = m_facts.constBegin(); other != m_facts.constEnd(); ++other) {
            if (other == it || !other.value().hasLibrary) {
                continue;
            }
            // A stick backup being browsed is a catalogs-only cache with
            // the newest mtime in the room. Offered as a clone source it
            // would resolve, by label, to the very archive it came from --
            // and BackupStick would rewrite that archive from the cache.
            // BackupStick refuses too; this keeps the offer from ever
            // appearing on an empty stick's card.
            if (infrastructure::local::isBrowsedBackupRoot(std::filesystem::path(other.key().toStdString()))) {
                continue;
            }
            StickBackupAdviceInput::PeerStick peer;
            peer.mountPoint = other.key().toStdString();
            peer.label = other.value().stickLabel;
            peer.stickIdentifier = other.value().stickIdentifier;
            peer.fingerprint = other.value().fingerprint;
            peer.databaseFingerprints = other.value().databaseFingerprints;
            peer.catalogModifiedAtUnix = other.value().catalogModifiedAtUnix;
            peer.usedBytes = other.value().usedBytes;
            input.peers.push_back(std::move(peer));
        }

        const StickBackupAdvice result = application::adviseStickBackup(input);
        QVariantMap map;
        map["state"] = QString::fromUtf8(std::string(application::toString(result.state)).c_str());
        map["matchedBy"] = QString::fromUtf8(std::string(application::toString(result.matchedBy)).c_str());
        map["backupPath"] = QString::fromStdString(result.backupPath.string());
        map["backupLabel"] = QString::fromStdString(result.backupLabel);
        map["backupCreatedAt"] = isoTime(result.backupCreatedAtUnix);
        map["trackOverlap"] = result.trackOverlap;
        map["cueOverlap"] = result.cueOverlap;
        map["detail"] = QString::fromStdString(result.detail);
        map["cloneSource"] = sourceToVariant(result.cloneSource);
        map["updateSource"] = sourceToVariant(result.updateSource);
        map["diverged"] = result.diverged;
        advice[it.key()] = map;
    }
    m_advice = std::move(advice);
    emit adviceChanged();
}

}  // namespace seabass::gui
