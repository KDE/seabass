// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The library's own test, with nothing but the library and audio made
// here: the interface (the one call gives what the parts give, pieces
// give what the whole gives, what cannot be analyzed says so), and the
// results on that audio against tests/results.txt, which is what ties a
// result to its revision (version.hpp): a result that has changed under
// a revision that has not fails here.
//
//   musicanalyze_test <results.txt>           check
//   musicanalyze_test --write <results.txt>   write the file again, which
//                                             is refused for a part whose
//                                             results changed and whose
//                                             revision did not
//
// (Where it is the audio made here that was changed and not the library,
// remove the file and write it anew.)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "musicanalyze/analysis.hpp"
#include "musicanalyze/version.hpp"

namespace
{

using namespace musicanalyze;

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

// ---------------------------------------------------------------- audio

// Noise that is the same on every machine: the standard library's
// distributions are not.
class Noise
{
public:
    explicit Noise(std::uint32_t seed) : m_state(seed) {}
    double next()
    {
        m_state = m_state * 1664525u + 1013904223u;
        return static_cast<double>(static_cast<std::int32_t>(m_state)) / 2147483648.0;
    }

private:
    std::uint32_t m_state;
};

struct Audio
{
    std::vector<float> samples;
    int rate = 0;
};

Audio silence(int rate, double seconds)
{
    return Audio{std::vector<float>(static_cast<std::size_t>(seconds * rate), 0.0f), rate};
}

// A kick drum: a tone falling from 140 to 50 Hz, gone in a fifth of a
// second.
void addKick(Audio &audio, double atSeconds, double gain)
{
    const auto start = static_cast<std::size_t>(atSeconds * audio.rate);
    const auto length = static_cast<std::size_t>(0.2 * audio.rate);
    double phase = 0.0;
    for (std::size_t i = 0; i < length && start + i < audio.samples.size(); ++i) {
        const double t = static_cast<double>(i) / audio.rate;
        phase += 2.0 * std::numbers::pi * (50.0 + 90.0 * std::exp(-t / 0.03)) / audio.rate;
        audio.samples[start + i] += static_cast<float>(gain * std::sin(phase) * std::exp(-t / 0.06));
    }
}

// A burst of noise: bright (a hat) or with a body at 190 Hz (a snare).
void addBurst(Audio &audio, double atSeconds, double gain, double seconds, bool body, Noise &noise)
{
    const auto start = static_cast<std::size_t>(atSeconds * audio.rate);
    const auto length = static_cast<std::size_t>(seconds * audio.rate);
    double before = 0.0;
    for (std::size_t i = 0; i < length && start + i < audio.samples.size(); ++i) {
        const double t = static_cast<double>(i) / audio.rate;
        const double now = noise.next();
        double value = body ? 0.6 * now + 0.6 * std::sin(2.0 * std::numbers::pi * 190.0 * t) : (now - before) / 2.0;
        before = now;
        value *= std::exp(-t / (seconds / 4.0));
        audio.samples[start + i] += static_cast<float>(gain * value);
    }
}

// A kick on every beat and a hat between the beats.
Audio house(int rate, double bpm, double seconds, double firstBeat)
{
    Audio audio = silence(rate, seconds);
    Noise noise(1);
    const double beat = 60.0 / bpm;
    for (double at = firstBeat; at < seconds - 0.5; at += beat) {
        addKick(audio, at, 0.7);
        addBurst(audio, at + beat / 2.0, 0.15, 0.05, false, noise);
    }
    return audio;
}

// A click: a millisecond of a tone, swelling and fading. At 8 kHz it is
// above the bands the onset function is made of (the highest ends at
// 5 kHz and lets a fortieth of it through), so that it has next to no
// say in where the beats are looked for, and at the top of the band a
// grid is set on its hits by.
void addClick(Audio &audio, double atSeconds, double gain, double hz = 8000.0)
{
    const auto start = static_cast<std::size_t>(atSeconds * audio.rate);
    const auto length = static_cast<std::size_t>(0.001 * audio.rate);
    for (std::size_t i = 0; i < length && start + i < audio.samples.size(); ++i) {
        const double t = static_cast<double>(i) / audio.rate;
        const double swell = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(length));
        audio.samples[start + i] += static_cast<float>(gain * swell * std::sin(2.0 * std::numbers::pi * hz * t));
    }
}

// house() with a click on every kick, `clickAfterMs` after the kick
// begins; with `scatterMs`, each click up to that much earlier or later
// than that, differently at every beat. The clicks stop at
// `clicksUntil` seconds, and are tones at `hz`.
Audio clicking(int rate, double bpm, double seconds, double firstBeat, double clickAfterMs, double scatterMs,
               double clicksUntil = 1e9, double hz = 8000.0)
{
    Audio audio = house(rate, bpm, seconds, firstBeat);
    Noise noise(4);
    const double beat = 60.0 / bpm;
    for (double at = firstBeat; at < seconds - 0.5 && at < clicksUntil; at += beat) {
        addClick(audio, at + (clickAfterMs + scatterMs * noise.next()) / 1000.0, 0.25, hz);
    }
    return audio;
}

// A kick and a snare taking turns, a hat on every beat between: the
// rhythm that can be counted at the tempo or at its half.
Audio kickAndSnare(int rate, double bpm, double seconds, double firstBeat)
{
    Audio audio = silence(rate, seconds);
    Noise noise(2);
    const double beat = 60.0 / bpm;
    int count = 0;
    for (double at = firstBeat; at < seconds - 0.5; at += beat, ++count) {
        if (count % 2 == 0) {
            addKick(audio, at, 0.7);
        } else {
            addBurst(audio, at, 0.45, 0.12, true, noise);
        }
        addBurst(audio, at + beat / 2.0, 0.1, 0.04, false, noise);
    }
    return audio;
}

