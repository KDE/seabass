// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The rekordbox cue writer reads back what it wrote, and puts the files
// back when that is not what it meant.
//
// WHALESHARK (2026-09-26) held two .DAT files, both last touched by a
// sync, whose memory list the reader could not parse. Nothing on the
// stick said so until a later read failed. The OneLibrary writer checks
// its own write by reading it back (m_verifyDb); this is the same
// discipline for the analysis files. The test damages a file between the
// write and the read-back, through the writer's test hook, and requires
// the call to throw naming that file, with BOTH files byte for byte as
// they were before the call: the .EXT as well when the .DAT failed, so a
// failed track never leaves its lists disagreeing.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
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

void writeFile(const fs::path &path, const std::string &bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), std::streamsize(bytes.size()));
}

// The first entry of the file's last PCOB loses its magic: the kind of
// list WHALESHARK carried, in place, the file's size unchanged.
void breakLastLegacyEntry(const fs::path &path)
{
    std::string bytes = readFile(path);
    size_t last = std::string::npos;
    for (const auto &section : sectionsOf(bytes)) {
        if (section.fourcc == "PCOB" && section.bytes.size() > 24) {
            last = section.offset;
        }
    }
    if (last != std::string::npos) {
        bytes.replace(last + 24, 4, std::string("\x00\x00\xff\xff", 4));
        writeFile(path, bytes);
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: rekordbox_cue_writer_read_back_test <anonymized library dir>\n";
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

    const fs::path scratch = testing::scratchRoot() / "rekordbox-cue-writer-read-back";
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

    const AnlzPathIndex index(rootUtf8);
    RekordboxCueWriter writer(rootUtf8, &index);

    // Hot cues in both ranges and a memory cue, so both files change.
    std::vector<domain::CuePoint> cues;
    for (int slot : {1, 5}) {
        domain::CuePoint hot;
        hot.kind = domain::CuePoint::Kind::Hot;
        hot.hotCueNumber = slot;
        hot.positionMs = 1000.0 * slot;
        cues.push_back(hot);
    }
    domain::CuePoint memory;
    memory.kind = domain::CuePoint::Kind::Memory;
    memory.positionMs = 5662;
    cues.push_back(memory);

    struct Case
    {
        std::string name;
        fs::path damaged;  // empty: nothing is damaged
        int writes;        // the .DAT is never written once the .EXT failed
    };
    for (const Case &c : {Case{"nothing damaged", {}, 2}, Case{"the .EXT damaged after its write", extPath, 1},
                          Case{"the .DAT damaged after its write", datPath, 2}}) {
        std::cout << c.name << ":\n";
        writeFile(datPath, datOriginal);
        writeFile(extPath, extOriginal);
        int written = 0;
        RekordboxCueWriter::setAfterWriteForTesting([&](const std::string &path) {
            ++written;
            if (!c.damaged.empty() && pathFromUtf8(path) == c.damaged) {
                breakLastLegacyEntry(c.damaged);
            }
        });
        std::string error;
        try {
            writer.writeHotCues(std::to_string(trackId), cues);
        } catch (const std::exception &e) {
            error = e.what();
        }
        RekordboxCueWriter::setAfterWriteForTesting({});
        check(written == c.writes, std::to_string(c.writes) + " file(s) written and read back");
        if (c.damaged.empty()) {
            check(error.empty(), "the write succeeded" + (error.empty() ? std::string() : " (threw: " + error + ")"));
            check(readFile(datPath) != datOriginal && readFile(extPath) != extOriginal, "and changed both files");
            continue;
        }
        std::cout << "    " << error << "\n";
        check(!error.empty(), "the write failed");
        check(error.find(pathToUtf8(c.damaged)) != std::string::npos, "naming the damaged file");
        check(readFile(datPath) == datOriginal, "the .DAT is back as it was");
        check(readFile(extPath) == extOriginal, "the .EXT is back as it was");
    }

    fs::remove_all(scratch);
    if (failures > 0) {
        std::cout << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
