// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// playbackShiftFor() over the committed MP3s in tests/fixtures/mp3-lead-in:
// what the player adds to cues and beat grids to put them on the
// timeline it plays on (#50).
//
//   playback_timeline_test FIXTURE_DIR

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "domain/matching_policy.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/audio/playback_timeline.hpp"

using seabass::domain::MatchingPolicy;
using seabass::domain::Mp3LeadIn;
using seabass::infrastructure::audio::GridTimeline;
using seabass::infrastructure::audio::PlaybackShift;
using seabass::infrastructure::audio::playbackShiftFor;
using seabass::infrastructure::audio::readMp3LeadIn;

namespace
{

bool near(double a, double b)
{
    return std::fabs(a - b) < 0.001;
}

// Puts the policy back however the test leaves it.
struct PolicyRestore
{
    bool was = MatchingPolicy::correctMp3CueTiming();
    ~PolicyRestore() { MatchingPolicy::setCorrectMp3CueTiming(was); }
};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: playback_timeline_test FIXTURE_DIR\n";
        return 2;
    }
    const std::string dir = std::string(argv[1]) + "/";
    const PolicyRestore restore;
    MatchingPolicy::setCorrectMp3CueTiming(true);

    // A LAME file at 44.1 kHz: rekordbox counts 2257 samples (51.179 ms)
    // more than Engine. A rekordbox library's cues and grid both come
    // down by that; an Engine library's grid is already on the playback
    // timeline and only its cues (kept on rekordbox's) move.
    constexpr double Lame44100Ms = -2257 * 1000.0 / 44100;
    {
        const auto leadIn = readMp3LeadIn(dir + "lame-cbr-44100.mp3");
        assert(leadIn);
        const PlaybackShift rb = playbackShiftFor(leadIn, true, GridTimeline::Rekordbox);
        assert(near(rb.cuesMs, Lame44100Ms));
        assert(near(rb.beatsMs, Lame44100Ms));
        const PlaybackShift engine = playbackShiftFor(leadIn, true, GridTimeline::Engine);
        assert(near(engine.cuesMs, Lame44100Ms));
        assert(engine.beatsMs == 0.0);
    }

    // FFmpeg's Lavc file: the tag frame is skipped by both, so only the
    // trimmed delay separates them, 1105 samples (25.057 ms).
    {
        const auto leadIn = readMp3LeadIn(dir + "ffmpeg-lavc-44100.mp3");
        assert(leadIn);
        const PlaybackShift rb = playbackShiftFor(leadIn, true, GridTimeline::Rekordbox);
        assert(near(rb.cuesMs, -1105 * 1000.0 / 44100));
        assert(near(rb.beatsMs, -1105 * 1000.0 / 44100));
    }

    // No tag frame at all: the two decoders agree, nothing moves.
    {
        const auto leadIn = readMp3LeadIn(dir + "lame-notag-44100.mp3");
        assert(leadIn);
        const PlaybackShift rb = playbackShiftFor(leadIn, true, GridTimeline::Rekordbox);
        assert(rb.cuesMs == 0.0 && rb.beatsMs == 0.0);
    }

    // The correction switched off leaves everything where the libraries
    // hold it, and a file whose lead-in could not be read is not guessed at.
    {
        const auto leadIn = readMp3LeadIn(dir + "lame-cbr-44100.mp3");
        const PlaybackShift off = playbackShiftFor(leadIn, false, GridTimeline::Rekordbox);
        assert(off.cuesMs == 0.0 && off.beatsMs == 0.0);
        const PlaybackShift unread = playbackShiftFor(std::nullopt, true, GridTimeline::Rekordbox);
        assert(unread.cuesMs == 0.0 && unread.beatsMs == 0.0);
    }

    // By path: the policy and the extension gate what is read.
    {
        const PlaybackShift byPath = playbackShiftFor(dir + "lame-cbr-44100.mp3", GridTimeline::Rekordbox);
        assert(near(byPath.cuesMs, Lame44100Ms));
        assert(near(byPath.beatsMs, Lame44100Ms));
        const PlaybackShift wav = playbackShiftFor(dir + "silence.wav", GridTimeline::Rekordbox);
        assert(wav.cuesMs == 0.0 && wav.beatsMs == 0.0);
        const PlaybackShift missing = playbackShiftFor(dir + "not-there.mp3", GridTimeline::Rekordbox);
        assert(missing.cuesMs == 0.0 && missing.beatsMs == 0.0);
        MatchingPolicy::setCorrectMp3CueTiming(false);
        const PlaybackShift off = playbackShiftFor(dir + "lame-cbr-44100.mp3", GridTimeline::Rekordbox);
        assert(off.cuesMs == 0.0 && off.beatsMs == 0.0);
        MatchingPolicy::setCorrectMp3CueTiming(true);
    }

    std::cout << "playback_timeline_test: all passed\n";
    return 0;
}