// A kick on every beat of a tempo that climbs evenly from one to the
// other: what a straight grid cannot follow. With `clickAfterMs`, a
// click that long after every kick begins.
Audio climbing(int rate, double fromBpm, double toBpm, double seconds, double firstBeat, std::optional<double> clickAfterMs = std::nullopt)
{
    Audio audio = silence(rate, seconds);
    Noise noise(3);
    for (double at = firstBeat; at < seconds - 0.5;) {
        const double bpm = fromBpm + (toBpm - fromBpm) * at / seconds;
        addKick(audio, at, 0.7);
        if (clickAfterMs) {
            addClick(audio, at + *clickAfterMs / 1000.0, 0.25);
        }
        addBurst(audio, at + 30.0 / bpm, 0.15, 0.05, false, noise);
        at += 60.0 / bpm;
    }
    return audio;
}

// A passage as a band plays it: a soft kick on every beat of a tempo that
// swings by 2 percent either way, every kick up to 12 ms early or late,
// a hat between them up to 12 ms off its kick, and under louder hits
// (two a second) that keep no time at all. `beats` gets the moments of
// the kicks, in ms.
Audio looselyPlayed(int rate, double seconds, std::vector<double> &beats)
{
    Audio audio = silence(rate, seconds);
    Noise early(7);
    Noise sound(8);
    for (double at = 0.4; at < seconds - 0.6;) {
        const double bpm = 104.0 * (1.0 + 0.02 * std::sin(2.0 * std::numbers::pi * at / 23.0));
        const double hit = at + 0.012 * early.next();
        beats.push_back(hit * 1000.0);
        addKick(audio, hit, 0.35);
        addBurst(audio, hit + 30.0 / bpm + 0.012 * early.next(), 0.175, 0.05, false, sound);
        at += 60.0 / bpm;
    }
    Noise where(9);
    for (int i = 0; i < static_cast<int>(2.0 * seconds); ++i) {
        addBurst(audio, 0.5 * (where.next() + 1.0) * (seconds - 1.0), 0.6, 0.12, true, sound);
    }
    return audio;
}

// Four chords, two seconds each, round and round: each note a tone with
// two overtones, the chord's root an octave down under it. The notes are
// MIDI numbers.
Audio chords(int rate, const std::vector<std::vector<int>> &progression, double seconds)
{
    Audio audio = silence(rate, seconds);
    const double chordSeconds = 2.0;
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const std::vector<int> &chord = progression[static_cast<std::size_t>(t / chordSeconds) % progression.size()];
        const double into = std::fmod(t, chordSeconds);
        const double envelope = std::min(1.0, into / 0.02) * std::exp(-into / 1.5);
        double value = 0.0;
        for (std::size_t n = 0; n < chord.size(); ++n) {
            const double f = 440.0 * std::pow(2.0, (chord[n] - 69) / 12.0);
            value += std::sin(2.0 * std::numbers::pi * f * t) + 0.4 * std::sin(4.0 * std::numbers::pi * f * t)
                     + 0.2 * std::sin(6.0 * std::numbers::pi * f * t);
            if (n == 0) {
                value += std::sin(std::numbers::pi * f * t);
            }
        }
        audio.samples[i] = static_cast<float>(0.12 * envelope * value);
    }
    return audio;
}

// -------------------------------------------------------------- results

// A beat may be this far from where the file has it, in ms. Four builds
// (GCC with and without optimization, Clang, Clang with fused
// multiply-add as on ARM) put every beat of the file on the same
// thousandth of a millisecond, and wrote the same digits for every other
// number in it; the room here and in room() is a few times the last
// digit written.
constexpr double GridTimeRoomMs = 0.005;

// One line of results.txt: a part, a name, and what was found.
struct Line
{
    std::string part;
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;
};

std::string number(double value, int digits)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}

// How far a number may be from the one written down and count as the
// same result: the room another compiler's or another machine's
// arithmetic needs, and no more. 0: to the digit.
double room(const std::string &part, const std::string &field, double written)
{
    if (part == "grid") {
        if (field == "times") {
            return GridTimeRoomMs;
        }
        return field == "bpm" || field == "confidence" ? 5e-5 : 0.0;
    }
    if (part == "key") {
        return field == "margin" ? 5e-5 : 0.0;
    }
    return field == "entries" ? 0.0 : 1e-5 * std::abs(written) + 5e-9;
}

// A grid's line holds every beat, so that one beat moved shows.
Line gridLine(const std::string &name, const Audio &audio, TempoRange range = {})
{
    const GridAnalysis grid = gridFromSamples(audio.samples, audio.rate, range);
    std::string times;
    for (const Beat &beat : grid.beats) {
        times += (times.empty() ? "" : ",") + number(beat.timeMs, 3);
    }
    return Line{"grid",
                name,
                {{"beats", std::to_string(grid.beats.size())},
                 {"bpm", number(usualBpm(grid.beats), 5)},
                 {"confidence", number(grid.confidence, 5)},
                 {"follows", grid.followsTempo ? "1" : "0"},
                 {"times", times.empty() ? std::string("none") : times}}};
}

Line keyLine(const std::string &name, const Audio &audio)
{
    const KeyAnalysis key = keyFromSamples(audio.samples, audio.rate);
    return Line{"key", name, {{"key", key.key ? keyName(*key.key) : std::string("none")}, {"margin", number(key.margin, 5)}}};
}

Line waveformLine(const std::string &name, const Audio &audio)
{
    const AudioWaveform waveform = waveformFromSamples(audio.samples, audio.rate);
    double low = 0.0, mid = 0.0, high = 0.0, overall = 0.0;
    for (const WaveformEntry &entry : waveform.entries) {
        low += entry.low;
        mid += entry.mid;
        high += entry.high;
        overall += entry.overall;
    }
    const double n = std::max<double>(1.0, static_cast<double>(waveform.entries.size()));
    // The waveform in seven columns, and of those the middle one.
    const std::vector<WaveformEntry> seven = reduceWaveform(waveform.entries, 7);
    const double reduced = seven.size() == 7 ? seven[3].low + seven[3].mid + seven[3].high + seven[3].overall : 0.0;
    return Line{"waveform",
                name,
                {{"entries", std::to_string(waveform.entries.size())},
                 {"low", number(low / n, 9)},
                 {"mid", number(mid / n, 9)},
                 {"high", number(high / n, 9)},
                 {"overall", number(overall / n, 9)},
                 {"reduced", number(reduced, 9)}}};
}

