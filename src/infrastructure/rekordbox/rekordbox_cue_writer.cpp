// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace
{

constexpr uint32_t Pco2Fourcc = 0x50434f32;  // "PCO2"

// This writer keeps THREE lists in step, because a real export has
// three and rekordbox maintains all of them:
//
//   PCO2 in ANLZ0000.EXT   the modern list, every hot cue plus colour
//                          and comment, read by Nexus2 and later
//   PCOB in ANLZ0000.DAT   the legacy list, hot cues 1-3
//   PCOB in ANLZ0000.EXT   the legacy list, hot cues 4-8
//
// It used to write PCO2 alone, on the reasoning that PCO2 is a strict
// superset and well-behaved readers prefer it. That reasoning was
// wrong about real hardware, and this file used to say so as a known
// gap -- "everywhere except pre-Nexus2 hardware, which would see a
// stale PCOB". An XDJ-RX2 then showed exactly that: three pads lit from
// the untouched legacy lists while the two cues a sync had just written
// into PCO2 were invisible (issue #33). The .DAT file had not been
// opened since 2017.
//
// What stopped it being implemented before was that no real PCOB write
// had been captured to check against, and this project does not write a
// format it has only read about. That is now answered by the survey in
// anlz_legacy_cue_codec.hpp: 7968 real PCOB sections, 119 real entries,
// every field fixed or explained. The split above is the part that
// cannot be guessed from the spec -- .DAT holds 1-3 and .EXT holds 4-8,
// never both -- and putting every cue in both lists would give an older
// player pads that should not be lit.

bool isCueListSection(const AnlzRawSection &section, uint32_t listType)
{
    return section.fourcc == Pco2Fourcc && section.rawBytes.size() >= 16 &&
           readU32BE(section.rawBytes, 12) == listType;
}

std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> parseColor(const std::string &color)
{
    if (color.size() == 7 && color[0] == '#') {
        auto hexByte = [&](size_t pos) {
            return static_cast<uint8_t>(std::stoi(color.substr(pos, 2), nullptr, 16));
        };
        return std::make_tuple(hexByte(1), hexByte(3), hexByte(5));
    }
    return std::nullopt;
}

// Overwrites the existing PCO2 section of `listType` with `entries`, or
// appends a freshly encoded one if the file doesn't have one yet (a track
// with no memory cues at all may genuinely have no memory-cues PCO2
// section on disk -- see anlz_cue_codec.hpp's confidence notes for the
// real empty-section example this shape is confirmed against). Does
// nothing if there's no existing section AND nothing to write, so a
// track that has and needs neither list is left byte-for-byte untouched.
void writeCueList(AnlzFile &file, uint32_t listType, const std::vector<RawHotCueEntry> &entries)
{
    auto sectionIt = std::find_if(file.sections.begin(), file.sections.end(),
                                   [listType](const AnlzRawSection &s) { return isCueListSection(s, listType); });
    if (sectionIt == file.sections.end() && entries.empty()) {
        return;
    }
    std::string encoded = AnlzCueCodec::encodeHotCues(entries, listType);
    if (sectionIt != file.sections.end()) {
        sectionIt->rawBytes = encoded;
    } else {
        file.sections.push_back({Pco2Fourcc, encoded});
    }
}

constexpr uint32_t PcobFourcc = 0x50434f42;  // "PCOB"

bool isLegacyCueListSection(const AnlzRawSection &section, uint32_t listType)
{
    return section.fourcc == PcobFourcc && section.rawBytes.size() >= 20 &&
           readU32BE(section.rawBytes, 12) == listType;
}

// The legacy list of `listType` as the file holds it now, entry bytes
// and all. Empty when the file has no such section, which is normal.
std::vector<LegacyCueEntry> existingLegacyCues(const AnlzFile &file, uint32_t listType)
{
    for (const auto &section : file.sections) {
        if (!isLegacyCueListSection(section, listType)) {
            continue;
        }
        try {
            return AnlzLegacyCueCodec::decodeCues(section.rawBytes);
        } catch (const std::exception &) {
            // Damaged on the stick: the rewrite below replaces the
            // section wholesale, which is how such damage gets repaired.
            return {};
        }
    }
    return {};
}

// Same rule writeCueList() follows for PCO2: replace the section if the
// file has one, append one if it does not and there is something to
// write, and leave a file that has neither and needs neither untouched.
void writeLegacyCueList(AnlzFile &file, uint32_t listType, const std::vector<LegacyCueEntry> &entries)
{
    auto sectionIt = std::find_if(file.sections.begin(), file.sections.end(),
                                   [listType](const AnlzRawSection &s) {
                                       return isLegacyCueListSection(s, listType);
                                   });
    if (sectionIt == file.sections.end() && entries.empty()) {
        return;
    }
    if (sectionIt != file.sections.end()) {
        // Replacing: the section's own memory_count goes back with it.
        sectionIt->rawBytes =
            AnlzLegacyCueCodec::encodeCues(entries, listType, AnlzLegacyCueCodec::memoryCountOf(sectionIt->rawBytes));
    } else {
        file.sections.push_back({PcobFourcc, AnlzLegacyCueCodec::encodeCues(entries, listType)});
    }
}

// A cue whose bytes the file already had is written back unchanged; one
// that is new or has moved is built fresh. Matched on slot and position,
// the only two fields a legacy entry carries that the domain model also
// has, and each raw entry is handed out once.
std::optional<LegacyCueEntry> carryOverLegacy(const LegacyCueEntry &wanted, std::vector<LegacyCueEntry> &from)
{
    for (auto &have : from) {
        if (have.rawBytes.empty() || have.hotCueNumber != wanted.hotCueNumber || have.timeMs != wanted.timeMs
            || have.isLoop != wanted.isLoop || (have.isLoop && have.loopEndMs != wanted.loopEndMs)) {
            continue;
        }
        LegacyCueEntry taken = have;
        have.rawBytes.clear();  // consumed
        return taken;
    }
    return std::nullopt;
}

}  // namespace

