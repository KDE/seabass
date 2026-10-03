// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include "infrastructure/fs_remove.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace fs = std::filesystem;

namespace
{

// The same constants anlz_legacy_cue_codec.cpp works from; see the survey
// in its header for where each comes from.
constexpr uint32_t PcobFourcc = 0x50434f42;  // "PCOB"
constexpr uint32_t PcptFourcc = 0x50435054;  // "PCPT"
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

// A .DAT the RX2 wrote on its way down: its beat grid and both waveform
// previews are the empty sections (24 and 20 bytes, header only), where
// any analysis rekordbox exported has beats and pixels in all three.
bool isSkeleton(const std::string &datPath)
{
    try {
        const AnlzFile file = AnlzFile::readRaw(datPath);
        bool grid = false;
        bool wave = false;
        bool wave2 = false;
        for (const auto &section : file.sections) {
            switch (section.fourcc) {
            case 0x5051545A:  // "PQTZ"
                grid = section.rawBytes.size() == 24;
                break;
            case 0x50574156:  // "PWAV"
                wave = section.rawBytes.size() == 20;
                break;
            case 0x50575632:  // "PWV2"
                wave2 = section.rawBytes.size() == 20;
                break;
            default:
                break;
            }
        }
        return grid && wave && wave2;
    } catch (const std::exception &) {
        return false;
    }
}

bool isDatAnalysisName(const std::string &name)
{
    // ANLZ followed by digits and .DAT, as rekordbox and the players
    // spell it. Case kept strict: that is how both write it.
    if (name.size() < 9 || name.compare(0, 4, "ANLZ") != 0 || name.compare(name.size() - 4, 4, ".DAT") != 0) {
        return false;
    }
    return std::all_of(name.begin() + 4, name.end() - 4, [](unsigned char c) { return std::isdigit(c) != 0; });
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

std::optional<LegacyMemoryListFinding> auditTrackAnalysis(const std::string &pioneerRoot,
                                                          const std::string &analyzePath,
                                                          const std::function<bool(const std::string &)> &named)
{
    LegacyMemoryListFinding finding;
    const std::string dat = datAnlzPath(pioneerRoot, analyzePath);
    const fs::path datFs = pathFromUtf8(dat);
    std::error_code ec;
    if (fs::is_regular_file(datFs, ec)) {
        finding.datPath = dat;
        try {
            const AnlzFile file = AnlzFile::readRaw(dat);
            for (const auto &section : file.sections) {
                if (section.fourcc != PcobFourcc || section.rawBytes.size() < SectionHeaderSize
                    || readU32BE(section.rawBytes, 12) != CueListTypeMemory) {
                    continue;
                }
                finding.shape = auditLegacyMemoryList(section.rawBytes);
                break;
            }
        } catch (const std::exception &e) {
            finding.shape.malformed = e.what();
        }
    }

    // What else answers to an analysis name in the track's directory,
    // and whether it is anybody's. rekordbox puts a second track's
    // analysis in the same directory as ANLZ0001.* (ten of them in the
    // committed fixture, each named from its own row), so the name
    // alone says nothing: debris is what no row names, has no .EXT, and
    // holds no analysis.
    const fs::path dir = datFs.parent_path();
    const std::string own = pathToUtf8(datFs.filename());
    const std::string rowDir = analyzePath.substr(0, analyzePath.size() - own.size());
    if (fs::is_directory(dir, ec)) {
        for (const auto &entry : fs::directory_iterator(dir, ec)) {
            const std::string name = pathToUtf8(entry.path().filename());
            if (name == own || !isDatAnalysisName(name) || !entry.is_regular_file(ec)) {
                continue;
            }
            if (named(rowDir + name)) {
                continue;
            }
            const fs::path ext = entry.path().parent_path() / (name.substr(0, name.size() - 4) + ".EXT");
            if (fs::exists(ext, ec)) {
                continue;
            }
            if (!isSkeleton(pathToUtf8(entry.path()))) {
                continue;
            }
            finding.debris.push_back(pathToUtf8(entry.path()));
        }
        std::sort(finding.debris.begin(), finding.debris.end());
    }

    if (!finding.anything()) {
        return std::nullopt;
    }
    return finding;
}

std::string repairTrackAnalysis(const LegacyMemoryListFinding &finding)
{
    std::string account;
    if (finding.shape.repairable()) {
        AnlzFile file = AnlzFile::readRaw(finding.datPath);
        bool replaced = false;
        for (auto &section : file.sections) {
            if (section.fourcc != PcobFourcc || section.rawBytes.size() < SectionHeaderSize
                || readU32BE(section.rawBytes, 12) != CueListTypeMemory) {
                continue;
            }
            // Audited again from the bytes on disk now, not from the
            // scan's: a save in between (a sync of this track rewrites
            // the list) leaves nothing to do, and that is not an error.
            const LegacyMemoryListShape now = auditLegacyMemoryList(section.rawBytes);
            if (now.repairable()) {
                section.rawBytes = repairLegacyMemoryList(section.rawBytes);
                replaced = true;
            }
            break;
        }
        if (replaced) {
            file.writeRaw(finding.datPath);
            account += "rebuilt the memory list of " + finding.datPath + " (" + std::to_string(finding.shape.entries)
                       + " entries kept)";
        } else {
            account += "memory list of " + finding.datPath + " was already in shape";
        }
    }
    for (const auto &debris : finding.debris) {
        std::string failure;
        if (!removeEntry(pathFromUtf8(debris), failure)) {
            throw std::runtime_error("could not remove " + debris + ": " + failure);
        }
        account += (account.empty() ? "" : "; ") + std::string("removed ") + debris;
    }
    return account;
}

}  // namespace seabass::infrastructure::rekordbox
