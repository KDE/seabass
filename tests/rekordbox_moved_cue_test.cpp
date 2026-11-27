// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A cue that moves keeps what the file holds of it (Align Cue Points).
//
// On a copy of the cue-offsets stick's rekordbox export, hot cue B of
// track 102 is given a comment in its analysis file, by hand, as rekordbox
// stores one. Then the writer is handed the track's cue list with that cue
// 2 ms earlier:
//  - named as moved (rekordbox::movedCues), the cue is at its new place
//    with its comment, and every other cue's bytes are as they were;
//  - not named, the same write encodes it bare and the comment is gone,
//    which is what this test would see if the naming did nothing.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;

namespace
{

uint32_t u32(const std::string &bytes, std::size_t at)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at])) << 24)
           | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16)
           | (static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8)
           | static_cast<uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

void putU32(std::string &bytes, std::size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        bytes[at + static_cast<std::size_t>(i)] = static_cast<char>((value >> (24 - 8 * i)) & 0xff);
    }
}

// The file's PCO2 section of `listType`, or null.
AnlzRawSection *cueSection(AnlzFile &file, uint32_t listType)
{
    for (AnlzRawSection &section : file.sections) {
        if (section.rawBytes.size() >= 16 && section.rawBytes.compare(0, 4, "PCO2") == 0 && u32(section.rawBytes, 12) == listType) {
            return &section;
        }
    }
    return nullptr;
}

std::vector<RawHotCueEntry> entriesOf(const std::string &extPath, uint32_t listType)
{
    AnlzFile file = AnlzFile::readRaw(extPath);
    AnlzRawSection *section = cueSection(file, listType);
    return section ? AnlzCueCodec::decodeHotCues(section->rawBytes, listType) : std::vector<RawHotCueEntry>{};
}

