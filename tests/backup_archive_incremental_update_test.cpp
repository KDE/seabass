// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
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
namespace hashing = seabass::infrastructure::hashing;

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

std::uint64_t g_bytesRead = 0;  // every byte any MemorySource handed out

class MemorySource : public EntrySource
{
public:
    explicit MemorySource(std::string content) : m_content(std::move(content)) {}
    std::size_t read(std::span<std::byte> out) override
    {
        std::size_t take = std::min(out.size(), m_content.size() - m_pos);
        std::memcpy(out.data(), m_content.data() + m_pos, take);
        m_pos += take;
        g_bytesRead += take;
        return take;
    }

private:
    std::string m_content;
    std::size_t m_pos = 0;
};

// Cancels the token once `cancelAfter` bytes have been handed out.
class CancellingSource : public EntrySource
{
public:
    CancellingSource(std::string content, std::size_t cancelAfter, CancellationToken token)
        : m_inner(std::move(content)), m_cancelAfter(cancelAfter), m_token(std::move(token))
    {
    }
    std::size_t read(std::span<std::byte> out) override
    {
        std::size_t got = m_inner.read(out);
        m_given += got;
        if (m_given >= m_cancelAfter) {
            m_token.cancel();
        }
        return got;
    }

private:
    MemorySource m_inner;
    std::size_t m_cancelAfter;
    std::size_t m_given = 0;
    CancellationToken m_token;
};

struct ModelItem
{
    std::string content;
    std::int64_t mtime = 0;
    bool directory = false;
};
using Model = std::map<std::string, ModelItem>;

struct UpdateStats
{
    std::size_t appended = 0;
    std::size_t carried = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t expectedDeadBytes = 0;  // old file bytes not carried forward
};

BackupManifest baseManifest()
{
    BackupManifest manifest;
    manifest.stickIdentifier = "uuid-x";
    manifest.stickLabel = "MODEL";
    manifest.createdAtUnix = 1'757'000'000;
    return manifest;
}

// The mini stat-diff the real use case will do: an entry is carried when
// the model still has the path with the same size (files) and mtime.
UpdateStats applyUpdate(InMemoryArchiveFile &archive, InMemoryArchiveFile &journal, const Model &model,
                        std::size_t chunkSize = 256)
{
    UpdateStats stats;
    std::vector<CentralEntry> carried;
    std::map<std::string, hashing::Sha256Digest> carriedHashes;
    std::map<std::string, std::uint32_t> carriedCrcs;
    std::uint64_t priorEocd = 0;
    if (archive.size() > 0) {
        Zip64Reader reader = Zip64Reader::open(archive);
        priorEocd = reader.layout().endOfCentralDirectoryOffset;
        std::size_t manifestIndex = *reader.findEntry(ManifestEntryName);
        BackupManifest previous = *BackupManifest::parse(reader.readEntryToString(manifestIndex));
        stats.expectedDeadBytes = reader.layout().fileSize;
        for (std::size_t i = 0; i < reader.entries().size(); ++i) {
            if (i == manifestIndex) {
                continue;
            }
            const CentralEntry &entry = reader.entries()[i];
            std::string path = entry.isDirectory ? entry.name.substr(0, entry.name.size() - 1) : entry.name;
            auto it = model.find(path);
            bool keep = it != model.end() && it->second.directory == entry.isDirectory && it->second.mtime == entry.mtimeUnix
                        && (entry.isDirectory || it->second.content.size() == entry.size);
            if (!keep) {
                continue;
            }
            carried.push_back(entry);
            const ManifestRow *row = previous.findRow(path);
            assert(row != nullptr);
            carriedHashes[path] = row->sha256;
            carriedCrcs[path] = row->crc32;
            stats.expectedDeadBytes -= reader.footprint(i);
        }
    }
    stats.carried = carried.size();
    std::set<std::string> carriedPaths;
    for (const CentralEntry &entry : carried) {
        carriedPaths.insert(entry.isDirectory ? entry.name.substr(0, entry.name.size() - 1) : entry.name);
    }

    ArchiveUpdater updater(archive, journal, carried, priorEocd, chunkSize);
    updater.begin();
    BackupManifest manifest = baseManifest();
    std::uint64_t readBefore = g_bytesRead;
    for (const auto &[path, item] : model) {
        ManifestRow row;
        row.path = path;
        row.mtimeUnix = item.mtime;
        row.kind = item.directory ? ManifestRow::Kind::Directory : ManifestRow::Kind::File;
        row.size = item.directory ? 0 : item.content.size();
        if (carriedPaths.count(path) != 0) {
            row.sha256 = carriedHashes[path];
            row.crc32 = carriedCrcs[path];
        } else if (item.directory) {
            updater.appendDirectory(path, item.mtime);
            ++stats.appended;
        } else {
            MemorySource source(item.content);
            auto appended = updater.appendFile(path, item.mtime, source, CancellationToken::none());
            assert(appended.has_value());
            row.sha256 = appended->sha256;
            row.crc32 = appended->entry.crc32;
            ++stats.appended;
        }
        manifest.rows.push_back(row);
    }
    updater.commit(manifest);
    stats.bytesRead = g_bytesRead - readBefore;
    return stats;
}

