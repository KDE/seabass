// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The load-bearing test of the crash-safety design: after ANY crash
// during an incremental update -- the file cut at any byte, or writes
// after the last durable barrier lost or reordered -- running recovery
// must leave exactly the old archive or exactly the new one. Anything
// else is a failure. Runs entirely in memory (InMemoryArchiveFile), so
// "every byte" is affordable.

#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "infrastructure/stick_backup/archive_recovery.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/in_memory_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

using namespace seabass::infrastructure::stick_backup;
using seabass::application::CancellationToken;
using seabass::infrastructure::hashing::Sha256;

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

struct Scenario
{
    std::vector<std::byte> oldImage;  // committed generation 1
    std::vector<std::byte> newImage;  // committed generation 2
    std::shared_ptr<FaultClock> clock;
    std::unique_ptr<InMemoryArchiveFile> archive;  // holds the full mutation log of generation 2
    std::unique_ptr<InMemoryArchiveFile> journal;
    WriterBoundaries boundaries;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> newDataRanges;  // [dataOffset, end) of entries written by gen 2
};

ManifestRow fileRow(const std::string &path, const std::string &content, std::int64_t mtime, std::uint32_t crc32)
{
    return {ManifestRow::Kind::File, path, content.size(), mtime, Sha256::of(content), "", crc32};
}

// Generation 1: three small files and a directory. Generation 2 on top:
// replace one, add one file and one directory, delete one. Sizes chosen
// so the whole appended region (entries + manifest + trailer) is a few
// KB and can be swept byte by byte. `chunk` is small so appends are many
// separate mutations (more reorderings to try).
Scenario buildScenario(std::size_t fileScale, std::size_t chunk)
{
    Scenario s;
    s.clock = std::make_shared<FaultClock>();
    std::string a = pseudoRandom(5 * fileScale, 1), b = pseudoRandom(7 * fileScale, 2), c = pseudoRandom(12 * fileScale, 3);
    std::uint32_t crcOfA = 0;
    {
        InMemoryArchiveFile archive(s.clock);
        InMemoryArchiveFile journal(s.clock);
        ArchiveUpdater updater(archive, journal, {}, 0, chunk);
        updater.begin();
        BackupManifest manifest;
        manifest.createdAtUnix = 1'757'000'001;
        updater.appendDirectory("d", 10);
        manifest.rows.push_back({ManifestRow::Kind::Directory, "d", 0, 10, {}, ""});
        for (auto [name, content, mtime] : {std::tuple{"d/a", a, 11}, std::tuple{"d/b", b, 12}, std::tuple{"d/c", c, 13}}) {
            MemorySource src(content);
            auto done = updater.appendFile(name, mtime, src, CancellationToken::none());
            manifest.rows.push_back(fileRow(name, content, mtime, done->entry.crc32));
            if (std::string(name) == "d/a") {
                crcOfA = done->entry.crc32;
            }
        }
        updater.commit(manifest);
        s.oldImage = archive.bytes();
    }

    s.archive = std::make_unique<InMemoryArchiveFile>(s.oldImage, s.clock);
    s.journal = std::make_unique<InMemoryArchiveFile>(s.clock);
    Zip64Reader prior = Zip64Reader::open(*s.archive);
    std::vector<CentralEntry> carried;
    for (const CentralEntry &e : prior.entries()) {
        if (e.name == "d/" || e.name == "d/a") {  // b replaced, c deleted, manifest never carried
            carried.push_back(e);
        }
    }
    ArchiveUpdater updater(*s.archive, *s.journal, carried, prior.layout().endOfCentralDirectoryOffset, chunk);
    updater.begin();
    BackupManifest manifest;
    manifest.createdAtUnix = 1'757'000'002;
    manifest.rows.push_back({ManifestRow::Kind::Directory, "d", 0, 10, {}, ""});
    manifest.rows.push_back(fileRow("d/a", a, 11, crcOfA));
    std::string b2 = pseudoRandom(8 * fileScale, 4), e = pseudoRandom(9 * fileScale, 5);
    for (auto [name, content, mtime] : {std::tuple{"d/b", b2, 20}, std::tuple{"d/e", e, 21}}) {
        MemorySource src(content);
        auto done = updater.appendFile(name, mtime, src, CancellationToken::none());
        manifest.rows.push_back(fileRow(name, content, mtime, done->entry.crc32));
        // data starts after local header (30) + name + extras (16+4 zip64, 5+4 timestamp)
        std::uint64_t dataStart = done->entry.localHeaderOffset + 30 + std::string(name).size() + 29;
        s.newDataRanges.emplace_back(dataStart, dataStart + done->entry.size);
    }
    updater.appendDirectory("d/sub", 22);
    manifest.rows.push_back({ManifestRow::Kind::Directory, "d/sub", 0, 22, {}, ""});
    s.boundaries = updater.commit(manifest).boundaries;
    s.newImage = s.archive->bytes();
    assert(s.journal->size() == 0);
    return s;
}

