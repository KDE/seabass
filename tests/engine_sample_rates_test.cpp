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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <djinterop/djinterop.hpp>

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
        auto db = djinterop::engine::create_database(library.string());
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
        const SampleRateAudit audit = auditSampleRates(library.string(), probe);
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
        const SampleRateRepair repair = repairSampleRates(library.string(), audit.missing);
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        assert(repair.skipped == 1 && "nothing to write is a skip, not a failure");

        auto db = djinterop::engine::load_database(library.string());
        auto fixed = db.track_by_id(withoutRate);
        assert(fixed && fixed->sample_rate() && *fixed->sample_rate() == 48000.0);
        auto untouched = db.track_by_id(withRate);
        assert(untouched && *untouched->sample_rate() == 44100.0);

        const SampleRateAudit after = auditSampleRates(library.string(), probe);
        assert(after.missing.size() == 1 && after.missing[0].trackId == noFile);
        assert(after.fixable() == 0);
        std::cout << "case 2 (the file's answer is written, and only that) OK\n";
    }

    // 3. Without a probe the audit still reports: a page can say how many
    //    rows are affected before deciding whether to read a thousand
    //    files.
    {
        const SampleRateAudit audit = auditSampleRates(library.string());
        assert(audit.missing.size() == 1);
        assert(audit.fixable() == 0);
        std::cout << "case 3 (a count without a probe is still a count) OK\n";
    }

    // 4. A directory with no Engine library is not a fault.
    {
        const SampleRateAudit audit = auditSampleRates((root / "Contents").string());
        assert(audit.error.empty() && audit.tracksChecked == 0 && audit.missing.empty());
        std::cout << "case 4 (no Engine library, nothing to report) OK\n";
    }

    fs::remove_all(root);
    std::cout << "engine_sample_rates_test: all cases passed\n";
    return 0;
}
