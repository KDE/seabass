// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

namespace seabass::domain
{

// Where the music actually starts and stops inside a file, as opposed to
// where the file does.
//
// Two copies of one recording routinely differ in stored length while
// carrying identical music: an encoder pads, a rip keeps the run-out, a
// re-export trims. That padding is silence, so the length of what is
// *not* silent is the comparable number, and it is the only one a tool
// that never analyses audio for its own sake has any business computing.
struct AudioContentSpan
{
    double totalSeconds = 0.0;
    double leadingSilenceSeconds = 0.0;
    double trailingSilenceSeconds = 0.0;

    // Length of the music between the two silences. Clamped at zero: a
    // file that is silent end to end has no content rather than a
    // negative amount of it.
    double contentSeconds() const
    {
        const double content = totalSeconds - leadingSilenceSeconds - trailingSilenceSeconds;
        return content > 0.0 ? content : 0.0;
    }
};

// Decodes a file far enough to say where its music begins and ends.
//
// A port in the domain rather than under application/ports, because
// DuplicateTrackFinder -- domain, and the one caller -- needs it, and
// domain deliberately includes nothing from the layers above it. The
// implementations live in infrastructure/audio.
//
// Every implementation may answer nullopt, and callers must treat that
// as "no opinion", never as "not a match": a build without a decoder, a
// file the backend refuses, a file not on the stick any more and a probe
// that timed out all land here. Refusing to guess is the whole point --
// this feeds a destructive caller.
class AudioContentProbe
{
public:
    virtual ~AudioContentProbe() = default;
    virtual std::optional<AudioContentSpan> measure(const std::string &absoluteFilePath) = 0;
};

}  // namespace seabass::domain
