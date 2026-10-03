// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// The legacy memory cue list (the .DAT file's memory PCOB, see
// anlz_legacy_cue_codec.hpp) in a shape a player chokes on (#33, #55).
//
// From 5282555e to 6e0f1c09 RekordboxCueWriter wrote the list with a
// header that says "empty" (0xFFFFFFFF) while it holds entries, and left
// two or more entries unlinked. An XDJ-RX2 (firmware 1.43) reads such a
// list as empty, drops the entries on its next memory save, and leaves a
// list sized for one more entry than it holds, the extra slot zero. Its
// memory-loop save after that hangs the player, which then has to be
// powered off and leaves a skeleton ANLZ0001.DAT behind. The writer fix
// reaches a track only when Seabass writes it again; this finds and
// repairs the rest.
struct LegacyMemoryListShape
{
    int entries = 0;
    // The header's last-entry field is not entries - 1 (0xFFFFFFFF when
    // the list is empty).
    bool headerStale = false;
    // Entries not chained by index: entry i must name i - 1 and i + 1,
    // 0xFFFF at either end.
    bool unlinked = false;
    // len_tag exceeds 24 + 56 * entries and the excess is all zero: the
    // player's rewrite of a stale list.
    bool zeroSlots = false;
    // Nonempty: not a list this can read, so not one it repairs either.
    std::string malformed;

    bool damaged() const { return headerStale || unlinked || zeroSlots; }
    bool repairable() const { return damaged() && malformed.empty(); }
};

// Looks at one memory PCOB section. A section whose type is not the
// memory list is reported as malformed.
LegacyMemoryListShape auditLegacyMemoryList(const std::string &pcobSectionBytes);

// The same list re-encoded with every entry kept byte for byte, the
// header and links set from the entries and the zero slots dropped.
// Throws std::runtime_error when the shape is malformed or not damaged.
std::string repairLegacyMemoryList(const std::string &pcobSectionBytes);

// One track's analysis directory, looked at.
struct LegacyMemoryListFinding
{
    // The .DAT export.pdb names for the track; empty when the file is
    // missing, in which case only debris can be reported.
    std::string datPath;
    LegacyMemoryListShape shape;
    // What the RX2 leaves behind when it hangs: an ANLZ*.DAT beside the
    // track's that no row names, with no .EXT of its own number, holding
    // an empty beat grid and waveform. Reported, and removed by the
    // repair, since nothing refers to it. (A second full set in the
    // directory is rekordbox's own: it numbers a second track's analysis
    // ANLZ0001 and names it from that track's row.)
    std::vector<std::string> debris;

    bool anything() const { return shape.damaged() || !shape.malformed.empty() || !debris.empty(); }
};

// The track's .DAT memory list and the files next to it. `analyzePath` is
// the row's analyze_path as AnlzPathIndex gives it, and `named` answers
// whether any row names a path spelled the same way (AnlzPathIndex::
// names). Nothing is returned when the list is healthy and the
// directory holds no debris.
std::optional<LegacyMemoryListFinding> auditTrackAnalysis(const std::string &pioneerRoot,
                                                          const std::string &analyzePath,
                                                          const std::function<bool(const std::string &)> &named);

// Repairs the .DAT's memory list in place (through AnlzFile, with its
// staleness check) and removes the debris files. Returns a one-line
// account of what it did. Throws on any failure, before or after a write;
// callers back the .DAT up first.
std::string repairTrackAnalysis(const LegacyMemoryListFinding &finding);

}  // namespace seabass::infrastructure::rekordbox
