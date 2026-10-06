// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "infrastructure/rekordbox/anlz_file.hpp"

// The comparison at the heart of the cue list audit (#55, #60), on its
// own: the legacy memory list's shape, and a track's two generations of
// cue list read and compared, from analysis file bytes wherever they come
// from. legacy_memory_list_audit.hpp reads them from the stick for
// tools/stick_damage_audit and can repair what it finds; the rekordbox
// and OneLibrary readers ask the same question of the bytes they already
// read for a track's cues (readAnalysisFileCues), so seabass-cli scan can
// count the disagreements without a second pass over the files.
// Nothing here writes, and nothing here needs more than the two codecs.

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

// One cue as one of the two generations of list holds it, for comparing
// them (#60). The legacy PCOB lists carry no colour; `color` is the
// modern PCO2 entry's, when it has one.
struct ListedCue
{
    uint32_t pad = 0;  // 1-8 for a hot cue, 0 for a memory cue
    uint32_t timeMs = 0;
    bool isLoop = false;
    uint32_t loopEndMs = 0;
    std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> color;
};

// A track's four cue lists: the legacy PCOB lists of both files taken
// together (hot cues 1-3 and the memory cues in the .DAT, 4-8 in the
// .EXT), which is what an XDJ-RX2 and a CDJ-3000X show, and the modern
// PCO2 lists in the .EXT, which Seabass reads first (#33, #60).
struct TrackCueLists
{
    std::vector<ListedCue> legacyHot;
    std::vector<ListedCue> legacyMemory;
    std::vector<ListedCue> modernHot;
    std::vector<ListedCue> modernMemory;
    // Memory cues in the .EXT's legacy list, where neither rekordbox nor
    // a player puts any (all 3988 surveyed files) and the writer never
    // writes: counted in legacyMemory, and a reason to leave the track
    // alone, since no rewrite would bring that list in step.
    size_t extLegacyMemory = 0;
};

// Where the two generations disagree. From 2026-09-02 (the first cue
// writer) to 5282555e (2026-09-18) RekordboxCueWriter wrote PCO2 alone,
// so a track written then can show one set of pads on a player and
// another in Seabass.
//
// "The same cue" is decided as the reader decides it when it merges the
// two (kaitai_rekordbox_reader.cpp, appendLegacyCues): a hot cue by its
// pad, a memory cue by position, positions within
// LocalRestorePlanner::PositionToleranceMs. rekordbox itself stores one
// cue in the two lists a millisecond, or 52 ms, apart, and a check that
// compared exactly would offer to rewrite rekordbox's own files over
// that. A cue on one side only, or a pad more than the tolerance away,
// or a loop on one side and a cue on the other, is a disagreement.
// Cues are paired in order of position, so the pairing found is the one
// with the smallest gaps.
struct CueListDisagreement
{
    bool hot = false;
    bool memory = false;
    TrackCueLists lists;
    // What Seabass shows for the track: the modern lists, plus every
    // legacy cue they do not already hold (a hot cue on a pad PCO2 does
    // not use, a memory cue no PCO2 one is the same as). That is how the
    // reader merges the two, and what "keep what Seabass wrote" keeps, so
    // a cue only the legacy list has is not lost by choosing Seabass.
    std::vector<ListedCue> seabassHot;
    std::vector<ListedCue> seabassMemory;
    // The player and Seabass show the same cues; only the modern list
    // lacks some the legacy one has, which a reader of PCO2 alone (a
    // newer player, rekordbox) would not show.
    bool viewsAgree = false;
    // Nonempty: a list holds a cue the writer cannot put in both
    // generations (a hot cue outside pads 1-8), so it is left alone.
    std::string unrepairable;

    bool any() const { return hot || memory; }
};

// Nothing when the lists agree.
std::optional<CueListDisagreement> compareCueLists(const TrackCueLists &lists);

// The PCOB and PCO2 sections of one analysis file, out of its bytes, the
// way readCueSections() takes them out of the file: walked header to
// header. Throws, naming `label`, when the bytes are not an analysis file
// or a section length points outside them.
std::vector<AnlzRawSection> cueSectionsOfBytes(const std::string &bytes, const std::string &label);

// The PCOB and PCO2 sections of the analysis file at `path`, reading the
// section headers and those sections alone. A real .EXT is some 167 KB of
// which the lists are a few hundred bytes, mostly waveforms after them;
// reading every track's whole file over USB would make the check the
// slowest on the hub. Throws when the file is missing, is not an ANLZ
// file, or a section length points outside it.
std::vector<AnlzRawSection> readCueSections(const std::string &path);

// Whether a section is the .DAT's legacy memory list.
bool isMemoryPcob(const AnlzRawSection &section);

// Both files' lists out of their cue sections. Throws, naming the list
// and the file by its label, when one does not decode; a memory PCOB is
// read leniently when auditLegacyMemoryList() can read it, so a stale
// header or the RX2's zero slot is compared by its real entries.
TrackCueLists cueListsOf(const std::vector<AnlzRawSection> &dat, const std::vector<AnlzRawSection> &ext,
                         const std::string &datLabel, const std::string &extLabel);

// Two entries are the same cue the way the reader decides when it merges
// the two lists: positions within LocalRestorePlanner::PositionToleranceMs,
// a loop only with a loop that ends within it too. The pad is the
// caller's to compare.
bool sameListedCue(const ListedCue &a, const ListedCue &b);

// What Seabass shows of one kind of cue: the modern list, and every
// legacy cue it does not already hold, merged exactly as the reader's
// appendLegacyCues() merges them: a hot cue is known when its pad is
// taken, a memory cue when any cue already in the list (a legacy one
// appended before it included) sits within the tolerance, loop or not.
std::vector<ListedCue> seabassView(const std::vector<ListedCue> &modern, const std::vector<ListedCue> &legacy,
                                   bool hot);

// What one track's two files say, both read: the .DAT's memory list's
// shape, and the two generations of list compared. The lists are not
// compared when the memory list is malformed (there is no telling what
// it holds) or a list does not decode (`listsMalformed` says which).
struct CueListsCompared
{
    LegacyMemoryListShape shape;
    std::optional<CueListDisagreement> disagreement;
    std::string listsMalformed;
};
CueListsCompared compareCueSections(const std::vector<AnlzRawSection> &dat, const std::vector<AnlzRawSection> &ext,
                                    const std::string &datLabel, const std::string &extLabel);

}  // namespace seabass::infrastructure::rekordbox
