// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the rekordbox cue writer makes of a legacy cue list that is not
// in the shape rekordbox writes.
//
// WHALESHARK (2026-09-26) came back from a sync with a .DAT whose memory
// PCOB held one "entry" that was not a PCPT entry at all. Whoever wrote
// those bytes, the writer must never be the one to put such a list on a
// stick, and must never keep one it finds there. Every case below plants
// one malformed or unusual list in a real fixture .DAT, writes the cues a
// sync would write, and requires both files to come out strictly well
// formed (anlz_strict_check.hpp: kaitai with no tolerance, plus the
// survey's invariants) holding exactly the cues written.
//
// The case that failed before the fix is "an entry that is 56 bytes but
// not PCPT": decodeCues() read it without looking at its magic, the
// carry-over matched it on slot and time, and encodeCues() wrote its
// bytes back verbatim -- a list of exactly the WHALESHARK kind, produced
// by the writer from a damaged one. The same went for an entry whose
// len_entry was 52 or 60. And a damaged section's memory_count was kept
// when the section was rebuilt, so rewriting WHALESHARK's list carried
// its 0000ffff into a list that was otherwise sound.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

#include "anlz_strict_check.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::rekordbox;
using namespace anlz_strict;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) {
        ++failures;
    }
}

void putU32(std::string &out, uint32_t v)
{
    out.push_back(char(v >> 24));
    out.push_back(char(v >> 16));
    out.push_back(char(v >> 8));
    out.push_back(char(v));
}

void putU16(std::string &out, uint16_t v)
{
    out.push_back(char(v >> 8));
    out.push_back(char(v));
}

void setU32(std::string &s, size_t at, uint32_t v)
{
    s[at] = char(v >> 24);
    s[at + 1] = char(v >> 16);
    s[at + 2] = char(v >> 8);
    s[at + 3] = char(v);
}

// A legacy entry as real files hold it, built here byte by byte rather
// than by the codec under test. `lenEntry` other than 56 truncates or
// pads it and says so in its own header.
std::string entry(uint32_t slot, uint32_t timeMs, uint32_t loopEndMs = 0, bool olderGeneration = false,
                  uint32_t lenEntry = 56)
{
    std::string e;
    e += "PCPT";
    putU32(e, 28);
    putU32(e, lenEntry);
    putU32(e, slot);
    putU32(e, olderGeneration ? 1 : 0);
    putU32(e, olderGeneration ? 0 : 0x00010000);
    putU16(e, 0xFFFF);
    putU16(e, 0xFFFF);
    e.push_back(char(loopEndMs ? 2 : 1));
    e += std::string("\x00\x03\xe8", 3);
    putU32(e, timeMs);
    putU32(e, loopEndMs ? loopEndMs : (olderGeneration ? 0 : 0xFFFFFFFF));
    e.append(16, '\0');
    e.resize(lenEntry, '\0');
    return e;
}

// A PCOB section with the given entries; header fields overridable to
// plant the malformed ones.
std::string pcob(uint32_t listType, const std::vector<std::string> &entries, uint32_t memoryCount = 0xFFFFFFFF,
                 int numCuesOverride = -1, int lenTagOverride = -1)
{
    std::string body;
    for (const auto &e : entries) {
        body += e;
    }
    std::string s = "PCOB";
    putU32(s, 24);
    putU32(s, lenTagOverride >= 0 ? uint32_t(lenTagOverride) : uint32_t(24 + body.size()));
    putU32(s, listType);
    putU16(s, 0);
    putU16(s, numCuesOverride >= 0 ? uint16_t(numCuesOverride) : uint16_t(entries.size()));
    putU32(s, memoryCount);
    s += body;
    return s;
}

// `original` with its PCOB of `listType` replaced by `planted` (which may
// be several sections, or one whose own length fields lie), len_file
// set to the result's size.
std::string plant(const std::string &original, uint32_t listType, const std::string &planted)
{
    for (const auto &section : sectionsOf(original)) {
        if (section.fourcc == "PCOB" && be32(section.bytes, 12) == listType) {
            std::string out = original.substr(0, section.offset) + planted
                + original.substr(section.offset + section.bytes.size());
            setU32(out, 8, uint32_t(out.size()));
            return out;
        }
    }
    throw std::runtime_error("the fixture .DAT has no PCOB of that type to replace");
}

