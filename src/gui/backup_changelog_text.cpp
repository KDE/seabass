// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/backup_changelog_text.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using namespace seabass::infrastructure::stick_backup;

namespace
{

QString humanBytes(std::uint64_t bytes)
{
    static const char *units[] = {"bytes", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QString::number(bytes) + QStringLiteral(" bytes")
                     : QString::number(value, 'f', 1) + QLatin1Char(' ') + QLatin1String(units[unit]);
}

QString describeStatus(BackupStatus status)
{
    switch (status) {
    case BackupStatus::Complete: return QStringLiteral("complete");
    case BackupStatus::PartialCancelled: return QStringLiteral("stopped, kept what was copied");
    case BackupStatus::PartialConflict: return QStringLiteral("incomplete: a DJ app started mid-run");
    case BackupStatus::PartialDbTooLarge: return QStringLiteral("incomplete: the database was too large");
    case BackupStatus::PartialSkipped: return QStringLiteral("incomplete: some files could not be read");
    }
    return QStringLiteral("complete");
}

}  // namespace

QString writeChangelogFile(const fs::path &archivePath, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return QString();
    };

    std::optional<BackupManifest> manifest;
    try {
        // Read-only: opening a backup to look at its history must never
        // be able to change it, and a read-write open would create the
        // file if it were missing.
        PosixArchiveFile file(archivePath, PosixArchiveFile::OpenMode::ReadOnly);
        std::string openError;
        std::optional<Zip64Reader> reader = Zip64Reader::tryOpen(file, &openError);
        if (!reader) {
            return fail(QStringLiteral("This backup could not be read: ") + QString::fromStdString(openError));
        }
        std::optional<std::size_t> index = reader->findEntry(ManifestEntryName);
        if (!index) {
            return fail(QStringLiteral("This backup has no index, so it has no history to show."));
        }
        manifest = BackupManifest::parse(reader->readEntryToString(*index));
    } catch (const std::exception &e) {
        return fail(QStringLiteral("This backup could not be opened: ") + QString::fromUtf8(e.what()));
    }
    if (!manifest) {
        return fail(QStringLiteral("This backup's index is damaged, so its history cannot be trusted."));
    }
    if (manifest->generations.empty()) {
        // Not a failure, and worth saying plainly: backups written before
        // the log existed simply have nothing to show, and "no history"
        // must not read as "damaged".
        return fail(QStringLiteral("This backup has no history recorded yet. Backups only started keeping one "
                                    "recently; the next update to this one will begin its log."));
    }

    const QString directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (directory.isEmpty()) {
        return fail(QStringLiteral("Nowhere to write the file to."));
    }
    const QString name = QFileInfo(QString::fromStdString(archivePath.filename().string())).completeBaseName();
    const QString path = QDir(directory).filePath(name + QStringLiteral("-history.txt"));

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return fail(QStringLiteral("Could not write ") + path);
    }
    QTextStream text(&out);

    text << "Backup history\n";
    text << "==============\n\n";
    text << "Archive:  " << QString::fromStdString(archivePath.string()) << "\n";
    text << "Stick:    " << QString::fromStdString(manifest->stickLabel) << "\n";
    if (!manifest->userName.empty()) {
        text << "Name:     " << QString::fromStdString(manifest->userName) << "\n";
    }
    if (manifest->sourceReadOnly) {
        text << "Note:     taken from a stick the system had already made read-only, so this is an\n";
        text << "          emergency copy of a damaged filesystem.\n";
    }
    text << "\n";
    text << manifest->generations.size() << " run"
          << (manifest->generations.size() == 1 ? "" : "s") << " recorded, newest first.\n";
    text << "Only runs that changed something appear: a backup that found nothing to do writes\n";
    text << "nothing. At most " << MaxGenerationRows << " are kept.\n\n";

    // Newest first: the last run is the one anybody opening this wants.
    for (auto it = manifest->generations.rbegin(); it != manifest->generations.rend(); ++it) {
        const GenerationRow &generation = *it;
        text << QDateTime::fromSecsSinceEpoch(generation.createdAtUnix).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
              << "  " << describeStatus(generation.status) << "\n";
        text << "    added " << generation.added << ", changed " << generation.changed << ", removed "
              << generation.removed << "; " << humanBytes(generation.bytesRead) << " read from the stick\n";
        if (!generation.userName.empty()) {
            text << "    called \"" << QString::fromStdString(generation.userName) << "\" at the time\n";
        }
        text << "\n";
    }

    text.flush();
    out.close();
    return path;
}

}  // namespace seabass::gui