std::vector<Line> resultsNow()
{
    const Audio house126 = house(44100, 126.0, 60.0, 0.25);
    const Audio house126at48 = house(48000, 126.0, 60.0, 0.25);
    const Audio turns170 = kickAndSnare(44100, 170.0, 60.0, 0.4);
    const Audio climb = climbing(44100, 118.0, 128.0, 90.0, 0.3);
    std::vector<double> played;
    const Audio loose = looselyPlayed(44100, 100.0, played);
    // A minor, D minor, E, A minor; and E flat, A flat, B flat, E flat.
    const Audio aMinor = chords(44100, {{57, 60, 64}, {50, 53, 57}, {52, 56, 59}, {57, 60, 64}}, 40.0);
    const Audio eFlat = chords(44100, {{51, 55, 58}, {56, 60, 63}, {58, 62, 65}, {51, 55, 58}}, 40.0);
    return {gridLine("house-126", house126),
            gridLine("house-126-at-48-khz", house126at48),
            gridLine("kick-and-snare-170", turns170),
            gridLine("kick-and-snare-170-range-60-110", turns170, TempoRange{60.0, 110.0}),
            gridLine("climbing-118-to-128", climb),
            gridLine("loosely-played", loose),
            keyLine("chords-a-minor", aMinor),
            keyLine("chords-e-flat", eFlat),
            waveformLine("house-126", house126),
            waveformLine("chords-a-minor", aMinor)};
}

int revisionOf(const std::string &part)
{
    return part == "grid" ? GridRevision : part == "key" ? KeyRevision : WaveformRevision;
}

const char *revisionName(const std::string &part)
{
    return part == "grid" ? "GridRevision" : part == "key" ? "KeyRevision" : "WaveformRevision";
}

struct Written
{
    bool there = false;
    std::map<std::string, int> revisions;
    // "part name" to its fields.
    std::map<std::string, std::map<std::string, std::string>> lines;
};

Written readResults(const std::string &path)
{
    Written written;
    std::ifstream in(path);
    written.there = in.good();
    std::string text;
    while (std::getline(in, text)) {
        if (text.empty() || text[0] == '#') {
            continue;
        }
        std::istringstream words(text);
        std::string part, name, field;
        words >> part >> name;
        if (part == "revision") {
            int revision = 0;
            words >> revision;
            written.revisions[name] = revision;
            continue;
        }
        while (words >> field) {
            const std::size_t equals = field.find('=');
            if (equals != std::string::npos) {
                written.lines[part + " " + name][field.substr(0, equals)] = field.substr(equals + 1);
            }
        }
    }
    return written;
}

std::vector<double> numbers(const std::string &text)
{
    std::vector<double> found;
    std::istringstream in(text);
    std::string one;
    while (std::getline(in, one, ',')) {
        char *end = nullptr;
        const double value = std::strtod(one.c_str(), &end);
        if (end != one.c_str()) {
            found.push_back(value);
        }
    }
    return found;
}

// What of `now` is not what is written, one sentence each; empty when
// the part's results are the same.
std::vector<std::string> changes(const std::string &part, const std::vector<Line> &now, const Written &written)
{
    std::vector<std::string> found;
    std::size_t lines = 0;
    for (const Line &line : now) {
        if (line.part != part) {
            continue;
        }
        ++lines;
        const auto at = written.lines.find(line.part + " " + line.name);
        if (at == written.lines.end()) {
            found.push_back(line.name + ": not in the file");
            continue;
        }
        for (const auto &[field, value] : line.fields) {
            const auto was = at->second.find(field);
            if (was == at->second.end()) {
                found.push_back(line.name + ": no " + field + " in the file");
                continue;
            }
            if (was->second == value) {
                continue;
            }
            if (field == "key") {
                found.push_back(line.name + ": key was " + was->second + " and is " + value);
                continue;
            }
            // A number, or numbers with commas between: each against its own.
            const std::vector<double> before = numbers(was->second);
            const std::vector<double> after = numbers(value);
            if (before.size() != after.size()) {
                found.push_back(line.name + ": " + field + " had " + std::to_string(before.size()) + " numbers and has "
                                + std::to_string(after.size()));
                continue;
            }
            std::size_t apart = 0;
            std::size_t worst = 0;
            for (std::size_t i = 0; i < before.size(); ++i) {
                if (std::abs(after[i] - before[i]) > room(part, field, before[i])) {
                    ++apart;
                    worst = std::abs(after[i] - before[i]) > std::abs(after[worst] - before[worst]) ? i : worst;
                }
            }
            if (apart > 0) {
                found.push_back(line.name + ": " + field + " was " + number(before[worst], 6) + " and is " + number(after[worst], 6)
                                + (before.size() > 1 ? " at number " + std::to_string(worst + 1) + ", " + std::to_string(apart) + " of "
                                                           + std::to_string(before.size()) + " changed"
                                                     : ""));
            }
        }
    }
    std::size_t writtenLines = 0;
    for (const auto &entry : written.lines) {
        writtenLines += entry.first.rfind(part + " ", 0) == 0 ? 1 : 0;
    }
    if (writtenLines != lines) {
        found.push_back("the file has " + std::to_string(writtenLines) + " lines for it, the test " + std::to_string(lines));
    }
    return found;
}

const std::vector<std::string> Parts{"grid", "key", "waveform"};

void checkResults(const std::string &path)
{
    const Written written = readResults(path);
    check(written.there, "no results file at " + path);
    if (!written.there) {
        return;
    }
    const std::vector<Line> now = resultsNow();
    for (const std::string &part : Parts) {
        const auto revision = written.revisions.find(part);
        const int writtenWith = revision == written.revisions.end() ? 0 : revision->second;
        const std::vector<std::string> changed = changes(part, now, written);
        for (const std::string &change : changed) {
            std::cerr << "  " << part << " " << change << "\n";
        }
        if (writtenWith != revisionOf(part)) {
            check(false, "results.txt was written with " + std::string(revisionName(part)) + " " + std::to_string(writtenWith)
                             + " and the library is at " + std::to_string(revisionOf(part))
                             + ": write the file again (musicanalyze_test --write)");
        } else {
            check(changed.empty(), "the " + part + "'s results have changed and " + revisionName(part) + " is still "
                                       + std::to_string(revisionOf(part))
                                       + ": raise it in version.hpp, then write the file again (musicanalyze_test --write)");
        }
    }
}