void verifyMatchesModel(const InMemoryArchiveFile &archive, const Model &model, std::uint64_t expectedDeadBytes)
{
    std::string error;
    assert(verifyArchiveTail(archive, 0, &error));
    Zip64Reader reader = Zip64Reader::open(archive);
    std::size_t manifestIndex = *reader.findEntry(ManifestEntryName);
    BackupManifest manifest = *BackupManifest::parse(reader.readEntryToString(manifestIndex));
    assert(reader.entries().size() == model.size() + 1);
    std::uint64_t footprints = 0;
    for (std::size_t i = 0; i < reader.entries().size(); ++i) {
        footprints += reader.footprint(i);
        if (i == manifestIndex) {
            continue;
        }
        const CentralEntry &entry = reader.entries()[i];
        std::string path = entry.isDirectory ? entry.name.substr(0, entry.name.size() - 1) : entry.name;
        auto it = model.find(path);
        assert(it != model.end());
        assert(it->second.directory == entry.isDirectory);
        assert(it->second.mtime == entry.mtimeUnix);
        if (!entry.isDirectory) {
            assert(reader.readEntryToString(i) == it->second.content);
            const ManifestRow *row = manifest.findRow(path);
            assert(row != nullptr && row->sha256 == Sha256::of(it->second.content));
        }
    }
    std::uint64_t dead = reader.layout().centralDirectoryOffset - footprints;
    assert(dead == expectedDeadBytes);
}

Model startingModel()
{
    Model model;
    model["Contents"] = {"", 100, true};
    model["Contents/a.mp3"] = {pseudoRandom(3000, 1), 1000};
    model["Contents/b.mp3"] = {pseudoRandom(500, 2), 1001};
    model["Contents/c.mp3"] = {pseudoRandom(1234, 3), 1002};
    model["Engine Library/Database2/m.db"] = {pseudoRandom(4096, 4), 2000};
    model["notes.txt"] = {"hello", 3000};
    return model;
}

}  // namespace

