// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The Engine reader asks its sample rate source only about a row that
// has a cue or a loop and records no rate.
//
// Engine keeps cues as sample offsets. A row the player has not analysed
// yet records no sample rate, and the reader used to take 44.1 kHz for
// it, which puts the cues of a 48 kHz file 9 percent late. Asking the
// file costs a file open per row on a USB stick, so the reader asks only
// where the answer moves a cue: never for a row with a stored rate, never
// for a row with nothing cued. Over a library made here, six rows:
//
//   analysed     48 kHz stored, main cue at 48000      never asked, 1000 ms
//   unanalysed   no rate, hot cue 1 at 48000            asked, 48 kHz: 1000 ms
//   bare         no rate, nothing cued                  never asked
//   unanswered   no rate, main cue at 44100             asked, no answer: 1000 ms (the guess)
//   looped       no rate, loop 3 from 96000 to 144000   asked, 48 kHz: 2000 to 3000 ms
//   throwing     no rate, hot cue 2 at 88200            asked, throws: 2000 ms (the guess), a warning
//
// Through readTracks() with the source set, through fillCues() over a
// read with none (the catalog cache's Cues stage), and fillCues() with no
// source, which asks nothing and changes nothing.

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using infrastructure::engine::LibdjinteropEngineReader;

namespace
{

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-6;
}

const domain::Track &byTitle(const std::vector<domain::Track> &tracks, const std::string &title)
{
    for (const auto &t : tracks) {
        if (t.title == title) {
            return t;
        }
    }
    assert(false && "a row of the library made here");
    std::abort();
}

struct CountingWarnings : application::ProgressReporter
{
    void start(const std::string &, size_t) override {}
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &message) override { warnings.push_back(message); }
    std::vector<std::string> warnings;
};

// The six rows' times, at the rates the header lists.
void expectRightTimes(const std::vector<domain::Track> &tracks)
{
    assert(tracks.size() == 6);
    {
        const auto &t = byTitle(tracks, "analysed");
        assert(t.cues.size() == 1 && t.cues[0].kind == domain::CuePoint::Kind::Memory);
        assert(near(t.cues[0].positionMs, 1000.0));
    }
    {
        const auto &t = byTitle(tracks, "unanalysed");
        assert(t.cues.size() == 1 && t.cues[0].kind == domain::CuePoint::Kind::Hot && t.cues[0].hotCueNumber == 1);
        assert(near(t.cues[0].positionMs, 1000.0) && "48000 samples at the file's 48 kHz");
    }
    assert(byTitle(tracks, "bare").cues.empty());
    {
        const auto &t = byTitle(tracks, "unanswered");
        assert(t.cues.size() == 1 && near(t.cues[0].positionMs, 1000.0) && "44100 samples at the 44.1 kHz guess");
    }
    {
        const auto &t = byTitle(tracks, "looped");
        assert(t.cues.size() == 1 && t.cues[0].isLoop && t.cues[0].hotCueNumber == 3);
        assert(near(t.cues[0].positionMs, 2000.0) && near(t.cues[0].loopEndMs, 3000.0));
    }
    {
        const auto &t = byTitle(tracks, "throwing");
        assert(t.cues.size() == 1 && t.cues[0].hotCueNumber == 2);
        assert(near(t.cues[0].positionMs, 2000.0) && "88200 samples at the 44.1 kHz guess");
    }
}

}  // namespace