int writeResults(const std::string &path)
{
    const Written written = readResults(path);
    const std::vector<Line> now = resultsNow();
    if (written.there) {
        for (const std::string &part : Parts) {
            const auto revision = written.revisions.find(part);
            if (revision != written.revisions.end() && revision->second == revisionOf(part) && !changes(part, now, written).empty()) {
                for (const std::string &change : changes(part, now, written)) {
                    std::cerr << "  " << part << " " << change << "\n";
                }
                std::cerr << "not written: the " << part << "'s results have changed and " << revisionName(part) << " is still "
                          << revisionOf(part) << ". Raise it in version.hpp first.\n";
                return 1;
            }
        }
    }
    std::ofstream out(path);
    out << "# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>\n"
        << "# SPDX-License-Identifier: CC0-1.0\n"
        << "#\n"
        << "# What each part of the library makes of the audio its test generates, under the\n"
        << "# revision it was written with (include/musicanalyze/version.hpp). Written by\n"
        << "# musicanalyze_test --write, never by hand.\n";
    for (const std::string &part : Parts) {
        out << "revision " << part << " " << revisionOf(part) << "\n";
    }
    for (const Line &line : now) {
        out << line.part << " " << line.name;
        for (const auto &[field, value] : line.fields) {
            out << " " << field << "=" << value;
        }
        out << "\n";
    }
    std::cout << "written: " << path << "\n";
    return out.good() ? 0 : 1;
}

// ------------------------------------------------------------ interface

void theOneCallIsItsParts()
{
    const Audio audio = house(44100, 126.0, 30.0, 0.25);
    const Analysis all = analyze(audio.samples, audio.rate);
    const GridAnalysis grid = gridFromSamples(audio.samples, audio.rate);
    check(all.grid.error.empty() && all.grid.beats.size() > 50, "the one call finds the beats of a kick on every beat");
    bool sameBeats = all.grid.beats.size() == grid.beats.size();
    for (std::size_t i = 0; sameBeats && i < grid.beats.size(); ++i) {
        sameBeats = all.grid.beats[i].timeMs == grid.beats[i].timeMs && all.grid.beats[i].beatInBar == grid.beats[i].beatInBar;
    }
    check(sameBeats, "the one call's grid is gridFromSamples()'s");
    check(std::abs(all.bpm - 126.0) < 0.05, "the one call's tempo is the track's, 126: " + number(all.bpm, 3));
    const KeyAnalysis key = keyFromSamples(audio.samples, audio.rate);
    check(all.key.key == key.key && all.key.margin == key.margin, "the one call's key is keyFromSamples()'s");
    const AudioWaveform waveform = waveformFromSamples(audio.samples, audio.rate);
    check(all.waveform.entries.size() == waveform.entries.size() && all.waveform.sampleCount == audio.samples.size(),
          "the one call's waveform is waveformFromSamples()'s");
    check(all.revisions == revisions() && all.revisions.grid == GridRevision && all.revisions.key == KeyRevision
              && all.revisions.waveform == WaveformRevision,
          "the one call says which revisions made its result");

    const Analysis ranged = analyze(audio.samples, audio.rate, TempoRange{60.0, 110.0});
    check(std::abs(ranged.bpm - 63.0) < 0.05, "a range that has no place for 126 gives its half: " + number(ranged.bpm, 3));
}

void whatCannotBeAnalyzedSaysSo()
{
    const std::vector<float> some(44100, 0.1f);
    const auto blamesTheRate = [](const std::string &error) { return error.find("sample rate") != std::string::npos; };
    check(gridFromSamples({}, 44100).error == "no samples", "no samples: the grid says so, and does not blame the sample rate");
    check(blamesTheRate(gridFromSamples(some, 0).error), "no sample rate: the grid says so");
    check(blamesTheRate(gridFromSamples(some, -44100).error), "a sample rate under 0: the grid says so");
    check(blamesTheRate(gridFromSamples({}, 0).error), "no sample rate and no samples: the grid names the rate");
    check(blamesTheRate(gridFromSamples(some, 4000).error), "4 kHz: the grid says so");
    check(blamesTheRate(gridFromSamples(some, GridLowestSampleRateHz - 1).error), "one under the lowest rate: the grid says so");
    check(gridFromSamples(std::vector<float>(8000 * 20, 0.0f), GridLowestSampleRateHz).error.empty(), "8 kHz: a grid is looked for");
    check(keyFromSamples({}, 44100).error == "no samples", "no samples: the key says so, and does not blame the sample rate");
    check(blamesTheRate(keyFromSamples(some, 0).error), "no sample rate: the key says so");
    check(blamesTheRate(keyFromSamples(some, 4000).error), "4 kHz: the key says so");
    check(blamesTheRate(keyFromSamples(some, KeyLowestSampleRateHz - 1).error), "one under the lowest rate: the key says so");
    check(keyFromSamples(some, KeyLowestSampleRateHz).error.empty(), "8 kHz: a key is looked for");
    check(waveformFromSamples(some, 4000).entries.empty(), "4 kHz: no waveform");
    check(waveformFromSamples(some, WaveformLowestSampleRateHz - 1).entries.empty() && !waveformFromSamples(some, WaveformLowestSampleRateHz).entries.empty(),
          "a waveform from the lowest rate on, and none one under it");
    // Three seconds of a kick on every beat: too short to repeat itself,
    // which is no beats and nobody's error.
    const Audio shortTrack = house(44100, 126.0, 3.0, 0.25);
    const GridAnalysis tooShort = gridFromSamples(shortTrack.samples, shortTrack.rate);
    check(tooShort.error.empty() && tooShort.beats.empty(), "three seconds of music: no beats, and no error");
    const Analysis nothing = analyze({}, 44100);
    check(!nothing.grid.error.empty() && nothing.bpm == 0.0 && !nothing.key.key && nothing.waveform.entries.empty(),
          "the one call on no samples: an error, and nothing made up");
    const Analysis quiet = analyze(std::vector<float>(44100 * 20, 0.0f), 44100);
    check(quiet.grid.error.empty() && quiet.grid.beats.empty() && !quiet.key.key, "silence: no beats, no key, and no error");
}

