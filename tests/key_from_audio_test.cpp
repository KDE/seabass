// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Seabass's key detector on audio whose key is known because it was made
// for the test: chords from one scale, moved to other keys, tuned off
// concert pitch, at another sample rate, and the generated tracks of the
// benchmark, each of which states its key. What the detector is worth on
// real music is measured by tools/analysis_benchmark against the keys
// rekordbox gives; here it is that the machinery does what it says.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include "domain/audio_analysis.hpp"
#include "domain/key_from_audio.hpp"
#include "synth_track/synth_track.hpp"

using namespace seabass;
using namespace seabass::domain;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

// A note as an instrument would sound it: a fundamental and four
// overtones, each half as strong as the one below, fading over its length.
void addNote(std::vector<float> &audio, int rate, double startS, double lengthS, double hz, double level)
{
    const auto from = static_cast<std::size_t>(startS * rate);
    const auto count = static_cast<std::size_t>(lengthS * rate);
    for (std::size_t i = 0; i < count && from + i < audio.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double fade = std::exp(-2.0 * t / lengthS) * std::min(1.0, t / 0.01);
        double value = 0.0;
        for (int overtone = 1; overtone <= 5; ++overtone) {
            value += std::sin(2.0 * std::numbers::pi * hz * overtone * t) / std::pow(2.0, overtone - 1);
        }
        audio[from + i] += static_cast<float>(level * fade * value);
    }
}

double midiHz(double midi, double cents = 0.0)
{
    return 440.0 * std::pow(2.0, (midi - 69.0 + cents / 100.0) / 12.0);
}

// A minute of four chords, a bar each, with a bass note under each: I,
// IV, V, I in major (tonic, subdominant, dominant) or i, iv, V, i in
// minor, the tonic an octave below middle C's octave plus `tonic`
// semitones.
std::vector<float> progression(int tonic, bool minor, int rate, double cents = 0.0, double seconds = 60.0)
{
    std::vector<float> audio(static_cast<std::size_t>(seconds * rate), 0.0f);
    const int third = minor ? 3 : 4;
    struct Chord
    {
        int root;
        int third;
    };
    // The dominant is major in both modes, as a minor key's is in practice.
    const std::array<Chord, 4> chords{Chord{0, third}, Chord{5, third}, Chord{7, 4}, Chord{0, third}};
    const double bar = 2.0;
    for (double at = 0.0; at + bar <= seconds; at += bar) {
        const Chord &chord = chords[static_cast<std::size_t>(std::llround(at / bar)) % chords.size()];
        const double root = 48.0 + tonic + chord.root;
        addNote(audio, rate, at, bar, midiHz(root - 12.0, cents), 0.25);
        for (const double note : {root, root + chord.third, root + 7.0}) {
            addNote(audio, rate, at, bar, midiHz(note, cents), 0.12);
            addNote(audio, rate, at + bar / 2.0, bar / 2.0, midiHz(note + 12.0, cents), 0.08);
        }
    }
    return audio;
}

std::string found(const std::vector<float> &audio, int rate)
{
    const KeyAnalysis analysis = keyFromSamples(audio, rate);
    return analysis.key ? keyName(*analysis.key) : std::string("none");
}

void namesAsRekordboxSpellsThem()
{
    const std::array<const char *, 12> major{"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    for (int tonic = 0; tonic < 12; ++tonic) {
        check(keyName(MusicalKey{tonic, false}) == major[static_cast<std::size_t>(tonic)], "the name of a major key");
        check(keyName(MusicalKey{tonic, true}) == std::string(major[static_cast<std::size_t>(tonic)]) + "m",
              "the name of a minor key");
    }
    check(keyName(MusicalKey{13, true}) == "Dbm" && keyName(MusicalKey{-1, false}) == "B", "a tonic outside 0 to 11 wraps");
}

// One held A: the picture has it in A's place in the octave it sounds in,
// and its overtones where they fall.
void thePictureOfOneNote()
{
    const int rate = 44100;
    std::vector<float> tone(static_cast<std::size_t>(rate) * 6);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / rate));
    }
    const auto picture = pitchPicture(tone, rate);
    check(picture.has_value(), "one note: a picture");
    if (picture) {
        // A3 (220 Hz) is the second octave from C2, note 9.
        const double a3 = (*picture)[1 * 12 + 9];
        check(a3 > 0.9, "one note: nearly all of the picture is that note, was " + std::to_string(a3));
        double all = 0.0;
        for (const double value : *picture) {
            all += value;
        }
        check(std::fabs(all - 1.0) < 1e-6, "one note: the picture adds up to 1");
    }
}

