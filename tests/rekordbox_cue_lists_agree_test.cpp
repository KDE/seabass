// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// After a rekordbox cue write, every list in the export says the same
// thing.
//
// This is the test that was missing when issue #33 happened. A rekordbox
// export stores its hot cues three times -- PCO2 in the .EXT file, the
// legacy PCOB in the .EXT, and the legacy PCOB in the .DAT -- and the
// writer updated only the first. Everything we had still passed, because
// the round-trip test read back through the same list it had just
// written. An XDJ-RX2, which reads the other two, showed stale pads.
//
// So the assertion here is deliberately not "the cue we wrote can be read
// back". It is "no representation of this track disagrees with another",
// checked by reading the raw bytes of all three rather than through the
// project's own reader, which prefers PCO2 and would hide exactly the
// failure this guards. A fourth list, or a writer that forgets one,
// fails here.
//
// It also checks the inverse, which is the half that rots quietly: a cue
// REMOVED from a track must disappear from every list. A stale extra pad
// on a player is as wrong as a missing one, and nothing else would catch
// it.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::rekordbox;

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

uint32_t readU32(const std::string &bytes, size_t offset)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset])) << 24)
        | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 8)
        | static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3]));
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Every section of an ANLZ file, by fourcc, read straight from disk.
std::vector<std::pair<std::string, std::string>> sectionsOf(const fs::path &path)
{
    std::vector<std::pair<std::string, std::string>> out;
    const std::string data = readWholeFile(path);
    if (data.size() < 8 || data.compare(0, 4, "PMAI") != 0) {
        return out;
    }
    size_t pos = readU32(data, 4);
    while (pos + 12 <= data.size()) {
        const uint32_t lenTag = readU32(data, pos + 8);
        if (lenTag < 12 || pos + lenTag > data.size()) {
            break;
        }
        out.emplace_back(data.substr(pos, 4), data.substr(pos, lenTag));
        pos += lenTag;
    }
    return out;
}

// slot -> position, from the legacy list of `listType` in one file.
std::map<uint32_t, uint32_t> legacyCues(const fs::path &path, uint32_t listType)
{
    std::map<uint32_t, uint32_t> out;
    for (const auto &[fourcc, bytes] : sectionsOf(path)) {
        if (fourcc != "PCOB" || readU32(bytes, 12) != listType) {
            continue;
        }
        for (const auto &cue : AnlzLegacyCueCodec::decodeCues(bytes)) {
            out[cue.hotCueNumber] = cue.timeMs;
        }
    }
    return out;
}

// The same, from the modern list.
std::map<uint32_t, uint32_t> modernCues(const fs::path &path, uint32_t listType)
{
    std::map<uint32_t, uint32_t> out;
    for (const auto &[fourcc, bytes] : sectionsOf(path)) {
        if (fourcc != "PCO2" || readU32(bytes, 12) != listType) {
            continue;
        }
        for (const auto &cue : AnlzCueCodec::decodeHotCues(bytes, listType)) {
            out[cue.hotCueNumber] = cue.timeMs;
        }
    }
    return out;
}

