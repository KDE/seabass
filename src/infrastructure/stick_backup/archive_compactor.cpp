// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/archive_compactor.hpp"

#include <unordered_map>

#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::infrastructure::stick_backup
{

bool shouldSuggestCompaction(const DeadSpaceReport &report)
{
    return report.deadBytes > 0 && (report.deadRatio() >= SuggestCompactionRatio || report.deadBytes >= SuggestCompactionBytes);
}

CompactionResult compactArchive(const Zip64Reader &source, const BackupManifest &manifest, ArchiveFile &destination,
                                application::CancellationToken cancel,
                                const std::function<void(std::uint64_t, std::uint64_t)> &progress)
{
    if (destination.size() != 0) {
        throw ArchiveFormatError("compaction destination is not empty");
    }
    CompactionResult result;
    result.bytesBefore = source.layout().fileSize;

    std::unordered_map<std::string, const ManifestRow *> rows;
    rows.reserve(manifest.rows.size());
    std::uint64_t totalBytes = 0;
    for (const ManifestRow &row : manifest.rows) {
        rows.emplace(row.kind == ManifestRow::Kind::Directory ? row.path + "/" : row.path, &row);
    }
    for (const CentralEntry &entry : source.entries()) {
        if (entry.name != ManifestEntryName) {
            totalBytes += entry.size;
        }
    }

    Zip64Writer writer(destination, {});
    std::uint64_t copied = 0;
    for (std::size_t i = 0; i < source.entries().size(); ++i) {
        const CentralEntry &entry = source.entries()[i];
        if (entry.name == ManifestEntryName) {
            continue;
        }
        if (cancel.cancelled()) {
            result.cancelled = true;
            return result;
        }
        auto row = rows.find(entry.name);
        if (row == rows.end()) {
            throw ArchiveFormatError("entry not described by the manifest: " + entry.name);
        }
        if (entry.isDirectory) {
            writer.addDirectory(entry.name, entry.mtimeUnix);
            ++result.entries;
            continue;
        }
        Zip64Writer::EntrySink sink = writer.beginFile(entry.name, entry.mtimeUnix);
        source.readEntry(i, [&](std::span<const std::byte> piece) {
            sink.write(piece);
            if (progress) {
                progress(copied + sink.bytesWritten(), totalBytes);
            }
        });
        CentralEntry written = sink.finish();
        if (written.size != entry.size || written.crc32 != entry.crc32) {
            throw ArchiveFormatError("entry does not match its central directory record: " + entry.name);
        }
        if (sink.sha256() != row->second->sha256) {
            throw ArchiveFormatError("entry does not match its manifest hash: " + entry.name);
        }
        copied += written.size;
        ++result.entries;
    }
    writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
    result.bytesAfter = destination.size();
    return result;
}

}  // namespace seabass::infrastructure::stick_backup