int main()
{
    const fs::path stick = testing::scratchRoot() / "seabass_engine_reader_sample_rate_source_test";
    const fs::path root = stick / "Engine Library";
    fs::remove_all(stick);
    fs::create_directories(stick);

    {
        auto db = djinterop::engine::create_database(pathToUtf8(root));
        const auto hotCues = [](size_t pad, double offset) {
            std::vector<std::optional<djinterop::hot_cue>> cues(8);
            cues[pad - 1] = djinterop::hot_cue{"", offset, djinterop::pad_color{}};
            return cues;
        };

        djinterop::track_snapshot analysed;
        analysed.title = "analysed";
        analysed.relative_path = "../Contents/analysed.mp3";
        analysed.sample_rate = 48000.0;
        analysed.main_cue = 48000.0;
        db.create_track(analysed);

        djinterop::track_snapshot unanalysed;
        unanalysed.title = "unanalysed";
        unanalysed.relative_path = "../Contents/unanalysed.mp3";
        unanalysed.hot_cues = hotCues(1, 48000.0);
        db.create_track(unanalysed);

        djinterop::track_snapshot bare;
        bare.title = "bare";
        bare.relative_path = "../Contents/bare.mp3";
        db.create_track(bare);

        djinterop::track_snapshot unanswered;
        unanswered.title = "unanswered";
        unanswered.relative_path = "../Contents/unanswered.mp3";
        unanswered.main_cue = 44100.0;
        db.create_track(unanswered);

        djinterop::track_snapshot looped;
        looped.title = "looped";
        looped.relative_path = "../Contents/looped.mp3";
        looped.loops.resize(8);
        looped.loops[2] = djinterop::loop{"", 96000.0, 144000.0, djinterop::pad_color{}};
        db.create_track(looped);

        djinterop::track_snapshot throwing;
        throwing.title = "throwing";
        throwing.relative_path = "../Contents/throwing.mp3";
        throwing.hot_cues = hotCues(2, 88200.0);
        db.create_track(throwing);

        // The precondition: only the first row records a rate.
        for (const auto &t : db.tracks()) {
            assert(t.sample_rate().has_value() == (t.title() == std::optional<std::string>("analysed")));
        }
    }

    const auto file = [&](const std::string &name) {
        return pathToUtf8((root / ".." / "Contents" / (name + ".mp3")).lexically_normal());
    };
    std::map<std::string, int> asked;
    const LibdjinteropEngineReader::SampleRateSource source = [&](const std::string &path) -> std::optional<double> {
        ++asked[path];
        if (path == file("throwing")) {
            throw std::runtime_error("cannot open");
        }
        if (path == file("unanswered")) {
            return std::nullopt;
        }
        return 48000.0;
    };
    const std::map<std::string, int> askedOnceEach{
        {file("unanalysed"), 1}, {file("unanswered"), 1}, {file("looped"), 1}, {file("throwing"), 1}};

    // 1. readTracks() with the source set.
    {
        CountingWarnings warnings;
        LibdjinteropEngineReader reader(pathToUtf8(root));
        reader.setProgressReporter(warnings);
        reader.setSampleRateSource(source);
        const auto tracks = reader.readTracks();
        expectRightTimes(tracks);
        assert(asked == askedOnceEach && "only the four rows with a cue and no rate, once each");
        assert(warnings.warnings.size() == 1 && warnings.warnings[0].find("cannot open") != std::string::npos);
    }
    std::cout << "case 1 (readTracks asks 4 of 6 rows, once each: not the analysed one, not the bare one) OK\n";

    // 2. A read with no source has every rate-less row at the guess and
    //    asks nothing; fillCues() with no source changes nothing.
    LibdjinteropEngineReader plain(pathToUtf8(root));
    const auto guessed = plain.readTracks();
    assert(near(byTitle(guessed, "unanalysed").cues[0].positionMs, 48000.0 / 44100.0 * 1000.0));
    assert(near(byTitle(guessed, "looped").cues[0].positionMs, 96000.0 / 44100.0 * 1000.0));
    {
        auto same = guessed;
        plain.fillCues(same);
        for (size_t i = 0; i < same.size(); ++i) {
            assert(same[i].cues.size() == guessed[i].cues.size());
            for (size_t c = 0; c < same[i].cues.size(); ++c) {
                assert(same[i].cues[c].positionMs == guessed[i].cues[c].positionMs);
            }
        }
    }
    std::cout << "case 2 (no source: the 44.1 kHz guess, and fillCues changes nothing) OK\n";

    // 3. fillCues() with the source over that read: the same rows asked,
    //    the same times as case 1.
    {
        asked.clear();
        LibdjinteropEngineReader cues(pathToUtf8(root));
        CountingWarnings warnings;
        cues.setProgressReporter(warnings);
        cues.setSampleRateSource(source);
        auto corrected = guessed;
        cues.fillCues(corrected);
        expectRightTimes(corrected);
        assert(asked == askedOnceEach && "fillCues asks the same four rows, once each");
        assert(warnings.warnings.size() == 1);
    }
    std::cout << "case 3 (fillCues over a source-less read asks the same 4 rows and gives the same times) OK\n";

    fs::remove_all(stick);
    std::cout << "engine_reader_sample_rate_source_test: all cases passed\n";
    return 0;
}
