#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// One cue entry in the shape ANLZ's PCO2 (cue_extended_entry) format
// actually stores it -- used for both hot cues (hotCueNumber 1-8) and
// memory cues (hotCueNumber == 0, per the kaitai spec's own
// classification: a cue_extended_entry_t is only "hot" when its
// hot_cue field is nonzero, regardless of which list/section it's in).
// Freshly encoded entries carry no comment (the byte shape validated
// against real rekordbox-written entries has an empty one). Entries read
// from a file keep their exact bytes in rawBytes, and the encoder writes
// those back verbatim: a rewrite that replaces the whole list must not
// flatten the comment, legacy colour id or loop of a cue it did not
// touch, which is what every save used to do.
struct RawHotCueEntry
{
    uint32_t hotCueNumber = 0;  // 1-8 for a hot cue; 0 for a memory cue
    uint32_t timeMs = 0;
    std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> color;  // (r, g, b), if any
    // A loop: the player repeats between timeMs and loopEndMs. Encoded
    // as the spec's cue_entry_type loop (2) with loop_time set, the same
    // fields the reader takes them from.
    bool isLoop = false;
    uint32_t loopEndMs = 0;
    // The entry exactly as it sat in the file, set by the decoder. When
    // present, encodeHotCues() appends it unchanged instead of encoding
    // the fields above.
    std::string rawBytes;
};

// cue_list_type values for the PCO2 section's `type` field -- which of
// the two independent cue lists (hot cues vs memory cues) a given PCO2
// section holds. Matches Anlz::CUE_LIST_TYPE_HOT_CUES/_MEMORY_CUES in
// the vendored kaitai spec used for reading.
constexpr uint32_t CueListTypeMemory = 0;
constexpr uint32_t CueListTypeHot = 1;

// Encodes/decodes a PCO2 (cue_extended_tag) section of an ANLZ file --
// either the hot-cues list or the memory-cues list, selected by
// `listType` (a track's ANLZ file has up to one PCO2 section of each
// type, at most).
//
// Confidence levels, from real-data validation against seven hot-cue
// entries across four different real rekordbox-written files, plus one
// real (empty) memory-cues section:
//  - the section-level container (fourcc/len_header/len_tag/type/
//    num_cues/padding) is the same shape for both list types: HIGH --
//    confirmed against a real empty CueListTypeMemory section
//    (`50434f32 00000014 00000014 00000000 0000 0000`), which matches
//    this same layout with type=0 and num_cues=0.
//  - the per-entry layout (magic/lengths/type/hot_cue/time/color_id/
//    loop_numerator/denominator/len_comment/color fields): HIGH for hot
//    cues (see above); for memory cues, no real *populated* memory-cue
//    entry has been captured to verify against yet, so this reuses the
//    hot-cue entry layout on the (spec-supported, but not
//    independently confirmed for a real populated case) assumption that
//    entries are shaped identically regardless of which list they're
//    in -- true of the read-side kaitai spec, which parses both list
//    types' entries with the exact same `cue_extended_entry_t` type.
//  - the 3-byte field after `type` (called "pad3" here): HIGH that it's a
//    fixed constant -- 0x0003E8 in all seven real hot-cue examples checked.
//  - loop_time: HIGH that 0xFFFFFFFF means "not a loop" -- true in six of
//    seven examples (one real cue had 0 instead; either value appears to
//    be accepted, we use the more common one).
//  - the 7-byte field after `color_id` (called "pad7" here): LOW
//    confidence. The Kaitai spec this project vendors treats it as opaque
//    padding, but real data shows its first byte is always 0x01, while
//    the next two bytes vary per cue in a way we could not correlate with
//    time, hot cue number, or color. It may be an undocumented per-cue
//    ordering/sequence field. We default new entries to byte 0 = 0x01
//    (well-evidenced) plus an incrementing counter for the next two bytes
//    (matches the *shape* of real data -- distinct, nonzero, varying per
//    cue -- without claiming to know its real meaning). This is the one
//    place in the writer where hardware/rekordbox verification genuinely
//    matters before trusting it for a gig -- doubly so for memory cues,
//    given the unconfirmed-populated-entry caveat above.
class AnlzCueCodec
{
public:
    static std::vector<RawHotCueEntry> decodeHotCues(const std::string &pco2SectionBytes,
                                                       uint32_t listType = CueListTypeHot);
    static std::string encodeHotCues(const std::vector<RawHotCueEntry> &cues, uint32_t listType = CueListTypeHot);
};

}  // namespace seabass::infrastructure::rekordbox
