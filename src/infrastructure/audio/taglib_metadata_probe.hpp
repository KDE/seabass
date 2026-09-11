// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/track_metadata_probe.hpp"

namespace seabass::infrastructure::audio
{

// Reads a file's tags and audio properties with TagLib.
//
// Chosen over widening QtMultimediaDurationProbe deliberately, see
// docs/unreferenced-file-cleanup-plan.md for the measurements. In
// short: QMediaPlayer metadata is a side effect of opening a demuxer --
// asynchronous, needs a QCoreApplication and a nested event loop, and
// which fields arrive depends on the platform backend (FFmpeg,
// MediaFoundation and AVFoundation do not agree). TagLib is
// synchronous, Qt-free, backend-identical everywhere, gives a
// dependable bitrate (which QMediaMetaData does not, and which
// DuplicateCleanupPlanner picks survivors by), and reads only headers
// and tag frames rather than the stream. Measured on the same warm 400
// files: 0.07 ms each against the Qt probe's 7.5 ms, 103x.
//
// Lives in its own CMake target (seabass_taglib) rather than in
// seabass_core, so the core library -- and therefore the Qt-free
// corpus_test -- keeps its dependency list unchanged. Callers that were
// built without TagLib get application::NullTrackMetadataProbe instead.
//
// Thread-safe in the only sense that matters here: it holds no state,
// and TagLib opens each file independently, so several probes can run
// on different threads over different files.
class TagLibMetadataProbe : public application::TrackMetadataProbe
{
public:
    std::optional<application::FileMetadata> read(const std::string &absoluteFilePath) override;
};

}  // namespace seabass::infrastructure::audio