std::vector<std::string> rekordboxCueFilesFor(const std::string &pioneerRoot, const std::string &analyzePath)
{
    std::vector<std::string> files{extAnlzPath(pioneerRoot, analyzePath)};
    const std::string dat = datAnlzPath(pioneerRoot, analyzePath);
    if (std::filesystem::exists(dat)) {
        files.push_back(dat);
    }
    return files;
}

RekordboxCueWriter::RekordboxCueWriter(std::string pioneerRoot) : m_pioneerRoot(std::move(pioneerRoot)) {}

RekordboxCueWriter::RekordboxCueWriter(std::string pioneerRoot, const AnlzPathIndex *pathIndex)
    : m_pioneerRoot(std::move(pioneerRoot)), m_pathIndex(pathIndex)
{
}

std::optional<std::string> RekordboxCueWriter::analyzePathFor(uint32_t trackId) const
{
    return m_pathIndex ? m_pathIndex->pathFor(trackId) : findAnlzPathForTrackId(m_pioneerRoot, trackId);
}

void RekordboxCueWriter::writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues)
{
    uint32_t trackId = static_cast<uint32_t>(std::stoul(trackSourceId));

    auto analyzePath = analyzePathFor(trackId);
    if (!analyzePath) {
        throw std::runtime_error("no rekordbox track with id=" + trackSourceId + " (or it has no analysis file)");
    }
    std::string extPath = extAnlzPath(m_pioneerRoot, *analyzePath);

    auto file = AnlzFile::readRaw(extPath);

    // Whatever the file holds now, with each entry's exact bytes. A cue
    // in the new list that matches one of these (same slot or time, same
    // loop, a colour that does not contradict) is written back verbatim,
    // so its comment, legacy colour id and loop survive a rewrite the
    // domain model cannot represent in full.
    auto existing = [&](uint32_t listType) {
        std::vector<RawHotCueEntry> entries;
        for (const auto &section : file.sections) {
            if (isCueListSection(section, listType)) {
                try {
                    entries = AnlzCueCodec::decodeHotCues(section.rawBytes, listType);
                } catch (const std::exception &) {
                    // A damaged list on the stick: nothing to carry over.
                    // The rewrite below replaces the section wholesale,
                    // which is what every write did before carry-over
                    // existed and how such damage gets repaired.
                    entries.clear();
                }
                break;
            }
        }
        return entries;
    };
    std::vector<RawHotCueEntry> existingHot = existing(CueListTypeHot);
    std::vector<RawHotCueEntry> existingMemory = existing(CueListTypeMemory);
    // Each raw entry is handed out once (two memory cues at one position
    // must not both inherit the same bytes), and a cue whose colour is
    // new, or set where the file had none, is encoded fresh so the
    // colour reaches the file.
    auto carryOver = [](const RawHotCueEntry &wanted, std::vector<RawHotCueEntry> &from) -> std::optional<RawHotCueEntry> {
        for (auto &have : from) {
            if (have.rawBytes.empty() || have.hotCueNumber != wanted.hotCueNumber || have.timeMs != wanted.timeMs
                || have.isLoop != wanted.isLoop || (have.isLoop && have.loopEndMs != wanted.loopEndMs)) {
                continue;
            }
            if (wanted.color && (!have.color || *wanted.color != *have.color)) {
                continue;  // a genuinely new colour: encode it fresh
            }
            RawHotCueEntry taken = have;
            have.rawBytes.clear();  // consumed
            return taken;
        }
        return std::nullopt;
    };

    std::vector<RawHotCueEntry> hotEntries;
    std::vector<RawHotCueEntry> memoryEntries;
    for (const auto &cue : cues) {
        RawHotCueEntry entry;
        entry.timeMs = static_cast<uint32_t>(cue.positionMs);
        entry.color = parseColor(cue.color);
        entry.isLoop = cue.isLoop;
        entry.loopEndMs = cue.isLoop ? static_cast<uint32_t>(cue.loopEndMs) : 0;
        if (cue.kind == domain::CuePoint::Kind::Hot) {
            entry.hotCueNumber = static_cast<uint32_t>(cue.hotCueNumber);
            hotEntries.push_back(carryOver(entry, existingHot).value_or(entry));
        } else {
            entry.hotCueNumber = 0;  // memory cues carry no hot-cue slot
            memoryEntries.push_back(carryOver(entry, existingMemory).value_or(entry));
        }
    }

    writeCueList(file, CueListTypeHot, hotEntries);
    writeCueList(file, CueListTypeMemory, memoryEntries);

    // The legacy lists, from the same cues, so the three never disagree.
    // Built from `cues` rather than from the PCO2 entries above, because
    // those may carry raw PCO2 bytes that must never reach a PCOB.
    auto legacyOf = [&](bool wantHot, uint32_t lowSlot, uint32_t highSlot) {
        std::vector<LegacyCueEntry> out;
        for (const auto &cue : cues) {
            const bool isHot = cue.kind == domain::CuePoint::Kind::Hot;
            if (isHot != wantHot) {
                continue;
            }
            const uint32_t slot = isHot ? static_cast<uint32_t>(cue.hotCueNumber) : 0;
            if (isHot && (slot < lowSlot || slot > highSlot)) {
                continue;
            }
            LegacyCueEntry entry;
            entry.hotCueNumber = slot;
            entry.timeMs = static_cast<uint32_t>(cue.positionMs);
            entry.isLoop = cue.isLoop;
            entry.loopEndMs = cue.isLoop ? static_cast<uint32_t>(cue.loopEndMs) : 0;
            out.push_back(std::move(entry));
        }
        // Descending slot, which is how real hot lists written by the
        // current rekordbox generation are ordered (45 of 51 real
        // sections; see anlz_legacy_cue_codec.hpp). A memory list has no
        // slot to sort by, so it keeps the order the cues arrived in.
        if (wantHot) {
            std::stable_sort(out.begin(), out.end(), [](const LegacyCueEntry &a, const LegacyCueEntry &b) {
                return a.hotCueNumber > b.hotCueNumber;
            });
        }
        return out;
    };

    auto carriedOver = [](std::vector<LegacyCueEntry> wanted, std::vector<LegacyCueEntry> have) {
        for (auto &entry : wanted) {
            if (auto kept = carryOverLegacy(entry, have)) {
                entry = *kept;
            }
        }
        return wanted;
    };

    // Hot cues 4-8 and nothing else go in the .EXT file's own PCOB.
    writeLegacyCueList(file, CueListTypeHot,
                        carriedOver(legacyOf(true, 4, 8), existingLegacyCues(file, CueListTypeHot)));
    file.writeRaw(extPath);

    // Hot cues 1-3 and the memory cues go in the .DAT file's. A track
    // whose .DAT is missing keeps working: the .EXT above is already
    // written, and a player that reads only .DAT had nothing to read
    // for this track in the first place.
    const std::string datPath = datAnlzPath(m_pioneerRoot, *analyzePath);
    if (!std::filesystem::exists(datPath)) {
        return;
    }
    auto datFile = AnlzFile::readRaw(datPath);
    auto legacySectionBytes = [&datFile]() {
        std::string joined;
        for (const auto &section : datFile.sections) {
            if (section.fourcc == PcobFourcc) {
                joined += section.rawBytes;
            }
        }
        return joined;
    };
    const std::string before = legacySectionBytes();
    writeLegacyCueList(datFile, CueListTypeHot,
                        carriedOver(legacyOf(true, 1, 3), existingLegacyCues(datFile, CueListTypeHot)));
    writeLegacyCueList(datFile, CueListTypeMemory,
                        carriedOver(legacyOf(false, 0, 0), existingLegacyCues(datFile, CueListTypeMemory)));
    // Only when something actually moved. Most saves change hot cues in
    // slots this file does not hold, and rewriting it anyway would cost
    // a durable write per track and put every .DAT on the stick into the
    // next incremental backup for nothing.
    if (legacySectionBytes() != before) {
        datFile.writeRaw(datPath);
    }
}

}  // namespace seabass::infrastructure::rekordbox
