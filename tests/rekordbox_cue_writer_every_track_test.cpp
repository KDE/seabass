// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RekordboxCueWriter over every real analysis file in the fixture.
//
// The codec's own test round-trips 7968 real PCOB sections, and the
// writer's tests drive it on a handful of tracks. What had never run is
// the writer over ALL of them with the edit a Sync makes: every cue the
// track already has kept, a memory cue added where none sits (on most
// tracks into a .DAT memory list that is empty), and a hot cue added in
// each of the two slot ranges, so both legacy lists and PCO2 all change.
// Then the added memory cue is taken away again, which must put the
// .DAT's memory list back exactly as it was.
//
// "As it was" means as the writer leaves it for the track's own cues,
// which is written first as a baseline, and not always as rekordbox
// left it: rekordbox keeps memory cues in PCO2 and leaves the legacy
// memory list empty (196 of the 199 fixture tracks that have memory
// cues), while this writer mirrors every memory cue into the .DAT's
// legacy list. That difference is counted and printed, not failed; it
// is a decision about the format, not damage.
//
// After each write both files are parsed STRICTLY: the kaitai parser the
// reader is built on, with no tolerance, plus the survey's invariants
// that kaitai does not check (anlz_legacy_cue_codec.hpp): a PCOB's
// len_tag is 24 + 56 * num_cues, and every entry starts with PCPT, with
// len_header 28 and len_entry 56. Then the cues are read back through
// the reader and must be exactly the set written, hot cues 1-3 in the
// .DAT's legacy list only and 4-8 in the .EXT's only.
//
// This is the test the WHALESHARK damage (2026-09-26: a memory PCOB whose
// "entry" did not start with PCPT) called for. Every failing track is
// printed with its file, the section, and the section's first 96 bytes.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <kaitai/kaitaistream.h>

#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::rekordbox;
using Anlz = rekordbox_anlz_t;

namespace
{

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32_t be32(const std::string &s, size_t at)
{
    return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) | (uint32_t(uint8_t(s[at + 2])) << 8)
        | uint32_t(uint8_t(s[at + 3]));
}

uint16_t be16(const std::string &s, size_t at)
{
    return uint16_t((uint16_t(uint8_t(s[at])) << 8) | uint8_t(s[at + 1]));
}

std::string hexDump(const std::string &bytes, size_t limit = 96)
{
    std::ostringstream out;
    const size_t n = std::min(bytes.size(), limit);
    for (size_t i = 0; i < n; ++i) {
        if (i % 16 == 0) {
            out << "\n        " << std::setw(4) << std::setfill('0') << std::hex << i << ":";
        }
        out << ' ' << std::setw(2) << std::setfill('0') << std::hex << unsigned(uint8_t(bytes[i]));
    }
    return out.str();
}

struct Section
{
    size_t offset = 0;
    std::string fourcc;
    std::string bytes;
};

// The file's sections as they sit on disk, without judging them.
std::vector<Section> sectionsOf(const std::string &data)
{
    std::vector<Section> out;
    if (data.size() < 12 || data.compare(0, 4, "PMAI") != 0) {
        return out;
    }
    size_t pos = be32(data, 4);
    while (pos + 12 <= data.size()) {
        const uint32_t lenTag = be32(data, pos + 8);
        if (lenTag < 12 || pos + lenTag > data.size()) {
            break;
        }
        out.push_back({pos, data.substr(pos, 4), data.substr(pos, lenTag)});
        pos += lenTag;
    }
    return out;
}

// What a legacy list of one type holds in a file, or that it has none.
enum class ListState { Absent, Empty, Populated };

struct LegacyList
{
    ListState state = ListState::Absent;
    std::string bytes;
};

LegacyList legacyList(const std::string &data, uint32_t listType)
{
    for (const auto &section : sectionsOf(data)) {
        if (section.fourcc == "PCOB" && section.bytes.size() >= 24 && be32(section.bytes, 12) == listType) {
            return {be16(section.bytes, 18) == 0 ? ListState::Empty : ListState::Populated, section.bytes};
        }
    }
    return {};
}

