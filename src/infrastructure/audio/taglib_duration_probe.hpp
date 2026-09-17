// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/track_duration_probe.hpp"

namespace seabass::infrastructure::audio
{

// A track's length, read by TagLib -- the same reader
// TagLibMetadataProbe uses, behind the narrower port the scans want.
//
// Preferred over QtMultimediaDurationProbe wherever TagLib is compiled
// in, for the reasons that header already gives (synchronous, Qt-free,
// identical on every platform, headers only) and for one that is not
// optional: on macOS QMediaPlayer never leaves LoadingMedia in this
// app's processes, so the Qt probe returns nothing after its timeout for
// every file. A 2344-track stick spent five seconds per track to learn
// nothing; seabass-cli sync looked hung, and the app's own scans would
// have stalled the same way.
class TagLibDurationProbe : public application::TrackDurationProbe
{
public:
    std::optional<double> durationSeconds(const std::string &absoluteFilePath) override;
};

}  // namespace seabass::infrastructure::audio
