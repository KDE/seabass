// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/cue_list_check.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "domain/local_restore.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

// The same constants anlz_legacy_cue_codec.cpp works from; see the survey
// in its header for where each comes from.
constexpr uint32_t PcobFourcc = 0x50434f42;  // "PCOB"
constexpr uint32_t PcptFourcc = 0x50435054;  // "PCPT"
constexpr uint32_t Pco2Fourcc = 0x50434f32;  // "PCO2"
constexpr uint32_t PmaiFourcc = 0x504d4149;  // "PMAI"
constexpr size_t SectionHeaderSize = 24;
constexpr size_t EntrySize = 56;
constexpr uint16_t OrderSentinel = 0xFFFF;
constexpr uint32_t NoLastEntry = 0xFFFFFFFFu;

bool allZero(const std::string &bytes, size_t from)
{
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.end(),
                       [](char c) { return c == '\0'; });
}

// Entries at their fixed slots, with the links and header checked but not
// required: the lenient read that checkSection() deliberately is not.
std::vector<LegacyCueEntry> entriesOf(const std::string &s, int count)
{
    std::vector<LegacyCueEntry> out;
    for (int i = 0; i < count; ++i) {
        const size_t offset = SectionHeaderSize + EntrySize * static_cast<size_t>(i);
        LegacyCueEntry entry;
        entry.rawBytes = s.substr(offset, EntrySize);
        AnlzLegacyCueCodec::checkEntry(entry.rawBytes);
        entry.hotCueNumber = readU32BE(entry.rawBytes, 12);
        const unsigned char type = static_cast<unsigned char>(entry.rawBytes[28]);
        entry.timeMs = readU32BE(entry.rawBytes, 32);
        entry.isLoop = type == 2;
        entry.loopEndMs = entry.isLoop ? readU32BE(entry.rawBytes, 36) : 0;
        out.push_back(std::move(entry));
    }
    return out;
}

ListedCue listed(uint32_t pad, uint32_t timeMs, bool isLoop, uint32_t loopEndMs)
{
    ListedCue cue;
    cue.pad = pad;
    cue.timeMs = timeMs;
    cue.isLoop = isLoop;
    cue.loopEndMs = isLoop ? loopEndMs : 0;
    return cue;
}

