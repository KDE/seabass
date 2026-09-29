// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The rekordbox cue writer leaves out a cue it cannot write as a time.
//
// Every position in the analysis files is an unsigned 32-bit count of
// milliseconds. The writer cast the domain's double straight in, so a
// cue before the start of the track wrapped: Sanctum on WHALESHARK
// (2026-09-26) carried hot cue 1 at 0xffff667c, Engine's -39300 ms,
// which a player reads as some 49 days in. domain::isJunkCue calls any
// negative position junk, a sentinel that points nowhere, so the writer
// now leaves such a cue off the rekordbox side, and a loop whose end is
// negative with it. The test writes a set holding both kinds beside two
// good cues and requires exactly the good ones in all four lists, each
// file strictly well formed, and no time in either file past 2^31 ms.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <kaitai/kaitaistream.h>

#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_cue_codec.hpp"
#include "infrastructure/rekordbox/anlz_legacy_cue_codec.hpp"
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

// (list, slot, time, loop end) for one entry of one list.
using Entry = std::tuple<std::string, uint32_t, uint32_t, uint32_t>;

// Every entry of every cue list in a file, decoded by the codecs; a
// list the codec refuses is reported as such, not skipped.
std::set<Entry> entriesOf(const fs::path &path, const std::string &file)
{
    std::set<Entry> out;
    for (const auto &section : sectionsOf(readFile(path))) {
        if (section.fourcc != "PCOB" && section.fourcc != "PCO2") {
            continue;
        }
        const uint32_t type = be32(section.bytes, 12);
        const std::string list = file + " " + section.fourcc + (type == CueListTypeHot ? " hot" : " memory");
        try {
            if (section.fourcc == "PCOB") {
                for (const auto &e : AnlzLegacyCueCodec::decodeCues(section.bytes)) {
                    out.insert({list, e.hotCueNumber, e.timeMs, e.isLoop ? e.loopEndMs : 0});
                }
            } else {
                for (const auto &e : AnlzCueCodec::decodeHotCues(section.bytes, type)) {
                    out.insert({list, e.hotCueNumber, e.timeMs, e.isLoop ? e.loopEndMs : 0});
                }
            }
        } catch (const std::exception &e) {
            out.insert({list + " UNREADABLE: " + e.what(), 0, 0, 0});
        }
    }
    return out;
}

domain::CuePoint hot(int slot, double ms)
{
    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Hot;
    cue.hotCueNumber = slot;
    cue.positionMs = ms;
    return cue;
}

domain::CuePoint memory(double ms)
{
    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Memory;
    cue.positionMs = ms;
    return cue;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_cue_writer_negative_position_test <anonymized library dir>\n";
        return 2;
    }
    const fs::path fixtureRoot = fs::path(argv[1]) / "rekordbox";
    const std::string fixtureUtf8 = pathToUtf8(fixtureRoot);
    const AnlzPathIndex fixtureIndex(fixtureUtf8);
    uint32_t trackId = 0;
    std::string analyzePath;
    for (uint32_t id = 1; id < 50000 && trackId == 0; ++id) {
        const auto path = fixtureIndex.pathFor(id);
        if (path && fs::exists(pathFromUtf8(datAnlzPath(fixtureUtf8, *path)))
            && fs::exists(pathFromUtf8(extAnlzPath(fixtureUtf8, *path)))) {
            trackId = id;
            analyzePath = *path;
        }
    }
    if (trackId == 0) {
        std::cerr << "no fixture track with both analysis files\n";
        return 2;
    }

    const fs::path scratch = testing::scratchRoot() / "rekordbox-cue-writer-negative-position";
    fs::remove_all(scratch);
    const fs::path root = scratch / "PIONEER";
    fs::create_directories(root / "rekordbox");
    fs::copy_file(fixtureRoot / "rekordbox" / "export.pdb", root / "rekordbox" / "export.pdb");
    const std::string rootUtf8 = pathToUtf8(root);
    const fs::path datPath = pathFromUtf8(datAnlzPath(rootUtf8, analyzePath));
    const fs::path extPath = pathFromUtf8(extAnlzPath(rootUtf8, analyzePath));
    fs::create_directories(datPath.parent_path());
    fs::copy_file(pathFromUtf8(datAnlzPath(fixtureUtf8, analyzePath)), datPath);
    fs::copy_file(pathFromUtf8(extAnlzPath(fixtureUtf8, analyzePath)), extPath);

    domain::CuePoint loop = hot(6, 3000);
    loop.isLoop = true;
    loop.loopEndMs = -10;
    // Two cues the files can hold, in slot 2 (.DAT) and as a memory cue,
    // and five they cannot: Sanctum's own -39300 ms in slot 1 (.DAT),
    // Engine's -1 sentinel in slot 5 (.EXT), a memory cue a fraction of
    // a millisecond before the start, a loop ending before it, and NaN.
    const std::vector<domain::CuePoint> cues = {hot(1, -39300), hot(2, 2000),       hot(5, -1), memory(-0.5),
                                                memory(4000),   memory(std::nan("")), loop};

    const AnlzPathIndex index(rootUtf8);
    RekordboxCueWriter writer(rootUtf8, &index);
    std::string error;
    try {
        writer.writeHotCues(std::to_string(trackId), cues);
    } catch (const std::exception &e) {
        error = e.what();
    }
    check(error.empty(), "the write succeeded" + (error.empty() ? std::string() : " (threw: " + error + ")"));

    for (const auto &path : {datPath, extPath}) {
        const auto problems = strictProblems(path);
        for (const auto &p : problems) {
            std::cout << "    " << p << "\n";
        }
        check(problems.empty(), pathToUtf8(path.filename()) + " is strictly well formed");
    }

    std::set<Entry> written = entriesOf(datPath, ".DAT");
    for (const auto &e : entriesOf(extPath, ".EXT")) {
        written.insert(e);
    }
    const std::set<Entry> expected = {
        {".DAT PCOB hot", 2, 2000, 0},
        {".DAT PCOB memory", 0, 4000, 0},
        {".EXT PCO2 hot", 2, 2000, 0},
        {".EXT PCO2 memory", 0, 4000, 0},
    };
    bool wrapped = false;
    for (const auto &[list, slot, time, end] : written) {
        std::cout << "    " << list << " slot " << slot << " at " << time << " ms" << (end ? " to " + std::to_string(end) : "")
                  << "\n";
        wrapped = wrapped || time >= 0x80000000u || end >= 0x80000000u;
    }
    check(!wrapped, "no time in either file is past 2^31 ms");
    check(written == expected, "exactly the two good cues, in each of the four lists");

    fs::remove_all(scratch);
    if (failures > 0) {
        std::cout << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
