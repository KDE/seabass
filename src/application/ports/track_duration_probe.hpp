// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

namespace seabass::application
{

// Port for reading a track's playing length straight from the audio
// file, for the (common) case where neither catalog recorded one.
//
// This exists because Engine leaves `Track.length` NULL until it has
// analyzed a track -- 77.6% of rows on a real 1564-track stick -- and
// length is the only field that can tell a radio edit from an extended
// mix of the same artist+title. Without it DuplicateTrackFinder cannot
// safely group anything, so Clean Up finds almost nothing (31 groups
// instead of 235 on that stick).
//
// Kept as a port specifically so seabass_core stays Qt-free: the only
// implementation that reads real audio is QtMultimediaDurationProbe,
// which lives in its own target and links Qt. The CLI and the GUI both
// get it when Qt is present; NullTrackDurationProbe stands in when it
// is not, and callers must behave correctly (just less usefully) with
// that.
class TrackDurationProbe
{
public:
    virtual ~TrackDurationProbe() = default;

    // Duration in seconds, or nullopt when it cannot be read -- a
    // missing/unreadable file, an unsupported codec, or no backend at
    // all. Never throws: an unreadable file is an ordinary answer here,
    // not an error, because a stale catalog row pointing at a deleted
    // file is a normal thing to meet on a real stick.
    virtual std::optional<double> durationSeconds(const std::string &absoluteFilePath) = 0;
};

// Always answers "don't know". Used when the build has no Qt, so
// callers need no null checks and behave identically to meeting a stick
// whose files are all unreadable.
class NullTrackDurationProbe : public TrackDurationProbe
{
public:
    std::optional<double> durationSeconds(const std::string &) override { return std::nullopt; }
};

}  // namespace seabass::application