// Whether every cue of one list has its own partner in the other, cues
// paired in order of pad, kind (a loop pairs only with a loop) and
// position. Within one pad and kind the positions lie on a line, where
// pairing in sorted order is the pairing whose largest gap is smallest,
// so no pairing another order would find within the tolerance is missed.
bool listsMatch(std::vector<ListedCue> a, std::vector<ListedCue> b, bool byPad)
{
    if (a.size() != b.size()) {
        return false;
    }
    const auto order = [byPad](const ListedCue &x, const ListedCue &y) {
        if (byPad && x.pad != y.pad) {
            return x.pad < y.pad;
        }
        if (x.isLoop != y.isLoop) {
            return y.isLoop;
        }
        return x.timeMs < y.timeMs;
    };
    std::stable_sort(a.begin(), a.end(), order);
    std::stable_sort(b.begin(), b.end(), order);
    for (size_t i = 0; i < a.size(); ++i) {
        if ((byPad && a[i].pad != b[i].pad) || !sameListedCue(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

// Walks the sections of an analysis file of `end` bytes, `readAt` giving
// the bytes at an offset, and keeps the PCOB and PCO2 ones: the shared
// walk of readCueSections() and cueSectionsOfBytes().
template<typename ReadAt>
std::vector<AnlzRawSection> walkCueSections(uint64_t end, const std::string &path, ReadAt readAt)
{
    if (end < 12) {
        throw std::runtime_error(path + " is too short to be an analysis file");
    }
    const std::string header = readAt(0, 12);
    if (readU32BE(header, 0) != PmaiFourcc) {
        throw std::runtime_error(path + " is not an analysis file (no PMAI header)");
    }
    const uint64_t lenHeader = readU32BE(header, 4);
    const uint64_t lenFile = std::min<uint64_t>(readU32BE(header, 8), end);
    std::vector<AnlzRawSection> out;
    uint64_t pos = lenHeader;
    while (pos + 12 <= lenFile) {
        const std::string sectionHeader = readAt(pos, 12);
        const uint32_t fourcc = readU32BE(sectionHeader, 0);
        const uint32_t lenTag = readU32BE(sectionHeader, 8);
        if (lenTag < 12 || pos + lenTag > lenFile) {
            throw std::runtime_error(path + ": the section at offset " + std::to_string(pos)
                                     + " has an impossible length");
        }
        if (fourcc == PcobFourcc || fourcc == Pco2Fourcc) {
            out.push_back({fourcc, readAt(pos, lenTag)});
        }
        pos += lenTag;
    }
    return out;
}

}  // namespace

LegacyMemoryListShape auditLegacyMemoryList(const std::string &s)
{
    LegacyMemoryListShape shape;
    if (s.size() < SectionHeaderSize || readU32BE(s, 0) != PcobFourcc) {
        shape.malformed = "not a PCOB section";
        return shape;
    }
    if (readU32BE(s, 4) != SectionHeaderSize) {
        shape.malformed = "len_header is " + std::to_string(readU32BE(s, 4)) + ", not 24";
        return shape;
    }
    if (readU32BE(s, 12) != CueListTypeMemory) {
        shape.malformed = "a hot cue list, not the memory list";
        return shape;
    }
    const uint32_t lenTag = readU32BE(s, 8);
    const uint16_t count = readU16BE(s, 18);
    shape.entries = count;
    const size_t expected = SectionHeaderSize + EntrySize * count;
    if (lenTag != s.size()) {
        shape.malformed = "len_tag " + std::to_string(lenTag) + " for a section of " + std::to_string(s.size())
                          + " bytes";
        return shape;
    }
    if (s.size() < expected) {
        shape.malformed = "declares " + std::to_string(count) + " entries in " + std::to_string(s.size()) + " bytes";
        return shape;
    }
    if (s.size() > expected) {
        if (!allZero(s, expected)) {
            shape.malformed = std::to_string(s.size() - expected) + " bytes past the last entry that are not zero";
            return shape;
        }
        shape.zeroSlots = true;
    }
    try {
        entriesOf(s, count);
    } catch (const std::exception &e) {
        shape.malformed = e.what();
        return shape;
    }

    const uint32_t last = readU32BE(s, 20);
    const uint32_t wantLast = count == 0 ? NoLastEntry : static_cast<uint32_t>(count - 1);
    shape.headerStale = last != wantLast;

    for (uint16_t i = 0; i < count; ++i) {
        const size_t offset = SectionHeaderSize + EntrySize * i;
        const uint16_t previous = readU16BE(s, offset + 24);
        const uint16_t next = readU16BE(s, offset + 26);
        const uint16_t wantPrevious = i == 0 ? OrderSentinel : static_cast<uint16_t>(i - 1);
        const uint16_t wantNext = i + 1 == count ? OrderSentinel : static_cast<uint16_t>(i + 1);
        if (previous != wantPrevious || next != wantNext) {
            shape.unlinked = true;
            break;
        }
    }
    return shape;
}

std::string repairLegacyMemoryList(const std::string &s)
{
    const LegacyMemoryListShape shape = auditLegacyMemoryList(s);
    if (!shape.malformed.empty()) {
        throw std::runtime_error("legacy memory list cannot be repaired: " + shape.malformed);
    }
    if (!shape.damaged()) {
        throw std::runtime_error("legacy memory list is not damaged");
    }
    // Every entry byte for byte, the encoder setting the header's last
    // entry and each entry's links from the list; the zero slots are
    // simply not there any more. The player's own entries survive, which
    // is the point: a rebuild from PCO2 would lose what the RX2 saved.
    return AnlzLegacyCueCodec::encodeCues(entriesOf(s, shape.entries), CueListTypeMemory);
}

std::optional<CueListDisagreement> compareCueLists(const TrackCueLists &lists)
{
    CueListDisagreement d;
    d.hot = !listsMatch(lists.legacyHot, lists.modernHot, /*byPad=*/true);
    d.memory = !listsMatch(lists.legacyMemory, lists.modernMemory, /*byPad=*/false);
    if (!d.any()) {
        return std::nullopt;
    }
    for (const auto *list : {&lists.legacyHot, &lists.modernHot}) {
        for (const auto &cue : *list) {
            if (cue.pad < 1 || cue.pad > 8) {
                d.unrepairable = "a hot cue on pad " + std::to_string(cue.pad) + ", which the lists cannot both hold";
            }
        }
    }
    d.seabassHot = seabassView(lists.modernHot, lists.legacyHot, true);
    d.seabassMemory = seabassView(lists.modernMemory, lists.legacyMemory, false);
    d.viewsAgree = listsMatch(lists.legacyHot, d.seabassHot, true) && listsMatch(lists.legacyMemory, d.seabassMemory, false);
    if (lists.extLegacyMemory > 0) {
        d.unrepairable = "memory cues in the .EXT's legacy list, where no writer puts them";
    }
    d.lists = lists;
    return d;
}

std::vector<AnlzRawSection> readCueSections(const std::string &path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error(path + " could not be opened");
    }
    in.seekg(0, std::ios::end);
    const auto end = static_cast<uint64_t>(in.tellg());
    return walkCueSections(end, path, [&in, &path](uint64_t offset, size_t length) {
        std::string bytes(length, '\0');
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(bytes.data(), static_cast<std::streamsize>(length));
        if (static_cast<size_t>(in.gcount()) != length) {
            throw std::runtime_error(path + " ends inside a section");
        }
        return bytes;
    });
}

std::vector<AnlzRawSection> cueSectionsOfBytes(const std::string &bytes, const std::string &label)
{
    return walkCueSections(bytes.size(), label, [&bytes, &label](uint64_t offset, size_t length) {
        if (offset + length > bytes.size()) {
            throw std::runtime_error(label + " ends inside a section");
        }
        return bytes.substr(static_cast<size_t>(offset), length);
    });
}

bool isMemoryPcob(const AnlzRawSection &section)
{
    return section.fourcc == PcobFourcc && section.rawBytes.size() >= SectionHeaderSize
           && readU32BE(section.rawBytes, 12) == CueListTypeMemory;
}

TrackCueLists cueListsOf(const std::vector<AnlzRawSection> &dat, const std::vector<AnlzRawSection> &ext,
                         const std::string &datPath, const std::string &extPath)
{
    TrackCueLists lists;
    auto legacy = [&lists](const AnlzRawSection &section, const std::string &path, bool inExt) {
        const uint32_t type = section.rawBytes.size() >= 16 ? readU32BE(section.rawBytes, 12) : 0xFFFFFFFFu;
        if (type == CueListTypeMemory) {
            const LegacyMemoryListShape shape = auditLegacyMemoryList(section.rawBytes);
            if (!shape.malformed.empty()) {
                throw std::runtime_error(path + ": legacy memory list: " + shape.malformed);
            }
            for (const auto &entry : entriesOf(section.rawBytes, shape.entries)) {
                lists.legacyMemory.push_back(listed(0, entry.timeMs, entry.isLoop, entry.loopEndMs));
                if (inExt) {
                    ++lists.extLegacyMemory;
                }
            }
            return;
        }
        if (type != CueListTypeHot) {
            throw std::runtime_error(path + ": a legacy cue list of type " + std::to_string(type));
        }
        try {
            for (const auto &entry : AnlzLegacyCueCodec::decodeCues(section.rawBytes)) {
                lists.legacyHot.push_back(listed(entry.hotCueNumber, entry.timeMs, entry.isLoop, entry.loopEndMs));
            }
        } catch (const std::exception &e) {
            throw std::runtime_error(path + ": legacy hot cue list: " + e.what());
        }
    };
    for (const auto &section : dat) {
        if (section.fourcc == PcobFourcc) {
            legacy(section, datPath, false);
        }
    }
    for (const auto &section : ext) {
        if (section.fourcc == PcobFourcc) {
            legacy(section, extPath, true);
            continue;
        }
        if (section.fourcc != Pco2Fourcc || section.rawBytes.size() < 16) {
            continue;
        }
        const uint32_t type = readU32BE(section.rawBytes, 12);
        if (type != CueListTypeHot && type != CueListTypeMemory) {
            throw std::runtime_error(extPath + ": a cue list of type " + std::to_string(type));
        }
        try {
            for (const auto &entry : AnlzCueCodec::decodeHotCues(section.rawBytes, type)) {
                ListedCue cue = listed(type == CueListTypeHot ? entry.hotCueNumber : 0, entry.timeMs, entry.isLoop,
                                       entry.loopEndMs);
                cue.color = entry.color;
                (type == CueListTypeHot ? lists.modernHot : lists.modernMemory).push_back(cue);
            }
        } catch (const std::exception &e) {
            throw std::runtime_error(extPath + ": cue list: " + e.what());
        }
    }
    return lists;
}

bool sameListedCue(const ListedCue &a, const ListedCue &b)
{
    const double tolerance = domain::LocalRestorePlanner::PositionToleranceMs;
    if (std::abs(double(a.timeMs) - double(b.timeMs)) > tolerance || a.isLoop != b.isLoop) {
        return false;
    }
    return !a.isLoop || std::abs(double(a.loopEndMs) - double(b.loopEndMs)) <= tolerance;
}

std::vector<ListedCue> seabassView(const std::vector<ListedCue> &modern, const std::vector<ListedCue> &legacy,
                                   bool hot)
{
    const double tolerance = domain::LocalRestorePlanner::PositionToleranceMs;
    std::vector<ListedCue> out = modern;
    for (const auto &cue : legacy) {
        const bool known = std::any_of(out.begin(), out.end(), [&](const ListedCue &have) {
            return hot ? have.pad == cue.pad
                       : std::abs(double(have.timeMs) - double(cue.timeMs)) <= tolerance;
        });
        if (!known) {
            out.push_back(cue);
        }
    }
    return out;
}

CueListsCompared compareCueSections(const std::vector<AnlzRawSection> &dat, const std::vector<AnlzRawSection> &ext,
                                    const std::string &datLabel, const std::string &extLabel)
{
    CueListsCompared compared;
    for (const auto &section : dat) {
        if (isMemoryPcob(section)) {
            compared.shape = auditLegacyMemoryList(section.rawBytes);
            break;
        }
    }
    // A memory list this cannot read is reported as such, and the lists
    // are not compared: there is no telling what it holds.
    if (!compared.shape.malformed.empty()) {
        return compared;
    }
    try {
        compared.disagreement = compareCueLists(cueListsOf(dat, ext, datLabel, extLabel));
    } catch (const std::exception &e) {
        compared.listsMalformed = e.what();
    }
    return compared;
}

}  // namespace seabass::infrastructure::rekordbox