void theRangeIsMadeValid()
{
    const TempoRange standard = usableTempoRange({});
    check(standard.lowBpm == 70.0 && standard.highBpm == 180.0, "the default range is 70 to 180");
    const TempoRange widened = usableTempoRange({100.0, 110.0});
    check(widened.lowBpm == 100.0 && widened.highBpm == 200.0, "a range under an octave is widened at the top");
    const TempoRange clamped = usableTempoRange({5.0, 1000.0});
    check(clamped.lowBpm == 40.0 && clamped.highBpm == 240.0, "a range is kept between 40 and 240");
    const TempoRange none = usableTempoRange({-1.0, 0.0});
    check(none.lowBpm == 70.0 && none.highBpm == 180.0, "what is no range is the default one");
}

void piecesGiveTheWhole()
{
    const Audio audio = house(44100, 126.0, 12.0, 0.25);
    const AudioWaveform whole = waveformFromSamples(audio.samples, audio.rate);
    WaveformMeter meter(audio.rate);
    // Pieces of uneven length, one of them empty.
    std::size_t at = 0;
    for (std::size_t length = 1; at < audio.samples.size(); length = length * 7 % 9973 + 1) {
        const std::size_t take = std::min(length, audio.samples.size() - at);
        meter.feed(std::span<const float>(audio.samples.data() + at, take));
        meter.feed({});
        at += take;
    }
    const AudioWaveform pieces = meter.finish();
    bool same = pieces.entries.size() == whole.entries.size() && pieces.sampleCount == whole.sampleCount
                && pieces.sampleRateHz == whole.sampleRateHz && !whole.entries.empty();
    for (std::size_t i = 0; same && i < whole.entries.size(); ++i) {
        same = pieces.entries[i].low == whole.entries[i].low && pieces.entries[i].mid == whole.entries[i].mid
               && pieces.entries[i].high == whole.entries[i].high && pieces.entries[i].overall == whole.entries[i].overall;
    }
    check(same, "a waveform fed in pieces is the waveform of the whole, to the digit");
    check(whole.entries.size() == waveformEntryCount(audio.samples.size(), audio.rate), "a waveform has the entries its length makes");
    WaveformMeter slow(4000);
    slow.feed(audio.samples);
    check(slow.finish().entries.empty(), "a meter at 4 kHz measures nothing");
    check(reduceWaveform(whole.entries, 400).size() == 400 && reduceWaveform({}, 400).empty(), "a waveform is reduced to the columns asked for");
}

// `audio` handed over in pieces of uneven length, one of them empty.
template <typename Fed>
void inPieces(const Audio &audio, Fed &fed)
{
    std::size_t at = 0;
    for (std::size_t length = 1; at < audio.samples.size(); length = length * 7 % 99991 + 1) {
        const std::size_t take = std::min(length, audio.samples.size() - at);
        fed.feed(std::span<const float>(audio.samples.data() + at, take));
        fed.feed({});
        at += take;
    }
}

bool sameGrid(const GridAnalysis &a, const GridAnalysis &b)
{
    bool same = a.beats.size() == b.beats.size() && a.followsTempo == b.followsTempo && a.confidence == b.confidence && a.error == b.error;
    for (std::size_t i = 0; same && i < a.beats.size(); ++i) {
        same = a.beats[i].timeMs == b.beats[i].timeMs && a.beats[i].beatInBar == b.beats[i].beatInBar;
    }
    return same;
}

