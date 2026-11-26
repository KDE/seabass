// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/recording_encoder.hpp"

namespace seabass::application
{

// A front cover ready to be embedded: a baseline JPEG and its size in
// pixels (CoverArtPreparer makes one from any picture).
struct CoverArt
{
    std::string jpeg;
    int width = 0;
    int height = 0;
};

// What a kept recording is tagged with. Text is UTF-8; an empty field and
// a year of 0 are not written. An empty cover is no cover.
struct RecordingTags
{
    std::string artist;
    std::string title;
    std::string album;
    std::string genre;
    int year = 0;
    CoverArt cover;
};

// What RecordingTagWriter::tagInPlace did: written, or the reason not.
struct RecordingTagWriteResult
{
    bool written = false;
    std::string error;
    // When written: the tagged file as it is now (its new size; its
    // STREAMINFO block is the one it had).
    VerifiedFlac tagged;
};

// Writes the tags and the front cover of an encoded recording (a FLAC)
// into the file itself, and reads them back to compare before it says
// written.
//
// The file is one that was proven to hold the recording and has not been
// given its name yet (the encoder's temporary file): before anything is
// written its size and STREAMINFO block must be what `file` says, and
// after, its STREAMINFO block must be unchanged (with the MD5 of the
// samples) and its audio frames the very bytes they were before the write.
// On a failure the file may have been changed: it is the caller's, who
// removes it. Nothing else is written, no copy and no file beside it.
//
// Never throws.
class RecordingTagWriter
{
public:
    virtual ~RecordingTagWriter() = default;

    virtual RecordingTagWriteResult tagInPlace(const VerifiedFlac &file, const RecordingTags &tags) = 0;
};

}  // namespace seabass::application