// Everything wrong with one file after a write, each problem naming the
// section and showing its bytes. Empty means it is well formed.
std::vector<std::string> strictProblems(const fs::path &path)
{
    std::vector<std::string> problems;
    const std::string data = readFile(path);
    const std::string where = pathToUtf8(path);

    // The survey's invariants, which kaitai does not check itself: it
    // reads a fixed 56 bytes per entry whatever len_entry says, and never
    // compares len_tag with num_cues.
    size_t walked = data.size() >= 8 ? be32(data, 4) : 0;
    for (const auto &section : sectionsOf(data)) {
        walked = section.offset + section.bytes.size();
        if (section.fourcc != "PCOB") {
            continue;
        }
        const std::string &s = section.bytes;
        auto fail = [&](const std::string &what) {
            std::ostringstream msg;
            msg << where << ": PCOB at offset " << section.offset << ": " << what << hexDump(s);
            problems.push_back(msg.str());
        };
        if (s.size() < 24 || be32(s, 4) != 24) {
            fail("len_header is not 24");
            continue;
        }
        const uint16_t numCues = be16(s, 18);
        if (be32(s, 8) != 24u + 56u * numCues) {
            fail("len_tag " + std::to_string(be32(s, 8)) + " is not 24 + 56 * " + std::to_string(numCues));
            continue;
        }
        if (be16(s, 16) != 0) {
            fail("the two bytes before num_cues are not zero");
        }
        for (uint16_t i = 0; i < numCues; ++i) {
            const size_t at = 24 + size_t(56) * i;
            if (s.compare(at, 4, "PCPT") != 0 || be32(s, at + 4) != 28 || be32(s, at + 8) != 56) {
                fail("entry " + std::to_string(i) + " is not a 56-byte PCPT entry with len_header 28");
                break;
            }
        }
    }
    if (walked != data.size()) {
        problems.push_back(where + ": sections do not tile the file (stopped at " + std::to_string(walked) + " of "
                           + std::to_string(data.size()) + ")");
    }

    // And the parser itself, with no tolerance at all.
    try {
        std::istringstream in(data, std::ios::binary);
        kaitai::kstream ks(&in);
        Anlz anlz(&ks);
        for (const auto &section : *anlz.sections()) {
            if (section->fourcc() != Anlz::SECTION_TAGS_CUES) {
                continue;
            }
            auto *tag = dynamic_cast<Anlz::cue_tag_t *>(section->body());
            if (!tag || tag->cues()->size() != tag->num_cues()) {
                problems.push_back(where + ": kaitai read a PCOB whose entries do not match num_cues");
            }
        }
    } catch (const std::exception &e) {
        problems.push_back(where + ": kaitai could not parse it: " + e.what());
    }
    return problems;
}

// slot -> position of each hot cue in a file's legacy hot list, read
// through kaitai (strict).
std::multimap<uint32_t, uint32_t> legacyHotSlots(const fs::path &path)
{
    std::multimap<uint32_t, uint32_t> out;
    const std::string data = readFile(path);
    std::istringstream in(data, std::ios::binary);
    kaitai::kstream ks(&in);
    Anlz anlz(&ks);
    for (const auto &section : *anlz.sections()) {
        if (section->fourcc() != Anlz::SECTION_TAGS_CUES) {
            continue;
        }
        auto *tag = dynamic_cast<Anlz::cue_tag_t *>(section->body());
        if (!tag || tag->type() != Anlz::CUE_LIST_TYPE_HOT_CUES) {
            continue;
        }
        for (const auto &cue : *tag->cues()) {
            out.emplace(cue->hot_cue(), cue->time());
        }
    }
    return out;
}

// A cue reduced to what the writer promises to keep.
using CueKey = std::tuple<int, int, double, bool, double>;

CueKey keyOf(const domain::CuePoint &cue)
{
    const bool hot = cue.kind == domain::CuePoint::Kind::Hot;
    return {hot ? 1 : 0, hot ? cue.hotCueNumber : 0, cue.positionMs, cue.isLoop, cue.isLoop ? cue.loopEndMs : 0.0};
}