void audioInPiecesIsTheAudioWhole()
{
    // Long enough for the filters that run backwards to do so in several
    // blocks, at a rate that is halved first and at one that is not.
    // And a track whose grid is set on its clicks, so that the band the
    // clicks are read in is in it.
    for (const Audio &audio :
         {climbing(44100, 118.0, 128.0, 75.0, 0.3), house(32000, 126.0, 50.0, 0.25), clicking(44100, 126.0, 50.0, 0.25, 1.0, 0.0)}) {
        const std::string at = " at " + std::to_string(audio.rate) + " Hz";
        const GridAnalysis whole = gridFromSamples(audio.samples, audio.rate);
        BeatGridAnalyzer grid(audio.rate);
        inPieces(audio, grid);
        const GridAnalysis pieces = grid.finish();
        check(whole.beats.size() > 80 && sameGrid(whole, pieces), "a grid fed in pieces is the grid of the whole, to the digit" + at);
        check(!grid.finish().error.empty(), "a grid analyzer that has given its grid is spent" + at);
    }
    const Audio tune = chords(44100, {{57, 60, 64}, {50, 53, 57}, {52, 56, 59}, {57, 60, 64}}, 30.0);
    const KeyAnalysis wholeKey = keyFromSamples(tune.samples, tune.rate);
    KeyAnalyzer key(tune.rate);
    inPieces(tune, key);
    const KeyAnalysis piecesKey = key.finish();
    check(wholeKey.key && wholeKey.key == piecesKey.key && wholeKey.margin == piecesKey.margin, "a key fed in pieces is the key of the whole, to the digit");

    const Audio track = house(44100, 126.0, 40.0, 0.25);
    const Analysis whole = analyze(track.samples, track.rate, TempoRange{88.0, 175.0});
    Analyzer analyzer(track.rate, TempoRange{88.0, 175.0});
    inPieces(track, analyzer);
    const Analysis pieces = analyzer.finish();
    bool sameWaveform = whole.waveform.entries.size() == pieces.waveform.entries.size() && whole.waveform.sampleCount == pieces.waveform.sampleCount;
    for (std::size_t i = 0; sameWaveform && i < whole.waveform.entries.size(); ++i) {
        sameWaveform = whole.waveform.entries[i].low == pieces.waveform.entries[i].low && whole.waveform.entries[i].overall == pieces.waveform.entries[i].overall;
    }
    check(sameGrid(whole.grid, pieces.grid) && whole.bpm == pieces.bpm && whole.key.key == pieces.key.key && whole.key.margin == pieces.key.margin
              && sameWaveform && pieces.revisions == revisions(),
          "an analysis fed in pieces is the analysis of the whole, to the digit");

    BeatGridAnalyzer none(44100);
    check(!none.finish().error.empty(), "a grid analyzer fed nothing says so");
    BeatGridAnalyzer noRate(0);
    noRate.feed(track.samples);
    check(noRate.finish().error.find("sample rate") != std::string::npos, "a grid analyzer without a sample rate says so");
    KeyAnalyzer noRateKey(0);
    noRateKey.feed(track.samples);
    check(noRateKey.finish().error.find("sample rate") != std::string::npos, "a key analyzer without a sample rate says so");
    // Fed again after it has given its result, each is as one fed
    // nothing: an error or an empty result, and never the old one again.
    BeatGridAnalyzer spentGrid(track.rate);
    spentGrid.feed(track.samples);
    check(spentGrid.finish().beats.size() > 50, "a grid analyzer at 44.1 kHz finds the beats");
    spentGrid.feed(track.samples);
    const GridAnalysis afterSpent = spentGrid.finish();
    const GridAnalysis andAgain = spentGrid.finish();
    check(!afterSpent.error.empty() && afterSpent.beats.empty() && !andAgain.error.empty() && andAgain.beats.empty(),
          "a grid analyzer that has given its grid is spent, fed again or not");
    WaveformMeter spentMeter(track.rate);
    spentMeter.feed(track.samples);
    check(!spentMeter.finish().entries.empty(), "a meter at 44.1 kHz measures a track");
    spentMeter.feed(track.samples);
    check(spentMeter.finish().entries.empty(), "a meter that has given its waveform is spent");
    analyzer.feed(track.samples);
    const Analysis again = analyzer.finish();
    check(!again.grid.error.empty() && again.grid.beats.empty() && again.bpm == 0.0 && !again.key.key && !again.key.error.empty()
              && again.waveform.entries.empty(),
          "an analyzer that has given its analysis is spent: errors, and nothing made up");
    KeyAnalyzer slowKey(4000);
    slowKey.feed(track.samples);
    check(slowKey.finish().error.find("sample rate") != std::string::npos, "a key analyzer at 4 kHz says it is the sample rate");
    KeyAnalyzer spent(44100);
    spent.feed(track.samples);
    check(spent.finish().error.empty(), "a key analyzer at 44.1 kHz reads a track");
    spent.feed(track.samples);
    check(spent.finish().error == "no samples", "a key analyzer that has given its key is spent, and does not blame the sample rate");
    Analyzer nothing(44100);
    const Analysis empty = nothing.finish();
    check(!empty.grid.error.empty() && !empty.key.error.empty() && empty.waveform.entries.empty() && empty.bpm == 0.0,
          "an analyzer fed nothing: errors, and nothing made up");
}

// Where a grid sits against the kicks of house() or clicking(): the
// middle, over its beats, of how many ms a beat is after the beginning
// of the kick nearest to it.
double afterTheKicks(const GridAnalysis &grid, double bpm, double firstBeat)
{
    const double beat = 60000.0 / bpm;
    std::vector<double> after;
    for (const Beat &at : grid.beats) {
        const double kick = firstBeat * 1000.0 + std::round((at.timeMs - firstBeat * 1000.0) / beat) * beat;
        after.push_back(at.timeMs - kick);
    }
    if (after.empty()) {
        return 1e9;
    }
    std::sort(after.begin(), after.end());
    return after[after.size() / 2];
}

// One track made at every sample rate a file comes in, and at the lowest
// one read: the same grid, wherever the bands' filters and the halving
// of the rate fall. And the same of a track whose kicks carry a click,
// at 3 kHz so that the lowest rate holds it: at every rate the grid is
// set on the click. From 22 kHz of sample rate, which holds 10 kHz,
// there is a far louder tick up there as well, 2 ms before every kick:
// that is above the band a grid is set on its hits by, and the grid
// stays on the click.
void everySampleRateGivesTheGrid()
{
    const Audio reference = house(44100, 126.0, 40.0, 0.25);
    const GridAnalysis expected = gridFromSamples(reference.samples, reference.rate);
    check(expected.beats.size() > 80, "the track at 44.1 kHz has its beats");
    for (const int rate : {GridLowestSampleRateHz, 11025, 22050, 32000, 48000, 96000, 192000}) {
        const std::string at = " at " + std::to_string(rate) + " Hz";
        const Audio audio = house(rate, 126.0, 40.0, 0.25);
        const Analysis analysis = analyze(audio.samples, audio.rate);
        check(analysis.grid.error.empty() && !analysis.grid.followsTempo && std::abs(analysis.bpm - 126.0) < 0.01,
              "the tempo is 126" + at + ": " + number(analysis.bpm, 4));
        double farthest = 0.0;
        const bool sameCount = analysis.grid.beats.size() == expected.beats.size();
        for (std::size_t i = 0; sameCount && i < expected.beats.size(); ++i) {
            farthest = std::max(farthest, std::abs(analysis.grid.beats[i].timeMs - expected.beats[i].timeMs));
        }
        check(sameCount && farthest < 0.3,
              "the beats are where 44.1 kHz has them" + at + ": " + std::to_string(analysis.grid.beats.size()) + " beats for "
                  + std::to_string(expected.beats.size()) + ", farthest by " + number(farthest, 3) + " ms");
        check(analysis.waveform.entries.size() == 6000 && analysis.key.error.empty(), "a waveform of 40 s and a key looked for" + at);
    }

    const auto withClicks = [](int rate) {
        Audio audio = clicking(rate, 126.0, 40.0, 0.25, 1.0, 0.0, 1e9, 3000.0);
        if (rate >= 22050) {
            for (double at = 0.25; at < 40.0 - 0.5; at += 60.0 / 126.0) {
                addClick(audio, at - 0.002, 1.0, 10000.0);
            }
        }
        return gridFromSamples(audio.samples, audio.rate);
    };
    const GridAnalysis expectedOnClicks = withClicks(44100);
    const double onTheClick = afterTheKicks(expectedOnClicks, 126.0, 0.25);
    check(std::abs(onTheClick - (1.0 - HitBeforeTheClickMs)) < 0.15,
          "a grid is set on clicks at 3 kHz as on any, at 44.1 kHz: " + number(onTheClick, 3) + " ms after the kicks");
    for (const int rate : {GridLowestSampleRateHz, 11025, 22050, 32000, 48000, 96000, 192000}) {
        const GridAnalysis grid = withClicks(rate);
        double farthest = 0.0;
        const bool sameCount = grid.beats.size() == expectedOnClicks.beats.size() && grid.beats.size() > 80;
        for (std::size_t i = 0; sameCount && i < grid.beats.size(); ++i) {
            farthest = std::max(farthest, std::abs(grid.beats[i].timeMs - expectedOnClicks.beats[i].timeMs));
        }
        check(sameCount && farthest < 0.3,
              "the beats of a track with clicks are where 44.1 kHz has them at " + std::to_string(rate) + " Hz: farthest by "
                  + number(farthest, 3) + " ms");
    }
}

