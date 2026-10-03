// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

#include "application/path_key.hpp"
#include "domain/local_restore.hpp"
#include "domain/track.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace fs = std::filesystem;

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

// The cue lists of one analysis file and nothing else, read by seeking
// from section header to section header. A real .EXT is some 167 KB of
// which the lists are a few hundred bytes, mostly waveforms after them;
// reading every track's whole file over USB would make this check the
// slowest on the hub. Throws when the file is missing, is not an ANLZ
// file, or a section length points outside it.
std::vector<AnlzRawSection> readCueSections(const std::string &path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error(path + " could not be opened");
    }
    in.seekg(0, std::ios::end);
    const auto end = static_cast<uint64_t>(in.tellg());
    auto readAt = [&in, &path](uint64_t offset, size_t length) {
        std::string bytes(length, '\0');
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(bytes.data(), static_cast<std::streamsize>(length));
        if (static_cast<size_t>(in.gcount()) != length) {
            throw std::runtime_error(path + " ends inside a section");
        }
        return bytes;
    };
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

bool isMemoryPcob(const AnlzRawSection &section)
{
    return section.fourcc == PcobFourcc && section.rawBytes.size() >= SectionHeaderSize
           && readU32BE(section.rawBytes, 12) == CueListTypeMemory;
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

// The lists out of both files' sections. Throws, naming the list, when
// one does not decode; a memory PCOB is read leniently when
// auditLegacyMemoryList() can read it, so a stale header or the RX2's
// zero slot is compared by its real entries.
TrackCueLists listsOf(const std::vector<AnlzRawSection> &dat, const std::vector<AnlzRawSection> &ext,
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

// The reader's "same cue" (kaitai_rekordbox_reader.cpp): positions within
// the restore planner's tolerance, a loop matching only a loop that ends
// within it too. The pad is the caller's to compare.
bool sameCue(const ListedCue &a, const ListedCue &b)
{
    const double tolerance = domain::LocalRestorePlanner::PositionToleranceMs;
    if (std::abs(double(a.timeMs) - double(b.timeMs)) > tolerance || a.isLoop != b.isLoop) {
        return false;
    }
    return !a.isLoop || std::abs(double(a.loopEndMs) - double(b.loopEndMs)) <= tolerance;
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
        if ((byPad && a[i].pad != b[i].pad) || !sameCue(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

// What Seabass shows: the modern list, and every legacy cue it does not
// already hold, merged exactly as appendLegacyCues() merges them: a hot
// cue is known when its pad is taken, a memory cue when any cue already
// in the list (a legacy one appended before it included) sits within
// the tolerance, loop or not.
std::vector<ListedCue> seabassViewOf(const std::vector<ListedCue> &modern, const std::vector<ListedCue> &legacy,
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

// The PCO2 entry that is the same cue as `cue`, for its colour.
const ListedCue *partnerOf(const ListedCue &cue, const std::vector<ListedCue> &in, bool byPad)
{
    const ListedCue *best = nullptr;
    for (const auto &other : in) {
        if ((byPad && other.pad != cue.pad) || !sameCue(cue, other)) {
            continue;
        }
        if (!best || std::abs(double(other.timeMs) - double(cue.timeMs))
                         < std::abs(double(best->timeMs) - double(cue.timeMs))) {
            best = &other;
        }
    }
    return best;
}

std::string colorText(const std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> &color)
{
    if (!color) {
        return {};
    }
    char text[8];
    std::snprintf(text, sizeof text, "#%02X%02X%02X", std::get<0>(*color), std::get<1>(*color),
                  std::get<2>(*color));
    return text;
}

domain::CuePoint cuePoint(const ListedCue &listedCue, bool hot)
{
    domain::CuePoint cue;
    cue.kind = hot ? domain::CuePoint::Kind::Hot : domain::CuePoint::Kind::Memory;
    cue.hotCueNumber = hot ? static_cast<int>(listedCue.pad) : 0;
    cue.positionMs = listedCue.timeMs;
    cue.isLoop = listedCue.isLoop;
    cue.loopEndMs = listedCue.isLoop ? listedCue.loopEndMs : 0;
    cue.color = colorText(listedCue.color);
    return cue;
}

// The cues both generations of list are rewritten from. Player: the
// legacy lists, each cue taking the colour of the PCO2 entry that is the
// same cue (RekordboxCueWriter then carries that entry's bytes over,
// comment and all, when the position is the same to the millisecond).
// Seabass: what Seabass shows, the PCO2 lists with the legacy cues they
// lack.
std::vector<domain::CuePoint> cuesToKeep(const TrackCueLists &lists, KeepCueList keep)
{
    std::vector<domain::CuePoint> cues;
    auto add = [&cues](std::vector<ListedCue> from, bool hot, const std::vector<ListedCue> *colours) {
        if (hot) {
            std::stable_sort(from.begin(), from.end(), [](const ListedCue &a, const ListedCue &b) {
                return a.pad < b.pad;
            });
        }
        for (ListedCue cue : from) {
            if (colours) {
                if (const ListedCue *partner = partnerOf(cue, *colours, hot)) {
                    cue.color = partner->color;
                }
            }
            cues.push_back(cuePoint(cue, hot));
        }
    };
    if (keep == KeepCueList::Player) {
        add(lists.legacyHot, true, &lists.modernHot);
        add(lists.legacyMemory, false, &lists.modernMemory);
    } else {
        add(seabassViewOf(lists.modernHot, lists.legacyHot, true), true, nullptr);
        add(seabassViewOf(lists.modernMemory, lists.legacyMemory, false), false, nullptr);
    }
    return cues;
}

TrackCueLists readListsOf(const std::string &datPath, const std::string &extPath)
{
    return listsOf(readCueSections(datPath), readCueSections(extPath), datPath, extPath);
}

// The file as written, read back with the writer's own check
// (analysisFileReadBackProblem). Puts `before` back and throws when it
// fails, as RekordboxCueWriter does for its own writes.
void readBackOrRestore(const std::string &path, const std::string &intended, const std::string &before)
{
    const std::optional<std::string> problem = analysisFileReadBackProblem(path, intended);
    if (!problem) {
        return;
    }
    const bool restored = writeFileDurablyAtomic(path, before);
    throw std::runtime_error(path + " failed its check after writing (" + *problem + "); "
                             + (restored ? "it was put back as it was"
                                         : "it could NOT be put back and needs restoring from the backup"));
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

TrackCueLists readTrackCueLists(const std::string &pioneerRoot, const std::string &analyzePath)
{
    return readListsOf(datAnlzPath(pioneerRoot, analyzePath), extAnlzPath(pioneerRoot, analyzePath));
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
    d.seabassHot = seabassViewOf(lists.modernHot, lists.legacyHot, true);
    d.seabassMemory = seabassViewOf(lists.modernMemory, lists.legacyMemory, false);
    d.viewsAgree = listsMatch(lists.legacyHot, d.seabassHot, true) && listsMatch(lists.legacyMemory, d.seabassMemory, false);
    if (lists.extLegacyMemory > 0) {
        d.unrepairable = "memory cues in the .EXT's legacy list, where no writer puts them";
    }
    d.lists = lists;
    return d;
}

LegacyMemoryListFinding examineTrackAnalysis(const std::string &pioneerRoot, const std::string &paddedAnalyzePath,
                                             const std::function<bool(const std::string &)> &named)
{
    // export.pdb keeps its strings in fixed-length fields padded with
    // spaces (NULs in some), and the padding is no part of the name: the
    // same trim normalizedPathKey() makes, which the #59 test needs for a
    // row naming "ANLZ0000.DAT   ". Left on, the files were "missing" and
    // the debris rule cut the row's directory out of the padded string.
    std::string analyzePath = paddedAnalyzePath;
    while (!analyzePath.empty()
           && (analyzePath.back() == ' ' || analyzePath.back() == '\t' || analyzePath.back() == '\0')) {
        analyzePath.pop_back();
    }
    LegacyMemoryListFinding finding;
    finding.pioneerRoot = pioneerRoot;
    finding.analyzePath = analyzePath;
    const std::string dat = datAnlzPath(pioneerRoot, analyzePath);
    const std::string ext = extAnlzPath(pioneerRoot, analyzePath);
    const fs::path datFs = pathFromUtf8(dat);
    std::error_code ec;
    if (fs::is_regular_file(datFs, ec)) {
        finding.datPath = dat;
    }
    // The .DAT's memory list first, on its own: an RX2 hangs on it
    // whatever state the .EXT is in.
    std::vector<AnlzRawSection> datSections;
    bool datRead = false;
    if (finding.datPath.empty()) {
        finding.unreadable = dat + " is missing";
    } else {
        try {
            datSections = readCueSections(dat);
            datRead = true;
        } catch (const std::exception &e) {
            finding.unreadable = e.what();
        }
    }
    if (datRead) {
        for (const auto &section : datSections) {
            if (isMemoryPcob(section)) {
                finding.shape = auditLegacyMemoryList(section.rawBytes);
                break;
            }
        }
    }
    // Then the two generations compared, which needs the .EXT too.
    if (datRead) {
        std::vector<AnlzRawSection> extSections;
        if (!fs::is_regular_file(pathFromUtf8(ext), ec)) {
            finding.unreadable = ext + " is missing";
        } else {
            try {
                extSections = readCueSections(ext);
                finding.examined = true;
            } catch (const std::exception &e) {
                finding.unreadable = e.what();
            }
        }
        // A memory list this cannot read is reported as such, and the
        // lists are not compared: there is no telling what it holds.
        if (finding.examined && finding.shape.malformed.empty()) {
            try {
                finding.disagreement = compareCueLists(listsOf(datSections, extSections, dat, ext));
            } catch (const std::exception &e) {
                finding.listsMalformed = e.what();
            }
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
            const fs::path extFile = entry.path().parent_path() / (name.substr(0, name.size() - 4) + ".EXT");
            if (fs::exists(extFile, ec)) {
                continue;
            }
            if (!isSkeleton(pathToUtf8(entry.path()))) {
                continue;
            }
            finding.debris.push_back(pathToUtf8(entry.path()));
        }
        std::sort(finding.debris.begin(), finding.debris.end());
    }
    return finding;
}

std::optional<LegacyMemoryListFinding> auditTrackAnalysis(const std::string &pioneerRoot,
                                                          const std::string &analyzePath,
                                                          const std::function<bool(const std::string &)> &named)
{
    LegacyMemoryListFinding finding = examineTrackAnalysis(pioneerRoot, analyzePath, named);
    if (!finding.anything()) {
        return std::nullopt;
    }
    return finding;
}

void CueListTally::add(const LegacyMemoryListFinding &finding)
{
    if (finding.examined) {
        ++examined;
    } else {
        ++unreadable;
    }
    if (finding.shape.headerStale || finding.shape.unlinked) {
        ++legacyHeader;
    }
    if (finding.shape.zeroSlots) {
        ++playerRewritten;
    }
    if (finding.disagreement && finding.disagreement->any()) {
        ++disagree;
    }
    strayFiles += finding.debris.size();
}

CueListTally &CueListTally::operator+=(const CueListTally &other)
{
    examined += other.examined;
    unreadable += other.unreadable;
    legacyHeader += other.legacyHeader;
    playerRewritten += other.playerRewritten;
    disagree += other.disagree;
    strayFiles += other.strayFiles;
    return *this;
}

CueListScan scanCueLists(const std::string &pioneerRoot, const std::vector<std::string> &analyzePaths,
                         const std::function<bool(const std::string &)> &named, const std::function<void()> &each)
{
    CueListScan scan;
    std::set<std::string> seen;
    std::set<std::string> seenDebris;
    for (const auto &analyzePath : analyzePaths) {
        if (each) {
            each();
        }
        if (analyzePath.empty() || !seen.insert(application::normalizedPathKey(analyzePath)).second) {
            continue;
        }
        LegacyMemoryListFinding finding = examineTrackAnalysis(pioneerRoot, analyzePath, named);
        // Two tracks analysed into one directory would each report the
        // same stray file; it is one file, reported and removed once.
        auto &debris = finding.debris;
        debris.erase(std::remove_if(debris.begin(), debris.end(),
                                    [&seenDebris](const std::string &file) {
                                        return !seenDebris.insert(application::normalizedPathKey(file)).second;
                                    }),
                     debris.end());
        scan.tally.add(finding);
        if (finding.anything() || !finding.unreadable.empty()) {
            scan.findings.push_back(std::move(finding));
        }
    }
    return scan;
}

std::string repairTrackAnalysis(const LegacyMemoryListFinding &finding, KeepCueList keep)
{
    std::string account;
    auto say = [&account](const std::string &what) { account += (account.empty() ? "" : "; ") + what; };
    if (finding.shape.repairable()) {
        AnlzFile file = AnlzFile::readRaw(finding.datPath);
        const std::string before = file.toBytes();
        bool replaced = false;
        for (auto &section : file.sections) {
            if (!isMemoryPcob(section)) {
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
            readBackOrRestore(finding.datPath, file.toBytes(), before);
            say("rebuilt the memory list of " + finding.datPath + " (" + std::to_string(finding.shape.entries)
                + " entries kept)");
        } else {
            say("memory list of " + finding.datPath + " was already in shape");
        }
    }
    if (finding.listsFixable()) {
        // From the files as they are now, the memory list just repaired
        // included, so the player's own entries are what the writer
        // carries over.
        const TrackCueLists now = readTrackCueLists(finding.pioneerRoot, finding.analyzePath);
        const auto disagreement = compareCueLists(now);
        if (!disagreement) {
            say("cue lists of " + finding.analyzePath + " already agree");
        } else if (!disagreement->unrepairable.empty()) {
            say("cue lists of " + finding.analyzePath + " left alone: " + disagreement->unrepairable);
        } else {
            const std::vector<domain::CuePoint> cues = cuesToKeep(now, keep);
            RekordboxCueWriter writer(finding.pioneerRoot);
            writer.writeCuesToAnalysisFile(finding.analyzePath, cues, RekordboxCueWriter::Rewrite::OnlyIfChanged);
            if (compareCueLists(readTrackCueLists(finding.pioneerRoot, finding.analyzePath))) {
                throw std::runtime_error("the cue lists of " + finding.analyzePath
                                         + " still disagree after writing them from one list");
            }
            say("rewrote the cue lists of " + finding.analyzePath + " from "
                + (keep == KeepCueList::Player ? "the player's" : "Seabass's") + " list ("
                + std::to_string(cues.size()) + " cues)");
        }
    }
    for (const auto &debris : finding.debris) {
        std::string failure;
        if (!removeEntry(pathFromUtf8(debris), failure)) {
            throw std::runtime_error("could not remove " + debris + ": " + failure);
        }
        say("removed " + debris);
    }
    return account;
}

}  // namespace seabass::infrastructure::rekordbox
