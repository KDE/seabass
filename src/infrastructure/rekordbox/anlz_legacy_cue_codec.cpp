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

std::vector<LegacyCueEntry> AnlzLegacyCueCodec::decodeCues(const std::string &pcobSectionBytes)
{
    std::vector<LegacyCueEntry> result;
    if (pcobSectionBytes.size() < SectionHeaderSize) {
        return result;
    }
    const uint16_t numCues = readU16BE(pcobSectionBytes, 18);

    size_t offset = SectionHeaderSize;
    for (uint16_t i = 0; i < numCues; ++i) {
        if (offset + EntryHeaderSize > pcobSectionBytes.size()) {
            throw std::runtime_error("PCOB section truncated while decoding cue entries");
        }
        const uint32_t lenEntry = readU32BE(pcobSectionBytes, offset + 8);
        if (lenEntry < EntryHeaderSize || offset + lenEntry > pcobSectionBytes.size()) {
            throw std::runtime_error("PCOB cue entry declares a length the section cannot hold");
        }
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
        entry.rawBytes = pcobSectionBytes.substr(offset, lenEntry);
        result.push_back(std::move(entry));
        offset += lenEntry;
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
        entries += encodeEntry(cue);
    }

    std::string out;
    out.reserve(SectionHeaderSize + entries.size());
    appendU32(out, 0x50434f42);  // "PCOB"
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
    return out;
}

}  // namespace seabass::infrastructure::rekordbox