// A straight grid is set on the clicks of its hits: it goes where they
// go, a millisecond for a millisecond, as long as the beats agree on
// where the click is, and by 5 ms at most.
void aStraightGridIsSetOnItsClicks()
{
    const double bpm = 126.0;
    const double first = 0.25;
    const auto placed = [&](double clickAfterMs, double scatterMs) {
        const Audio audio = clicking(44100, bpm, 40.0, first, clickAfterMs, scatterMs);
        const GridAnalysis grid = gridFromSamples(audio.samples, audio.rate);
        check(grid.error.empty() && !grid.followsTempo && grid.beats.size() > 80,
              "a kick with a click " + number(clickAfterMs, 0) + " ms after it begins has its straight grid");
        return afterTheKicks(grid, bpm, first);
    };
    // Clicks that agree on no place (each up to 12 ms either way) leave
    // the grid where the onsets put it: that is where a grid without
    // this would be, and what the others are measured from.
    const double byOnsets = placed(1.0, 12.0);
    const double byOnsetsToo = placed(3.0, 12.0);
    check(std::abs(byOnsets - byOnsetsToo) < 0.2,
          "clicks that agree on no place move nothing, wherever their middle is: " + number(byOnsets, 3) + " and " + number(byOnsetsToo, 3));
    check(std::abs(byOnsets + 3.4) < 0.2, "the onsets put the grid of this kick 3.4 ms before the kick begins: " + number(byOnsets, 3));
    // The moment a click is taken to be at is where its size, summed
    // over a millisecond, grows fastest: for one that swells and fades
    // within a millisecond that is where it begins. (The onsets put
    // this kick's grid some 3 ms before the kick begins, so a click a
    // millisecond either side of that beginning is within the 5 ms.)
    for (const double clickAfterMs : {-1.0, 1.0}) {
        const double at = placed(clickAfterMs, 0.0);
        check(std::abs(at - (clickAfterMs - HitBeforeTheClickMs)) < 0.15,
              "a grid is " + number(HitBeforeTheClickMs, 1) + " ms before the clicks of its kicks, " + number(clickAfterMs, 0)
                  + " ms after they begin: " + number(at, 3) + " ms after them");
    }
    // Beats without a click (over half of the track here, from 18 s on)
    // do not decide the place: it is that of the half of the beats on
    // which the band is strongest, and nearly all of those have a click.
    {
        const Audio audio = clicking(44100, bpm, 40.0, first, 1.0, 0.0, 18.0);
        const double at = afterTheKicks(gridFromSamples(audio.samples, audio.rate), bpm, first);
        check(std::abs(at - (1.0 - HitBeforeTheClickMs)) < 0.15,
              "a grid is set on its clicks though over half of the track has none: " + number(at, 3) + " ms after the kicks");
    }
    // Clicks 10 and 14 ms after the kick begins are both over 5 ms from
    // where the onsets put the grid: it goes 5 ms towards them and no
    // further. And as far the other way, for clicks before it.
    const double far = placed(10.0, 0.0);
    const double farther = placed(14.0, 0.0);
    check(std::abs(far - (byOnsets + 5.0)) < 0.2 && std::abs(farther - (byOnsets + 5.0)) < 0.2,
          "a grid goes 5 ms towards its clicks at most: from " + number(byOnsets, 3) + " to " + number(far, 3) + " and " + number(farther, 3));
    const double before = placed(-12.0, 0.0);
    const double longBefore = placed(-16.0, 0.0);
    check(std::abs(before - (byOnsets - 5.0)) < 0.2 && std::abs(longBefore - (byOnsets - 5.0)) < 0.2,
          "a grid goes 5 ms back towards its clicks at most: from " + number(byOnsets, 3) + " to " + number(before, 3) + " and " + number(longBefore, 3));

    // A grid that follows the tempo is not set on its clicks: each of
    // its beats is set on its own onset, and clicks 3 ms before the
    // kicks and 3 ms after them make one grid.
    const Audio clicksBefore = climbing(44100, 118.0, 128.0, 75.0, 0.3, -3.0);
    const Audio clicksAfter = climbing(44100, 118.0, 128.0, 75.0, 0.3, 3.0);
    const GridAnalysis followedBefore = gridFromSamples(clicksBefore.samples, clicksBefore.rate);
    const GridAnalysis followedAfter = gridFromSamples(clicksAfter.samples, clicksAfter.rate);
    const bool followedBoth = followedBefore.followsTempo && followedAfter.followsTempo && followedBefore.beats.size() > 80
                              && followedBefore.beats.size() == followedAfter.beats.size();
    double apart = 0.0;
    for (std::size_t i = 0; followedBoth && i < followedBefore.beats.size(); ++i) {
        apart = std::max(apart, std::abs(followedBefore.beats[i].timeMs - followedAfter.beats[i].timeMs));
    }
    check(followedBoth && apart < 0.5, "a grid that follows the tempo does not go with its clicks: farthest apart by " + number(apart, 3) + " ms");
}

