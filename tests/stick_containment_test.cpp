// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// isUnderStickRoot() is the last question asked before a file on a stick
// is deleted, whoever built the list. Its two halves are tested
// (path_key_test, stick_path_match_test); the joint between them was
// not, and the joint is where a path that normalises to something
// outside the stick could still answer "yes".
//
// Every assertion below is about one of two mistakes, and only one of
// them is recoverable:
//
//   - false for a file that IS on the stick: Clean Up leaves rubbish
//     behind, and the user runs it again.
//   - true for a file that is NOT on the stick: Seabass deletes
//     somebody's file off their internal disk.

#include <cassert>
#include <iostream>
#include <string>

#include "infrastructure/cleanup/stick_containment.hpp"

using seabass::infrastructure::cleanup::isUnderStickRoot;

int main()
{
    // The ordinary case.
    assert(isUnderStickRoot("/media/RV2/Contents/track.mp3", "/media/RV2"));
    assert(isUnderStickRoot("/media/RV2/PIONEER/rekordbox/export.pdb", "/media/RV2"));

    // The root itself is not something under the root. A manifest that
    // somehow recorded the mount point as an entry must not hand Clean
    // Up the whole stick to remove.
    assert(!isUnderStickRoot("/media/RV2", "/media/RV2"));
    assert(!isUnderStickRoot("/media/RV2/", "/media/RV2"));
    assert(!isUnderStickRoot("/media/RV2", "/media/RV2/"));

    // A label that is a prefix of another label is the ordinary
    // situation on a desk with several sticks. RV22 is not on RV2.
    assert(!isUnderStickRoot("/media/RV22/Contents/track.mp3", "/media/RV2"));
    assert(!isUnderStickRoot("/media/RV2-BACKUP/Contents/track.mp3", "/media/RV2"));

    // Case and separator spelling: exFAT and NTFS are case-insensitive,
    // and a manifest written on Windows carries backslashes that Linux
    // then reads back.
    assert(isUnderStickRoot("/MEDIA/rv2/CONTENTS/Track.mp3", "/media/RV2"));
    assert(isUnderStickRoot("D:\\Contents\\track.mp3", "d:/contents"));
    assert(isUnderStickRoot("D:/Contents/track.mp3", "D:\\Contents"));
    // Still case-insensitively wrong about a different stick.
    assert(!isUnderStickRoot("/media/rv22/Contents/track.mp3", "/media/RV2"));

    // Accents. macOS hands out decomposed names from an exFAT directory
    // read, so the same folder arrives spelled two ways: "Kölsch" as one
    // code point, and as "Ko" plus a combining diaeresis. Both are on
    // the stick, and a false here would mean Clean Up walked past every
    // accented folder on a stick that had been near a Mac.
    assert(isUnderStickRoot("/media/RV2/Ko\xCC\x88lsch/a.mp3", "/media/RV2"));
    assert(isUnderStickRoot("/media/RV2/K\xC3\xB6lsch/a.mp3", "/media/RV2"));
    assert(isUnderStickRoot("/media/K\xC3\xB6lsch/a.mp3", "/media/Ko\xCC\x88lsch"));

    // Padded paths. export.pdb keeps its strings in fixed-length fields
    // and pads them with spaces or NULs, so a path straight out of a
    // rekordbox catalog arrives longer than the file's real name.
    assert(isUnderStickRoot("/media/RV2/Contents/track.mp3   ", "/media/RV2"));
    assert(isUnderStickRoot(std::string("/media/RV2/Contents/track.mp3\0\0", 31), "/media/RV2"));

    // The one that would be a disaster. A manifest records absolute
    // paths; a mount point can move underneath it; and nothing stops a
    // recorded path carrying "..". Normalising must happen BEFORE the
    // containment test, not after, or this reads as a file on the stick.
    assert(!isUnderStickRoot("/media/RV2/../../etc/passwd", "/media/RV2"));
    assert(!isUnderStickRoot("/media/RV2/Contents/../../../home/sebas/Music/a.mp3", "/media/RV2"));
    // The same trick that comes back onto the stick is genuinely on it.
    assert(isUnderStickRoot("/media/RV2/Contents/../Contents/track.mp3", "/media/RV2"));
    assert(isUnderStickRoot("/media/RV2/./Contents//track.mp3", "/media/RV2"));

    // An empty root is what the locator reports for a stick that is
    // present but not mounted. It must not answer for every path there
    // is, and an empty path must not be on every stick there is.
    assert(!isUnderStickRoot("/media/RV2/Contents/track.mp3", ""));
    assert(!isUnderStickRoot("", "/media/RV2"));
    assert(!isUnderStickRoot("", ""));
    // A root that normalises away to nothing is the same situation.
    assert(!isUnderStickRoot("/media/RV2/Contents/track.mp3", "   "));

    // A relative path is not on any stick: there is no root it could be
    // compared against, and resolving it against the process's working
    // directory is exactly the guess this must not make.
    assert(!isUnderStickRoot("Contents/track.mp3", "/media/RV2"));

    std::cout << "stick_containment_test passed\n";
    return 0;
}
