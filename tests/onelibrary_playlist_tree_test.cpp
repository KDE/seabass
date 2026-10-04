// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// OneLibraryCueWriter's playlist tree (#62): the steps rekordbox 7.2.18 took
// on a stick (create Q1, a long Unicode name first in the level, a folder
// holding a playlist, reorder Q1, delete Q1, delete the folder), replayed
// on rekordbox's own starting exportLibrary.db. After each step the tree
// and every playlist's content must be what rekordbox's database held
// (tests/fixtures/onelibrary_playlist_tree/expected.tsv, read from its
// snapshots: path, sequenceNo, folder, content ids). Two differences are
// deliberate: reordering Q1 leaves Q1 where it is (rekordbox also moved
// it to the top of the level), and deleting a playlist leaves its tracks
// (rekordbox deleted those no other playlist held).

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::onelibrary::OneLibraryCueWriter;

namespace
{

const std::string LongName = "T\xc3\xabst \xc3\x9cn\xc3\xaf" "code Playlist with a name long enough that it cannot be a short device "
                             "string, which tops out at one hundred and twenty six bytes";

// The step's lines as expected.tsv spells them, from the writer's tree.
std::vector<std::string> describe(OneLibraryCueWriter &w)
{
    const auto tree = w.playlistTree();
    std::map<int64_t, OneLibraryCueWriter::PlaylistNode> byId;
    for (const auto &n : tree) {
        byId[n.id] = n;
    }
    std::map<std::string, std::string> lines;
    for (const auto &n : tree) {
        std::string path = n.name;
        for (int64_t p = n.parentId; p != 0; p = byId.at(p).parentId) {
            path = byId.at(p).name + "/" + path;
        }
        std::string content;
        int64_t expectedSequence = 1;
        for (const auto &[c, sequence] : w.playlistContent(path)) {
            assert(sequence == expectedSequence++ && "sequenceNo runs 1, 2, 3... with no gap");
            content += (content.empty() ? "" : ",") + std::to_string(c);
        }
        lines[path] = path + "\t" + std::to_string(n.sequenceNo) + "\t" + (n.isFolder ? "1" : "0") + "\t" + content;
    }
    assert(w.playlistContentRowsWithoutPlaylist() == 0 && "no content left for a deleted playlist");
    std::vector<std::string> out;
    for (const auto &[path, line] : lines) {
        out.push_back(line);
    }
    return out;
}

std::map<int, std::vector<std::string>> expectedSteps(const fs::path &file)
{
    std::map<int, std::vector<std::string>> steps;
    std::ifstream in(file);
    std::string line;
    int step = 0;
    while (std::getline(in, line)) {
        if (line.rfind("step ", 0) == 0) {
            step = std::stoi(line.substr(5));
        } else if (!line.empty()) {
            steps[step].push_back(line);
        }
    }
    return steps;
}

bool same(const std::vector<std::string> &mine, const std::vector<std::string> &theirs, int step)
{
    if (mine == theirs) {
        return true;
    }
    std::cerr << "step " << step << " differs\n  seabass:\n";
    for (const auto &l : mine) {
        std::cerr << "    " << l << "\n";
    }
    std::cerr << "  rekordbox:\n";
    for (const auto &l : theirs) {
        std::cerr << "    " << l << "\n";
    }
    return false;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: onelibrary_playlist_tree_test <tests/fixtures/onelibrary_playlist_tree>\n";
        return 2;
    }
    const fs::path ref = seabass::pathFromUtf8(argv[1]);
    const auto expected = expectedSteps(ref / "expected.tsv");
    assert(expected.size() == 6);
    const fs::path stick = seabass::testing::scratchRoot() / "seabass_onelibrary_playlist_tree_test";
    fs::remove_all(stick);
    // A copy first: SQLite rewrites -wal and -shm beside whatever it opens.
    fs::create_directories(stick / "PIONEER" / "rekordbox");
    for (const char *name : {"exportLibrary.db", "exportLibrary.db-wal"}) {
        fs::copy_file(ref / "0-base" / "PIONEER" / "rekordbox" / name, stick / "PIONEER" / "rekordbox" / name);
    }
    const auto file = [&](const char *artist, const char *album, const char *name) {
        return seabass::pathToUtf8(stick / "Contents" / artist / album / name);
    };
    OneLibraryCueWriter w(seabass::pathToUtf8(stick / "PIONEER"));