// A grid that was moved has every beat that lies in the audio, as one
// that was not: a beat the move brings in at the start or at the end is
// there.
void aMovedGridHasEveryBeatInTheAudio()
{
    const double bpm = 126.0;
    const double beat = 60000.0 / bpm;
    // The first kick 2 ms into the track: the onsets put its beat before
    // the track begins, and clicks 4 ms after the kicks bring it in.
    const Audio early = clicking(44100, bpm, 40.0, 0.002, 4.0, 0.0);
    const GridAnalysis head = gridFromSamples(early.samples, early.rate);
    check(head.beats.size() > 80 && head.beats.front().timeMs >= 0.0 && head.beats.front().timeMs < 10.0,
          "a beat the move brings into the start of the track is there: the first beat at "
              + (head.beats.empty() ? std::string("none") : number(head.beats.front().timeMs, 3)) + " ms");
    // The track ends 2 ms before a beat as the onsets place it (some
    // 3.4 ms before a kick would begin there), and clicks 12 ms before
    // the kicks take the grid 5 ms back: that beat is in the track.
    const double first = 0.25;
    const double endMs = first * 1000.0 - 3.4 + 84.0 * beat - 2.0;
    const Audio late = clicking(44100, bpm, endMs / 1000.0, first, -12.0, 0.0);
    const GridAnalysis tail = gridFromSamples(late.samples, late.rate);
    check(tail.beats.size() > 80 && tail.beats.back().timeMs <= endMs && tail.beats.back().timeMs > endMs - 10.0,
          "a beat the move brings into the end of the track is there: the last beat at "
              + (tail.beats.empty() ? std::string("none") : number(tail.beats.back().timeMs, 3)) + " ms of " + number(endMs, 3));
}

// The tempo of a passage whose hits come back to within some 10 ms and
// not to the millisecond is heard, and its grid follows the beats: as
// many beats as were played, and most of them within 25 ms of one. (No
// 8 s of it repeat to the millisecond; a straight grid, which is what a
// track without a rhythm is given, is on such beats one time in four.)
void aTempoIsHeardThoughNoHitIsOnTheMillisecond()
{
    std::vector<double> beats;
    const Audio audio = looselyPlayed(44100, 100.0, beats);
    const GridAnalysis grid = gridFromSamples(audio.samples, audio.rate);
    std::size_t near = 0;
    for (const double beat : beats) {
        double nearest = 1e9;
        for (const Beat &ours : grid.beats) {
            nearest = std::min(nearest, std::abs(ours.timeMs - beat));
        }
        near += nearest <= 25.0 ? 1 : 0;
    }
    check(grid.error.empty() && grid.followsTempo, "a loosely played passage under hits that keep no time is followed");
    check(beats.size() > 150 && 10 * near >= 6 * beats.size(),
          "its grid is on the beats: " + std::to_string(near) + " of " + std::to_string(beats.size()) + " played beats have one of ours within 25 ms");
    check(20 * grid.beats.size() >= 19 * beats.size() && 20 * grid.beats.size() <= 21 * beats.size(),
          "it has as many beats as were played, to a twentieth: " + std::to_string(grid.beats.size()) + " for " + std::to_string(beats.size()));
}

// What keeps no time is no rhythm to a curve that hears 10 ms wide
// either: hits anywhere, so many a second, and noise. No grid of theirs
// follows anything, and none is sure of itself.
void whatKeepsNoTimeIsNoRhythm()
{
    for (const std::uint32_t seed : {9u, 21u}) {
        for (const int aSecond : {2, 5}) {
            Audio audio = silence(44100, 100.0);
            Noise sound(8);
            Noise where(seed);
            for (int i = 0; i < aSecond * 100; ++i) {
                addBurst(audio, 0.5 * (where.next() + 1.0) * 99.0, 0.6, 0.12, i % 2 == 0, sound);
            }
            const GridAnalysis grid = gridFromSamples(audio.samples, audio.rate);
            check(grid.error.empty() && !grid.followsTempo && grid.confidence == 0.0,
                  std::to_string(aSecond) + " hits a second that keep no time: no grid followed, and none sure (confidence "
                      + number(grid.confidence, 3) + ")");
        }
    }
    Audio hiss = silence(44100, 100.0);
    Noise noise(5);
    for (float &sample : hiss.samples) {
        sample = static_cast<float>(0.3 * noise.next());
    }
    const GridAnalysis grid = gridFromSamples(hiss.samples, hiss.rate);
    check(grid.error.empty() && !grid.followsTempo && grid.confidence == 0.0,
          "noise: no grid followed, and none sure (confidence " + number(grid.confidence, 3) + ")");
}

void theVersionIsTheHeaders()
{
    const std::string expected = std::to_string(MUSICANALYZE_VERSION_MAJOR) + "." + std::to_string(MUSICANALYZE_VERSION_MINOR) + "."
                                 + std::to_string(MUSICANALYZE_VERSION_PATCH);
    check(version() == expected, "version() is the header's: " + std::string(version()));
    check(revisions().grid >= 1 && revisions().key >= 1 && revisions().waveform >= 1, "every part has a revision");
}

}  // namespace

int main(int argc, char **argv)
{
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.size() == 2 && arguments[0] == "--write") {
        return writeResults(arguments[1]);
    }
    if (arguments.size() != 1 || arguments[0].rfind("--", 0) == 0) {
        std::cerr << "usage: musicanalyze_test <results.txt> | --write <results.txt>\n";
        return 2;
    }
    theVersionIsTheHeaders();
    theRangeIsMadeValid();
    whatCannotBeAnalyzedSaysSo();
    theOneCallIsItsParts();
    piecesGiveTheWhole();
    audioInPiecesIsTheAudioWhole();
    everySampleRateGivesTheGrid();
    aStraightGridIsSetOnItsClicks();
    aMovedGridHasEveryBeatInTheAudio();
    aTempoIsHeardThoughNoHitIsOnTheMillisecond();
    whatKeepsNoTimeIsNoRhythm();
    checkResults(arguments[0]);
    if (failures != 0) {
        std::cerr << failures << " failed\n";
        return 1;
    }
    std::cout << "musicanalyze " << version() << " (grid " << GridRevision << ", key " << KeyRevision << ", waveform " << WaveformRevision
              << "): all passed\n";
    return 0;
}
