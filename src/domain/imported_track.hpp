// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "domain/track_tags.hpp"

namespace seabass::domain
{

// What the audio itself says about a file, measured from it rather than
// read from a tag. 0 where the reader could not tell.
struct AudioFormat
{
    double durationSeconds = 0.0;
    // kbit/s.
    int bitrate = 0;
    int sampleRateHz = 0;
    // Bits per sample of a PCM or lossless file; 0 for a lossy one.
    int bitsPerSample = 0;
    std::uint64_t fileSizeBytes = 0;
};

// One imported track as every library on the stick is to describe it:
// the single source the three row writers are filled from, so that
// DeviceLibrary, OneLibrary and Engine say the same about a track that
// came from one file. Nothing here belongs to one format.
//
// Texts are ready for a row: trimmed, well-formed UTF-8, empty where the
// file says nothing (domain::isStorableText holds for each).
struct ImportedTrack
{
    // From the stick's root, '/' between names: "/Contents/Artist/Album/
    // 01 Title.flac". The planner's destination with a leading '/'.
    std::string stickPath;
    // Never empty: the tag's title, or the file's name without its
    // extension, which is what rekordbox shows for an untagged file.
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string genre;
    std::string label;
    std::string remixer;
    std::string composer;
    // As the tag spells it: "8A", "Am", "A minor". A tag's key is some
    // software's analysis written back, kept until Seabass has its own.
    std::string key;
    std::string comment;
    // 0: the tag holds none.
    double bpm = 0.0;
    int year = 0;
    int trackNumber = 0;
    // Whole seconds as rekordbox counts them: the largest whole number
    // below the length, so 179 for a file of exactly 180 s and 180 for
    // one of 180.04 s (eleven tracks of the T73 export, FLAC, AIFF, MP3).
    int lengthSeconds = 0;
    // The file's exact length, for a format that keeps it (Engine).
    double durationSeconds = 0.0;
    // kbit/s as rekordbox writes it: 0 for FLAC, the file's own otherwise
    // (320 for an MP3, 1536 for 48 kHz 16 bit AIFF).
    int bitrate = 0;
    // 16 for a lossy file, as rekordbox writes for MP3 and AAC; the
    // file's own for PCM and FLAC.
    int bitDepth = 0;
    int sampleRateHz = 0;
    std::uint64_t fileSizeBytes = 0;
    // The day of the import, "YYYY-MM-DD".
    std::string date;
};

// The description of the file that was copied to `stickPath`, from its
// tags and its audio. A tag text that is not UTF-8 is treated as absent
// rather than stored broken; blanks around a text go. A year or track
// number outside what a row holds (negative, a year past 9999, a number
// past 9999) is 0.
//
// Throws std::invalid_argument for a `stickPath` that is not a path from
// the stick's root to a file, or a `date` that is not a day as
// YYYY-MM-DD: both are the caller's own values, not the file's.
ImportedTrack describeImportedTrack(const TrackTags &tags, const AudioFormat &audio, std::string_view stickPath,
                                    std::string_view date);

// `text` without the blanks, line ends and NULs around it; empty when it
// is not well-formed UTF-8 or has a NUL inside.
std::string storableText(std::string_view text);

}  // namespace seabass::domain
