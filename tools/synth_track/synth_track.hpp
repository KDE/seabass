// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/beat_grid.hpp"

// Music-like tracks whose beat grid is known exactly, made from a
// description (docs/track-analysis-plan.md, "The synthetic tracks"). For
// the analysis tests and tools/analysis_benchmark only: not shipped.
//
// A track is a simple dance track: kick, snare or clap, hats, a bass line
// and pad chords in a stated key, a reverb, the bass and pad ducked under
// the kick, a soft limiter. Its tempo follows a programme that may stay
// put, drift, ramp, step or alternate; the drums may scatter around the
// beat like a live drummer's while the true grid stays the smooth one.
namespace seabass::synth
{

// A pseudo-random generator and the two distributions the tracks use,
// written out here rather than taken from <random>: the standard's
// engines are fixed, but its distributions are not, and differ between
// standard libraries, so one seed would make another track on another
// platform.
class Random
{
public:
    explicit Random(std::uint64_t seed);
    std::uint64_t next();
    // Uniform in [0, 1), 53 bits.
    double uniform();
    // Uniform in [-1, 1).
    double bipolar() { return 2.0 * uniform() - 1.0; }
    // Standard normal (Box and Muller).
    double normal();

private:
    std::uint64_t m_state[4];
    bool m_haveSpare = false;
    double m_spare = 0.0;
};

// One stretch of the tempo programme: `beats` beat intervals whose tempo
// goes in a straight line from startBpm (the first interval) towards
// endBpm (reached by the first interval after the stretch). The last
// stretch's endBpm holds past its end.
struct TempoSegment
{
    int beats = 0;
    double startBpm = 120.0;
    double endBpm = 120.0;
};

struct TempoProgramme
{
    std::vector<TempoSegment> segments;
    // A slow drift on top: driftBpm * sin(2 pi k / driftPeriodBeats) on
    // interval k. 0 for none.
    double driftBpm = 0.0;
    double driftPeriodBeats = 0.0;
};

// The tempo from beat k to beat k + 1.
double bpmOfInterval(const TempoProgramme &tempo, int k);

enum class DrumPattern {
    // Kick on every beat, snare or clap on 2 and 4, hats on the eighths.
    FourOnTheFloor,
    // Kick on 1 and 3 and the and of 3, snare on 2 and 4.
    Rock,
    // A broken beat: kick on 1, the and of 2 and the and of 3, snare on 2
    // and 4 with ghost notes, hats on the sixteenths.
    Breakbeat,
    // Drum and bass played half time: kick on 1 and the and of 3, snare
    // on 3 only, hats on the eighths. The tempo is twice what the snare
    // suggests.
    HalfTime,
};

const char *drumPatternName(DrumPattern pattern);

struct Section
{
    std::string name;
    int bars = 0;
    bool kick = false;
    bool snare = false;
    bool hats = false;
    bool bass = false;
    bool pad = false;
    // A snare roll over the section's last beat.
    bool fill = false;
};

struct SynthTrackSpec
{
    std::string name;
    std::string description;
    std::uint64_t seed = 1;
    int sampleRate = 44100;
    // Where beat 0 is; the track is silent before it.
    double firstBeatMs = 0.0;
    TempoProgramme tempo;
    DrumPattern pattern = DrumPattern::FourOnTheFloor;
    // A clap on the backbeat instead of a snare.
    bool clap = false;
    // Where the off-beat eighth sits in its beat (0.5 straight). With
    // swingSixteenths, where each off sixteenth sits in its eighth.
    double swing = 0.5;
    bool swingSixteenths = false;
    // The standard deviation of every drum hit around its place on the
    // true grid, in ms (cut off at three of them). 0 for a machine.
    double jitterMs = 0.0;
    // The key: its tonic as a MIDI note in the bass octave, and its mode.
    int keyRootMidi = 45;  // A2
    bool minor = true;
    std::string keyName = "A minor";
    std::vector<Section> sections;
    // Ringing out after the last bar.
    double tailMs = 2000.0;
};

enum class Instrument { Kick, Snare, Clap, Hat, OpenHat, Bass, Pad };

const char *instrumentName(Instrument instrument);

// One note or drum hit as it was played: its time (with the jitter in
// it), and the beat it sits on, or -1 off the beat.
struct SynthHit
{
    double timeMs = 0.0;
    Instrument instrument = Instrument::Kick;
    int beatIndex = -1;
};

struct SynthTrack
{
    int sampleRate = 0;
    // Mono, -1 to 1.
    std::vector<float> samples;
    // The true grid: every beat of every bar, bar places 1 to 4.
    std::vector<domain::Beat> beats;
    std::vector<SynthHit> hits;
};

int totalBars(const SynthTrackSpec &spec);

// The true grid alone, without rendering anything.
std::vector<domain::Beat> synthGrid(const SynthTrackSpec &spec);

SynthTrack renderSynthTrack(const SynthTrackSpec &spec);

std::vector<std::string> synthPresetNames();
std::optional<SynthTrackSpec> synthPreset(const std::string &name);

// 16-bit PCM, one channel. False with *error when the file cannot be
// written.
bool writeWav16(const std::string &path, const std::vector<float> &samples, int sampleRate, std::string *error);

// One line per beat: index, time in ms, place in the bar, and the tempo
// to the next beat, tab-separated under a header line.
bool writeGridTsv(const std::string &path, const SynthTrackSpec &spec, const std::vector<domain::Beat> &beats,
                  std::string *error);

}  // namespace seabass::synth
