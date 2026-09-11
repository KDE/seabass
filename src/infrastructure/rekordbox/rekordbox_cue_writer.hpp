// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

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

    void writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues) override;

private:
    std::optional<std::string> analyzePathFor(uint32_t trackId) const;

    std::string m_pioneerRoot;
    const AnlzPathIndex *m_pathIndex = nullptr;
};

}  // namespace seabass::infrastructure::rekordbox
