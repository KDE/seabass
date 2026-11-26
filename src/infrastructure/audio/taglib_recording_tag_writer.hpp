// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <string>

#include "application/ports/recording_tag_writer.hpp"
#include "infrastructure/audio/recording_temp_names.hpp"

namespace seabass::infrastructure::audio
{

// The copy the tags are written into is named "<file's name>.<16 hex
// digits>.seabass-tag-tmp" (RecordingTagTempSuffix), a name of each
// write's own, so that two writes never take each other's copy. TagLib is
// handed it as a FLAC (TagLib::FLAC::File), not by its extension. The
// writer removes only its own copy; a leftover is swept by a later keep
// job (LocalKeepRecordingFiles::sweepLeftovers()).

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
// A file that is not there is a failure ("it is not there"), and nothing
// beside it is looked at: a copy left by an interrupted write is put back
// only by recoverInterruptedTagWrite(), when asked.
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

// What recoverInterruptedTagWrite() did.
struct InterruptedTagWriteRecovery
{
    bool putBack = false;
    // What was found and done, in words.
    std::string message;
};

// On FAT and exFAT replacing a file by rename is a delete and then a
// rename on the disk, and a power cut between the two leaves only the
// tag writer's copy. Asked for a FLAC that is not there, this looks for
// that copy beside it: when there is exactly one and it is a FLAC (its
// STREAMINFO reads), it is put back under the file's name by a rename that
// never replaces; otherwise everything is left as it is. A copy is never
// deleted.
//
// What can be proven of a copy that is put back: that it is a FLAC whose
// STREAMINFO block reads. Not whether it is the tagged copy that was
// checked or one whose write was cut short: its audio and tags are not
// checked again. So nothing calls this during a keep job: there a FLAC
// that is not there vanished after the encoder placed it, and a copy of
// the same name is an earlier job's. It is for a step the person asks
// for. Never throws.
InterruptedTagWriteRecovery recoverInterruptedTagWrite(const std::string &flacPath);

}  // namespace seabass::infrastructure::audio