// A track tuned a third of a semitone sharp has its notes in the upper
// third of each semitone; whatever then lies a further third up belongs
// to the same note, not half to the next. An A a third sharp with a
// weaker tone another third above it is one note in the picture; read
// as if the track were at concert pitch it would be an A and a B flat.
void tunedBetweenTheNotes()
{
    const int rate = 44100;
    std::vector<float> tone(static_cast<std::size_t>(rate) * 6);
    const double sharp = 220.0 * std::pow(2.0, 1.0 / 36.0);
    const double sharper = 220.0 * std::pow(2.0, 2.0 / 36.0);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        tone[i] = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * sharp * t)
                                     + 0.24 * std::sin(2.0 * std::numbers::pi * sharper * t));
    }
    const auto picture = pitchPicture(tone, rate);
    check(picture.has_value(), "tuned a third sharp: a picture");
    if (picture) {
        const double a3 = (*picture)[1 * 12 + 9];
        const double bFlat3 = (*picture)[1 * 12 + 10];
        check(a3 > 0.9, "tuned a third sharp: all of it is the A, was " + std::to_string(a3));
        check(bFlat3 < 0.05, "tuned a third sharp: none of it is a B flat, was " + std::to_string(bFlat3));
    }
}

// The detector is a port of a prototype in Python whose weights were
// learned with that prototype's own picture of each track, so the port
// must see what the prototype saw. A signal made here at the prototype's
// own rate: ten segments of two seconds, three tones each, each segment
// louder than the one before (the frames must count the same all the
// same), the third tone 20 cents sharp and a weak fourth a third of a
// semitone under the first (the thirds of a semitone must be told apart).
// The 60 numbers are the prototype's own for that signal
// (builds/track-analysis-work/keyport/golden.py, not in the repository:
// it reads the prototype where it lives).
std::vector<float> goldenSignal()
{
    const int rate = 11025;
    const std::array<std::array<int, 3>, 10> notes{{{45, 52, 60}, {50, 57, 65}, {52, 59, 68}, {45, 60, 64}, {43, 55, 62},
                                                     {48, 55, 64}, {41, 53, 60}, {52, 56, 71}, {45, 57, 72}, {40, 52, 59}}};
    const std::size_t segment = static_cast<std::size_t>(rate) * 2;
    std::vector<float> audio(segment * notes.size());
    for (std::size_t s = 0; s < notes.size(); ++s) {
        const double gain = 0.05 + 0.02 * static_cast<double>(s);
        const std::array<std::array<double, 3>, 4> tones{{{static_cast<double>(notes[s][0]), 0.0, 1.0},
                                                          {static_cast<double>(notes[s][1]), 0.0, 0.8},
                                                          {static_cast<double>(notes[s][2]), 20.0, 0.6},
                                                          {static_cast<double>(notes[s][0]), -100.0 / 3.0, 0.3}}};
        for (std::size_t i = 0; i < segment; ++i) {
            const double t = static_cast<double>(i) / rate;
            double value = 0.0;
            for (const auto &tone : tones) {
                value += tone[2] * std::sin(2.0 * std::numbers::pi * midiHz(tone[0], tone[1]) * t);
            }
            audio[s * segment + i] = static_cast<float>(gain * value);
        }
    }
    return audio;
}

void thePrototypesOwnPicture()
{
    static constexpr std::array<double, 60> Prototype{
        0.000000, 0.000000, 0.000000, 0.000000, 0.029497, 0.055537, 0.000348, 0.053740, 0.000000, 0.143340, 0.000000, 0.000000,
        0.055002, 0.000357, 0.053762, 0.000000, 0.167024, 0.036135, 0.000012, 0.079890, 0.037125, 0.078752, 0.000005, 0.047404,
        0.064645, 0.000000, 0.013866, 0.000000, 0.027787, 0.014476, 0.000000, 0.000000, 0.013704, 0.000011, 0.000000, 0.013493,
        0.014083, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000,
        0.000000, 0.000000, 0.000000, 0.000000, 0.000000, 0.000001, 0.000001, 0.000001, 0.000001, 0.000001, 0.000001, 0.000001,
    };
    const std::vector<float> audio = goldenSignal();
    const auto picture = pitchPicture(audio, 11025);
    check(picture.has_value(), "the prototype's signal: a picture");
    if (picture) {
        double worst = 0.0;
        for (std::size_t i = 0; i < 60; ++i) {
            worst = std::max(worst, std::fabs((*picture)[i] - Prototype[i]));
        }
        // The prototype works in single precision; the numbers above are
        // printed to six places.
        check(worst < 2e-4, "the prototype's signal: every one of the 60 numbers is the prototype's, the furthest off by "
                                + std::to_string(worst));
    }
    check(found(audio, 11025) == "Am", "the prototype's signal: A minor, as the prototype has it, found " + found(audio, 11025));
    // At this rate no sample is left out and none filtered: a sample that
    // is no number is silence on that way through as well. One in every
    // frame: were they left in, no frame could be read and there would be
    // no key.
    std::vector<float> slipped = audio;
    for (std::size_t i = 2000; i < slipped.size(); i += 4000) {
        slipped[i] = i % 8000 == 2000 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    }
    check(found(slipped, 11025) == "Am", "the prototype's signal with a sample that is no number in every frame: found "
                                             + found(slipped, 11025));
    // What this check does not tell apart, since they move a peak by far
    // less than a third of a semitone: the parabola through the
    // logarithms from one through the heights themselves, and the
    // symmetric window from the periodic one.
}