// The cue list the entries stand for, colours left out (a cue with no
// colour named contradicts none in the file).
std::vector<domain::CuePoint> cuesOf(const std::vector<RawHotCueEntry> &hot, const std::vector<RawHotCueEntry> &memory)
{
    std::vector<domain::CuePoint> cues;
    const auto add = [&cues](const RawHotCueEntry &entry, bool isHot) {
        domain::CuePoint cue;
        cue.kind = isHot ? domain::CuePoint::Kind::Hot : domain::CuePoint::Kind::Memory;
        cue.hotCueNumber = isHot ? static_cast<int>(entry.hotCueNumber) : 0;
        cue.positionMs = entry.timeMs;
        cue.isLoop = entry.isLoop;
        cue.loopEndMs = entry.isLoop ? entry.loopEndMs : 0.0;
        cues.push_back(cue);
    };
    for (const RawHotCueEntry &entry : hot) {
        add(entry, true);
    }
    for (const RawHotCueEntry &entry : memory) {
        add(entry, false);
    }
    return cues;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_moved_cue_test <cue-offsets stick>\n";
        return 2;
    }
    const fs::path stick = testing::scratchRoot() / "seabass_rekordbox_moved_cue_test";
    fs::remove_all(stick);
    fs::create_directories(stick);
    fs::copy(pathFromUtf8(argv[1]) / "PIONEER", stick / "PIONEER", fs::copy_options::recursive);
    const std::string root = pathToUtf8(stick / "PIONEER");

    const auto analyzePath = findAnlzPathForTrackId(root, 102);
    assert(analyzePath && "track 102 has an analysis file");
    const std::string ext = extAnlzPath(root, *analyzePath);

    // "Drop" in UTF-16, big end first, as rekordbox stores a comment.
    const std::string comment(std::string("\0D\0r\0o\0p", 8));
    uint32_t placeBefore = 0;
    {
        AnlzFile file = AnlzFile::readRaw(ext);
        AnlzRawSection *section = cueSection(file, CueListTypeHot);
        assert(section && "the file has a hot cue list");
        std::vector<RawHotCueEntry> entries = AnlzCueCodec::decodeHotCues(section->rawBytes, CueListTypeHot);
        bool given = false;
        for (RawHotCueEntry &entry : entries) {
            if (entry.hotCueNumber != 2) {
                continue;
            }
            assert(entry.rawBytes.size() >= 44 && u32(entry.rawBytes, 40) == 0 && "hot cue B has no comment yet");
            // The fixed part, the comment, then what followed (the colour).
            entry.rawBytes.insert(44, comment);
            putU32(entry.rawBytes, 40, static_cast<uint32_t>(comment.size()));
            putU32(entry.rawBytes, 8, static_cast<uint32_t>(entry.rawBytes.size()));
            placeBefore = entry.timeMs;
            given = true;
        }
        assert(given && "track 102 has a hot cue B");
        section->rawBytes = AnlzCueCodec::encodeHotCues(entries, CueListTypeHot);
        file.writeRaw(ext);
    }
    const std::vector<RawHotCueEntry> hotBefore = entriesOf(ext, CueListTypeHot);
    const std::vector<RawHotCueEntry> memoryBefore = entriesOf(ext, CueListTypeMemory);
    const std::vector<domain::CuePoint> cues = cuesOf(hotBefore, memoryBefore);
    assert(placeBefore > 2);

    const auto movedBy = [&](double ms) {
        std::vector<domain::CuePoint> after = cues;
        for (domain::CuePoint &cue : after) {
            if (cue.kind == domain::CuePoint::Kind::Hot && cue.hotCueNumber == 2) {
                cue.positionMs += ms;
            }
        }
        return after;
    };
    const auto hotCueB = [&]() {
        for (const RawHotCueEntry &entry : entriesOf(ext, CueListTypeHot)) {
            if (entry.hotCueNumber == 2) {
                return entry;
            }
        }
        assert(false && "hot cue B is still in the list");
        return RawHotCueEntry{};
    };

    // 1. Named as moved: at its new place, comment and all.
    {
        const std::vector<domain::CuePoint> after = movedBy(-2.0);
        const std::vector<MovedCue> moved = movedCues(cues, after);
        assert(moved.size() == 1 && moved[0].hot && moved[0].hotCueNumber == 2 && moved[0].fromMs == placeBefore
               && moved[0].toMs == placeBefore - 2);
        RekordboxCueWriter(root).writeHotCuesMoved("102", after, moved);

        const RawHotCueEntry b = hotCueB();
        assert(b.timeMs == placeBefore - 2 && "moved 2 ms earlier");
        assert(u32(b.rawBytes, 40) == comment.size() && b.rawBytes.find(comment) != std::string::npos
               && "its comment went with it");
        const std::vector<RawHotCueEntry> hotAfter = entriesOf(ext, CueListTypeHot);
        assert(hotAfter.size() == hotBefore.size());
        for (std::size_t i = 0; i < hotBefore.size(); ++i) {
            if (hotBefore[i].hotCueNumber != 2) {
                assert(hotAfter[i].rawBytes == hotBefore[i].rawBytes && "every other hot cue, byte for byte");
            } else {
                assert(hotAfter[i].rawBytes.size() == hotBefore[i].rawBytes.size());
                for (std::size_t at = 0; at < hotBefore[i].rawBytes.size(); ++at) {
                    assert((at >= 20 && at < 24) || hotAfter[i].rawBytes[at] == hotBefore[i].rawBytes[at]);
                }
            }
        }
        const std::vector<RawHotCueEntry> memoryAfter = entriesOf(ext, CueListTypeMemory);
        assert(memoryAfter.size() == memoryBefore.size());
        for (std::size_t i = 0; i < memoryBefore.size(); ++i) {
            assert(memoryAfter[i].rawBytes == memoryBefore[i].rawBytes && "every memory cue, byte for byte");
        }
        std::cout << "case 1 (a cue named as moved keeps its comment; the others keep every byte) OK\n";
    }

    // 2. The same kind of write with the move not named: the cue is at
    //    its place, bare. What case 1 would look like were `moved` unused.
    {
        std::vector<domain::CuePoint> now = movedBy(-2.0);
        std::vector<domain::CuePoint> after = movedBy(-4.0);
        (void)now;
        RekordboxCueWriter(root).writeHotCues("102", after);
        const RawHotCueEntry b = hotCueB();
        assert(b.timeMs == placeBefore - 4);
        assert(u32(b.rawBytes, 40) == 0 && b.rawBytes.find(comment) == std::string::npos
               && "not named as moved: encoded afresh, without the comment");
        std::cout << "case 2 (the same move not named loses the comment) OK\n";
    }

    fs::remove_all(stick);
    std::cout << "rekordbox_moved_cue_test: all cases passed\n";
    return 0;
}
