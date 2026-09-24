// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A track whose Engine row does not say its sample rate, found and put
// right from the file itself.
//
// It matters because Engine stores cue positions as sample offsets: with
// no rate, everything reading the library guesses 44.1 kHz, and a 48 kHz
// track's cues land 9% out -- almost half a minute at the five-minute
// mark.

#include "infrastructure/engine/engine_sample_rates.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>
#include <zlib.h>

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::engine;

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_engine_sample_rates_test";
    fs::remove_all(root);
    const fs::path library = root / "Engine Library";
    fs::create_directories(library);
    fs::create_directories(root / "Contents");
    for (const char *name : {"knows.mp3", "quiet.mp3"}) {
        std::ofstream out(root / "Contents" / name, std::ios::binary);
        out << "AUDIO";
    }

    std::int64_t withRate = 0;
    std::int64_t withoutRate = 0;
    std::int64_t noFile = 0;
    {
        auto db = djinterop::engine::create_database(seabass::pathToUtf8(library));
        djinterop::track_snapshot known;
        known.title = "Knows Its Rate";
        known.relative_path = "../Contents/knows.mp3";
        known.sample_rate = 44100.0;
        withRate = db.create_track(known).id();

        djinterop::track_snapshot silent;
        silent.title = "Says Nothing";
        silent.relative_path = "../Contents/quiet.mp3";
        withoutRate = db.create_track(silent).id();

        djinterop::track_snapshot missing;
        missing.title = "File Is Gone";
        missing.relative_path = "../Contents/gone.mp3";
        noFile = db.create_track(missing).id();
    }

    // 1. The audit finds the rows that cannot say, and asks the file.
    {
        const auto probe = [](const std::string &file) { return file.find("quiet.mp3") != std::string::npos ? 48000.0 : 0.0; };
        const SampleRateAudit audit = auditSampleRates(seabass::pathToUtf8(library), probe);
        assert(audit.error.empty());
        assert(audit.tracksChecked == 3);
        assert(audit.missing.size() == 2 && "the one that knows is not a finding");
        assert(audit.fixable() == 1 && "only the one whose file could be asked");
        assert(audit.missing[0].trackId == withoutRate && "fixable first");
        assert(audit.missing[0].sampleRateFromFile == 48000.0);
        assert(audit.missing[1].trackId == noFile);
        assert(audit.missing[1].sampleRateFromFile == 0.0);
        std::cout << "case 1 (rows with no sample rate are found, and the file is asked) OK\n";

        // 2. The repair writes what the file said, leaves the rest, and
        //    the audit then has nothing to say about that track.
        const SampleRateRepair repair = repairSampleRates(seabass::pathToUtf8(library), audit.missing);
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        assert(repair.skipped == 1 && "nothing to write is a skip, not a failure");

        auto db = djinterop::engine::load_database(seabass::pathToUtf8(library));
        auto fixed = db.track_by_id(withoutRate);
        assert(fixed && fixed->sample_rate() && *fixed->sample_rate() == 48000.0);
        auto untouched = db.track_by_id(withRate);
        assert(untouched && *untouched->sample_rate() == 44100.0);

        const SampleRateAudit after = auditSampleRates(seabass::pathToUtf8(library), probe);
        assert(after.missing.size() == 1 && after.missing[0].trackId == noFile);
        assert(after.fixable() == 0);
        std::cout << "case 2 (the file's answer is written, and only that) OK\n";
    }

    // 3. Without a probe the audit still reports: a page can say how many
    //    rows are affected before deciding whether to read a thousand
    //    files.
    {
        const SampleRateAudit audit = auditSampleRates(seabass::pathToUtf8(library));
        assert(audit.missing.size() == 1);
        assert(audit.fixable() == 0);
        std::cout << "case 3 (a count without a probe is still a count) OK\n";
    }

    // 5. A row whose data record cannot be decoded at all (on a real stick:
    //    "Compressed data is less than the minimum size of 4 bytes") is not
    //    a finding. Seabass does not understand such a record, and says
    //    nothing about what it does not understand: the row is not listed,
    //    its file is not asked, and nothing is written. Nine such rows used
    //    to be offered as fixable and then skipped, save after save.
    {
        djinterop::track_snapshot damagedTrack;
        damagedTrack.relative_path = "../Contents/damaged.mp3";
        damagedTrack.title = "Damaged";
        damagedTrack.sample_rate = 44100.0;
        std::ofstream(root / "Contents" / "damaged.mp3") << "mp3";
        int64_t damaged = 0;
        {
            auto db = djinterop::engine::load_database(seabass::pathToUtf8(library));
            damaged = db.create_track(damagedTrack).id();
        }
        {
            sqlite3 *handle = nullptr;
            assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
            const std::string sql = "UPDATE PerformanceData SET trackData = X'00' WHERE trackId = " + std::to_string(damaged);
            assert(sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(handle);
        }
        int asked = 0;
        const auto probe = [&asked](const std::string &file) {
            if (file.find("damaged.mp3") != std::string::npos) {
                asked++;
            }
            return 48000.0;
        };
        const SampleRateAudit audit = auditSampleRates(seabass::pathToUtf8(library), probe);
        assert(audit.error.empty());
        for (const auto &e : audit.missing) {
            assert(e.trackId != damaged && "not a finding: Seabass does not understand the record");
        }
        assert(asked == 0 && "its file is not asked");
        const SampleRateRepair repair = repairSampleRates(seabass::pathToUtf8(library), audit.missing);
        assert(repair.error.empty() && repair.repaired == 0 && "nothing is written for it");
        std::cout << "case 5 (an undecodable record is left alone, and unmentioned) OK\n";
    }

    // 6. A record as an Engine 3.x player writes it: the same six fields
    //    and 24 more bytes after them (three zeroed doubles, as observed on
    //    a stick a Prime 4 had played). Not damaged: its rate reads, so it
    //    is not a finding, and it is never rewritten. libdjinterop used to
    //    demand exactly 44 bytes and threw on these, which had Seabass call
    //    seven healthy tracks unreadable and offer to "fix" them.
    {
        djinterop::track_snapshot prime4Track;
        prime4Track.relative_path = "../Contents/prime4.mp3";
        prime4Track.title = "Played On A Prime 4";
        prime4Track.sample_rate = 44100.0;
        prime4Track.duration = std::chrono::milliseconds{180000};
        int64_t prime4 = 0;
        {
            auto db = djinterop::engine::load_database(seabass::pathToUtf8(library));
            prime4 = db.create_track(prime4Track).id();
        }
        {
            // Re-store the record with 24 zero bytes appended, compressed
            // the way Engine stores it: a big-endian length, then zlib.
            sqlite3 *handle = nullptr;
            assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
            sqlite3_stmt *read = nullptr;
            const std::string select = "SELECT trackData FROM PerformanceData WHERE trackId = " + std::to_string(prime4);
            assert(sqlite3_prepare_v2(handle, select.c_str(), -1, &read, nullptr) == SQLITE_OK && sqlite3_step(read) == SQLITE_ROW);
            const auto *stored = static_cast<const unsigned char *>(sqlite3_column_blob(read, 0));
            const int storedSize = sqlite3_column_bytes(read, 0);
            assert(storedSize > 4);
            uLongf plainSize = 44;
            std::vector<unsigned char> plain(68, 0);
            assert(uncompress(plain.data(), &plainSize, stored + 4, static_cast<uLong>(storedSize - 4)) == Z_OK && plainSize == 44);
            sqlite3_finalize(read);
            uLongf packedSize = compressBound(68);
            std::vector<unsigned char> packed(4 + packedSize);
            assert(compress2(packed.data() + 4, &packedSize, plain.data(), 68, Z_DEFAULT_COMPRESSION) == Z_OK);
            packed.resize(4 + packedSize);
            packed[0] = 0; packed[1] = 0; packed[2] = 0; packed[3] = 68;
            sqlite3_stmt *write = nullptr;
            const std::string update = "UPDATE PerformanceData SET trackData = ? WHERE trackId = " + std::to_string(prime4);
            assert(sqlite3_prepare_v2(handle, update.c_str(), -1, &write, nullptr) == SQLITE_OK);
            sqlite3_bind_blob(write, 1, packed.data(), static_cast<int>(packed.size()), SQLITE_TRANSIENT);
            assert(sqlite3_step(write) == SQLITE_DONE && sqlite3_changes(handle) == 1);
            sqlite3_finalize(write);
            sqlite3_close(handle);
        }
        const auto probe = [](const std::string &) { return 48000.0; };
        const SampleRateAudit audit = auditSampleRates(seabass::pathToUtf8(library), probe);
        for (const auto &e : audit.missing) {
            assert(e.trackId != prime4 && "a 3.x record reads, and is not a finding");
        }
        auto db = djinterop::engine::load_database(seabass::pathToUtf8(library));
        auto track = db.track_by_id(prime4);
        assert(track && track->sample_rate() && *track->sample_rate() == 44100.0);
        std::cout << "case 6 (an Engine 3.x record with extra bytes reads, and is left alone) OK\n";
    }

    // 4. A directory with no Engine library is not a fault.
    {
        const SampleRateAudit audit = auditSampleRates(seabass::pathToUtf8(root / "Contents"));
        assert(audit.error.empty() && audit.tracksChecked == 0 && audit.missing.empty());
        std::cout << "case 4 (no Engine library, nothing to report) OK\n";
    }

    fs::remove_all(root);
    std::cout << "engine_sample_rates_test: all cases passed\n";
    return 0;
}
