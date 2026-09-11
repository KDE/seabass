// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

namespace seabass::application
{

// Everything worth knowing about an audio file that can be read from the
// file itself rather than from a catalog. Deliberately the whole set in
// one struct: every caller that wants one of these fields wants several,
// and reading them one at a time would re-open the file per field.
struct FileMetadata
{
    std::string title;
    std::string artist;
    std::string album;
    double durationSeconds = 0.0;
    int bitrate = 0;     // kbps, 0 if unknown
    int sampleRate = 0;  // Hz, 0 if unknown

    // True when the backend could not read a real length and computed
    // one from the stream size and bitrate instead -- a VBR MP3 with no
    // Xing/VBRI header, where the answer can be seconds out.
    //
    // This is NOT a quality-of-metadata nicety. DuplicateTrackFinder
    // groups on duration within a 2-second tolerance, and the caller
    // that acts on those groups deletes files. A guess that lands in the
    // wrong group would delete a track the DJ owns and no database will
    // ever mention again, so callers whose outcome is destructive must
    // refuse to act on a track with this set, rather than treating the
    // number as a fact. See docs/unreferenced-file-cleanup-plan.md.
    //
    // A container-derived length (MP4, FLAC, Ogg, WAV) is exact and
    // never sets this.
    bool durationIsEstimated = false;
};

// Port for reading a file's own metadata, for tracks that either aren't
// in any catalog (the unreferenced-file cleanup) or whose catalog row
// left fields empty -- Engine leaves `Track.length` NULL until it has
// analyzed a track, 77.6% of rows on a real 1564-track stick.
//
// Kept as a port for the same reason TrackDurationProbe is: seabass_core
// stays free of any audio-library dependency, and the CLI, the GUI and
// the Qt-free corpus_test all get whichever implementation the build
// found. TagLibMetadataProbe is the real one; NullTrackMetadataProbe
// stands in when TagLib was not found, and callers must behave
// correctly (just less usefully) with that.
class TrackMetadataProbe
{
public:
    virtual ~TrackMetadataProbe() = default;

    // Metadata for the file, or nullopt when it cannot be read at all --
    // a missing file, an unsupported container, or anything that yields
    // no playing length (which includes a non-audio file that merely has
    // an audio extension). Never throws: an unreadable file is an
    // ordinary answer here, not an error, because a stale catalog row
    // pointing at a deleted file is a normal thing to meet on a real
    // stick.
    //
    // Individual fields are best-effort within a successful read: a file
    // with no tags still yields a duration and bitrate, with title and
    // artist left empty.
    virtual std::optional<FileMetadata> read(const std::string &absoluteFilePath) = 0;
};

// Always answers "don't know". Used when the build has no TagLib, so
// callers need no null checks and behave identically to meeting a stick
// whose files are all unreadable.
class NullTrackMetadataProbe : public TrackMetadataProbe
{
public:
    std::optional<FileMetadata> read(const std::string &) override { return std::nullopt; }
};

}  // namespace seabass::application
