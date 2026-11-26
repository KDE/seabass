// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/recording_tag_writer.hpp"

namespace seabass::infrastructure::audio
{

// RecordingTagWriter through TagLib, for FLAC files: the text goes into
// the Vorbis comment (ARTIST, TITLE, ALBUM, GENRE, DATE), the cover into
// one PICTURE block of type front cover, replacing what was there.
//
// The file is handed to TagLib as a FLAC (TagLib::FLAC::File), not by its
// extension, so the encoder's temporary name ("<FLAC>.<16 hex>
// .seabass-encode-tmp") is read and written as any FLAC. TagLib saves a
// FLAC's metadata in the file itself: over the old blocks and their
// padding, or, when those are too small, by moving the audio frames
// further into the same file. It writes no temporary file beside it and
// renames nothing (measured with strace on TagLib 1.13.1, a cover larger
// than the padding included: the only file opened is the one tagged, and
// recording_tag_writer_test checks the folder and the inode).
//
// The tags are read back with a fresh handle, the STREAMINFO block (with
// the MD5 of the samples) compared, and the audio frames hashed before and
// after and compared.
class TagLibRecordingTagWriter : public application::RecordingTagWriter
{
public:
    application::RecordingTagWriteResult tagInPlace(const application::VerifiedFlac &file,
                                                    const application::RecordingTags &tags) override;
};

}  // namespace seabass::infrastructure::audio
