// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Which OneLibrary rows are a Clean Up's leftovers, and onto which live
// rekordbox track each one's playlist entries belong. Hand-built tracks:
// the real case is WHALESHARK2, whose anonymized fixture cannot carry it
// (the anonymizer zeroes the deleted rows this reads).

#include <cassert>
#include <iostream>

#include "domain/cleanup_leftovers.hpp"

using namespace seabass::domain;

namespace
{

Track track(std::string format, std::string id, std::string path, std::string title, std::string artist,
            double duration)
{
    Track t;
    t.format = std::move(format);
    t.sourceId = std::move(id);
    t.filePath = std::move(path);
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.durationSeconds = duration;
    return t;
}

Track rb(std::string id, std::string path, std::string title, std::string artist, double duration)
{
    return track("rekordbox", std::move(id), std::move(path), std::move(title), std::move(artist), duration);
}

Track ol(std::string id, std::string path, std::string title, std::string artist, double duration)
{
    return track("onelibrary", std::move(id), std::move(path), std::move(title), std::move(artist), duration);
}

std::string asIs(const std::string &path)
{
    return path;
}

const CleanupLeftover *leftoverAt(const std::vector<CleanupLeftover> &all, const std::string &path)
{
    for (const auto &leftover : all) {
        if (leftover.row.filePath == path) {
            return &leftover;
        }
    }
    return nullptr;
}

}  // namespace