domain::CuePoint hotCue(int slot, double positionMs)
{
    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Hot;
    cue.hotCueNumber = slot;
    cue.positionMs = positionMs;
    return cue;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_cue_lists_agree_test <anonymized library dir>\n";
        return 2;
    }
    // The fixture's "rekordbox" directory is itself the PIONEER root:
    // it holds rekordbox/export.pdb and USBANLZ/ directly.
    const fs::path pioneerRoot = fs::path(argv[1]) / "rekordbox";
    const fs::path exportPdb = pioneerRoot / "rekordbox" / "export.pdb";
    if (!fs::exists(exportPdb)) {
        std::cerr << "no export.pdb under " << pioneerRoot << "\n";
        return 2;
    }

    // One track whose analysis pair is both present: the write touches
    // both files, so a track missing either proves nothing here.
    const AnlzPathIndex index(pioneerRoot.string());
    uint32_t chosenId = 0;
    std::string chosenPath;
    for (uint32_t id = 1; id < 50000 && chosenId == 0; ++id) {
        auto path = index.pathFor(id);
        if (!path) {
            continue;
        }
        const std::string dat = datAnlzPath(pioneerRoot.string(), *path);
        const std::string ext = extAnlzPath(pioneerRoot.string(), *path);
        if (fs::exists(dat) && fs::exists(ext)) {
            chosenId = id;
            chosenPath = *path;
        }
    }
    if (chosenId == 0) {
        std::cerr << "no track in the fixture has both a .DAT and an .EXT analysis file\n";
        return 2;
    }

    // A copy of just what the writer needs: the database it resolves
    // paths through, and that one track's analysis directory.
    const fs::path scratch = testing::scratchRoot() / "rekordbox-cue-lists-agree";
    fs::remove_all(scratch);
    const fs::path root = scratch / "PIONEER";
    fs::create_directories(root / "rekordbox");
    fs::copy_file(exportPdb, root / "rekordbox" / "export.pdb");
    const fs::path anlzDir = fs::path(datAnlzPath(pioneerRoot.string(), chosenPath)).parent_path();
    const fs::path anlzTarget = root / "USBANLZ" / anlzDir.parent_path().filename() / anlzDir.filename();
    fs::create_directories(anlzTarget);
    fs::copy(anlzDir, anlzTarget, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    const fs::path datPath = datAnlzPath(root.string(), chosenPath);
    const fs::path extPath = extAnlzPath(root.string(), chosenPath);
    std::cout << "track id " << chosenId << ", analysis at " << anlzTarget << "\n";

    // --- a full set of cues, every slot a player can show -------------
    std::vector<domain::CuePoint> cues;
    for (int slot = 1; slot <= 8; ++slot) {
        cues.push_back(hotCue(slot, slot * 10000.0));
    }
    domain::CuePoint memory;
    memory.kind = domain::CuePoint::Kind::Memory;
    memory.positionMs = 4399.0;
    cues.push_back(memory);

    RekordboxCueWriter writer(root.string());
    writer.writeHotCues(std::to_string(chosenId), cues);

    const auto modernHot = modernCues(extPath, CueListTypeHot);
    const auto datHot = legacyCues(datPath, CueListTypeHot);
    const auto extHot = legacyCues(extPath, CueListTypeHot);
    const auto datMemory = legacyCues(datPath, CueListTypeMemory);

    std::cout << "after writing 8 hot cues and a memory cue:\n";
    check(modernHot.size() == 8, "the modern list holds all 8 hot cues");
    check(datHot.size() == 3, "the .DAT legacy list holds 3 hot cues");
    check(extHot.size() == 5, "the .EXT legacy list holds 5 hot cues");

    for (uint32_t slot = 1; slot <= 3; ++slot) {
        check(datHot.count(slot) == 1, "slot " + std::to_string(slot) + " is in the .DAT legacy list");
        check(extHot.count(slot) == 0, "slot " + std::to_string(slot) + " is NOT in the .EXT legacy list");
    }
    for (uint32_t slot = 4; slot <= 8; ++slot) {
        check(extHot.count(slot) == 1, "slot " + std::to_string(slot) + " is in the .EXT legacy list");
        check(datHot.count(slot) == 0, "slot " + std::to_string(slot) + " is NOT in the .DAT legacy list");
    }

    // The point of the whole test: no list disagrees with another.
    bool agree = true;
    for (const auto &[slot, position] : modernHot) {
        const auto &legacy = slot <= 3 ? datHot : extHot;
        auto it = legacy.find(slot);
        if (it == legacy.end() || it->second != position) {
            agree = false;
            std::cout << "    slot " << slot << ": modern says " << position << ", legacy says "
                      << (it == legacy.end() ? "nothing" : std::to_string(it->second)) << "\n";
        }
    }
    check(agree, "every hot cue sits at the same position in the modern and legacy lists");
    check(datMemory.size() == 1 && datMemory.begin()->second == 4399,
          "the memory cue reached the .DAT legacy list");

    // --- and the half that rots quietly: a removed cue leaves ---------
    std::vector<domain::CuePoint> fewer{hotCue(1, 10000.0), hotCue(5, 50000.0)};
    writer.writeHotCues(std::to_string(chosenId), fewer);

    const auto modernAfter = modernCues(extPath, CueListTypeHot);
    const auto datAfter = legacyCues(datPath, CueListTypeHot);
    const auto extAfter = legacyCues(extPath, CueListTypeHot);
    const auto memoryAfter = legacyCues(datPath, CueListTypeMemory);

    std::cout << "after rewriting the track with only slots 1 and 5:\n";
    check(modernAfter.size() == 2, "the modern list dropped the cues that went");
    check(datAfter.size() == 1 && datAfter.count(1) == 1, "the .DAT legacy list kept only slot 1");
    check(extAfter.size() == 1 && extAfter.count(5) == 1, "the .EXT legacy list kept only slot 5");
    check(memoryAfter.empty(), "the memory cue that went is gone from the legacy list too");

    fs::remove_all(scratch);

    if (failures > 0) {
        std::cout << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
