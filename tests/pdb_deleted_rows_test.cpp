// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// deletedTrackFilePaths(): the files export.pdb has a deleted row for and
// no live one. The evidence that the rekordbox half removed a file, which
// is how Library Health tells a Clean Up's OneLibrary leftovers apart
// from a file only OneLibrary was ever given.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <set>

#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::rekordbox;

namespace
{

// A stick whose PIONEER folder holds only the committed fixture's
// export.pdb: all this reads.
fs::path freshStick(const fs::path &scratch)
{
    fs::remove_all(scratch);
    const fs::path pioneer = scratch / "PIONEER";
    fs::create_directories(pioneer / "rekordbox");
    fs::copy_file(seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox"
                      / "rekordbox" / "export.pdb",
                  pioneer / "rekordbox" / "export.pdb");
    return pioneer;
}

std::set<std::string> asSet(const std::vector<std::string> &paths)
{
    return {paths.begin(), paths.end()};
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_pdb_deleted_rows_test";
    const fs::path pioneer = freshStick(scratch);
    const std::string pdb = seabass::pathToUtf8(pioneer / "rekordbox" / "export.pdb");

    const auto live = KaitaiRekordboxReader(seabass::pathToUtf8(pioneer)).readAll();
    assert(live.size() == 1161 && "the fixture this test was written against");
    std::set<std::string> livePaths;
    for (const auto &track : live) {
        livePaths.insert(track.filePath);
    }

    // 1. Whatever the fixture already carries, none of it is a file a live
    //    row still lists: a rekordbox edit is a delete plus an append, so
    //    a deleted row beside a live one is history, not a removal.
    //
    //    398 is tools/pdb_capacity_probe's count for this file, measured
    //    separately: a number pinned, so a walk that stopped early or read
    //    nothing cannot pass by finding an empty set with nothing live in
    //    it. (The num_row_offsets bound is not what this pins: the phantom
    //    slots past it repeat real rows' offsets, so they add rows but no
    //    new paths.)
    const std::set<std::string> before = asSet(deletedTrackFilePaths(seabass::pathToUtf8(pioneer)));
    assert(before.size() == 398);
    for (const auto &path : before) {
        assert(!livePaths.count(path));
    }
    std::cout << "case 1 (" << before.size() << " deleted files, none of them live) OK\n";

    // 2. Removing a track -- what Clean Up does to the copy it drops --
    //    adds exactly its file, spelled exactly as the reader spells it,
    //    so it compares equal to OneLibrary's row for the same file.
    const auto &victim = live[live.size() / 2];
    {
        PdbRowWriter writer(pdb);
        assert(writer.removeTrack(static_cast<uint32_t>(std::stoul(victim.sourceId))));
        assert(writer.commit());
    }
    std::set<std::string> after = asSet(deletedTrackFilePaths(seabass::pathToUtf8(pioneer)));
    std::set<std::string> expected = before;
    expected.insert(victim.filePath);
    assert(after == expected);
    std::cout << "case 2 (a removed track is found, under the reader's own spelling) OK\n";

    // 3. What the anonymizer does to a fixture: zeroing the space no live
    //    row occupies takes the dead bodies with it, and with them the
    //    evidence. This is why no anonymized fixture can show a Clean Up
    //    leftover, and why the spec once measured "zero of the 290".
    {
        PdbRowWriter writer(pdb);
        assert(writer.zeroUnusedSpace() > 0);
        assert(writer.commit());
    }
    assert(!asSet(deletedTrackFilePaths(seabass::pathToUtf8(pioneer))).count(victim.filePath));
    std::cout << "case 3 (zeroed free space leaves no evidence) OK\n";

    fs::remove_all(scratch);
    std::cout << "pdb_deleted_rows_test: all cases passed\n";
    return 0;
}
