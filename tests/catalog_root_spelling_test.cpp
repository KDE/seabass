// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A doubled separator or a "." in the stick root does not change how a track's
// file is spelled.
//
// Sync pairs a rekordbox row with its Engine row by the exact file path the
// two readers give (domain::matchTracks), so they have to spell one file one
// way. The Engine reader normalizes its join; the rekordbox and OneLibrary
// readers joined the stick root (the PIONEER folder's parent_path()) and the
// stored path and stopped there. For a stick at a drive root given as "D:/" +
// "/PIONEER", parent_path() keeps "D://", and the join came back as
// D:\\Contents\... beside Engine's D:\Contents\..., so Sync on that stick
// matched nothing (Windows Round 11, 2026-11-06: "sync plans: 0 (rekordbox
// 100, engine 100)" in the live Sync check and in F6, and `seabass-cli sync`
// against "D://PIONEER" matched 0 tracks where "D:/PIONEER" matched 88).
//
// A trailing separator on the PIONEER folder itself, where parent_path()
// answers the PIONEER folder, is case 5 (#87).

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "application/use_cases/sync_libraries.hpp"
#include "domain/track.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::Track;
using seabass::infrastructure::onelibrary::OneLibraryReader;
using seabass::infrastructure::rekordbox::KaitaiRekordboxReader;
using seabass::infrastructure::rekordbox::trackFilePathOnStick;