int main()
{
    // 1. The WHALESHARK2 shape. "13_Maddix-Heute Nacht" was removed from
    //    export.pdb (a deleted row says so) and kept in OneLibrary, twice,
    //    because two rekordbox installations exported it. "42_..." is the
    //    copy Clean Up kept, live on both sides. The leftover is
    //    repairable onto it, and both of its OneLibrary rows are named.
    {
        const std::vector<Track> rekordbox = {rb("31", "/s/42_Maddix-Heute Nacht.mp3", "Heute Nacht", "Maddix", 300)};
        const std::vector<Track> oneLibrary = {
            ol("7", "/s/42_Maddix-Heute Nacht.mp3", "Heute Nacht", "Maddix", 300),
            ol("8", "/s/13_Maddix-Heute Nacht.mp3", "Heute Nacht", "Maddix", 301),
            ol("9", "/s/13_Maddix-Heute Nacht.mp3", "Heute Nacht", "Maddix", 301),
        };
        const auto found =
            CleanupLeftoverFinder::find(oneLibrary, rekordbox, {"/s/13_Maddix-Heute Nacht.mp3"}, asIs);
        assert(found.size() == 1);
        assert(found[0].kind == CleanupLeftover::Kind::Repairable);
        assert(found[0].survivor && found[0].survivor->filePath == "/s/42_Maddix-Heute Nacht.mp3");
        assert((found[0].rowIds == std::vector<std::string>{"8", "9"}));
        std::cout << "case 1 (a removed duplicate is repaired onto the copy Clean Up kept) OK\n";
    }

    // 2. No deleted row, no leftover: a file only OneLibrary lists is a
    //    divergence nothing here can decide, and is not this check's to
    //    report. The same file with a deleted row AND a live one (a
    //    rekordbox update is a delete plus an append) is not one either.
    {
        const std::vector<Track> rekordbox = {rb("1", "/s/a.mp3", "A", "X", 200)};
        const std::vector<Track> oneLibrary = {
            ol("1", "/s/a.mp3", "A", "X", 200),
            ol("2", "/s/only-here.mp3", "A", "X", 200),
        };
        assert(CleanupLeftoverFinder::find(oneLibrary, rekordbox, {}, asIs).empty());
        assert(CleanupLeftoverFinder::find(oneLibrary, rekordbox, {"/s/a.mp3"}, asIs).empty());
        std::cout << "case 2 (only a deleted row, and no live one, makes a leftover) OK\n";
    }

    // 3. The three reasons a leftover is reported and not repaired:
    //    nothing live matches it; two live copies match it and nothing says
    //    which one Clean Up kept; the one that matches has no OneLibrary
    //    row for the entries to move onto.
    {
        const std::vector<Track> rekordbox = {
            rb("1", "/s/twin-a.mp3", "Twin", "Y", 250),
            rb("2", "/s/twin-b.mp3", "Twin", "Y", 250),
            rb("3", "/s/kept.mp3", "Kept", "Z", 180),
        };
        const std::vector<Track> oneLibrary = {
            ol("10", "/s/lonely.mp3", "Lonely", "W", 100),
            ol("11", "/s/twin-c.mp3", "Twin", "Y", 250),
            ol("12", "/s/gone.mp3", "Kept", "Z", 180),
            ol("13", "/s/twin-a.mp3", "Twin", "Y", 250),
            ol("14", "/s/twin-b.mp3", "Twin", "Y", 250),
        };
        const auto found = CleanupLeftoverFinder::find(
            oneLibrary, rekordbox, {"/s/lonely.mp3", "/s/twin-c.mp3", "/s/gone.mp3"}, asIs);
        assert(found.size() == 3);
        assert(leftoverAt(found, "/s/lonely.mp3")->kind == CleanupLeftover::Kind::NoSurvivor);
        assert(!leftoverAt(found, "/s/lonely.mp3")->survivor);
        assert(leftoverAt(found, "/s/twin-c.mp3")->kind == CleanupLeftover::Kind::SeveralSurvivors);
        assert(!leftoverAt(found, "/s/twin-c.mp3")->survivor);
        assert(leftoverAt(found, "/s/gone.mp3")->kind == CleanupLeftover::Kind::SurvivorNotInOneLibrary);
        std::cout << "case 3 (no survivor, two survivors, a survivor OneLibrary lacks: reported, not repaired) OK\n";
    }

    // 4. A matching title is not enough: the duration has to agree within
    //    the exact-match tolerance, as it did for Clean Up. An edit of the
    //    same song three minutes shorter is a different track.
    {
        const std::vector<Track> rekordbox = {rb("1", "/s/radio.mp3", "Song", "V", 200)};
        const std::vector<Track> oneLibrary = {
            ol("1", "/s/radio.mp3", "Song", "V", 200),
            ol("2", "/s/extended.mp3", "Song", "V", 380),
        };
        const auto found = CleanupLeftoverFinder::find(oneLibrary, rekordbox, {"/s/extended.mp3"}, asIs);
        assert(found.size() == 1 && found[0].kind == CleanupLeftover::Kind::NoSurvivor);
        std::cout << "case 4 (a different length is a different track) OK\n";
    }

    // 5. Paths are compared through the caller's key, never raw: the two
    //    readers spell one file differently on Windows.
    {
        const auto lower = [](const std::string &path) {
            std::string out = path;
            for (char &c : out) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        };
        const std::vector<Track> rekordbox = {rb("1", "/S/Kept.mp3", "K", "Q", 200)};
        const std::vector<Track> oneLibrary = {
            ol("1", "/s/kept.mp3", "K", "Q", 200),
            ol("2", "/s/dup.mp3", "K", "Q", 200),
        };
        const auto found = CleanupLeftoverFinder::find(oneLibrary, rekordbox, {"/S/DUP.mp3"}, lower);
        assert(found.size() == 1 && found[0].kind == CleanupLeftover::Kind::Repairable);
        std::cout << "case 5 (paths compared through the key) OK\n";
    }

    // 6. Two live rekordbox rows for the SAME file (two installations
    //    exported it) are one survivor, not two: the leftover is
    //    repairable, not held back as "more than one copy".
    {
        const std::vector<Track> rekordbox = {
            rb("1", "/s/kept.mp3", "K", "Q", 200),
            rb("2", "/s/kept.mp3", "K", "Q", 200),
        };
        const std::vector<Track> oneLibrary = {
            ol("1", "/s/kept.mp3", "K", "Q", 200),
            ol("2", "/s/dup.mp3", "K", "Q", 200),
        };
        const auto found = CleanupLeftoverFinder::find(oneLibrary, rekordbox, {"/s/dup.mp3"}, asIs);
        assert(found.size() == 1 && found[0].kind == CleanupLeftover::Kind::Repairable);
        assert(found[0].survivor && found[0].survivor->filePath == "/s/kept.mp3");
        std::cout << "case 6 (two rows for one kept file are one survivor) OK\n";
    }

    std::cout << "cleanup_leftovers_test: all cases passed\n";
    return 0;
}
