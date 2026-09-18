// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The legacy PCOB cue list, checked against real rekordbox-written
// bytes rather than against our own idea of the format.
//
// Two things are proved here, both over the committed anonymized
// fixture (a real export, cue positions untouched by anonymization):
//
//   1. Every real PCOB section decodes and re-encodes to the exact same
//      bytes. A codec that round-trips 8000 real sections is one that
//      understood them; one that reorders a field, drops a byte or
//      normalises an older writer's values into a newer shape fails
//      here rather than on a player.
//   2. The split the writer depends on is a property of real data, not
//      an assumption: a .DAT file's hot list holds only slots 1-3 and a
//      .EXT file's only 4-8. If a future fixture disagrees, the writer's
//      rule is wrong and this says so.
//
// Both counts are asserted to be non-trivial. A fixture regenerated
// without cues would otherwise pass this test having checked nothing,
// which is the failure mode this project has been bitten by before.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::rekordbox;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cout << "  FAIL: " << what << "\n";
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

struct Section
{
    std::string fourcc;
    std::string bytes;
};

// The same section-level walk AnlzFile does, kept local so this test
// depends on the codec alone.
std::vector<Section> sectionsOf(const std::string &data)
{
    std::vector<Section> out;
    if (data.size() < 8 || data.compare(0, 4, "PMAI") != 0) {
        return out;
    }
    size_t pos = readU32(data, 4);
    while (pos + 12 <= data.size()) {
        const uint32_t lenTag = readU32(data, pos + 8);
        if (lenTag < 12 || pos + lenTag > data.size()) {
            break;
        }
        out.push_back({data.substr(pos, 4), data.substr(pos, lenTag)});
        pos += lenTag;
    }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: anlz_legacy_cue_codec_test <anonymized library dir>\n";
        return 2;
    }
    const fs::path anlzRoot = fs::path(argv[1]) / "rekordbox" / "USBANLZ";
    if (!fs::is_directory(anlzRoot)) {
        std::cerr << "no USBANLZ under " << anlzRoot << "\n";
        return 2;
    }

    size_t sectionsSeen = 0;
    size_t hotSections = 0;
    size_t entriesSeen = 0;
    size_t roundTripped = 0;
    std::set<uint32_t> datSlots;
    std::set<uint32_t> extSlots;

    for (const auto &entry : fs::recursive_directory_iterator(anlzRoot)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        const bool isDat = name.size() > 4 && name.compare(name.size() - 4, 4, ".DAT") == 0;
        const bool isExt = name.size() > 4 && name.compare(name.size() - 4, 4, ".EXT") == 0;
        if (!isDat && !isExt) {
            continue;
        }
        for (const auto &section : sectionsOf(readWholeFile(entry.path()))) {
            if (section.fourcc != "PCOB") {
                continue;
            }
            ++sectionsSeen;
            const uint32_t listType = readU32(section.bytes, 12);
            std::vector<LegacyCueEntry> decoded;
            try {
                decoded = AnlzLegacyCueCodec::decodeCues(section.bytes);
            } catch (const std::exception &e) {
                check(false, std::string("decoding ") + entry.path().string() + ": " + e.what());
                continue;
            }
            entriesSeen += decoded.size();
            if (listType == CueListTypeHot && !decoded.empty()) {
                ++hotSections;
                for (const auto &cue : decoded) {
                    (isDat ? datSlots : extSlots).insert(cue.hotCueNumber);
                }
            }
            const std::string reencoded =
                AnlzLegacyCueCodec::encodeCues(decoded, listType, AnlzLegacyCueCodec::memoryCountOf(section.bytes));
            if (reencoded == section.bytes) {
                ++roundTripped;
            } else {
                check(false, "re-encoding changed the bytes of " + entry.path().string());
            }
        }
    }

    std::cout << "PCOB sections: " << sectionsSeen << ", entries: " << entriesSeen
              << ", byte-identical round trips: " << roundTripped << "\n";
    std::cout << "hot lists with entries: " << hotSections << "\n";

    check(roundTripped == sectionsSeen, "every real PCOB section re-encodes to its own bytes");

    // Guards against a fixture that lost its cues making all of the
    // above pass having examined nothing.
    check(sectionsSeen >= 1000, "the fixture still holds a realistic number of PCOB sections");
    check(entriesSeen >= 100, "the fixture still holds a realistic number of cue entries");
    check(hotSections >= 30, "the fixture still holds a realistic number of populated hot lists");

    std::cout << "hot slots in .DAT:";
    for (uint32_t slot : datSlots) std::cout << " " << slot;
    std::cout << "\nhot slots in .EXT:";
    for (uint32_t slot : extSlots) std::cout << " " << slot;
    std::cout << "\n";

    // The rule RekordboxCueWriter splits on, asserted against real data.
    for (uint32_t slot : datSlots) {
        check(slot >= 1 && slot <= 3, "a .DAT hot list holds only slots 1-3");
    }
    for (uint32_t slot : extSlots) {
        check(slot >= 4 && slot <= 8, "an .EXT hot list holds only slots 4-8");
    }
    check(!datSlots.empty() && !extSlots.empty(), "both files contributed hot cues to check the split against");

    if (failures > 0) {
        std::cout << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