int main()
{
    // ---- First backup, then add / replace / delete / no-op ----
    {
        auto clock = std::make_shared<FaultClock>();
        InMemoryArchiveFile archive(clock);
        InMemoryArchiveFile journal(clock);
        Model model = startingModel();

        UpdateStats first = applyUpdate(archive, journal, model);
        assert(first.carried == 0 && first.appended == model.size());
        verifyMatchesModel(archive, model, 0);
        assert(journal.size() == 0);
        assert(recoverOnOpen(archive, journal).action == RecoveryOutcome::Action::Nothing);
        std::cout << "case 1 (first backup: no dead space, journal clear) OK\n";

        model["Contents/d.mp3"] = {pseudoRandom(777, 5), 1003};                 // added
        model["Contents/b.mp3"] = {pseudoRandom(600, 6), 1500};                 // replaced (size + mtime)
        model["notes.txt"] = {"hell0", 3001};                                   // replaced (same size, new mtime)
        model.erase("Contents/c.mp3");                                          // deleted
        model["Playlists"] = {"", 101, true};                                   // added dir
        std::uint64_t changedBytes = 777 + 600 + 5;

        UpdateStats second = applyUpdate(archive, journal, model);
        assert(second.appended == 4 && second.carried == 3);
        assert(second.bytesRead == changedBytes);  // unchanged files were never read
        verifyMatchesModel(archive, model, second.expectedDeadBytes);
        assert(second.expectedDeadBytes > 0);
        std::cout << "case 2 (incremental: only changed bytes read, dead space exact) OK\n";

        // No changes at all: nothing appended, still consistent.
        UpdateStats third = applyUpdate(archive, journal, model);
        assert(third.appended == 0 && third.bytesRead == 0);
        verifyMatchesModel(archive, model, third.expectedDeadBytes);
        std::cout << "case 3 (no-op update) OK\n";
    }

    // ---- Random rounds: no drift, dead space always exact ----
    {
        std::mt19937 rng(20260905);
        auto clock = std::make_shared<FaultClock>();
        InMemoryArchiveFile archive(clock);
        InMemoryArchiveFile journal(clock);
        Model model = startingModel();
        applyUpdate(archive, journal, model);
        for (int round = 0; round < 30; ++round) {
            int ops = 1 + static_cast<int>(rng() % 4);
            for (int op = 0; op < ops; ++op) {
                std::vector<std::string> files;
                for (const auto &[path, item] : model) {
                    if (!item.directory) {
                        files.push_back(path);
                    }
                }
                switch (rng() % 3) {
                case 0:
                    model["Contents/r" + std::to_string(round) + "_" + std::to_string(op) + ".mp3"] = {
                        pseudoRandom(rng() % 5000, rng()), static_cast<std::int64_t>(10'000 + round)};
                    break;
                case 1:
                    if (!files.empty()) {
                        const std::string &victim = files[rng() % files.size()];
                        model[victim] = {pseudoRandom(rng() % 5000, rng()), static_cast<std::int64_t>(20'000 + round)};
                    }
                    break;
                case 2:
                    if (files.size() > 1) {
                        model.erase(files[rng() % files.size()]);
                    }
                    break;
                }
            }
            UpdateStats stats = applyUpdate(archive, journal, model);
            verifyMatchesModel(archive, model, stats.expectedDeadBytes);
        }
        std::cout << "case 4 (30 random mutation rounds: content exact, dead space exact every round) OK\n";
    }

    // ---- Crash after the trailer was durable but before the journal clear ----
    {
        auto clock = std::make_shared<FaultClock>();
        InMemoryArchiveFile archive(clock);
        InMemoryArchiveFile journal(clock);
        Model model = startingModel();
        applyUpdate(archive, journal, model);
        model["Contents/x.mp3"] = {pseudoRandom(900, 9), 5000};
        applyUpdate(archive, journal, model);
        std::vector<std::byte> completed = archive.bytes();

        // The last barrier was the journal clear's; crash inside that
        // interval with the clear itself lost.
        std::size_t lastTick = clock->ticks - 1;
        auto dropAll = [](std::size_t) { return false; };
        InMemoryArchiveFile crashedArchive(archive.crashImage(lastTick, 0, dropAll));
        InMemoryArchiveFile crashedJournal(journal.crashImage(lastTick, 1, dropAll));
        assert(crashedArchive.bytes() == completed);
        assert(journal::read(crashedJournal).kind == JournalState::Kind::Valid);

        RecoveryOutcome outcome = recoverOnOpen(crashedArchive, crashedJournal);
        assert(outcome.action == RecoveryOutcome::Action::ConfirmedComplete);
        assert(crashedArchive.bytes() == completed);
        assert(crashedJournal.size() == 0);
        verifyMatchesModel(crashedArchive, model, 0 /* recomputed below */ + (Zip64Reader::open(crashedArchive).layout().centralDirectoryOffset
                                                                               - [&] {
                                                                                     Zip64Reader r = Zip64Reader::open(crashedArchive);
                                                                                     std::uint64_t f = 0;
                                                                                     for (std::size_t i = 0; i < r.entries().size(); ++i) f += r.footprint(i);
                                                                                     return f;
                                                                                 }()));
        UpdateStats rerun = applyUpdate(crashedArchive, crashedJournal, model);
        assert(rerun.appended == 0 && rerun.bytesRead == 0);
        std::cout << "case 5 (journal outlives a completed update: confirmed, not rolled back; rerun is a no-op) OK\n";
    }

    // ---- Cancel mid-file, then keep for later ----
    {
        auto clock = std::make_shared<FaultClock>();
        InMemoryArchiveFile archive(clock);
        InMemoryArchiveFile journal(clock);
        Model model;
        model["a.bin"] = {pseudoRandom(1000, 1), 1};
        model["b.bin"] = {pseudoRandom(5000, 2), 2};
        model["c.bin"] = {pseudoRandom(300, 3), 3};

        ArchiveUpdater updater(archive, journal, {}, 0, 256);
        updater.begin();
        BackupManifest manifest = baseManifest();
        manifest.status = BackupStatus::PartialCancelled;
        {
            MemorySource a(model["a.bin"].content);
            auto done = updater.appendFile("a.bin", 1, a, CancellationToken::none());
            manifest.rows.push_back({ManifestRow::Kind::File, "a.bin", 1000, 1, done->sha256, "", done->entry.crc32});
        }
        CancellationToken token;
        std::uint64_t beforeB = archive.size();
        {
            CancellingSource b(model["b.bin"].content, 1200, token);
            auto done = updater.appendFile("b.bin", 2, b, token);
            assert(!done.has_value());  // cancelled mid-file: abandoned
            assert(token.cancelled());
        }
        std::uint64_t abandoned = archive.size() - beforeB;
        assert(abandoned > 1200 && abandoned < 1200 + 256 + 200);
        {
            MemorySource c(model["c.bin"].content);
            assert(!updater.appendFile("c.bin", 3, c, token).has_value());  // token already fired: nothing written
        }
        updater.commit(manifest);  // "keep for later"

        Zip64Reader reader = Zip64Reader::open(archive);
        assert(reader.entries().size() == 2 && reader.entries()[0].name == "a.bin");
        BackupManifest kept = *BackupManifest::parse(reader.readEntryToString(1));
        assert(kept.status == BackupStatus::PartialCancelled);
        std::uint64_t footprints = reader.footprint(0) + reader.footprint(1);
        assert(reader.layout().centralDirectoryOffset - footprints == abandoned);

        // The next run resumes: a carried, b and c appended.
        UpdateStats resume = applyUpdate(archive, journal, model);
        assert(resume.carried == 1 && resume.appended == 2);
        assert(resume.bytesRead == 5000 + 300);
        verifyMatchesModel(archive, model, resume.expectedDeadBytes);
        std::cout << "case 6 (cancel mid-file, keep for later, resume) OK\n";
    }

    // ---- Cancel, then discard ----
    {
        auto clock = std::make_shared<FaultClock>();
        InMemoryArchiveFile archive(clock);
        InMemoryArchiveFile journal(clock);
        Model model = startingModel();
        applyUpdate(archive, journal, model);
        std::vector<std::byte> before = archive.bytes();
        Zip64Reader prior = Zip64Reader::open(archive);
        std::vector<CentralEntry> carried;
        for (const CentralEntry &e : prior.entries()) {
            if (e.name != ManifestEntryName) carried.push_back(e);
        }
        ArchiveUpdater updater(archive, journal, carried, prior.layout().endOfCentralDirectoryOffset, 256);
        updater.begin();
        MemorySource d(pseudoRandom(2000, 7));
        updater.appendFile("d.bin", 9, d, CancellationToken::none());
        assert(archive.size() > before.size());
        updater.abort();  // "discard"
        assert(archive.bytes() == before);
        assert(journal.size() == 0);
        verifyMatchesModel(archive, model, 0);

        // First backup discarded: nothing left.
        InMemoryArchiveFile fresh(clock);
        InMemoryArchiveFile freshJournal(clock);
        ArchiveUpdater firstRun(fresh, freshJournal, {}, 0, 256);
        firstRun.begin();
        MemorySource e(pseudoRandom(100, 8));
        firstRun.appendFile("e.bin", 1, e, CancellationToken::none());
        firstRun.abort();
        assert(fresh.size() == 0 && freshJournal.size() == 0);
        std::cout << "case 7 (cancel then discard restores the previous image byte-exactly; first backup ends empty) OK\n";
    }

    // ---- A journal left behind blocks a new update until recovered ----
    {
        auto clock = std::make_shared<FaultClock>();
        InMemoryArchiveFile archive(clock);
        InMemoryArchiveFile journal(clock);
        journal::write(journal, JournalRecord{0, 0});
        ArchiveUpdater updater(archive, journal, {}, 0, 256);
        bool threw = false;
        try {
            updater.begin();
        } catch (const ArchiveIoError &) {
            threw = true;
        }
        assert(threw);
        assert(recoverOnOpen(archive, journal).action == RecoveryOutcome::Action::RolledBack);
        assert(journal.size() == 0);
        std::cout << "case 8 (stale journal refuses begin() until recovered) OK\n";
    }

    // ---- Commit verification fails: journal stays, recovery rolls back ----
    {
        // A medium that acknowledges the trailer barrier and then hands
        // back a different data byte.
        class LyingFile : public InMemoryArchiveFile
        {
        public:
            using InMemoryArchiveFile::InMemoryArchiveFile;
            std::uint64_t corruptAt = 0;
            void barrier() override
            {
                InMemoryArchiveFile::barrier();
                if (++m_barriers == 3 && corruptAt > 0) {  // begin's journal barrier is on the other file; 1 = entries, 2 = trailer
                    corruptByteForTesting(corruptAt);
                }
            }

        private:
            int m_barriers = 0;
        };
        auto clock = std::make_shared<FaultClock>();
        LyingFile archive(clock);
        InMemoryArchiveFile journal(clock);
        Model model = startingModel();
        applyUpdate(archive, journal, model);  // barriers 1 and 2 on the archive
        std::vector<std::byte> before = archive.bytes();
        Zip64Reader prior = Zip64Reader::open(archive);
        std::vector<CentralEntry> carried;
        for (const CentralEntry &e : prior.entries()) {
            if (e.name != ManifestEntryName) carried.push_back(e);
        }
        ArchiveUpdater updater(archive, journal, carried, prior.layout().endOfCentralDirectoryOffset, 256);
        updater.begin();
        MemorySource d(pseudoRandom(2000, 7));
        auto appended = updater.appendFile("d.bin", 9, d, CancellationToken::none());
        archive.corruptAt = prior.layout().fileSize + 30 + 5 + 25 + 100;  // inside d.bin's data (header 30 + name 5 + extras 25)
        BackupManifest manifest = baseManifest();
        for (const CentralEntry &e : carried) {
            std::string path = e.isDirectory ? e.name.substr(0, e.name.size() - 1) : e.name;
            ManifestRow row;
            row.path = path;
            row.kind = e.isDirectory ? ManifestRow::Kind::Directory : ManifestRow::Kind::File;
            row.size = e.size;
            row.mtimeUnix = e.mtimeUnix;
            row.sha256 = Sha256::of(model[path].content);
            row.crc32 = e.crc32;
            manifest.rows.push_back(row);
        }
        manifest.rows.push_back({ManifestRow::Kind::File, "d.bin", 2000, 9, appended->sha256, "", appended->entry.crc32});
        bool threw = false;
        try {
            updater.commit(manifest);
        } catch (const ArchiveFormatError &e) {
            threw = std::string(e.what()).find("CRC mismatch in d.bin") != std::string::npos;
        }
        assert(threw);
        assert(journal::read(journal).kind == JournalState::Kind::Valid);  // deliberately left behind
        RecoveryOutcome outcome = recoverOnOpen(archive, journal);
        assert(outcome.action == RecoveryOutcome::Action::RolledBack);
        assert(archive.bytes() == before);
        std::cout << "case 9 (post-commit verification failure keeps the journal; recovery rolls back to the previous image) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
