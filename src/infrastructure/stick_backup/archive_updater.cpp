// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/archive_updater.hpp"

#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::infrastructure::stick_backup
{

ArchiveUpdater::ArchiveUpdater(ArchiveFile &archive, ArchiveFile &journalFile, std::vector<CentralEntry> carriedEntries,
                               std::uint64_t priorEocdOffset, std::size_t chunkSize)
    : m_archive(archive), m_journal(journalFile), m_writer(archive, std::move(carriedEntries)),
      m_carriedCount(m_writer.entries().size()), m_chunkSize(chunkSize)
{
    m_record.preLength = archive.size();
    m_record.preEocdOffset = priorEocdOffset;
    if (m_record.preLength > 0 && m_record.preEocdOffset == 0) {
        throw std::logic_error("ArchiveUpdater: an existing archive needs its EOCD offset");
    }
    if (m_record.preLength == 0 && m_carriedCount > 0) {
        throw std::logic_error("ArchiveUpdater: carried entries without an existing archive");
    }
}

void ArchiveUpdater::requireBegun() const
{
    if (m_state != State::Begun) {
        throw std::logic_error("ArchiveUpdater: operation requires begin() and no commit()/abort() yet");
    }
}

void ArchiveUpdater::begin()
{
    if (m_state != State::Created) {
        throw std::logic_error("ArchiveUpdater::begin() called twice");
    }
    journal::write(m_journal, m_record);
    m_state = State::Begun;
}

std::optional<ArchiveUpdater::AppendedEntry> ArchiveUpdater::appendFile(const std::string &name, std::int64_t mtimeUnix,
                                                                        EntrySource &source,
                                                                        application::CancellationToken cancel,
                                                                        const std::function<void(std::uint64_t)> &progress)
{
    requireBegun();
    if (cancel.cancelled()) {
        return std::nullopt;
    }
    Zip64Writer::EntrySink sink = m_writer.beginFile(name, mtimeUnix);
    std::vector<std::byte> buffer(m_chunkSize);
    while (true) {
        std::size_t got = source.read(buffer);
        if (got == 0) {
            break;
        }
        sink.write(std::span<const std::byte>(buffer.data(), got));
        if (progress) {
            progress(sink.bytesWritten());
        }
        if (cancel.cancelled()) {
            return std::nullopt;  // sink dies unfinished: bytes stay as dead space
        }
    }
    AppendedEntry result;
    result.entry = sink.finish();
    result.sha256 = sink.sha256();
    return result;
}

ArchiveUpdater::AppendedEntry ArchiveUpdater::appendFromMemory(const std::string &name, std::int64_t mtimeUnix,
                                                               std::span<const std::byte> content)
{
    requireBegun();
    AppendedEntry result;
    result.entry = m_writer.addFileFromMemory(name, mtimeUnix, content, &result.sha256);
    return result;
}

CentralEntry ArchiveUpdater::appendDirectory(const std::string &name, std::int64_t mtimeUnix)
{
    requireBegun();
    return m_writer.addDirectory(name, mtimeUnix);
}

void ArchiveUpdater::forgetLastEntries(std::size_t count)
{
    requireBegun();
    m_writer.forgetLastEntries(count);
}

ArchiveUpdater::CommitResult ArchiveUpdater::commit(const BackupManifest &manifest)
{
    requireBegun();
    // Barrier 1: every entry's bytes are on the medium before anything
    // that could make a reader look at them exists.
    m_archive.barrier();

    CommitResult result;
    result.boundaries = m_writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
    // Barrier 2: the new trailer is durable; from here on the file reads
    // as the new archive.
    m_archive.barrier();

    std::string error;
    if (!verifyArchiveTail(m_archive, m_record.preLength, &error)) {
        // Journal stays: the next open rolls this generation back.
        throw ArchiveFormatError("archive did not verify after commit: " + error);
    }
    journal::clear(m_journal);
    result.bytesAppended = m_archive.size() - m_record.preLength;
    m_state = State::Committed;
    return result;
}

void ArchiveUpdater::abort()
{
    requireBegun();
    m_archive.truncate(m_record.preLength);
    m_archive.barrier();
    journal::clear(m_journal);
    m_state = State::Aborted;
}

bool verifyArchiveTail(const ArchiveFile &archive, std::uint64_t fromOffset, std::string *error)
{
    auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    std::string openError;
    std::optional<Zip64Reader> reader = Zip64Reader::tryOpen(archive, &openError);
    if (!reader) {
        return fail(openError);
    }
    std::optional<std::size_t> manifestIndex = reader->findEntry(ManifestEntryName);
    if (!manifestIndex) {
        return fail("no manifest entry");
    }
    for (std::size_t i = 0; i < reader->entries().size(); ++i) {
        const CentralEntry &entry = reader->entries()[i];
        try {
            // Every entry's local header is checked against its CD record
            // (cheap, no data read): the CD is rewritten on every update,
            // so a carried entry's record is new bytes too.
            reader->dataOffset(i);
            if (entry.isDirectory || entry.localHeaderOffset < fromOffset) {
                continue;
            }
            if (!reader->verifyCrc(i)) {
                return fail("CRC mismatch in " + entry.name);
            }
            if (!reader->verifyDataDescriptor(i)) {
                return fail("data descriptor mismatch in " + entry.name);
            }
        } catch (const ArchiveFormatError &e) {
            return fail(e.what());
        }
    }
    std::string manifestError;
    std::optional<BackupManifest> manifest = BackupManifest::parse(reader->readEntryToString(*manifestIndex), &manifestError);
    if (!manifest) {
        return fail("manifest: " + manifestError);
    }
    // The manifest entry is the one entry no manifest row describes; its
    // timestamp is written equal to the manifest's own createdAt.
    if (reader->entries()[*manifestIndex].mtimeUnix != manifest->createdAtUnix) {
        return fail("manifest entry timestamp disagrees with the manifest header");
    }
    // The manifest must describe exactly the archive's entries -- a name
    // that differs on either side is the CD-corruption case ZIP itself
    // cannot detect.
    if (manifest->rows.size() + 1 != reader->entries().size()) {
        return fail("manifest lists " + std::to_string(manifest->rows.size()) + " rows for "
                    + std::to_string(reader->entries().size() - 1) + " entries");
    }
    std::unordered_map<std::string, const ManifestRow *> listed;
    listed.reserve(manifest->rows.size());
    for (const ManifestRow &row : manifest->rows) {
        listed.emplace(row.kind == ManifestRow::Kind::Directory ? row.path + "/" : row.path, &row);
    }
    for (std::size_t i = 0; i < reader->entries().size(); ++i) {
        if (i == *manifestIndex) {
            continue;
        }
        const CentralEntry &entry = reader->entries()[i];
        auto it = listed.find(entry.name);
        if (it == listed.end()) {
            return fail("entry not in manifest: " + entry.name);
        }
        const ManifestRow &row = *it->second;
        if ((row.kind == ManifestRow::Kind::Directory) != entry.isDirectory || row.size != entry.size
            || row.mtimeUnix != entry.mtimeUnix || (!entry.isDirectory && row.crc32 != entry.crc32)) {
            return fail("manifest row disagrees with the entry: " + entry.name);
        }
        listed.erase(it);
    }
    if (!listed.empty()) {
        return fail("manifest row without an entry: " + listed.begin()->first);
    }
    return true;
}

}  // namespace seabass::infrastructure::stick_backup
