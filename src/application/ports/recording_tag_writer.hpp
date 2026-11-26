// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

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

// What RecordingTagWriter::write did: written, or the reason not.
struct RecordingTagWriteResult
{
    bool written = false;
    std::string error;
};

// Writes the tags and the front cover of an encoded recording (a FLAC),
// and reads them back to compare before it says written.
//
// The audio of the file is not touched: what is tagged is a copy beside
// it, whose audio is compared with the file's before the copy replaces
// it. On any failure the file is left as it was and the copy is taken
// away.
//
// Never throws.
class RecordingTagWriter
{
public:
    virtual ~RecordingTagWriter() = default;

    virtual RecordingTagWriteResult write(const std::string &flacPath, const RecordingTags &tags) = 0;
};

}  // namespace seabass::application
