// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/recording_tag_writer.hpp"

namespace seabass::infrastructure::audio
{

// The suffix of the copy the tags are written into, for a scan that
// tidies leftovers.
inline constexpr const char *RecordingTagTempSuffix = ".seabass-tag-tmp.flac";

// RecordingTagWriter through TagLib, for FLAC files: the text goes into
// the Vorbis comment (ARTIST, TITLE, ALBUM, GENRE, DATE), the cover into
// one PICTURE block of type front cover, replacing what was there.
//
// The tags are written into a copy beside the file and read back from it;
// the copy's audio frames and STREAMINFO block (with the MD5 of the
// samples) are compared with the file's, byte for byte, and only then
// does the copy replace the file. The copy costs the file's size once
// more on the disk, for the time of the write.
class TagLibRecordingTagWriter : public application::RecordingTagWriter
{
public:
    application::RecordingTagWriteResult write(const std::string &flacPath,
                                               const application::RecordingTags &tags) override;
};

}  // namespace seabass::infrastructure::audio