void writeFile(const fs::path &path, const std::string &bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), std::streamsize(bytes.size()));
}

// slot/time of every entry in a file's legacy list of one type, strictly.
std::multimap<uint32_t, uint32_t> legacyEntries(const fs::path &path, uint32_t listType)
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
        if (tag && uint32_t(tag->type()) == listType) {
            for (const auto &cue : *tag->cues()) {
                out.emplace(cue->hot_cue(), cue->time());
            }
        }
    }
    return out;
}

// The damaged memory list from WHALESHARK's HANGING TREE .DAT, byte for
// byte: a new memory-list header whose memory_count reads 0000ffff, then
// bytes 24..55 of a PCPT loop entry, then a whole memory-list header.
std::string whalesharkMemoryList()
{
    const unsigned char bytes[] = {
        0x50, 0x43, 0x4f, 0x42, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x03, 0xe8,
        0x00, 0x00, 0x0c, 0x8d, 0x00, 0x00, 0x19, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x43, 0x4f, 0x42, 0x00, 0x00, 0x00, 0x18,
        0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff,
    };
    return std::string(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_cue_writer_malformed_lists_test <anonymized library dir>\n";
        return 2;
    }
    const fs::path fixtureRoot = fs::path(argv[1]) / "rekordbox";
    const std::string fixtureUtf8 = pathToUtf8(fixtureRoot);

    // One track with both files, whose .DAT has an empty memory list and
    // an empty hot list: the shape of both WHALESHARK tracks before the
    // sync.
    const AnlzPathIndex fixtureIndex(fixtureUtf8);
    uint32_t trackId = 0;
    std::string analyzePath;
    for (uint32_t id = 1; id < 50000 && trackId == 0; ++id) {
        const auto path = fixtureIndex.pathFor(id);
        if (!path) {
            continue;
        }
        const fs::path dat = pathFromUtf8(datAnlzPath(fixtureUtf8, *path));
        if (!fs::exists(dat) || !fs::exists(pathFromUtf8(extAnlzPath(fixtureUtf8, *path)))) {
            continue;
        }
        const std::string bytes = readFile(dat);
        if (legacyList(bytes, 0).state == ListState::Empty && legacyList(bytes, 1).state == ListState::Empty) {
            trackId = id;
            analyzePath = *path;
        }
    }
    if (trackId == 0) {
        std::cerr << "no fixture track with empty legacy lists in both files\n";
        return 2;
    }

    const fs::path scratch = testing::scratchRoot() / "rekordbox-cue-writer-malformed-lists";
    fs::remove_all(scratch);
    const fs::path root = scratch / "PIONEER";
    fs::create_directories(root / "rekordbox");
    fs::copy_file(fixtureRoot / "rekordbox" / "export.pdb", root / "rekordbox" / "export.pdb");
    const std::string rootUtf8 = pathToUtf8(root);
    const fs::path datPath = pathFromUtf8(datAnlzPath(rootUtf8, analyzePath));
    const fs::path extPath = pathFromUtf8(extAnlzPath(rootUtf8, analyzePath));
    fs::create_directories(datPath.parent_path());
    const std::string datOriginal = readFile(pathFromUtf8(datAnlzPath(fixtureUtf8, analyzePath)));
    const std::string extOriginal = readFile(pathFromUtf8(extAnlzPath(fixtureUtf8, analyzePath)));
    std::cout << "track id " << trackId << ", analysis " << analyzePath << "\n";

    const AnlzPathIndex index(rootUtf8);
    RekordboxCueWriter writer(rootUtf8, &index);

    // What the WHALESHARK sync wrote for HANGING TREE, less colours and
    // comments, which live in PCO2 alone.
    std::vector<domain::CuePoint> cues;
    {
        domain::CuePoint hot;
        hot.kind = domain::CuePoint::Kind::Hot;
        hot.hotCueNumber = 1;
        hot.positionMs = 3213;
        hot.isLoop = true;
        hot.loopEndMs = 6478;
        cues.push_back(hot);
        domain::CuePoint memory;
        memory.kind = domain::CuePoint::Kind::Memory;
        memory.positionMs = 5662;
        cues.push_back(memory);
    }

    struct Case
    {
        std::string name;
        uint32_t listType;
        std::string planted;
        // The bytes the list must come back as, when it must survive the
        // write unchanged; empty when it is rebuilt.
        std::string keptAs;
    };
    std::string notPcpt = entry(0, 5662);
    notPcpt.replace(0, 4, std::string("\x00\x00\xff\xff", 4));
    std::string hotNotPcpt = entry(1, 3213, 6478);
    hotNotPcpt.replace(0, 4, std::string("\x00\x00\xff\xff", 4));
    const std::string olderGeneration = pcob(0, {entry(0, 5662, 0, true)}, 0);

    const std::vector<Case> cases = {
        {"a memory list with a 20-byte header", 0,
         [] {
             std::string s = "PCOB";
             putU32(s, 20);
             putU32(s, 20);
             putU32(s, 0);
             putU32(s, 0);
             return s;
         }(),
         {}},
        {"a memory list with num_cues 0 but len_tag 80", 0, pcob(0, {entry(0, 5662)}, 0xFFFFFFFF, 0), {}},
        {"a memory entry with len_entry 52", 0, pcob(0, {entry(0, 5662, 0, false, 52)}), {}},
        {"a memory entry with len_entry 60", 0, pcob(0, {entry(0, 5662, 0, false, 60)}), {}},
        {"a memory entry that is 56 bytes but not PCPT", 0, pcob(0, {notPcpt}), {}},
        {"a hot entry that is 56 bytes but not PCPT", 1, pcob(1, {hotNotPcpt}), {}},
        {"the WHALESHARK memory list itself", 0, whalesharkMemoryList(), {}},
        {"a memory list of the older generation (status 1, memory_count 0)", 0, olderGeneration, olderGeneration},
        {"two memory lists", 0, pcob(0, {}) + pcob(0, {}), {}},
    };

    for (const auto &c : cases) {
        std::cout << c.name << ":\n";
        writeFile(datPath, plant(datOriginal, c.listType, c.planted));
        writeFile(extPath, extOriginal);
        try {
            writer.writeHotCues(std::to_string(trackId), cues);
        } catch (const std::exception &e) {
            check(false, std::string("the writer wrote the track (it threw: ") + e.what() + ")");
            continue;
        }
        bool wellFormed = true;
        for (const fs::path &file : {datPath, extPath}) {
            for (const auto &problem : strictProblems(file)) {
                std::cout << "    " << problem << "\n";
                wellFormed = false;
            }
        }
        check(wellFormed, "both files are strictly well formed");
        if (!wellFormed) {
            continue;
        }
        const auto memory = legacyEntries(datPath, 0);
        check(memory.size() == 1 && memory.begin()->second == 5662, "the .DAT memory list holds the one memory cue");
        const auto hot = legacyEntries(datPath, 1);
        check(hot.size() == 1 && hot.count(1) == 1 && hot.find(1)->second == 3213,
              "the .DAT hot list holds hot cue 1");
        // Only values real sections hold (the survey: 0xFFFFFFFF, or 0 in
        // a populated memory list), never one read out of a damaged
        // header such as WHALESHARK's 0000ffff.
        const std::string memoryList = legacyList(readFile(datPath), 0).bytes;
        const uint32_t memoryCount = memoryList.size() >= 24 ? be32(memoryList, 20) : 1;
        check(memoryCount == 0xFFFFFFFFu || memoryCount == 0, "the memory list's memory_count is one real files hold");
        if (!c.keptAs.empty()) {
            check(legacyList(readFile(datPath), c.listType).bytes == c.keptAs,
                  "the list was carried over byte for byte");
        }
    }

    fs::remove_all(scratch);
    if (failures > 0) {
        std::cout << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
