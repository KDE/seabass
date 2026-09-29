// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <optional>

#include "application/ports/cue_writer.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"

namespace seabass::infrastructure::rekordbox
{

// Writes hot cues into a rekordbox USB export by rewriting the target
// track's ANLZ .EXT file's PCO2 (hot cues) section -- see
// anlz_cue_codec.hpp for the format confidence notes and
// anlz_file.hpp for why this can be done safely without touching any
// section we don't understand. export.pdb itself is never modified: cue
// data lives entirely in the ANLZ files.
//
// This is the least-proven part of the whole project (see the plan's
// Risks section) -- validated so far by round-tripping real files
// byte-for-byte and cross-checking newly written cues with the
// independent, existing Kaitai-based reader, but NOT yet verified against
// real rekordbox software or real CDJ/XDJ hardware. Treat output as
// untrusted for a real gig until you've confirmed that yourself.
// Every file writeHotCues() may write for one track, in the order it
// writes them: the .EXT always, the .DAT when the track has one.
//
// Exists so a caller cannot back up less than the write touches. It used
// to be one file, so every backup site named extAnlzPath() directly;
// when the legacy lists brought the .DAT in, all eight of those sites
// were suddenly backing up too little, and a failed save could no longer
// be rolled back whole. One function now answers the question, and
// add_cue_mirror_failure_test is what noticed.
std::vector<std::string> rekordboxCueFilesFor(const std::string &pioneerRoot, const std::string &analyzePath);

class RekordboxCueWriter : public application::CueWriter
{
public:
    explicit RekordboxCueWriter(std::string pioneerRoot);

    // Same, but answering "which analysis file is this track's?" from an
    // index built once instead of by re-parsing export.pdb per call. The
    // index must outlive this writer and must have been built from the
    // same catalog; a save owns one and hands it to every writer it makes.
    // Passing nothing keeps the old per-call lookup, which is what the
    // single-shot callers (the command line, the waveform reader) want.
    RekordboxCueWriter(std::string pioneerRoot, const AnlzPathIndex *pathIndex);

    // Writes the .EXT and, when its lists change, the .DAT, reading each
    // back afterwards. A file that does not read back as written, or
    // holds a legacy list outside the surveyed shape, is put back as it
    // was (with the .EXT too, when the .DAT failed) and the call throws
    // naming it. A cue at a negative position (or a loop ending at
    // one) is left out: the files hold unsigned milliseconds, and such
    // a cue is junk that points nowhere in the track.
    void writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues) override;

    // Runs after each file is written, before it is read back, so a test
    // can damage a file there. Empty to turn it off.
    static void setAfterWriteForTesting(std::function<void(const std::string &path)> hook);

private:
    std::optional<std::string> analyzePathFor(uint32_t trackId) const;

    std::string m_pioneerRoot;
    const AnlzPathIndex *m_pathIndex = nullptr;
};

}  // namespace seabass::infrastructure::rekordbox