void chordsInEveryKey()
{
    const int rate = 44100;
    for (int tonic = 0; tonic < 12; ++tonic) {
        for (const bool minor : {false, true}) {
            const std::string expected = keyName(MusicalKey{tonic, minor});
            const std::string got = found(progression(tonic, minor, rate), rate);
            check(got == expected, "chords in " + expected + ": found " + got);
        }
    }
}

// A track tuned a quarter of a semitone flat or sharp is in the same key;
// so is the same audio at 48 kHz, where the detector takes every fourth
// sample of another rate.
void tuningAndSampleRate()
{
    for (const double cents : {-25.0, 25.0}) {
        const std::string got = found(progression(2, true, 44100, cents), 44100);
        check(got == "Dm", "D minor tuned " + std::to_string(static_cast<int>(cents)) + " cents off: found " + got);
    }
    for (const int rate : {22050, 48000, 96000}) {
        const std::string got = found(progression(7, false, rate), rate);
        check(got == "G", "G major at " + std::to_string(rate) + " Hz: found " + got);
    }
}

void nothingToReadAKeyFrom()
{
    const int rate = 44100;
    const KeyAnalysis none = keyFromSamples({}, rate);
    check(!none.key && !none.error.empty(), "no samples: no key, and an error");
    const KeyAnalysis noRate = keyFromSamples(progression(0, false, rate, 0.0, 4.0), 0);
    check(!noRate.key && !noRate.error.empty(), "no sample rate: no key, and an error");
    // Under a second and a half there is no frame to read.
    const KeyAnalysis brief = keyFromSamples(std::vector<float>(static_cast<std::size_t>(rate), 0.1f), rate);
    check(!brief.key && brief.error.empty(), "one second: no key, and that is no error");
    const KeyAnalysis silence = keyFromSamples(std::vector<float>(static_cast<std::size_t>(rate) * 10, 0.0f), rate);
    check(!silence.key && silence.error.empty(), "silence: no key, and that is no error");
    // A sample that is no number is silence for that sample.
    std::vector<float> slipped = progression(9, true, rate);
    slipped[5000] = std::numeric_limits<float>::quiet_NaN();
    slipped[90000] = std::numeric_limits<float>::infinity();
    check(found(slipped, rate) == "Am", "A minor with two samples that are no number: found " + found(slipped, rate));
    // Digital silence after the music adds nothing: every frame counts
    // the same, but one with nothing in it counts for nothing.
    std::vector<float> thenSilence = progression(4, true, rate);
    thenSilence.resize(thenSilence.size() + static_cast<std::size_t>(rate) * 120, 0.0f);
    check(found(thenSilence, rate) == "Em", "E minor and then two minutes of silence: found " + found(thenSilence, rate));
    // A sample rate too low for the five octaves is said to be one.
    const KeyAnalysis slow = keyFromSamples(std::vector<float>(40000, 0.1f), 4000);
    check(!slow.key && !slow.error.empty(), "4 kHz: no key, and an error that says why");
    check(!pitchPicture(std::vector<float>(40000, 0.1f), 4000).has_value(), "4 kHz: no picture");
    const KeyAnalysis sure = keyFromSamples(progression(9, true, rate), rate);
    check(sure.margin > 0.0, "the margin of a clear key is above zero");
}

// The benchmark's generated tracks each state their key.
void generatedTracks()
{
    // The kind of miss the detector makes, kept in sight: a major key
    // taken for its relative minor, which has the same notes. The
    // drummer's track is in E major and comes out as D flat minor.
    {
        const synth::SynthTrack track = synth::renderSynthTrack(*synth::synthPreset("live-drummer-96"));
        const std::string got = found(track.samples, track.sampleRate);
        check(got == "E" || got == "Dbm", "live-drummer-96: E major or its relative minor, found " + got);
    }
    for (const char *name : {"house-124", "trance-137.98", "drift-122"}) {
        const synth::SynthTrackSpec spec = *synth::synthPreset(name);
        const synth::SynthTrack track = synth::renderSynthTrack(spec);
        const std::string tonic = spec.keyName.substr(0, spec.keyName.find(' '));
        const std::string expected = spec.minor ? tonic + "m" : tonic;
        const std::string got = found(track.samples, track.sampleRate);
        check(got == expected, std::string(name) + ": the preset is in " + expected + ", found " + got);
    }
}

// The one call gives the key with the grid and the waveform.
void theOneCallHasTheKey()
{
    const int rate = 44100;
    const AudioAnalysis analysis = analyseAudio(progression(5, true, rate), rate);
    check(analysis.key.key && keyName(*analysis.key.key) == "Fm", "the one call: the key of the audio");
    const AudioAnalysis nothing = analyseAudio({}, rate);
    check(!nothing.key.key, "the one call: no samples, no key");
}

}  // namespace

int main()
{
    namesAsRekordboxSpellsThem();
    thePictureOfOneNote();
    thePrototypesOwnPicture();
    tunedBetweenTheNotes();
    chordsInEveryKey();
    tuningAndSampleRate();
    nothingToReadAKeyFrom();
    generatedTracks();
    theOneCallHasTheKey();
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "key_from_audio_test: all checks passed\n";
    return 0;
}
