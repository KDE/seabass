#include "infrastructure/rekordbox/anlz_cue_codec.hpp"

#include <stdexcept>

#include "infrastructure/rekordbox/big_endian.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

// See the confidence-level notes in anlz_cue_codec.hpp.
constexpr unsigned char Pad3Template[3] = {0x00, 0x03, 0xE8};
constexpr uint32_t NotALoopSentinel = 0xFFFFFFFFu;
constexpr unsigned char Pad7FirstByte = 0x01;

constexpr size_t FixedEntrySize = 40;  // magic..loop_denominator, before len_comment
constexpr size_t NoCommentEntrySize = FixedEntrySize + 4;  // + len_comment(=0)
constexpr size_t WithColorEntrySize = NoCommentEntrySize + 4;  // + color_code/r/g/b

}  // namespace

std::vector<RawHotCueEntry> AnlzCueCodec::decodeHotCues(const std::string &pco2SectionBytes, uint32_t listType)
{
    std::vector<RawHotCueEntry> result;
    if (pco2SectionBytes.size() < 20) {
        return result;
    }

    uint32_t type = readU32BE(pco2SectionBytes, 12);
    uint16_t numCues = readU16BE(pco2SectionBytes, 16);
    if (type != listType) {
        return result;
    }

    size_t offset = 20;
    for (uint16_t i = 0; i < numCues; ++i) {
        // NoCommentEntrySize (44), not FixedEntrySize (40): lenComment is
        // read from offset+40..+43, so the bounds check must cover through
        // that field, not stop one field short of it.
        if (offset + NoCommentEntrySize > pco2SectionBytes.size()) {
            throw std::runtime_error("PCO2 section truncated while decoding hot cue entries");
        }
        uint32_t lenEntry = readU32BE(pco2SectionBytes, offset + 8);
        uint32_t hotCue = readU32BE(pco2SectionBytes, offset + 12);
        const unsigned char entryType = static_cast<unsigned char>(pco2SectionBytes[offset + 16]);
        uint32_t time = readU32BE(pco2SectionBytes, offset + 20);
        uint32_t loopTime = readU32BE(pco2SectionBytes, offset + 24);
        uint32_t lenComment = readU32BE(pco2SectionBytes, offset + 40);

        RawHotCueEntry entry;
        entry.hotCueNumber = hotCue;
        entry.timeMs = time;
        entry.isLoop = entryType == 2;  // cue_entry_type::loop, per the spec
        entry.loopEndMs = entry.isLoop ? loopTime : 0;
        if (lenEntry >= NoCommentEntrySize && offset + lenEntry <= pco2SectionBytes.size()) {
            entry.rawBytes = pco2SectionBytes.substr(offset, lenEntry);
        }

        size_t colorPos = offset + FixedEntrySize + 4 + lenComment;
        if ((lenEntry - lenComment) > 44 && colorPos + 4 <= pco2SectionBytes.size()) {
            unsigned char r = static_cast<unsigned char>(pco2SectionBytes[colorPos + 1]);
            unsigned char g = static_cast<unsigned char>(pco2SectionBytes[colorPos + 2]);
            unsigned char b = static_cast<unsigned char>(pco2SectionBytes[colorPos + 3]);
            entry.color = std::make_tuple(r, g, b);
        }

        // Within a hot-cues list, an entry with hot_cue==0 is a non-hot
        // marker (per the read-side kaitai classification) and excluded;
        // within a memory-cues list every entry legitimately has
        // hot_cue==0, so none are filtered out.
        if (listType != CueListTypeHot || hotCue != 0) {
            result.push_back(entry);
        }
        offset += lenEntry;
    }

    return result;
}

std::string AnlzCueCodec::encodeHotCues(const std::vector<RawHotCueEntry> &cues, uint32_t listType)
{
    std::string body;
    appendU32BE(body, listType);
    appendU16BE(body, static_cast<uint16_t>(cues.size()));
    body += std::string(2, '\0');  // padding

    uint16_t orderCounter = 0;
    for (const auto &cue : cues) {
        if (!cue.rawBytes.empty()) {
            // An entry read from the file and not changed: back as it
            // was, comment, colour id and loop included. The size field
            // is checked so a torn raw entry cannot corrupt the section.
            if (cue.rawBytes.size() < NoCommentEntrySize || readU32BE(cue.rawBytes, 8) != cue.rawBytes.size()) {
                throw std::runtime_error("AnlzCueCodec: a carried-over cue entry is not self-consistent");
            }
            // Verbatim, the 2-byte field at 30..31 included: real entries
            // carry values there (0x01a8 in the captured coloured entry)
            // that are not a 1-based sequence, so this codec's counter is
            // a shape match for fresh entries, not a rule the file keeps.
            // Rewriting a captured value to fit the guess would be the
            // one edit here made without real data behind it.
            ++orderCounter;
            body += cue.rawBytes;
            continue;
        }
        bool hasColor = cue.color.has_value();
        uint32_t lenEntry = static_cast<uint32_t>(hasColor ? WithColorEntrySize : NoCommentEntrySize);

        std::string entry;
        entry += "PCP2";
        appendU32BE(entry, 16);  // len_header, matches real data
        appendU32BE(entry, lenEntry);
        appendU32BE(entry, cue.hotCueNumber);
        entry.push_back(static_cast<char>(cue.isLoop ? 2 : 1));  // cue_entry_type: loop (2) or cue point (1)
        entry += std::string(reinterpret_cast<const char *>(Pad3Template), 3);
        appendU32BE(entry, cue.timeMs);
        appendU32BE(entry, cue.isLoop ? cue.loopEndMs : NotALoopSentinel);
        entry.push_back(static_cast<char>(0));  // color_id (legacy field; RGB below is authoritative when present)
        entry.push_back(static_cast<char>(Pad7FirstByte));
        appendU16BE(entry, ++orderCounter);  // see confidence notes: shape-matched, not confirmed semantics
        entry += std::string(4, '\0');
        appendU16BE(entry, 0);  // loop_numerator
        appendU16BE(entry, 0);  // loop_denominator
        appendU32BE(entry, 0);  // len_comment (v1: no comment support)
        if (hasColor) {
            auto [r, g, b] = *cue.color;
            entry.push_back(static_cast<char>(0));  // color_code: unknown required semantics, RGB is authoritative
            entry.push_back(static_cast<char>(r));
            entry.push_back(static_cast<char>(g));
            entry.push_back(static_cast<char>(b));
        }

        if (entry.size() != lenEntry) {
            throw std::logic_error("AnlzCueCodec: encoded entry size mismatch (internal bug)");
        }
        body += entry;
    }

    std::string section;
    section += "PCO2";
    appendU32BE(section, 20);  // len_header: fourcc+len_header+len_tag+type+num_cues+pad, matches real data
    appendU32BE(section, static_cast<uint32_t>(12 + body.size()));  // len_tag
    section += body;
    return section;
}

}  // namespace seabass::infrastructure::rekordbox
