// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/archive_compactor.hpp"

#include <optional>
#include <unordered_map>

#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::infrastructure::stick_backup
{

bool shouldSuggestCompaction(const DeadSpaceReport &report)
{
    return report.deadBytes > 0 && (report.deadRatio() >= SuggestCompactionRatio || report.deadBytes >= SuggestCompactionBytes);
}

std::uint64_t compactedArchiveSize(const std::vector<CentralEntry> &entries)
{
    std::uint64_t offset = 0;
    std::uint64_t centralDirectory = 0;
    for (CentralEntry entry : entries) {
        // As Zip64Writer rewrites it: files stored and followed by a data
        // descriptor, directories as plain headers.
        const bool isFile = !entry.isDirectory;
        entry.localHeaderOffset = offset;
        entry.method = zip::MethodStore;
        entry.compressedSize = isFile ? entry.size : 0;
        entry.hasDataDescriptor = isFile;
        offset += Zip64Writer::localHeaderSize(entry, isFile);
        if (isFile) {
            offset += entry.size + zip::DataDescriptorSize;
        }
        centralDirectory += Zip64Writer::centralDirectoryEntrySize(entry);
    }
    return offset + centralDirectory + Zip64Writer::trailerSize();
}

std::uint64_t compactedArchiveSize(const Zip64Reader &source)
{
    std::vector<CentralEntry> order;
    order.reserve(source.entries().size());
    std::optional<CentralEntry> manifest;
    for (const CentralEntry &entry : source.entries()) {
        if (entry.name == ManifestEntryName) {
            manifest = entry;
        } else {
            order.push_back(entry);
        }
    }
    if (manifest) {
        order.push_back(*manifest);
    }
    return compactedArchiveSize(order);
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
        // The salvage log is copied across as it stands: it describes the
        // archive, so no manifest row describes IT, and compaction is
        // exactly what somebody runs on a salvage backup -- every
        // discarded read attempt is dead space in it. Refusing to
        // compact one, which is what demanding a row here did, left the
        // archive that most needs compacting the one archive that could
        // not be.
        //
        // Not regenerated, because compaction does not re-read the
        // stick and has nothing new to say; the rows it carries forward
        // still record which files are short.
        if (isArchiveMetadataEntry(entry.name)) {
            Zip64Writer::EntrySink copy = writer.beginFile(entry.name, entry.mtimeUnix);
            source.readEntry(i, [&](std::span<const std::byte> piece) { copy.write(piece); });
            // Checked against the source's own central directory record,
            // like every other entry below. The manifest hash cannot
            // apply here -- a metadata entry has no manifest row, which
            // is the whole reason for this branch -- but size and CRC
            // are the archive's own bookkeeping and are available for
            // any entry. Without them this was the one thing copied
            // through a compaction unverified, and it is the salvage
            // log: the record of which files came off a failing stick
            // short.
            const CentralEntry carried = copy.finish();
            if (carried.size != entry.size || carried.crc32 != entry.crc32) {
                throw ArchiveFormatError("entry does not match its central directory record: " + entry.name);
            }
            // Counted as copied, since it is in totalBytes above. It was
            // not, so on a salvage backup -- the one archive carrying a
            // metadata entry -- progress stopped short of the total by
            // the salvage log's size and never reached the end.
            copied += carried.size;
            if (progress) {
                progress(copied, totalBytes);
            }
            ++result.entries;
            continue;
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
