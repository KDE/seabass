// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/track_duration_probe.hpp"

namespace seabass::infrastructure::audio
{

// Reads a track's length with QtMultimedia (QMediaPlayer + its FFmpeg
// backend). The one implementation in this project that touches audio.
//
// Lives in its own CMake target (seabass_audio_qt) rather than in
// seabass_core, so the core library -- and therefore seabass-cli and the
// Qt-free corpus_test -- keep linking no Qt at all. Both the CLI and the
// GUI link this target when Qt6::Multimedia was found; when it was not,
// they use application::NullTrackDurationProbe instead.
//
// Needs a QCoreApplication to exist (it spins a nested QEventLoop), but
// no GUI, no QGuiApplication and no QPA platform plugin -- verified
// headless with DISPLAY and WAYLAND_DISPLAY unset. Measured on a real
// stick over USB: 1214 files in 12.7 s (~10 ms each), agreeing with
// ffprobe to within 0.05 s on every one of a 100-file sample.
class QtMultimediaDurationProbe : public application::TrackDurationProbe
{
public:
    // timeoutMs bounds a single file. A damaged file can otherwise leave
    // the backend never emitting a terminal mediaStatus, which would
    // hang the whole scan on one bad track.
    explicit QtMultimediaDurationProbe(int timeoutMs = 5000);

    std::optional<double> durationSeconds(const std::string &absoluteFilePath) override;

private:
    int m_timeoutMs;
};

}  // namespace seabass::infrastructure::audio