std::string describe(const std::vector<domain::CuePoint> &cues)
{
    std::vector<CueKey> keys;
    for (const auto &cue : cues) {
        keys.push_back(keyOf(cue));
    }
    std::sort(keys.begin(), keys.end());
    std::ostringstream out;
    for (const auto &[hot, slot, pos, loop, end] : keys) {
        out << (hot ? "hot" + std::to_string(slot) : std::string("mem")) << "@" << pos;
        if (loop) {
            out << "-" << end;
        }
        out << ' ';
    }
    return out.str();
}

// The same set, positions within 1 ms (the writer stores whole
// milliseconds).
bool sameCues(const std::vector<domain::CuePoint> &a, const std::vector<domain::CuePoint> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    std::vector<CueKey> ka;
    std::vector<CueKey> kb;
    for (const auto &cue : a) {
        ka.push_back(keyOf(cue));
    }
    for (const auto &cue : b) {
        kb.push_back(keyOf(cue));
    }
    std::sort(ka.begin(), ka.end());
    std::sort(kb.begin(), kb.end());
    for (size_t i = 0; i < ka.size(); ++i) {
        const auto &[h1, s1, p1, l1, e1] = ka[i];
        const auto &[h2, s2, p2, l2, e2] = kb[i];
        if (h1 != h2 || s1 != s2 || l1 != l2 || std::abs(p1 - p2) > 1.0 || std::abs(e1 - e2) > 1.0) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_cue_writer_every_track_test <anonymized library dir>\n";
        return 2;
    }
    const auto started = std::chrono::steady_clock::now();

    // The fixture's "rekordbox" directory is the PIONEER root. Copied
    // once: the fixture itself is never written.
    const fs::path fixtureRoot = fs::path(argv[1]) / "rekordbox";
    const fs::path scratch = testing::scratchRoot() / "rekordbox-cue-writer-every-track";
    fs::remove_all(scratch);
    const fs::path root = scratch / "PIONEER";
    fs::create_directories(root / "rekordbox");
    fs::copy_file(fixtureRoot / "rekordbox" / "export.pdb", root / "rekordbox" / "export.pdb");
    fs::copy(fixtureRoot / "USBANLZ", root / "USBANLZ", fs::copy_options::recursive);
    const std::string rootUtf8 = pathToUtf8(root);

    KaitaiRekordboxReader reader(rootUtf8);
    std::vector<domain::Track> tracks = reader.readTracks();
    reader.fillCues(tracks);
    const AnlzPathIndex index(rootUtf8);
    RekordboxCueWriter writer(rootUtf8, &index);

    int tracksWritten = 0;
    int noAnalysis = 0;
    int noDat = 0;
    int noFreeLowSlot = 0;
    int noFreeHighSlot = 0;
    std::map<std::string, int> memoryListBefore;   // .DAT memory list: absent / empty / populated
    std::map<std::string, int> memoryListAfter;    // and once the writer has rewritten the same cues
    std::map<std::string, int> datGeneration;      // PMAI header word at 12, the writer-generation marker
    std::map<std::string, int> entryGeneration;    // status of every legacy entry the fixture holds
    std::vector<std::string> failures;
    std::set<std::string> failedTracks;

    auto stateName = [](ListState s) {
        return s == ListState::Absent ? "absent" : s == ListState::Empty ? "empty" : "populated";
    };

    for (const auto &track : tracks) {
        if (track.format != "rekordbox") {
            continue;
        }
        const uint32_t id = static_cast<uint32_t>(std::stoul(track.sourceId));
        const auto analyzePath = index.pathFor(id);
        if (!analyzePath || !fs::exists(pathFromUtf8(extAnlzPath(rootUtf8, *analyzePath)))) {
            ++noAnalysis;
            continue;
        }
        const fs::path extPath = pathFromUtf8(extAnlzPath(rootUtf8, *analyzePath));
        const fs::path datPath = pathFromUtf8(datAnlzPath(rootUtf8, *analyzePath));
        const bool haveDat = fs::exists(datPath);
        if (!haveDat) {
            ++noDat;
        }
        const std::string datBefore = haveDat ? readFile(datPath) : std::string();
        const LegacyList memoryBefore = legacyList(datBefore, CueListTypeMemory);
        if (haveDat) {
            ++memoryListBefore[stateName(memoryBefore.state)];
            std::ostringstream gen;
            gen << "PMAI word 12 = " << be32(datBefore, 12);
            ++datGeneration[gen.str()];
        }
        for (const std::string &data : {datBefore, readFile(extPath)}) {
            for (const auto &section : sectionsOf(data)) {
                if (section.fourcc != "PCOB") {
                    continue;
                }
                for (size_t at = 24; at + 56 <= section.bytes.size(); at += 56) {
                    ++entryGeneration["status " + std::to_string(be32(section.bytes, at + 16)) + ", unknown1 0x"
                                      + [&] {
                                            std::ostringstream h;
                                            h << std::hex << be32(section.bytes, at + 20);
                                            return h.str();
                                        }()];
                }
            }
        }

        const std::string label = "track " + track.sourceId + " (" + pathToUtf8(extPath.parent_path()) + ")";
        auto fail = [&](const std::string &what) {
            failures.push_back(label + ": " + what);
            failedTracks.insert(track.sourceId);
        };

        // Every cue it has, a memory cue at a position none of them is
        // near, and a hot cue in a free slot of each range.
        std::vector<domain::CuePoint> original = track.cues;
        std::set<int> usedSlots;
        for (const auto &cue : original) {
            if (cue.kind == domain::CuePoint::Kind::Hot) {
                usedSlots.insert(cue.hotCueNumber);
            }
        }
        double memoryAt = 3333.0;
        auto crowded = [&](double at) {
            return std::any_of(original.begin(), original.end(), [at](const domain::CuePoint &cue) {
                return std::abs(cue.positionMs - at) < 1500.0;
            });
        };
        while (crowded(memoryAt)) {
            memoryAt += 1777.0;
        }
        domain::CuePoint addedMemory;
        addedMemory.kind = domain::CuePoint::Kind::Memory;
        addedMemory.positionMs = memoryAt;

        std::vector<domain::CuePoint> withAdded = original;
        withAdded.push_back(addedMemory);
        auto addHot = [&](int low, int high, double at) {
            for (int slot = low; slot <= high; ++slot) {
                if (!usedSlots.count(slot)) {
                    domain::CuePoint hot;
                    hot.kind = domain::CuePoint::Kind::Hot;
                    hot.hotCueNumber = slot;
                    hot.positionMs = at;
                    withAdded.push_back(hot);
                    return true;
                }
            }
            return false;
        };
        if (!addHot(1, 3, 11111.0)) {
            ++noFreeLowSlot;
        }
        if (!addHot(4, 8, 22222.0)) {
            ++noFreeHighSlot;
        }
        std::vector<domain::CuePoint> withoutMemory;
        for (const auto &cue : withAdded) {
            if (!(cue.kind == domain::CuePoint::Kind::Memory && cue.positionMs == memoryAt)) {
                withoutMemory.push_back(cue);
            }
        }

        auto writeAndVerify = [&](const std::vector<domain::CuePoint> &cues, const std::string &pass) {
            try {
                writer.writeHotCues(track.sourceId, cues);
            } catch (const std::exception &e) {
                fail(pass + ": the writer threw: " + e.what());
                return false;
            }
            bool ok = true;
            for (const fs::path &file : {extPath, datPath}) {
                if (!fs::exists(file)) {
                    continue;
                }
                for (const auto &problem : strictProblems(file)) {
                    fail(pass + ": " + problem);
                    ok = false;
                }
            }
            if (!ok) {
                return false;
            }
            std::vector<domain::Track> readBack{track};
            readBack[0].cues.clear();
            reader.fillCues(readBack);
            if (!sameCues(readBack[0].cues, cues)) {
                fail(pass + ": read back " + describe(readBack[0].cues) + "\n        but wrote " + describe(cues));
                ok = false;
            }
            // Hot cues 1-3 in the .DAT's legacy list and nowhere else,
            // 4-8 in the .EXT's.
            std::multimap<uint32_t, uint32_t> wantDat;
            std::multimap<uint32_t, uint32_t> wantExt;
            for (const auto &cue : cues) {
                if (cue.kind == domain::CuePoint::Kind::Hot) {
                    (cue.hotCueNumber <= 3 ? wantDat : wantExt)
                        .emplace(uint32_t(cue.hotCueNumber), uint32_t(cue.positionMs));
                }
            }
            if (haveDat && legacyHotSlots(datPath) != wantDat) {
                fail(pass + ": the .DAT legacy hot list is not exactly hot cues 1-3" + hexDump(legacyList(readFile(datPath), CueListTypeHot).bytes));
                ok = false;
            }
            if (legacyHotSlots(extPath) != wantExt) {
                fail(pass + ": the .EXT legacy hot list is not exactly hot cues 4-8" + hexDump(legacyList(readFile(extPath), CueListTypeHot).bytes));
                ok = false;
            }
            return ok;
        };

        ++tracksWritten;
        // First the track's own cues, unchanged: the baseline the second
        // write has to return to. It is not always rekordbox's own list,
        // and the difference is counted below rather than failed.
        if (!writeAndVerify(original, "write 0 (the cues it has)")) {
            continue;
        }
        const LegacyList memoryBaseline = legacyList(haveDat ? readFile(datPath) : std::string(), CueListTypeMemory);
        if (haveDat) {
            if (memoryBaseline.bytes == memoryBefore.bytes) {
                ++memoryListAfter["unchanged by rewriting the cues it has"];
            } else {
                ++memoryListAfter[std::string(stateName(memoryBefore.state)) + " in rekordbox's file, "
                                  + stateName(memoryBaseline.state) + " once rewritten"];
            }
        }
        if (!writeAndVerify(withAdded, "write 1 (memory cue added)")) {
            continue;
        }
        if (!writeAndVerify(withoutMemory, "write 2 (memory cue removed)")) {
            continue;
        }
        if (haveDat) {
            const LegacyList memoryAfter = legacyList(readFile(datPath), CueListTypeMemory);
            if (memoryAfter.bytes != memoryBaseline.bytes) {
                fail(std::string("the .DAT memory list is not byte-identical after the memory cue went\n      before:")
                     + hexDump(memoryBaseline.bytes) + "\n      after:" + hexDump(memoryAfter.bytes));
            }
        }
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    for (const auto &failure : failures) {
        std::cout << "FAIL " << failure << "\n";
    }
    std::cout << "\ntracks in export.pdb: " << tracks.size() << "\n"
              << "tracks written three times through RekordboxCueWriter: " << tracksWritten << "\n"
              << "tracks with no analysis file: " << noAnalysis << "\n"
              << "tracks with an .EXT but no .DAT: " << noDat << "\n"
              << "tracks with no free hot cue slot in 1-3: " << noFreeLowSlot << ", in 4-8: " << noFreeHighSlot
              << "\n";
    std::cout << ".DAT memory list before the writes:\n";
    for (const auto &[state, count] : memoryListBefore) {
        std::cout << "    " << state << ": " << count << "\n";
    }
    std::cout << ".DAT memory list, rekordbox's against the writer's for the same cues:\n";
    for (const auto &[state, count] : memoryListAfter) {
        std::cout << "    " << state << ": " << count << "\n";
    }
    std::cout << ".DAT writer generation:\n";
    for (const auto &[gen, count] : datGeneration) {
        std::cout << "    " << gen << ": " << count << "\n";
    }
    std::cout << "legacy entries in the fixture, by generation marker:\n";
    for (const auto &[gen, count] : entryGeneration) {
        std::cout << "    " << gen << ": " << count << "\n";
    }
    std::cout << "failing tracks: " << failedTracks.size() << " (" << failures.size() << " problems)\n"
              << "elapsed: " << std::fixed << std::setprecision(1) << seconds << " s\n";

    fs::remove_all(scratch);
    return failedTracks.empty() ? 0 : 1;
}