    // 1. Q1 with a01..a05.
    assert(w.createPlaylist("", "Q1", false) == 1);
    for (const auto &f : {file("Tone Artist 01", "Tone Album 01", "a01.mp3"), file("Tone Artist 02", "Tone Album 02", "a02.mp3"),
                          file("Tone Artist 03", "Tone Album 03", "a03.mp3"), file("Tone Artist 04", "Tone Album 04", "a04.mp3"),
                          file("Tone Artist 05", "Tone Album 01", "a05.mp3")}) {
        assert(w.addToPlaylist("Q1", f));
    }
    assert(same(describe(w), expected.at(1), 1));
    std::cout << "step 1 (the first playlist) OK\n";

    // 2. The long Unicode name, first in the level.
    assert(w.createPlaylist("", LongName, false, 0) == 2);
    for (const auto &f : {file("Tone Artist 01", "Tone Album 02", "a06.mp3"), file("Tone Artist 02", "Tone Album 03", "a07.mp3"),
                          file("Tone Artist 03", "Tone Album 04", "a08.mp3")}) {
        assert(w.addToPlaylist(LongName, f));
    }
    assert(same(describe(w), expected.at(2), 2));
    std::cout << "step 2 (a long Unicode name, first in its level) OK\n";

    // 3. A folder holding a playlist; a name its level has, and a parent
    //    that is not a folder, are refused.
    assert(w.createPlaylist("", "F1", true, 0) == 3);
    assert(w.createPlaylist("F1", "F1A", false) == 4);
    assert(w.addToPlaylist("F1/F1A", file("Nouvel Artiste", "Nouvel Album", "b01.mp3")));
    assert(w.addToPlaylist("F1/F1A", file("Tone Artist 01", "Tone Album 01", "c01.mp3")));
    for (const auto &[parent, name] : std::vector<std::pair<std::string, std::string>>{{"", "Q1"}, {"Q1", "inside"}, {"", "a/b"}}) {
        bool refused = false;
        try {
            w.createPlaylist(parent, name, false);
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused);
    }
    assert(same(describe(w), expected.at(3), 3));
    std::cout << "step 3 (a folder with a playlist; duplicates and non-folders refused) OK\n";

    // 4. a05 to the top of Q1: Q1's content as rekordbox's. The tree is
    //    not compared: rekordbox also moved Q1 to the top of its level.
    const std::vector<std::string> q1 = {file("Tone Artist 05", "Tone Album 01", "a05.mp3"), file("Tone Artist 01", "Tone Album 01", "a01.mp3"),
                                         file("Tone Artist 02", "Tone Album 02", "a02.mp3"), file("Tone Artist 03", "Tone Album 03", "a03.mp3"),
                                         file("Tone Artist 04", "Tone Album 04", "a04.mp3")};
    assert(w.reorderPlaylist("Q1", q1));
    assert(!w.reorderPlaylist("Q1", q1));
    {
        bool refused = false;
        try {
            w.reorderPlaylist("Q1", {q1[0], q1[1]});
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused);
        std::string q1Line;
        for (const auto &l : expected.at(4)) {
            if (l.rfind("Q1\t", 0) == 0) {
                q1Line = l.substr(l.rfind('\t') + 1);
            }
        }
        std::string mine;
        for (const auto &[c, sequence] : w.playlistContent("Q1")) {
            mine += (mine.empty() ? "" : ",") + std::to_string(c);
        }
        assert(!q1Line.empty() && mine == q1Line);
    }
    std::cout << "step 4 (reordering, as rekordbox did) OK\n";

    // 5. Deleting Q1; its tracks stay.
    assert(w.deletePlaylist("Q1") == 1);
    assert(w.deletePlaylist("Q1") == 0);
    assert(same(describe(w), expected.at(5), 5));
    std::cout << "step 5 (deleting a playlist) OK\n";

    // 6. Deleting F1 takes F1A and its content.
    assert(w.deletePlaylist("F1") == 2);
    assert(same(describe(w), expected.at(6), 6));
    std::cout << "step 6 (deleting a folder with its playlist) OK\n";

    // 7. Removing an entry numbers the rest from 1 again.
    assert(w.removeFromPlaylist(LongName, file("Tone Artist 02", "Tone Album 03", "a07.mp3")));
    {
        const auto lines = describe(w);
        assert(lines.size() == 1 && lines[0] == LongName + "\t0\t0\t6,8");
    }
    std::cout << "step 7 (a removal leaves no gap) OK\n";

    fs::remove_all(stick);
    std::cout << "onelibrary_playlist_tree_test: all steps passed\n";
    return 0;
}
