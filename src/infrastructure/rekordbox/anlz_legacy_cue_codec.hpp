// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// One entry of the LEGACY cue list -- ANLZ's PCOB (cue_tag) section,
// the list players older than the Nexus2 generation read. The modern
// PCO2 list is handled by anlz_cue_codec.hpp; this exists because both
// are present in a real export and rekordbox keeps them in step, so a
// writer that updates only PCO2 leaves those players showing stale pads.
// See RekordboxCueWriter for how the two are kept level.
//
// A legacy entry carries no colour and no comment: those fields exist
// only in PCO2. A cue that has them keeps them there, and appears here
// as the position and slot alone, which is exactly what real files do.
struct LegacyCueEntry
{
    uint32_t hotCueNumber = 0;  // 1-8 for a hot cue; 0 in a memory list
    uint32_t timeMs = 0;
    bool isLoop = false;
    uint32_t loopEndMs = 0;
    // The entry exactly as it sat in the file, set by the decoder. When
    // present, the encoder writes it back unchanged rather than building
    // one, so a cue nobody edited keeps every byte we do not model --
    // the same carry-over rule the PCO2 codec uses, and for the same
    // reason: two writer generations are visible in real data and we
    // must not rewrite one into the other's shape for no reason.
    std::string rawBytes;
};

// Encodes and decodes a PCOB section. `listType` selects which of the
// two lists it is, using the same CueListTypeHot/CueListTypeMemory
// constants the PCO2 codec declares (anlz_cue_codec.hpp).
//
// Byte shapes are taken from real rekordbox-written exports, not from
// the spec alone: 7968 PCOB sections across 3988 analysis files in
// tests/fixtures/anonymized_library, of which 51 hold hot cues (119
// entries) and 3 hold a memory cue. What that survey establishes:
//
//  - Section header is 24 bytes in every one of the 7968: fourcc,
//    len_header = 24, len_tag = 24 + 56 * num_cues (true in all 7968),
//    type, a 2-byte field that is 0 in all 7968, num_cues, and
//    memory_count. HIGH confidence for all of those but the last.
//  - memory_count is 0xFFFFFFFF in 7965 of the 7968 -- every hot list,
//    empty or not, and every EMPTY memory list -- and 0 in the three
//    populated memory lists. No reading of "a count of memory cues"
//    explains a populated list holding 0, so this codec does not invent
//    one: the value a section already had is written back with it, and
//    only a section being created from scratch takes the default, which
//    is the majority value except for a populated memory list, where it
//    is what all three real examples hold.
//  - Every entry is exactly 56 bytes with len_header 28, in all 119.
//    HIGH.
//  - order_first and order_last are 0xFFFF in all 119, so this writer
//    emits that rather than the running order the field's name suggests.
//    The kaitai spec flags both as unresolved; real data is unambiguous.
//    HIGH for what to write, unknown for what they mean.
//  - The 3 bytes after `type` are 00 03 e8 in all 119. HIGH.
//  - `type` is 1 for a cue and 2 for a loop (3 real loops). HIGH.
//  - loop_time is 0xFFFFFFFF when the entry is not a loop. HIGH: 94 of
//    the 97 non-loop entries. The other 3 carry 0, and every one of them
//    also has status 1 and unknown1 0 while the 94 have status 0 and
//    unknown1 0x00010000 -- two writer generations, consistent within
//    themselves. New entries are written in the majority shape.
//  - The 16 bytes after loop_time are zero in 110 of 119; the nonzero
//    ones are the loops and a handful of entries whose extra fields we
//    do not model. New entries write zeros; an entry carried over keeps
//    its own bytes, which is how those survive an edit.
//
// Hot cues split across the two files rather than being duplicated, and
// this is the part a reader of the spec would get wrong: the .DAT file's
// PCOB holds hot cues 1-3 and nothing else (64 entries, never a 4), and
// the .EXT file's PCOB holds 4-8 and nothing else (55 entries, never a
// 3). Writing every cue into both would give an older player three pads
// it should not have. See RekordboxCueWriter::legacySplit().
//
// Within a hot list, real sections are ordered by descending hot-cue
// number (45 of 51; the other 6 ascend and are all of the older writer
// generation). The encoder emits descending.
class AnlzLegacyCueCodec
{
public:
    static std::vector<LegacyCueEntry> decodeCues(const std::string &pcobSectionBytes);

    // The section's memory_count as it stands, for a caller replacing
    // that section and wanting to leave the field as it found it.
    static uint32_t memoryCountOf(const std::string &pcobSectionBytes);

    // `memoryCount` unset means "this section is new": see the note
    // above for what is then written.
    static std::string encodeCues(const std::vector<LegacyCueEntry> &cues, uint32_t listType,
                                   std::optional<uint32_t> memoryCount = std::nullopt);
};

}  // namespace seabass::infrastructure::rekordbox
