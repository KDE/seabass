// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "application/use_cases/compact_stick_backup.hpp"
#include "infrastructure/stick_backup/archive_compactor.hpp"
#include "infrastructure/stick_backup/archive_stats.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/in_memory_archive_file.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::stick_backup;
using seabass::application::CancellationToken;
using seabass::application::CompactionOutcome;
using seabass::application::CompactionPreflight;
using seabass::application::CompactStickBackup;
using seabass::application::CompactStickBackupOptions;
using seabass::infrastructure::hashing::Sha256;
namespace fs = std::filesystem;

namespace
{

std::string pseudoRandom(std::size_t size, std::uint64_t seed)
{
    std::string out(size, '\0');
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (std::size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<char>(x & 0xff);
    }
    return out;
}

class MemorySource : public EntrySource
{
public:
    explicit MemorySource(std::string content) : m_content(std::move(content)) {}
    std::size_t read(std::span<std::byte> out) override
    {
        std::size_t take = std::min(out.size(), m_content.size() - m_pos);
        std::memcpy(out.data(), m_content.data() + m_pos, take);
        m_pos += take;
        return take;
    }

private:
    std::string m_content;
    std::size_t m_pos = 0;
};

using Model = std::map<std::string, std::string>;  // path -> content ("" + trailing '/' = directory)

// Writes one generation on top of whatever is in the archive: entries
// whose content is unchanged are carried, everything else appended,
// dropped paths removed. Mirrors the real updater flow closely enough to
// produce realistic dead space.
// `salvageLog` non-empty writes SEABASS-SALVAGE.txt into this
// generation, the way a run off a damaged stick does: an entry that
// describes the archive, so no manifest row describes it.
void generation(InMemoryArchiveFile &archive, InMemoryArchiveFile &journal, const Model &model,
                const std::string &salvageLog = {})
{
    std::vector<CentralEntry> carried;
    std::map<std::string, ManifestRow> carriedRows;
    std::uint64_t priorEocd = 0;
    std::map<std::string, std::string> previousContent;
    if (archive.size() > 0) {
        Zip64Reader reader = Zip64Reader::open(archive);
        priorEocd = reader.layout().endOfCentralDirectoryOffset;
        std::size_t manifestIndex = *reader.findEntry(ManifestEntryName);
        BackupManifest previous = *BackupManifest::parse(reader.readEntryToString(manifestIndex));
        for (std::size_t i = 0; i < reader.entries().size(); ++i) {
            if (i == manifestIndex || isArchiveMetadataEntry(reader.entries()[i].name)) continue;
            const CentralEntry &e = reader.entries()[i];
            std::string content = e.isDirectory ? "" : reader.readEntryToString(i);
            auto it = model.find(e.name);
            if (it != model.end() && it->second == content) {
                carried.push_back(e);
                std::string path = e.isDirectory ? e.name.substr(0, e.name.size() - 1) : e.name;
                carriedRows[e.name] = *previous.findRow(path);
            }
        }
    }
    ArchiveUpdater updater(archive, journal, carried, priorEocd, 4096);
    updater.begin();
    BackupManifest manifest;
    manifest.stickIdentifier = "uuid";
    manifest.stickLabel = "COMPACT";
    manifest.createdAtUnix = 1'757'000'000 + static_cast<std::int64_t>(archive.size() % 1000);
    for (const auto &[name, content] : model) {
        auto c = carriedRows.find(name);
        if (c != carriedRows.end()) {
            manifest.rows.push_back(c->second);
            continue;
        }
        ManifestRow row;
        if (name.back() == '/') {
            updater.appendDirectory(name, 10);
            row.kind = ManifestRow::Kind::Directory;
            row.path = name.substr(0, name.size() - 1);
            row.mtimeUnix = 10;
        } else {
            MemorySource src(content);
            auto done = updater.appendFile(name, 20, src, CancellationToken::none());
            row.path = name;
            row.size = done->entry.size;
            row.mtimeUnix = 20;
            row.sha256 = done->sha256;
            row.crc32 = done->entry.crc32;
        }
        manifest.rows.push_back(row);
    }
    if (!salvageLog.empty()) {
        updater.appendFromMemory(std::string(SalvageLogEntryName), manifest.createdAtUnix,
                                 std::as_bytes(std::span<const char>(salvageLog.data(), salvageLog.size())));
    }
    updater.commit(manifest);
}

std::map<std::string, std::string> contents(const ArchiveFile &file)
{
    Zip64Reader reader = Zip64Reader::open(file);
    std::map<std::string, std::string> out;
    for (std::size_t i = 0; i < reader.entries().size(); ++i) {
        const CentralEntry &e = reader.entries()[i];
        out[e.name] = e.isDirectory ? "<dir>" : reader.readEntryToString(i);
    }
    return out;
}

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main()
{
    // Three generations of churn: replace, delete, add -> plenty of dead space.
    auto clock = std::make_shared<FaultClock>();
    InMemoryArchiveFile archive(clock);
    InMemoryArchiveFile journal(clock);
    Model model = {{"d/", ""}, {"d/a", pseudoRandom(30'000, 1)}, {"d/b", pseudoRandom(20'000, 2)}, {"c", pseudoRandom(5'000, 3)}};
    generation(archive, journal, model);
    model["d/a"] = pseudoRandom(31'000, 4);
    model.erase("c");
    model["e/"] = "";
    model["e/new"] = pseudoRandom(12'000, 5);
    generation(archive, journal, model);
    model["d/b"] = pseudoRandom(20'000, 6);
    generation(archive, journal, model);

    // ---- Dead space is exact and compaction removes exactly it ----
    {
        Zip64Reader reader = Zip64Reader::open(archive);
        DeadSpaceReport before = deadSpace(reader);
        assert(before.deadBytes > 0);
        assert(before.liveBytes + before.overheadBytes + before.deadBytes == before.fileSize);
        // Independent measurement: everything written by generations 1 and 2
        // that generation 3 does not reference.
        std::uint64_t referenced = 0;
        for (std::size_t i = 0; i < reader.entries().size(); ++i) {
            referenced += reader.footprint(i);
        }
        assert(before.deadBytes == before.fileSize - referenced - (before.fileSize - reader.layout().centralDirectoryOffset));
        assert(!shouldSuggestCompaction(DeadSpaceReport{1000, 900, 50, 50}));   // 5%
        assert(shouldSuggestCompaction(DeadSpaceReport{1000, 700, 50, 250}));   // 25%
        assert(shouldSuggestCompaction(DeadSpaceReport{100'000'000'000ull, 94'000'000'000ull, 0, 6'000'000'000ull}));  // 6%, but 6 GB

        BackupManifest manifest = *BackupManifest::parse(reader.readEntryToString(*reader.findEntry(ManifestEntryName)));
        InMemoryArchiveFile compacted;
        std::uint64_t lastDone = 0, lastTotal = 0;
        CompactionResult result = compactArchive(reader, manifest, compacted, CancellationToken::none(),
                                                 [&](std::uint64_t done, std::uint64_t total) {
                                                     lastDone = done;
                                                     lastTotal = total;
                                                 });
        assert(!result.cancelled);
        assert(result.entries == reader.entries().size() - 1);
        assert(result.bytesAfter == compacted.size());
        assert(result.bytesBefore - result.bytesAfter == before.deadBytes);
        assert(compactedArchiveSize(reader) == result.bytesAfter);
        assert(lastDone == lastTotal && lastTotal == 31'000 + 20'000 + 12'000);

        Zip64Reader after = Zip64Reader::open(compacted);
        DeadSpaceReport afterReport = deadSpace(after);
        assert(afterReport.deadBytes == 0);
        assert(contents(compacted) == contents(archive));
        std::string error;
        assert(verifyArchiveTail(compacted, 0, &error));
        BackupManifest compactedManifest = *BackupManifest::parse(after.readEntryToString(*after.findEntry(ManifestEntryName)));
        assert(compactedManifest.serialize() == manifest.serialize());
        std::cout << "case 1 (dead space exact; compaction reclaims exactly it; content and manifest identical) OK\n";

        // Cancellation leaves a destination the caller must throw away.
        CancellationToken token;
        token.cancel();
        InMemoryArchiveFile abandoned;
        assert(compactArchive(reader, manifest, abandoned, token).cancelled);
        assert(!Zip64Reader::tryOpen(abandoned).has_value());
        std::cout << "case 2 (cancelled compaction yields no usable archive) OK\n";
    }

    // ---- A damaged live entry is caught, never copied ----
    {
        Zip64Reader reader = Zip64Reader::open(archive);
        std::size_t victim = *reader.findEntry("e/new");
        std::vector<std::byte> bytes = archive.bytes();
        bytes[static_cast<std::size_t>(reader.dataOffset(victim)) + 100] ^= std::byte{0x01};
        InMemoryArchiveFile damaged(bytes);
        Zip64Reader damagedReader = Zip64Reader::open(damaged);
        BackupManifest manifest = *BackupManifest::parse(damagedReader.readEntryToString(*damagedReader.findEntry(ManifestEntryName)));
        InMemoryArchiveFile destination;
        bool threw = false;
        try {
            compactArchive(damagedReader, manifest, destination);
        } catch (const ArchiveFormatError &e) {
            threw = std::string(e.what()).find("e/new") != std::string::npos;
        }
        assert(threw);
        std::cout << "case 3 (compaction is a full verify: a flipped data byte stops it) OK\n";
    }

    // ---- The use case on disk: preflight, replace, cleanup, refusal ----
    {
        fs::path root = seabass::testing::scratchRoot() / "seabass_backup_archive_compaction_test";
        fs::remove_all(root);
        fs::create_directories(root);
        fs::path path = root / "stick.zip";
        {
            std::ofstream out(path, std::ios::binary);
            const std::vector<std::byte> &bytes = archive.bytes();
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        std::string original = readFile(path);

        CompactionPreflight pre = CompactStickBackup::preflight(path);
        assert(pre.error.empty());
        assert(pre.deadBytes > 0 && pre.archiveBytes == original.size());
        assert(pre.enoughFreeSpace);

        CompactStickBackupOptions tight;
        tight.archivePath = path;
        tight.freeSpaceMarginBytes = std::uint64_t{1} << 62;
        CompactionOutcome refused = CompactStickBackup::execute(tight);
        assert(refused.status == CompactionOutcome::Status::NotEnoughSpace);
        assert(refused.message.find("bytes free") != std::string::npos);
        assert(readFile(path) == original);

        // A stale temp file from an interrupted attempt is cleaned up.
        std::ofstream(CompactStickBackup::temporaryPathFor(path)) << "leftover";
        CompactStickBackupOptions options;
        options.archivePath = path;
        CompactionOutcome done = CompactStickBackup::execute(options);
        assert(done.status == CompactionOutcome::Status::Compacted);
        assert(pre.compactedBytes == done.bytesAfter);
        assert(pre.reclaimableBytes == done.bytesBefore - done.bytesAfter);
        assert(done.bytesBefore == original.size() && done.bytesAfter < done.bytesBefore);
        assert(fs::file_size(path) == done.bytesAfter);
        assert(!fs::exists(CompactStickBackup::temporaryPathFor(path)));
        {
            PosixArchiveFile file(path, PosixArchiveFile::OpenMode::ReadOnly);
            std::string error;
            assert(verifyArchiveTail(file, 0, &error));
            assert(contents(file) == contents(archive));
            assert(deadSpace(Zip64Reader::open(file)).deadBytes == 0);
        }
        CompactionOutcome nothing = CompactStickBackup::execute(options);
        assert(nothing.status == CompactionOutcome::Status::NothingToReclaim);
        assert(!CompactStickBackup::preflight(path).suggested);
        fs::remove_all(root);
        std::cout << "case 4 (on disk: preflight refusal leaves the file alone; compaction replaces atomically; stale temp cleaned; no-op afterwards) OK\n";
    }

    // ---- The compacted size follows the new layout, across 4 GiB too ----
    {
        // A first backup writes directories before the files; compaction
        // writes in listing order, so a directory can move past 4 GiB and
        // its central record grows by the 12-byte ZIP64 offset field. Only
        // offsets and sizes decide record sizes, so the layout is sized
        // without writing 5 GiB.
        CentralEntry big;
        big.name = "a/big.bin";
        big.size = big.compressedSize = 5ull << 30;
        CentralEntry dir;
        dir.name = "b/";
        dir.isDirectory = true;
        CentralEntry manifestEntry;
        manifestEntry.name = std::string(ManifestEntryName);
        manifestEntry.size = manifestEntry.compressedSize = 100;

        const std::uint64_t timestamp = 9;        // "UT" extra
        const std::uint64_t localZip64 = 4 + 16;  // local ZIP64 extra announcing the descriptor
        const std::uint64_t zip64Sizes = 4 + 16;  // central ZIP64 extra: both sizes
        const std::uint64_t zip64Offset = 4 + 8;  // central ZIP64 extra: offset only
        const std::uint64_t manifestName = manifestEntry.name.size();
        const std::uint64_t locals = (30 + 9 + localZip64 + timestamp) + big.size + 24  // a/big.bin
            + (30 + 2 + timestamp)                                                        // b/
            + (30 + manifestName + localZip64 + timestamp) + 100 + 24;                    // manifest
        const std::uint64_t trailer = 56 + 20 + 22;

        // Listing order, as compaction writes it: b/ and the manifest sit past 4 GiB.
        const std::uint64_t listed = compactedArchiveSize(std::vector<CentralEntry>{big, dir, manifestEntry});
        assert(listed == locals + (46 + 9 + zip64Sizes + timestamp) + (46 + 2 + zip64Offset + timestamp)
                             + (46 + manifestName + zip64Offset + timestamp) + trailer);
        // Directory first, as a first backup writes it: b/ needs no offset field.
        const std::uint64_t directoryFirst = compactedArchiveSize(std::vector<CentralEntry>{dir, big, manifestEntry});
        assert(listed - directoryFirst == zip64Offset);
        std::cout << "case 5 (compacted size follows the rewritten layout: a directory moved past 4 GiB costs 12 bytes) OK\n";
    }

    // ---- A salvage backup can be compacted, and keeps its log ---------
    //
    // A salvage archive carries SEABASS-SALVAGE.txt, which describes the
    // archive and so has no manifest row of its own -- and compaction
    // demanded a row for every entry it copied. That made the archive
    // that most needs compacting (every discarded read attempt is dead
    // space in it) the one archive that could not be compacted at all.
    {
        auto salvageClock = std::make_shared<FaultClock>();
        InMemoryArchiveFile salvageArchive(salvageClock);
        InMemoryArchiveFile salvageJournal(salvageClock);
        Model salvaged = {{"d/", ""}, {"d/a", pseudoRandom(9'000, 11)}};
        generation(salvageArchive, salvageJournal, salvaged);
        salvaged["d/a"] = pseudoRandom(9'500, 12);
        generation(salvageArchive, salvageJournal, salvaged, "PARTIAL  d/a\n");

        Zip64Reader reader = Zip64Reader::open(salvageArchive);
        assert(reader.findEntry(SalvageLogEntryName).has_value());
        BackupManifest manifest = *BackupManifest::parse(reader.readEntryToString(*reader.findEntry(ManifestEntryName)));
        InMemoryArchiveFile compacted;
        std::uint64_t lastDone = 0, lastTotal = 0;
        CompactionResult result = compactArchive(reader, manifest, compacted, CancellationToken::none(),
                                                 [&](std::uint64_t done, std::uint64_t total) {
                                                     lastDone = done;
                                                     lastTotal = total;
                                                 });
        assert(!result.cancelled);
        assert(compacted.size() < salvageArchive.size() && "it really did reclaim something");
        // The log's bytes are in the total, so they have to be in what
        // was copied too: progress on a salvage backup stopped short by
        // exactly the log's size and never reached the end.
        assert(lastTotal == 9'500 + std::string("PARTIAL  d/a\n").size());
        assert(lastDone == lastTotal && "a finished compaction reports all of it copied");

        Zip64Reader after = Zip64Reader::open(compacted);
        auto logIndex = after.findEntry(SalvageLogEntryName);
        assert(logIndex && "the account of what is missing survives compaction");
        assert(after.readEntryToString(*logIndex) == "PARTIAL  d/a\n");
        assert(after.findEntry("d/a").has_value() && "and so does the library");
        std::cout << "case 6 (a salvage archive compacts, and its log comes through) OK\n";
    }

    // ---- ...and its log is checked on the way through, like the rest ---
    //
    // The salvage log has no manifest row, so the hash check every other
    // entry gets cannot apply to it -- and for that reason it used to be
    // copied with no check at all. It is the account of which files came
    // off a failing stick short; a flipped byte in it must stop the
    // compaction the way one in a library file does.
    {
        auto clock = std::make_shared<FaultClock>();
        InMemoryArchiveFile archive6(clock);
        InMemoryArchiveFile journal6(clock);
        const std::string log = "PARTIAL  d/a  4096 of 9000 bytes\n" + std::string(400, '.') + "\n";
        generation(archive6, journal6, {{"d/", ""}, {"d/a", pseudoRandom(9'000, 13)}}, log);
        Zip64Reader reader = Zip64Reader::open(archive6);
        const std::size_t victim = *reader.findEntry(SalvageLogEntryName);
        assert(reader.entries()[victim].size == log.size());
        std::vector<std::byte> bytes = archive6.bytes();
        bytes[static_cast<std::size_t>(reader.dataOffset(victim)) + 3] ^= std::byte{0x01};
        InMemoryArchiveFile damaged(bytes);
        Zip64Reader damagedReader = Zip64Reader::open(damaged);
        BackupManifest manifest =
            *BackupManifest::parse(damagedReader.readEntryToString(*damagedReader.findEntry(ManifestEntryName)));
        InMemoryArchiveFile destination;
        bool threw = false;
        try {
            compactArchive(damagedReader, manifest, destination);
        } catch (const ArchiveFormatError &e) {
            threw = std::string(e.what()).find(SalvageLogEntryName) != std::string::npos;
        }
        assert(threw && "a damaged salvage log stops the compaction and is named");
        std::cout << "case 7 (a flipped byte in the salvage log stops compaction too) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
