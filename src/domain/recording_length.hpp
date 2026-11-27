// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>

// The lengths of a recording in words: the clock a person reads, and what
// a WAV header that was not completed says against what the file holds.
// The reader of the WAV and the keep job both say it through these, so
// that it is said one way.
namespace seabass::domain
{

// A number of frames as a length a person reads: "1:02:03" or "2:03",
// cut to whole seconds as a clock shows them (never rounded up). "0:00"
// for a sample rate of 0.
inline std::string recordingClock(std::uint64_t frames, int sampleRate)
{
    const std::uint64_t seconds = sampleRate > 0 ? frames / static_cast<std::uint64_t>(sampleRate) : 0;
    const auto two = [](std::uint64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    const std::uint64_t h = seconds / 3600;
    const std::uint64_t m = seconds / 60 % 60;
    return h > 0 ? std::to_string(h) + ":" + two(m) + ":" + two(seconds % 60)
                 : std::to_string(m) + ":" + two(seconds % 60);
}

// What a WAV header that was not completed says, against the audio the
// file holds.
struct UnfinishedHeaderLengths
{
    // Whole frames the file holds after the data chunk's header.
    std::uint64_t framesHeld = 0;
    // What the header declares, in whole frames; nullopt when it declares
    // no size.
    std::optional<std::uint64_t> framesDeclared;
    // Whether the declared length is one a recording can have; one that is
    // not is never said as a length.
    bool declaredUsable = true;
    // How many bytes less the file holds than the header declares.
    std::uint64_t bytesMissing = 0;
    int sampleRate = 0;
};

enum class RecordingTense
{
    Present,  // "the file holds": the recording as it is
    Past,     // "the file held": a recording that was kept
};

// The words after "header was not completed: ", without a full stop:
//
//   "it does not say how long the audio is, and the file holds 42:10 of audio"
//   "the header's length is not usable, and the file holds 42:10"
//   "it declares 1:00:00 of audio, and the file holds 42:10"
//
// and when the two lengths print the same, the bytes missing after them:
// "... and the file holds 42:10 (4000 bytes less than the header
// declares)". In the past tense "declared" and "held", and a header
// without a size: "it did not say how long the audio is, so the file was
// read to its end: it held 42:10 of audio".
inline std::string unfinishedHeaderText(const UnfinishedHeaderLengths &lengths, RecordingTense tense)
{
    const bool past = tense == RecordingTense::Past;
    const std::string holds = std::string("and the file ") + (past ? "held " : "holds ")
        + recordingClock(lengths.framesHeld, lengths.sampleRate);
    if (!lengths.framesDeclared) {
        if (past) {
            return "it did not say how long the audio is, so the file was read to its end: it held "
                + recordingClock(lengths.framesHeld, lengths.sampleRate) + " of audio";
        }
        return "it does not say how long the audio is, " + holds + " of audio";
    }
    if (!lengths.declaredUsable) {
        return "the header's length is not usable, " + holds;
    }
    const std::string declared = recordingClock(*lengths.framesDeclared, lengths.sampleRate);
    std::string text = std::string("it ") + (past ? "declared " : "declares ") + declared + " of audio, " + holds;
    if (declared == recordingClock(lengths.framesHeld, lengths.sampleRate)) {
        text += " (" + std::to_string(lengths.bytesMissing) + " bytes less than the header declares)";
    }
    return text;
}

}  // namespace seabass::domain