namespace
{

[[noreturn]] void fail(const std::string &why)
{
    std::cerr << "FAIL: " << why << "\n";
    std::exit(1);
}

std::set<std::string> filePaths(const std::vector<Track> &tracks)
{
    std::set<std::string> paths;
    for (const auto &track : tracks) {
        if (!track.filePath.empty()) {
            paths.insert(track.filePath);
        }
    }
    return paths;
}

std::set<std::string> artworkPaths(const std::vector<Track> &tracks)
{
    std::set<std::string> paths;
    for (const auto &track : tracks) {
        if (!track.artworkPath.empty()) {
            paths.insert(track.artworkPath);
        }
    }
    return paths;
}

void requireSame(const std::set<std::string> &clean, const std::set<std::string> &other, const std::string &what)
{
    if (clean.empty()) {
        fail(what + ": the clean root read no file paths at all");
    }
    if (clean != other) {
        for (const auto &path : other) {
            if (!clean.count(path)) {
                std::cerr << "  only through the other root: " << path << "\n";
                break;
            }
        }
        fail(what + ": the spelling of the root changes the file paths");
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: catalog_root_spelling_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }

    // 1. The join, with the roots a caller can hand it. A drive root is not
    //    something the fixture below can be copied to.
    const std::string stored = "/Contents/a b/x.mp3";
#ifdef _WIN32
    const std::string clean = "D:/";
    const std::string want = "D:\\Contents\\a b\\x.mp3";
    const std::vector<std::string> roots = {"D:/", "D:\\", "D://", "D:\\\\", "D:/\\", "D:/."};
#else
    const std::string clean = "/media/sebas/STICK";
    const std::string want = "/media/sebas/STICK/Contents/a b/x.mp3";
    const std::vector<std::string> roots = {"/media/sebas/STICK", "/media/sebas/STICK/", "/media/sebas/STICK//",
                                            "/media/sebas/./STICK", "/media//sebas/STICK"};
#endif
    for (const auto &root : roots) {
        const std::string got = trackFilePathOnStick(root, stored);
        if (got != want) {
            fail("root \"" + root + "\" + \"" + stored + "\" gave \"" + got + "\", want \"" + want + "\"");
        }
    }
    std::cout << "case 1 (a doubled separator or a dot in the stick root leaves one path) OK\n";

    // 1b. A stored path with more than one leading separator (#89). Stripping
    //     one left "/Contents/...", which operator/ takes as absolute: on
    //     Linux and macOS the stick root was gone. Only damaged data stores
    //     this, as far as anyone knows.
    for (const std::string damaged : {"//Contents/a b/x.mp3", "///Contents/a b/x.mp3", "/\\Contents/a b/x.mp3",
                                      "Contents/a b/x.mp3"}) {
        const std::string got = trackFilePathOnStick(clean, damaged);
        if (got != want) {
            fail("root \"" + clean + "\" + \"" + damaged + "\" gave \"" + got + "\", want \"" + want + "\"");
        }
    }
    std::cout << "case 1b (every leading separator of a stored path is dropped) OK\n";

    // 2. The spelling the Engine reader gives the same file, so the two can
    //    be compared by string.
    const fs::path engineSpelling = (seabass::pathFromUtf8(clean) / seabass::pathFromUtf8(stored.substr(1))).lexically_normal();
    if (trackFilePathOnStick(clean, stored) != seabass::pathToUtf8(engineSpelling)) {
        fail("the join and the Engine reader's normalized join spell one file two ways");
    }
    std::cout << "case 2 (the join spells a file as the Engine reader does) OK\n";

    // 3. Both readers that use the join, through a root that is not clean.
    //    "stick/./PIONEER": unlike a doubled separator, parent_path() keeps it.
    const fs::path fixture = seabass::pathFromUtf8(argv[1]);
    const fs::path scratch = seabass::testing::scratchRoot() / "catalog_root_spelling";
    seabass::testing::sandboxSeabassHome(scratch / "home");
    const fs::path stick = scratch / "stick";
    fs::remove_all(stick);
    fs::create_directories(stick);
    // A copy, never the fixture itself: the OneLibrary open may roll back a
    // journal.
    fs::copy(fixture / "rekordbox", stick / "PIONEER", fs::copy_options::recursive);
    const std::string pioneer = seabass::pathToUtf8(stick / "PIONEER");
    const std::string pioneerDotted = seabass::pathToUtf8(stick) + "/./PIONEER";

    requireSame(filePaths(KaitaiRekordboxReader(pioneer).readTracks()),
                filePaths(KaitaiRekordboxReader(pioneerDotted).readTracks()), "rekordbox");
    requireSame(filePaths(OneLibraryReader(pioneer).readTracks()),
                filePaths(OneLibraryReader(pioneerDotted).readTracks()), "OneLibrary");
    std::cout << "case 3 (the rekordbox and OneLibrary readers give the same paths through that root) OK\n";

    // 4. Artwork paths too, through the same root (#88): they are compared
    //    by string as well (scan_controller.cpp, the path-keyed dedup in
    //    engine_artwork.cpp), and kept the "." where filePath lost it.
    requireSame(artworkPaths(KaitaiRekordboxReader(pioneer).readTracks()),
                artworkPaths(KaitaiRekordboxReader(pioneerDotted).readTracks()), "rekordbox artwork");
    requireSame(artworkPaths(OneLibraryReader(pioneer).readTracks()),
                artworkPaths(OneLibraryReader(pioneerDotted).readTracks()), "OneLibrary artwork");
    std::cout << "case 4 (artwork paths are spelled one way through that root) OK\n";

    // 5. A trailing separator on the PIONEER folder (#87), as a tab-completed
    //    `seabass-cli sync --rekordbox /media/x/STICK/PIONEER/` hands it:
    //    parent_path() of that is the PIONEER folder itself, and every row
    //    named a file under STICK/PIONEER/Contents.
    for (const std::string &trailing : {pioneer + "/", pioneer + "//", pioneerDotted + "/"}) {
        requireSame(filePaths(KaitaiRekordboxReader(pioneer).readTracks()),
                    filePaths(KaitaiRekordboxReader(trailing).readTracks()), "rekordbox through \"" + trailing + "\"");
        requireSame(filePaths(OneLibraryReader(pioneer).readTracks()),
                    filePaths(OneLibraryReader(trailing).readTracks()), "OneLibrary through \"" + trailing + "\"");
        requireSame(artworkPaths(KaitaiRekordboxReader(pioneer).readTracks()),
                    artworkPaths(KaitaiRekordboxReader(trailing).readTracks()),
                    "rekordbox artwork through \"" + trailing + "\"");
    }
    std::cout << "case 5 (a trailing separator on the PIONEER folder leaves the paths alone) OK\n";

    // 6. Letter case (#86). SHAKEDOWN_8's OneLibrary lists
    //    "/Contents/Akki (De)/Midnight/" for a folder the stick, export.pdb
    //    and Engine spell "AKKI (DE)". FAT32 and exFAT are case-insensitive,
    //    so both spellings open one file; Sync paired rows by the exact
    //    path, and within one stick a row whose file is present gets no name
    //    fallback, so the track was silently left out. Which spellings name
    //    one file is the filesystem's to say: a hard link stands in for
    //    FAT's answer here, and two files that differ only in case, which a
    //    case-sensitive filesystem can hold, must stay apart.
    {
        const fs::path caseStick = scratch / "case_stick";
        fs::remove_all(caseStick);
        const fs::path upper = caseStick / "Contents" / "AKKI (DE)" / "Midnight";
        const fs::path mixed = caseStick / "Contents" / "Akki (De)" / "Midnight";
        fs::create_directories(upper);
        fs::create_directories(mixed);
        std::ofstream(upper / "01 Midnight.mp3") << "one file";
        fs::create_hard_link(upper / "01 Midnight.mp3", mixed / "01 Midnight.mp3");
        std::ofstream(upper / "02 Other.mp3") << "a file";
        std::ofstream(mixed / "02 Other.mp3") << "another file, the same name in other letters";

        const auto row = [](const std::string &format, const std::string &id, const fs::path &file) {
            Track t;
            t.format = format;
            t.sourceId = id;
            t.filePath = seabass::pathToUtf8(file);
            t.filename = seabass::pathToUtf8(file.filename());
            t.title = "Midnight";
            t.artist = "Akki (DE)";
            t.durationSeconds = 300.0;
            t.fileSizeBytes = fs::file_size(file);
            t.cues = {seabass::domain::CuePoint{seabass::domain::CuePoint::Kind::Hot, 1, 1000.0, "", ""}};
            return t;
        };
        const auto now = std::chrono::system_clock::now();
        const auto pairs = [&](const fs::path &a, const fs::path &b) {
            return seabass::application::SyncLibraries()
                .execute({row("onelibrary", "ol1", a)}, {row("engine", "e1", b)}, now, now)
                .size();
        };
        if (pairs(mixed / "01 Midnight.mp3", upper / "01 Midnight.mp3") != 1) {
            fail("one file spelled \"Akki (De)\" and \"AKKI (DE)\" was not paired");
        }
        if (pairs(mixed / "02 Other.mp3", upper / "02 Other.mp3") != 0) {
            fail("two files that differ only in letter case were paired");
        }
        std::cout << "case 6 (one file in two letter cases is one track, two files stay two) OK\n";
    }
    return 0;
}