enum class Verdict
{
    Old,
    New,
    Neither
};

Verdict recoverAndClassify(std::vector<std::byte> archiveBytes, std::vector<std::byte> journalBytes, const Scenario &s)
{
    InMemoryArchiveFile archive(std::move(archiveBytes));
    InMemoryArchiveFile journal(std::move(journalBytes));
    recoverOnOpen(archive, journal);
    if (journal.size() != 0) {
        return Verdict::Neither;
    }
    if (archive.bytes() == s.oldImage) {
        return Verdict::Old;
    }
    if (archive.bytes() == s.newImage) {
        return Verdict::New;
    }
    return Verdict::Neither;
}

std::vector<std::byte> journalWithRecord(const Scenario &s)
{
    InMemoryArchiveFile j;
    Zip64Reader old = Zip64Reader::open(InMemoryArchiveFile(s.oldImage));
    journal::write(j, JournalRecord{s.oldImage.size(), old.layout().endOfCentralDirectoryOffset});
    return j.bytes();
}

void report(const char *what, std::size_t olds, std::size_t news, std::size_t total)
{
    std::cout << what << ": " << total << " crash images -> " << olds << " rolled back to old, " << news
              << " confirmed new, 0 anything else OK\n";
}

}  // namespace

int main()
{
    // ---- Exhaustive truncation, small scenario ----
    {
        Scenario s = buildScenario(/*fileScale*/ 40, /*chunk*/ 64);
        std::vector<std::byte> record = journalWithRecord(s);
        std::size_t olds = 0, news = 0, total = 0;
        for (std::uint64_t cut = s.oldImage.size(); cut <= s.newImage.size(); ++cut) {
            std::vector<std::byte> image(s.newImage.begin(), s.newImage.begin() + static_cast<std::ptrdiff_t>(cut));
            Verdict v = recoverAndClassify(image, record, s);
            if (v == Verdict::Neither) {
                std::cerr << "truncation at " << cut << " of " << s.newImage.size() << " left neither old nor new\n";
                assert(false);
            }
            v == Verdict::Old ? ++olds : ++news;
            ++total;
        }
        // Only the untouched full image may read as new; every shorter cut
        // lands on old. (A cut inside the trailer can never masquerade as
        // a valid archive: the EOCD must end exactly at EOF.)
        assert(news == 1);
        report("case 1 (every truncation length, journal present)", olds, news, total);
    }

    // ---- Crash intervals x issued prefixes x random in-flight outcomes ----
    {
        Scenario s = buildScenario(40, 64);
        std::mt19937 rng(0xC0FFEE);
        std::size_t olds = 0, news = 0, total = 0;
        const std::size_t ticks = s.clock->ticks;
        for (std::size_t t = 0; t <= ticks; ++t) {
            std::size_t archiveIssuedMax = s.archive->mutationCountAtTick(t);
            std::size_t journalIssuedMax = s.journal->mutationCountAtTick(t);
            for (std::size_t ai = 0; ai <= archiveIssuedMax; ++ai) {
                for (std::size_t ji = 0; ji <= journalIssuedMax; ++ji) {
                    std::vector<std::function<bool(std::size_t)>> predicates;
                    predicates.emplace_back([](std::size_t) { return true; });
                    predicates.emplace_back([](std::size_t) { return false; });
                    for (int r = 0; r < 24; ++r) {
                        std::uint32_t seed = rng();
                        predicates.emplace_back([seed](std::size_t i) {
                            std::uint32_t h = seed ^ static_cast<std::uint32_t>(i * 2654435761u);
                            h ^= h >> 13;
                            h *= 0x5bd1e995u;
                            h ^= h >> 15;
                            return (h & 1u) != 0;
                        });
                    }
                    for (const auto &pred : predicates) {
                        Verdict v = recoverAndClassify(s.archive->crashImage(t, ai, pred), s.journal->crashImage(t, ji, pred), s);
                        if (v == Verdict::Neither) {
                            std::cerr << "crash in interval " << t << " (archive issued " << ai << ", journal issued " << ji
                                      << ") left neither old nor new\n";
                            assert(false);
                        }
                        v == Verdict::Old ? ++olds : ++news;
                        ++total;
                    }
                }
            }
        }
        assert(olds > 0 && news > 0);
        report("case 2 (barrier intervals, issued prefixes, lost/kept in-flight writes)", olds, news, total);
    }

    // ---- Larger append: boundaries +-64 plus a random sample ----
    {
        Scenario s = buildScenario(/*fileScale*/ 12'000, /*chunk*/ 4096);  // ~250 KB appended
        std::vector<std::byte> record = journalWithRecord(s);
        std::vector<std::uint64_t> cuts;
        auto around = [&](std::uint64_t at) {
            for (std::int64_t d = -64; d <= 64; ++d) {
                std::int64_t cut = static_cast<std::int64_t>(at) + d;
                if (cut >= static_cast<std::int64_t>(s.oldImage.size()) && cut <= static_cast<std::int64_t>(s.newImage.size())) {
                    cuts.push_back(static_cast<std::uint64_t>(cut));
                }
            }
        };
        around(s.oldImage.size());
        for (const auto &[start, end] : s.newDataRanges) {
            around(start);
            around(end);
            around(end + zip::DataDescriptorSize);
        }
        around(s.boundaries.manifestOffset);
        around(s.boundaries.centralDirectoryOffset);
        around(s.boundaries.zip64EndOfCentralDirectoryOffset);
        around(s.boundaries.zip64LocatorOffset);
        around(s.boundaries.endOfCentralDirectoryOffset);
        around(s.boundaries.endOffset);
        std::mt19937_64 rng(42);
        for (int i = 0; i < 1500; ++i) {
            cuts.push_back(s.oldImage.size() + rng() % (s.newImage.size() - s.oldImage.size() + 1));
        }
        std::size_t olds = 0, news = 0;
        for (std::uint64_t cut : cuts) {
            std::vector<std::byte> image(s.newImage.begin(), s.newImage.begin() + static_cast<std::ptrdiff_t>(cut));
            Verdict v = recoverAndClassify(image, record, s);
            assert(v != Verdict::Neither);
            v == Verdict::Old ? ++olds : ++news;
        }
        report("case 3 (larger append: every structural boundary +-64 bytes, 1500 random cuts)", olds, news, cuts.size());
    }

    // ---- Bit flips in the appended region are never silent ----
    {
        Scenario s = buildScenario(40, 64);
        std::mt19937_64 rng(7);
        std::vector<std::uint64_t> positions;
        for (const auto &[start, end] : s.newDataRanges) {
            for (int i = 0; i < 40; ++i) positions.push_back(start + rng() % (end - start));
            for (std::uint64_t p = end; p < end + zip::DataDescriptorSize; ++p) positions.push_back(p);  // descriptor
            for (std::uint64_t p = start - 29 - 3; p < start - 29; ++p) positions.push_back(p);        // local name
        }
        for (std::uint64_t p = s.boundaries.manifestOffset + 30 + ManifestEntryName.size() + 29; p < s.boundaries.centralDirectoryOffset; p += 3) {
            positions.push_back(p);  // manifest bytes
        }
        for (std::uint64_t p = s.boundaries.centralDirectoryOffset; p < s.boundaries.endOffset; ++p) {
            positions.push_back(p);  // central directory + trailer, every byte
        }
        std::size_t refusedOpen = 0, caughtByVerify = 0;
        for (std::uint64_t p : positions) {
            std::vector<std::byte> image = s.newImage;
            image[static_cast<std::size_t>(p)] ^= std::byte{0x40};
            InMemoryArchiveFile archive(image);
            std::string error;
            if (!Zip64Reader::tryOpen(archive, &error)) {
                ++refusedOpen;
                continue;
            }
            // Opens structurally: the tail verification (what commit and
            // recovery both run) must catch it.
            if (verifyArchiveTail(archive, s.oldImage.size(), &error)) {
                std::cerr << "flip at " << p << " went unnoticed; old end " << s.oldImage.size() << ", data ranges:";
                for (const auto &[start, end] : s.newDataRanges) std::cerr << " [" << start << "," << end << ")";
                std::cerr << ", manifest " << s.boundaries.manifestOffset << ", cd " << s.boundaries.centralDirectoryOffset
                          << ", zip64eocd " << s.boundaries.zip64EndOfCentralDirectoryOffset << ", locator "
                          << s.boundaries.zip64LocatorOffset << ", eocd " << s.boundaries.endOfCentralDirectoryOffset << ", end "
                          << s.boundaries.endOffset << "\n";
                assert(false);
            }
            ++caughtByVerify;
        }
        std::cout << "case 4 (bit flips: " << positions.size() << " positions, " << refusedOpen << " refused at open, "
                  << caughtByVerify << " caught by verification, 0 silent) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
