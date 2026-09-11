// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/open_stick_backup.hpp"

#include <fstream>
#include <memory>
#include <system_error>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/scratch_dir_guard.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/restore_path_sanitizer.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;
namespace sb = infrastructure::stick_backup;

namespace
{

bool startsWith(const std::string &text, const std::string &prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

bool OpenStickBackup::isAnalysisEntry(const std::string &entryName)
{
    return startsWith(entryName, "PIONEER/USBANLZ/");
}

bool OpenStickBackup::isCatalogEntry(const std::string &entryName)
{
    // Files sitting *directly* in PIONEER/rekordbox/ (export.pdb,
    // exportLibrary.db and its WAL, the device settings files) or
    // directly in Engine Library/Database2/ (m.db and its WAL).
    //
    // Directly, not recursively: Database2 also carries OverviewData/,
    // one small .rgb file per track, which nothing in Seabass reads --
    // Engine waveforms come out of m.db through libdjinterop
    // (LibdjinteropWaveformReader uses track->waveform()), not from those
    // files. Taking the whole subtree extracted 700 files and 7.2 MB
    // instead of 5 files and 2 MB, for data no page would ever open.
    for (const std::string &dir : {std::string("PIONEER/rekordbox/"), std::string("Engine Library/Database2/")}) {
        if (startsWith(entryName, dir) && entryName.find('/', dir.size()) == std::string::npos) {
            return true;
        }
    }
    return false;
}

OpenedStickBackup OpenStickBackup::execute(const fs::path &archivePath, const fs::path &cacheRoot)
{
    OpenedStickBackup result;

    // First, before the archive is so much as opened: a leftover from an
    // interrupted swap. If the real cache is missing, the leftover is
    // yesterday's copy -- the double-failure case at the end of this
    // function -- and it is put back, not deleted, so that whatever fails
    // from here on (an unreadable archive included) leaves that copy in
    // place. Only a leftover beside an intact cache is cleared, up front,
    // so a failure to clear it stops this open before any extraction
    // rather than after all of it.
    const fs::path retired = cacheRoot.string() + ".old";
    std::error_code ec;
    if (fs::exists(retired, ec)) {
        if (!fs::exists(cacheRoot, ec)) {
            fs::rename(retired, cacheRoot, ec);
            if (ec) {
                result.error = "The previous copy of this backup is at " + retired.string()
                               + " and could not be put back: " + ec.message();
                return result;
            }
        } else {
            fs::remove_all(retired, ec);
            if (fs::exists(retired, ec)) {
                result.error = "A previous copy of this backup could not be cleared away (" + retired.string()
                               + "). Something still has a file in it open -- possibly a scan of this "
                                 "backup that is still running in Seabass; wait for it and try again.";
                return result;
            }
        }
    }

    std::unique_ptr<sb::PosixArchiveFile> file;
    std::optional<sb::Zip64Reader> reader;
    try {
        file = std::make_unique<sb::PosixArchiveFile>(archivePath, sb::PosixArchiveFile::OpenMode::ReadOnly);
        reader = sb::Zip64Reader::open(*file);
    } catch (const std::exception &e) {
        result.error = std::string("That backup could not be read: ") + e.what();
        return result;
    }

    // Everything is extracted into a staging directory beside the real
    // one and swapped in only at the end. Two reasons. A re-open that
    // fails part-way (the archive was rewritten, a disk filled up) must
    // leave yesterday's good cache exactly as it was, still browsable with
    // its cues -- not half-replaced and marker-less, which reads as a
    // library with no cues and no explanation. And any reader that opens
    // the real directory while this runs sees a complete state, never one
    // with the marker missing.
    const fs::path staging = cacheRoot.string() + ".partial";
    fs::remove_all(staging, ec);
    if (ec) {
        result.error = "Could not clear the staging directory for the backup: " + ec.message();
        return result;
    }
    fs::create_directories(staging, ec);
    if (ec) {
        result.error = "Could not create a place to open the backup: " + ec.message();
        return result;
    }
    // Whatever happens below, a failure never leaves the staging tree
    // around to be mistaken for a cache. After a successful swap the
    // staging path no longer exists and the guard's remove_all is a
    // no-op -- so there is no "keep" flag to forget to set.
    infrastructure::ScratchDirGuard guard(staging);

    for (std::size_t index = 0; index < reader->entries().size(); ++index) {
        const sb::CentralEntry &entry = reader->entries()[index];
        if (entry.isDirectory) {
            continue;
        }
        if (isAnalysisEntry(entry.name)) {
            result.analysisFilesLeftInArchive++;
            continue;
        }
        if (!isCatalogEntry(entry.name)) {
            result.otherEntriesSkipped++;
            continue;
        }

        // The same rule a restore applies to every entry it writes: an
        // entry name is data from a file the user picked, and ".." or a
        // backslash segment must never resolve outside the cache. A bad
        // name fails the open rather than being skipped, because a backup
        // whose catalog entries are malformed is not one to browse.
        std::string reason;
        const auto safe = sb::sanitizeEntryName(entry.name, sb::hostTargetOs(), &reason);
        if (!safe) {
            result.error = "The backup holds an entry Seabass will not extract (\"" + entry.name + "\"): " + reason;
            return result;
        }

        const fs::path target = staging / *safe;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            result.error = "Could not create a directory while opening the backup: " + ec.message();
            return result;
        }
        std::string bytes;
        try {
            bytes = reader->readEntryToString(index);
        } catch (const std::exception &e) {
            result.error = "Could not read \"" + entry.name + "\" from the backup: " + e.what();
            return result;
        }
        // The same primitive every other catalog write in the app uses:
        // temp file, fsync, rename -- so a crash after the directory swap
        // cannot leave a present-but-empty database. longPathSafe for the
        // cache's depth on Windows, as restore already does for these
        // entries.
        if (!infrastructure::writeFileDurablyAtomic(infrastructure::longPathSafe(fs::absolute(target)).string(),
                                                    bytes)) {
            result.error = "Could not write \"" + entry.name + "\" while opening the backup.";
            return result;
        }
        result.filesExtracted++;
        result.bytesExtracted += bytes.size();
    }

    if (result.filesExtracted == 0) {
        result.error = "That backup holds no rekordbox or Engine DJ catalog.";
        return result;
    }

    // The marker that makes the extracted directory self-describing: it
    // is what tells every rekordbox reader built against this root to
    // pull analysis files out of the archive rather than looking for
    // USBANLZ next to the databases (which is not there, on purpose).
    // Written into staging, for the final directory, so it is present and
    // valid the instant the rename lands.
    if (!infrastructure::local::writeBrowsedBackupMarker(staging, archivePath, cacheRoot)) {
        result.error = "Could not record which backup this came from.";
        return result;
    }

    // The row's name comes from the backup's own manifest when it has a
    // readable one; a backup without one still browses, it just gets its
    // name from the directory instead.
    if (auto manifestIndex = reader->findEntry(std::string(sb::ManifestEntryName))) {
        try {
            if (auto manifest = sb::BackupManifest::parse(reader->readEntryToString(*manifestIndex))) {
                result.stickLabel = manifest->stickLabel;
            }
        } catch (const std::exception &) {
        }
    }

    // Swap, in an order where no failure leaves a half-cache behind:
    // the old directory is renamed aside whole (one atomic step, so a
    // reader holding one of its files cannot leave it half-deleted the
    // way remove_all could), the new one is renamed into place, and only
    // then is the old one removed. If the second rename fails, the first
    // is undone and yesterday's cache is exactly where it was.
    const bool hadPrevious = fs::exists(cacheRoot, ec);
    if (hadPrevious) {
        fs::rename(cacheRoot, retired, ec);
        if (ec) {
            result.error = "Could not set aside the previous copy of this backup: " + ec.message()
                           + " (a scan of it may still be running in Seabass; wait for it and try again).";
            return result;
        }
    }
    fs::rename(staging, cacheRoot, ec);
    if (ec) {
        result.error = "Could not move the opened backup into place: " + ec.message();
        if (hadPrevious) {
            std::error_code undo;
            fs::rename(retired, cacheRoot, undo);
            if (undo) {
                result.error += " The previous copy is still intact at " + retired.string() + ".";
            }
        }
        return result;
    }
    if (hadPrevious) {
        // Best effort: a file still open in the retired tree just delays
        // its removal to the next open; the live cache is complete either
        // way.
        fs::remove_all(retired, ec);
    }

    result.libraryRoot = cacheRoot;
    return result;
}

}  // namespace seabass::application
