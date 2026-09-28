// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"

#include <optional>
#include <stdexcept>

#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

// Every value here is what real rekordbox-written files hold; see the
// survey in anlz_legacy_cue_codec.hpp for the counts behind each.
constexpr uint32_t PcobFourcc = 0x50434f42;  // "PCOB"
constexpr uint32_t PcptFourcc = 0x50435054;  // "PCPT"
constexpr uint32_t SectionHeaderSize = 24;
constexpr uint32_t EntryHeaderSize = 28;
constexpr uint32_t EntrySize = 56;
constexpr uint32_t OrderSentinel = 0xFFFF;
constexpr uint32_t NotALoopSentinel = 0xFFFFFFFFu;
constexpr uint32_t ModernStatus = 0;
constexpr uint32_t ModernUnknown1 = 0x00010000u;
constexpr unsigned char AfterTypeTemplate[3] = {0x00, 0x03, 0xE8};
// What a section created from scratch gets; a section being replaced
// keeps whatever it already had. See the header's survey.
constexpr uint32_t DefaultMemoryCount = 0xFFFFFFFFu;
constexpr uint32_t PopulatedMemoryListCount = 0;

void appendU32(std::string &out, uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendU16(std::string &out, uint16_t value)
{
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

std::string encodeEntry(const LegacyCueEntry &cue)
{
    if (!cue.rawBytes.empty()) {
        return cue.rawBytes;
    }
    std::string out;
    out.reserve(EntrySize);
    appendU32(out, PcptFourcc);
    appendU32(out, EntryHeaderSize);
    appendU32(out, EntrySize);
    appendU32(out, cue.hotCueNumber);
    appendU32(out, ModernStatus);
    appendU32(out, ModernUnknown1);
    appendU16(out, static_cast<uint16_t>(OrderSentinel));
    appendU16(out, static_cast<uint16_t>(OrderSentinel));
    out.push_back(static_cast<char>(cue.isLoop ? 2 : 1));
    out.append(reinterpret_cast<const char *>(AfterTypeTemplate), sizeof(AfterTypeTemplate));
    appendU32(out, cue.timeMs);
    appendU32(out, cue.isLoop ? cue.loopEndMs : NotALoopSentinel);
    out.append(EntrySize - out.size(), '\0');
    return out;
}

}  // namespace

void AnlzLegacyCueCodec::checkEntry(const std::string &entryBytes)
{
    if (entryBytes.size() != EntrySize) {
        throw std::runtime_error("PCOB entry of " + std::to_string(entryBytes.size()) + " bytes, not 56");
    }
    if (readU32BE(entryBytes, 0) != PcptFourcc) {
        throw std::runtime_error("PCOB entry does not start with PCPT");
    }
    if (readU32BE(entryBytes, 4) != EntryHeaderSize || readU32BE(entryBytes, 8) != EntrySize) {
        throw std::runtime_error("PCOB entry declares len_header " + std::to_string(readU32BE(entryBytes, 4))
                                 + " and len_entry " + std::to_string(readU32BE(entryBytes, 8)) + ", not 28 and 56");
    }
}

void AnlzLegacyCueCodec::checkSection(const std::string &pcobSectionBytes)
{
    const std::string &s = pcobSectionBytes;
    if (s.size() < SectionHeaderSize || readU32BE(s, 0) != PcobFourcc) {
        throw std::runtime_error("not a PCOB section");
    }
    if (readU32BE(s, 4) != SectionHeaderSize) {
        throw std::runtime_error("PCOB len_header is " + std::to_string(readU32BE(s, 4)) + ", not 24");
    }
    const uint16_t numCues = readU16BE(s, 18);
    const uint64_t expected = SectionHeaderSize + uint64_t(EntrySize) * numCues;
    if (readU32BE(s, 8) != expected || s.size() != expected) {
        throw std::runtime_error("PCOB of " + std::to_string(numCues) + " entries is " + std::to_string(s.size())
                                 + " bytes with len_tag " + std::to_string(readU32BE(s, 8)) + ", not "
                                 + std::to_string(expected));
    }
    if (readU16BE(s, 16) != 0) {
        throw std::runtime_error("PCOB has a nonzero field before num_cues");
    }
    for (uint16_t i = 0; i < numCues; ++i) {
        checkEntry(s.substr(SectionHeaderSize + size_t(EntrySize) * i, EntrySize));
    }
}

std::vector<LegacyCueEntry> AnlzLegacyCueCodec::decodeCues(const std::string &pcobSectionBytes)
{
    std::vector<LegacyCueEntry> result;
    if (pcobSectionBytes.size() < SectionHeaderSize) {
        return result;
    }
    // Only a section in the shape rekordbox writes is read at all. This
    // used to take each entry's own len_entry on trust and never look at
    // its magic, so a damaged list decoded into entries whose rawBytes
    // were not PCPT entries -- and the writer, carrying an entry over on
    // slot and time, wrote those bytes back verbatim. A section that
    // fails here is one the caller treats as damaged and rebuilds.
    checkSection(pcobSectionBytes);
    const uint16_t numCues = readU16BE(pcobSectionBytes, 18);

    for (uint16_t i = 0; i < numCues; ++i) {
        const size_t offset = SectionHeaderSize + size_t(EntrySize) * i;
        LegacyCueEntry entry;
        entry.hotCueNumber = readU32BE(pcobSectionBytes, offset + 12);
        const unsigned char entryType = static_cast<unsigned char>(pcobSectionBytes[offset + 28]);
        entry.timeMs = readU32BE(pcobSectionBytes, offset + 32);
        const uint32_t loopTime = readU32BE(pcobSectionBytes, offset + 36);
        entry.isLoop = entryType == 2;
        entry.loopEndMs = entry.isLoop ? loopTime : 0;
        // Kept whole, so an entry nobody edited is written back byte for
        // byte -- including the fields this struct does not model and the
        // older writer generation's own values for the ones it does.
        entry.rawBytes = pcobSectionBytes.substr(offset, EntrySize);
        result.push_back(std::move(entry));
    }
    return result;
}

uint32_t AnlzLegacyCueCodec::memoryCountOf(const std::string &pcobSectionBytes)
{
    if (pcobSectionBytes.size() < SectionHeaderSize) {
        return DefaultMemoryCount;
    }
    return readU32BE(pcobSectionBytes, 20);
}

std::string AnlzLegacyCueCodec::encodeCues(const std::vector<LegacyCueEntry> &cues, uint32_t listType,
                                            std::optional<uint32_t> memoryCount)
{
    // Written in the order given, deliberately. Ordering is the caller's
    // decision (RekordboxCueWriter sorts a list it is rebuilding), and
    // doing it here would mean a section read from a file and written
    // straight back could come out reordered -- which is a change to a
    // file nobody asked to change, and it cost this codec its
    // byte-for-byte round trip on the six real sections written by the
    // older, ascending generation.
    const std::vector<LegacyCueEntry> &ordered = cues;

    std::string entries;
    for (const auto &cue : ordered) {
        const std::string bytes = encodeEntry(cue);
        // Refused rather than written: carried-over bytes are the only
        // way anything but a fresh entry gets here, and whatever put a
        // bad one in `cues`, it must not reach a stick.
        try {
            checkEntry(bytes);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("refusing to write a legacy cue entry that is not one: ") + e.what());
        }
        entries += bytes;
    }

    std::string out;
    out.reserve(SectionHeaderSize + entries.size());
    appendU32(out, PcobFourcc);
    appendU32(out, SectionHeaderSize);
    appendU32(out, static_cast<uint32_t>(SectionHeaderSize + entries.size()));
    appendU32(out, listType);
    appendU16(out, 0);
    appendU16(out, static_cast<uint16_t>(ordered.size()));
    const uint32_t forNewSection = (listType == CueListTypeMemory && !ordered.empty())
        ? PopulatedMemoryListCount
        : DefaultMemoryCount;
    appendU32(out, memoryCount.value_or(forNewSection));
    out += entries;
    checkSection(out);
    return out;
}

}  // namespace seabass::infrastructure::rekordbox
