// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: give Library Health something to repair -- rig check W6.
//
//   rig_plant_repairable <stick root> --plant
//   rig_plant_repairable <stick root> --restore
//
// --plant looks for a rekordbox duplicate pair (same artist, title and
// length, as DuplicateTrackFinder groups them) where every copy has its
// own audio file on the stick, and moves one copy's
// file into <stick root>/RIG-HIDDEN/ under its relative path. The move is
// recorded in RIG-HIDDEN/planted.tsv. That copy's rows now point at a
// missing file while its duplicate is healthy, which is the Repairable
// case. Rows in OneLibrary or Engine for the same file break with it.
//
// --restore moves every recorded file back and removes RIG-HIDDEN.
//
// No catalog is touched: only one audio file moves, within the stick.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "domain/duplicate_cue_consolidation.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

bool insideRoot(const fs::path &file, const fs::path &root)
{
    const fs::path relative = fs::relative(file, root);
    return !relative.empty() && *relative.begin() != "..";
}

int restore(const fs::path &root)
{
    const fs::path hidden = root / "RIG-HIDDEN";
    const fs::path record = hidden / "planted.tsv";
    if (!fs::exists(record)) {
        std::cout << "nothing planted on this stick\nRIG RESULT: PASS\n";
        return 0;
    }
    std::ifstream in(record);
    std::string line;
    int moved = 0;
    int bad = 0;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        // Both columns were written as UTF-8 (see plant()).
        const fs::path original = pathFromUtf8(line.substr(0, tab));
        const fs::path moveAside = hidden / pathFromUtf8(line.substr(tab + 1, line.find('\t', tab + 1) - tab - 1));
        if (fs::exists(moveAside) && !fs::exists(original)) {
            fs::create_directories(original.parent_path());
            fs::rename(moveAside, original);
            std::cout << "moved back " << pathToUtf8(original) << "\n";
            ++moved;
        } else if (fs::exists(original) && !fs::exists(moveAside)) {
            std::cout << "already back " << pathToUtf8(original) << "\n";
        } else {
            std::cout << "CANNOT RESTORE " << pathToUtf8(original) << " (hidden copy "
                      << (fs::exists(moveAside) ? "present" : "missing") << ", original "
                      << (fs::exists(original) ? "present" : "missing") << ")\n";
            ++bad;
        }
    }
    in.close();
    if (bad == 0) {
        fs::remove_all(hidden);
    }
    std::cout << "restored " << moved << " file(s)\nRIG RESULT: " << (bad == 0 ? "PASS" : "FAIL") << "\n";
    return bad == 0 ? 0 : 1;
}

int plant(const fs::path &root)
{
    const fs::path hidden = root / "RIG-HIDDEN";
    const fs::path record = hidden / "planted.tsv";
    if (fs::exists(record)) {
        std::cout << "already planted; run --restore first\nRIG RESULT: FAIL\n";
        return 1;
    }
    std::cout << "reading rekordbox:\n";
    const fs::path pioneer = root / "PIONEER";
    infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(pioneer));
    const std::vector<domain::Track> tracks = application::ScanLibrary(reader).execute();
    std::cout << "  " << tracks.size() << " tracks\n";

    const std::vector<domain::DuplicateGroup> groups = domain::DuplicateTrackFinder::find(tracks);
    std::size_t considered = 0;
    for (const domain::DuplicateGroup &group : groups) {
        if (group.tracks.size() < 2) {
            continue;
        }
        ++considered;
        bool usable = true;
        for (const domain::Track &track : group.tracks) {
            // Cues are allowed on the copies. The case being planted is
            // a catalog row whose file is no longer where it says, next
            // to a healthy duplicate -- cues have no bearing on that,
            // and requiring none made the check unplantable against a
            // real export: rekordbox marks its own cue on nearly every
            // track, so round 5 found a usable pair once in three
            // passes. What the copies carry is recorded below instead,
            // so a repair that loses cues is visible rather than
            // impossible to reach.
            if (track.filePath.empty() || !fs::is_regular_file(pathFromUtf8(track.filePath))
                || !insideRoot(pathFromUtf8(track.filePath), root)) {
                usable = false;
                break;
            }
        }
        // Two rows for one file are not two copies: moving it would break
        // both, and nothing healthy would be left to repair onto. Asked of
        // the filesystem, not the path strings: on exFAT and FAT32
        // "Track.mp3" and "track.mp3" are one file.
        for (std::size_t i = 0; usable && i < group.tracks.size(); ++i) {
            for (std::size_t j = i + 1; usable && j < group.tracks.size(); ++j) {
                std::error_code ec;
                if (fs::equivalent(pathFromUtf8(group.tracks[i].filePath), pathFromUtf8(group.tracks[j].filePath), ec)
                    || ec) {
                    usable = false;
                }
            }
        }
        if (!usable) {
            continue;
        }
        const domain::Track &survivor = group.tracks.front();
        const domain::Track &victim = group.tracks.back();
        const fs::path victimFile = pathFromUtf8(victim.filePath);
        const fs::path relative = fs::relative(victimFile, root);
        const fs::path moveAside = hidden / relative;
        fs::create_directories(moveAside.parent_path());
        fs::rename(victimFile, moveAside);
        // UTF-8 in both path columns, forward slashes in the relative one,
        // so --restore reads the record back the same way on every platform.
        std::ofstream(record, std::ios::app) << victim.filePath << '\t' << pathToGenericUtf8(relative) << '\t'
                                             << victim.sourceId << '\t' << victim.title << '\n';
        std::size_t cuesInGroup = 0;
        for (const domain::Track &copy : group.tracks) {
            cuesInGroup += copy.cues.size();
        }
        std::cout << "planted: \"" << victim.artist << " - " << victim.title << "\" (" << victim.durationSeconds
                  << " s), " << group.tracks.size() << " copies, " << cuesInGroup
                  << " cue(s) across them (" << victim.cues.size() << " on the one moved aside, "
                  << survivor.cues.size() << " on the healthy one)\n"
                  << "  moved aside rekordbox id " << victim.sourceId << ": " << victim.filePath << "\n"
                  << "  healthy copy rekordbox id " << survivor.sourceId << ": " << survivor.filePath << "\n"
                  << "RIG RESULT: PASS\n";
        return 0;
    }
    std::cout << "no duplicate group of separate files (" << considered << " groups looked at)\n"
              << "RIG RESULT: FAIL\n";
    return 1;
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc == 3 ? argv[2] : "";
    if (mode != "--plant" && mode != "--restore") {
        std::cerr << "usage: rig_plant_repairable <stick root> --plant|--restore\n";
        return 2;
    }
    try {
        const fs::path root = pathFromUtf8(argv[1]);
        return mode == "--plant" ? plant(root) : restore(root);
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }
}
