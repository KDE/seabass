// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <string>

#include "application/ports/recording_tag_writer.hpp"

namespace seabass::infrastructure::audio
{

// The suffix of the copy the tags are written into, for a scan that
// tidies leftovers. The whole name is "<file's name>.<16 hex
// digits>.seabass-tag-tmp.flac", a name of each write's own, so that two
// writes never take each other's copy. The writer removes only its own
// copy. A leftover beside its FLAC stays for a scan; one whose FLAC is
// not there is never deleted (see write()).
inline constexpr const char *RecordingTagTempSuffix = ".seabass-tag-tmp.flac";

// RecordingTagWriter through TagLib, for FLAC files: the text goes into
// the Vorbis comment (ARTIST, TITLE, ALBUM, GENRE, DATE), the cover into
// one PICTURE block of type front cover, replacing what was there.
//
// The tags are written into a copy beside the file and read back from it;
// the copy's audio frames and STREAMINFO block (with the MD5 of the
// samples) are compared with the file's, byte for byte, and only then
// does the copy replace the file. Before it does, the copy is flushed
// without being created (a copy that went away is a failure, never an
// empty file in the FLAC's place) and its size and audio are compared
// again with what was checked; the file's size is read once more after
// the rename. The copy costs the file's size once more on the disk, for
// the time of the write.
//
// On FAT and exFAT replacing a file by rename is a delete and then a
// rename on the disk, and a power cut between the two leaves only the
// copy. Asked to write the tags of a file that is not there, the writer
// looks for that copy: when there is exactly one and it is a FLAC, it is
// put back under the file's name; otherwise everything is left as it is.
// It is never deleted. Either way the answer is a failure that says what
// was found.
class TagLibRecordingTagWriter : public application::RecordingTagWriter
{
public:
    application::RecordingTagWriteResult write(const std::string &flacPath,
                                               const application::RecordingTags &tags) override;

    // For tests: called with the path of the checked copy before it is
    // flushed and looked at again.
    void setBeforePlaceForTests(std::function<void(const std::string &)> hook) { m_beforePlace = std::move(hook); }

private:
    std::function<void(const std::string &)> m_beforePlace;
};

}  // namespace seabass::infrastructure::audio
