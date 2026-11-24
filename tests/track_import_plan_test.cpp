// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::planTrackImport: what happens to each audio file offered for a
// stick, and the names the copies get there.

#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
#include <string>

#include "application/path_key.hpp"
#include "domain/track_import_plan.hpp"

using namespace seabass::domain;

namespace
{

// The bytes behind each path of a case, by a tag: two paths hold the same
// bytes when their tags are equal. A path nobody tagged holds its own.
std::map<std::string, std::string> g_bytes;
int g_byteComparisons = 0;

ImportPlanTools tools()
{
    ImportPlanTools t;
    t.pathKey = [](const std::string &path) { return seabass::application::normalizedPathKey(path); };
    t.sameBytes = [](const std::string &a, const std::string &b) {
        ++g_byteComparisons;
        const auto bytesOf = [](const std::string &path) {
            const auto it = g_bytes.find(path);
            return it == g_bytes.end() ? "own:" + path : it->second;
        };
        return bytesOf(a) == bytesOf(b);
    };
    return t;
}

ImportFile offer(std::string path, std::uint64_t bytes, std::string title = {}, std::string artist = {},
                 std::string album = {}, double duration = 200.0)
{
    ImportFile f;
    f.file.format = "disk";
    f.file.sourceId = path;
    f.file.filePath = path;
    f.file.filename = path.substr(path.find_last_of('/') + 1);
    f.file.fileSizeBytes = bytes;
    f.file.title = std::move(title);
    f.file.artist = std::move(artist);
    f.file.album = std::move(album);
    f.file.durationSeconds = duration;
    f.readable = true;
    return f;
}

Track row(std::string format, std::string path, std::string title = {}, std::string artist = {},
          double duration = 200.0)
{
    Track t;
    t.format = std::move(format);
    t.sourceId = path;
    t.filePath = path;
    t.filename = path.substr(path.find_last_of('/') + 1);
    t.fileSizeBytes = 1;
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.durationSeconds = duration;
    return t;
}

ImportStick stick()
{
    ImportStick s;
    s.root = "/media/STICK";
    s.freeBytes = 1'000'000;
    s.acceptedExtensions = importableExtensions({"rekordbox", "onelibrary", "engine"});
    return s;
}

}  // namespace

int main()
{
    // 1. A new file goes under Contents/<artist>/<album>/ with its own
    //    name; a file whose tags name neither goes where rekordbox puts
    //    such a file. The bytes to copy are the imports together.
    {
        const auto plan = planTrackImport(
            {offer("/home/dj/in/01 Song.mp3", 1000, "Song", "Someone", "The Album"),
             offer("/home/dj/in/untagged.flac", 2000)},
            stick(), tools(), {});
        assert(plan.entries.size() == 2);
        assert(plan.entries[0].verdict == ImportVerdict::Import);
        assert(plan.entries[0].destination == "Contents/Someone/The Album/01 Song.mp3");
        assert(plan.entries[1].verdict == ImportVerdict::Import);
        assert(plan.entries[1].destination == "Contents/UnknownArtist/UnknownAlbum/untagged.flac");
        assert(plan.bytesToCopy == 3000);
        assert(plan.count(ImportVerdict::Import) == 2);
        std::cout << "case 1 (a new file lands under Contents/artist/album with its own name) OK\n";
    }

    // 2. What the stick's libraries do not take, and what cannot be read,
    //    is refused before anything else is asked about it: no byte
    //    comparison, although a file of that size is on the stick.
    {
        ImportStick s = stick();
        s.files = {{"/media/STICK/Contents/A/B/x.mp3", 1000}};
        ImportFile silent = offer("/home/dj/in/zeros.mp3", 1000, "T", "A");
        silent.readable = false;
        ImportFile noLength = offer("/home/dj/in/nolength.mp3", 1000, "T", "A", {}, 0.0);
        g_byteComparisons = 0;
        const auto plan = planTrackImport({offer("/home/dj/in/notes.txt", 1000), offer("/home/dj/in/set.ogg", 1000),
                                           offer("/home/dj/in/noextension", 1000), silent, noLength},
                                          s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::UnsupportedFormat);
        // Engine lists Ogg Vorbis, rekordbox does not: not on a stick
        // that has both.
        assert(plan.entries[1].verdict == ImportVerdict::UnsupportedFormat);
        assert(plan.entries[2].verdict == ImportVerdict::UnsupportedFormat);
        assert(plan.entries[3].verdict == ImportVerdict::Unreadable);
        assert(plan.entries[4].verdict == ImportVerdict::Unreadable);
        assert(g_byteComparisons == 0);
        assert(plan.bytesToCopy == 0);

        ImportStick engineOnly = stick();
        engineOnly.acceptedExtensions = importableExtensions({"engine"});
        const auto ogg = planTrackImport({offer("/home/dj/in/set.OGG", 1000)}, engineOnly, tools(), {});
        assert(ogg.entries[0].verdict == ImportVerdict::Import);
        assert(importableExtensions({}).empty());
        assert(importableExtensions({"engine", "traktor"}).empty());
        std::cout << "case 2 (unsupported and unreadable files are refused first) OK\n";
    }

    // 3. The same bytes are on the stick under another name: nothing is
    //    copied, and the plan says whether a library lists that file. A
    //    file of the same size with other bytes is a new file.
    {
        ImportStick s = stick();
        s.files = {{"/media/STICK/Contents/A/B/renamed.mp3", 1000},
                   {"/media/STICK/Contents/A/B/stray.mp3", 1500},
                   {"/media/STICK/Contents/A/B/other.mp3", 1700}};
        s.libraryTracks = {row("engine", "/media/STICK/Contents/a/b/RENAMED.mp3")};
        g_bytes = {{"/home/dj/in/one.mp3", "one"},
                   {"/media/STICK/Contents/A/B/renamed.mp3", "one"},
                   {"/home/dj/in/two.mp3", "two"},
                   {"/media/STICK/Contents/A/B/stray.mp3", "two"}};
        const auto plan = planTrackImport({offer("/home/dj/in/one.mp3", 1000), offer("/home/dj/in/two.mp3", 1500),
                                           offer("/home/dj/in/three.mp3", 1700)},
                                          s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::AlreadyOnStick);
        assert(plan.entries[0].existingPath == "/media/STICK/Contents/A/B/renamed.mp3");
        assert(plan.entries[0].inLibrary);  // the row spells the path in another case
        assert(plan.entries[1].verdict == ImportVerdict::AlreadyOnStick);
        assert(plan.entries[1].existingPath == "/media/STICK/Contents/A/B/stray.mp3");
        assert(!plan.entries[1].inLibrary);
        assert(plan.entries[2].verdict == ImportVerdict::Import);
        assert(plan.bytesToCopy == 1700);
        g_bytes.clear();
        std::cout << "case 3 (the same bytes on the stick are not copied again) OK\n";
    }

    // 4. A file offered from the stick itself is on the stick: no copy,
    //    whatever the root is spelled like, and a folder whose name only
    //    starts like the stick's is not the stick.
    {
        ImportStick s = stick();
        s.root = "/media/STICK/";
        s.libraryTracks = {row("rekordbox", "/media/STICK/Contents/A/B/listed.mp3")};
        const auto plan = planTrackImport({offer("/media/STICK/Contents/A/B/listed.mp3", 1000),
                                           offer("/media/STICK/loose.mp3", 1000),
                                           offer("/media/STICK2/Contents/A/B/elsewhere.mp3", 1000, "E", "A", "B")},
                                          s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::AlreadyOnStick && plan.entries[0].inLibrary);
        assert(plan.entries[0].existingPath == "/media/STICK/Contents/A/B/listed.mp3");
        assert(plan.entries[1].verdict == ImportVerdict::AlreadyOnStick && !plan.entries[1].inLibrary);
        assert(plan.entries[2].verdict == ImportVerdict::Import);
        assert(plan.entries[2].destination == "Contents/A/B/elsewhere.mp3");
        std::cout << "case 4 (a file offered from the stick itself is not copied) OK\n";
    }

    // 5. The library holds the song as another file: a person decides.
    //    Chosen, it is imported and says so. Another length is another
    //    recording and is simply new.
    {
        ImportStick s = stick();
        s.files = {{"/media/STICK/Contents/Someone/X/Song.mp3", 900}};
        s.libraryTracks = {row("rekordbox", "/media/STICK/Contents/Someone/X/Song.mp3", "Song", "Someone", 200.5),
                           row("engine", "/media/STICK/Contents/Someone/X/Song.mp3", "Song", "Someone", 201.0)};
        const std::vector<ImportFile> offered = {
            offer("/home/dj/in/Someone - Song.flac", 5000, "Song", "Someone", "X", 200.0),
            offer("/home/dj/in/Someone - Song (Extended).flac", 7000, "Song", "Someone", "X", 320.0)};
        auto plan = planTrackImport(offered, s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::PossibleDuplicate);
        assert(plan.entries[0].existingPath == "/media/STICK/Contents/Someone/X/Song.mp3");
        assert(plan.entries[0].destination.empty());
        assert(plan.entries[1].verdict == ImportVerdict::Import);
        assert(plan.bytesToCopy == 7000);

        ImportChoices choices;
        choices.importAnyway = {"/home/dj/in/Someone - Song.flac"};
        plan = planTrackImport(offered, s, tools(), choices);
        assert(plan.entries[0].verdict == ImportVerdict::Import && plan.entries[0].importedAnyway);
        assert(plan.entries[0].destination == "Contents/Someone/X/Someone - Song.flac");
        assert(!plan.entries[1].importedAnyway);
        assert(plan.bytesToCopy == 12000);
        std::cout << "case 5 (a song the library already holds is a person's decision) OK\n";
    }

    // 6. The same file offered twice, by path or as a copy of the same
    //    bytes: the first carries the import, the second points at it.
    {
        g_bytes = {{"/home/dj/in/a.mp3", "a"}, {"/home/dj/copy/a (copy).mp3", "a"}};
        const auto plan = planTrackImport({offer("/home/dj/in/a.mp3", 1000), offer("/home/dj/in/a.mp3", 1000),
                                           offer("/home/dj/copy/a (copy).mp3", 1000),
                                           offer("/home/dj/in/b.mp3", 1000)},
                                          stick(), tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::Import);
        assert(plan.entries[1].verdict == ImportVerdict::OfferedTwice);
        assert(plan.entries[1].existingPath == "/home/dj/in/a.mp3");
        assert(plan.entries[2].verdict == ImportVerdict::OfferedTwice);
        assert(plan.entries[2].existingPath == "/home/dj/in/a.mp3");
        assert(plan.entries[3].verdict == ImportVerdict::Import);  // the same size, other bytes
        assert(plan.bytesToCopy == 2000);
        g_bytes.clear();
        std::cout << "case 6 (a file offered twice is imported once) OK\n";
    }

    // 7. Room: what is kept free is kept free, a file that does not fit
    //    is left out and a smaller one after it still goes. A person's
    //    "import anyway" does not make room. On FAT32 a file of 4 GiB is
    //    one byte too large whatever is free.
    {
        ImportStick s = stick();
        s.freeBytes = 10'000;
        s.reserveBytes = 4'000;
        auto plan = planTrackImport({offer("/in/a.mp3", 3000), offer("/in/b.mp3", 3001), offer("/in/c.mp3", 3000),
                                     offer("/in/d.mp3", 1)},
                                    s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::Import);
        assert(plan.entries[1].verdict == ImportVerdict::DoesNotFit);
        assert(plan.entries[1].destination.empty());
        assert(plan.entries[2].verdict == ImportVerdict::Import);
        assert(plan.entries[3].verdict == ImportVerdict::DoesNotFit);
        assert(plan.bytesToCopy == 6000);

        s.reserveBytes = 20'000;  // more than is free at all
        plan = planTrackImport({offer("/in/a.mp3", 1)}, s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::DoesNotFit);

        s = stick();
        s.freeBytes = 100;
        s.libraryTracks = {row("engine", "/media/STICK/Contents/x.mp3", "Song", "Someone")};
        ImportChoices choices;
        choices.importAnyway = {"/in/song.mp3"};
        plan = planTrackImport({offer("/in/song.mp3", 1000, "Song", "Someone")}, s, tools(), choices);
        assert(plan.entries[0].verdict == ImportVerdict::DoesNotFit && !plan.entries[0].importedAnyway);

        s = stick();
        s.freeBytes = std::uint64_t{64} << 30;
        s.maxFileBytes = maxFileBytesFor("vfat");
        assert(s.maxFileBytes == 4294967295u);
        plan = planTrackImport({offer("/in/long.wav", 4294967296u), offer("/in/fits.wav", 4294967295u)}, s, tools(),
                               {});
        assert(plan.entries[0].verdict == ImportVerdict::TooLargeForFilesystem);
        assert(plan.entries[1].verdict == ImportVerdict::Import);
        assert(maxFileBytesFor("exfat") == 0 && maxFileBytesFor("FAT32") == 4294967295u);
        std::cout << "case 7 (room on the stick, and FAT32's largest file) OK\n";
    }

    // 8. A place another file has is not taken: on the stick, or planned
    //    a line earlier, in any letter case. The number goes before the
    //    extension.
    {
        ImportStick s = stick();
        s.files = {{"/media/STICK/Contents/Someone/X/SONG.MP3", 900}};
        const auto plan = planTrackImport({offer("/in/1/song.mp3", 1000, "A", "Someone", "X", 100.0),
                                           offer("/in/2/Song.mp3", 1001, "B", "Someone", "X", 150.0),
                                           offer("/in/3/song.tar.mp3", 1002, "C", "Someone", "X", 170.0)},
                                          s, tools(), {});
        assert(plan.entries[0].destination == "Contents/Someone/X/song (2).mp3");
        assert(plan.entries[1].destination == "Contents/Someone/X/Song (3).mp3");
        assert(plan.entries[2].destination == "Contents/Someone/X/song.tar.mp3");
        std::cout << "case 8 (a taken place gets a numbered name) OK\n";
    }

    // 9. A folder the stick has under another letter case is one folder
    //    on FAT32 and exFAT: the new file's path spells it as the stick
    //    does, and a second file of the plan as the first one did.
    {
        ImportStick s = stick();
        s.files = {{"/media/STICK/Contents/Akki (De)/Old EP/a.mp3", 900}};
        const auto plan = planTrackImport({offer("/in/b.mp3", 1000, "B", "AKKI (DE)", "Old EP", 100.0),
                                           offer("/in/c.mp3", 1001, "C", "AKKI (DE)", "New EP", 150.0),
                                           offer("/in/d.mp3", 1002, "D", "akki (de)", "NEW ep", 170.0)},
                                          s, tools(), {});
        assert(plan.entries[0].destination == "Contents/Akki (De)/Old EP/b.mp3");
        assert(plan.entries[1].destination == "Contents/Akki (De)/New EP/c.mp3");
        assert(plan.entries[2].destination == "Contents/Akki (De)/New EP/d.mp3");
        std::cout << "case 9 (a folder keeps the spelling the stick has) OK\n";
    }

    // 10. Names the stick's filesystems and Windows take.
    {
        assert(stickSafePathSegment("AC/DC", "x") == "AC_DC");
        assert(stickSafePathSegment("What? <Why>: \"a|b\\c*\"", "x") == "What_ _Why__ _a_b_c__");
        assert(stickSafePathSegment("  Vol. 1...  ", "x") == "Vol. 1");
        assert(stickSafePathSegment("...", "UnknownArtist") == "UnknownArtist");
        assert(stickSafePathSegment("", "UnknownAlbum") == "UnknownAlbum");
        assert(stickSafePathSegment("..", "x") == "x");
        assert(stickSafePathSegment("tab\there\x7f", "x") == "tabhere");
        assert(stickSafePathSegment("K\xc3\xb6lsch", "x") == "K\xc3\xb6lsch");
        // Bytes that are not UTF-8 go: a lone continuation byte, a
        // sequence cut off at the end, an overlong slash.
        assert(stickSafePathSegment("a\x80z", "x") == "az");
        assert(stickSafePathSegment("a\xc3", "x") == "a");
        assert(stickSafePathSegment("a\xc0\xaf" "b", "x") == "ab");
        assert(stickSafePathSegment("con", "x") == "_con");
        assert(stickSafePathSegment("NUL.txt", "x") == "_NUL.txt");
        assert(stickSafePathSegment("COM1", "x") == "_COM1");
        assert(stickSafePathSegment("COM0", "x") == "COM0");
        assert(stickSafePathSegment("Console", "x") == "Console");
        // Cut on a character, counted as UTF-16 does: "ö" is one unit and
        // two bytes, a character beyond the BMP two units and four bytes.
        assert(stickSafePathSegment("\xc3\xb6\xc3\xb6\xc3\xb6", "x", 2) == "\xc3\xb6\xc3\xb6");
        assert(stickSafePathSegment("a\xf0\x9f\x8e\xb5" "b", "x", 2) == "a");
        assert(stickSafePathSegment("a\xf0\x9f\x8e\xb5" "b", "x", 3) == "a\xf0\x9f\x8e\xb5");
        // A cut that leaves a dot or a space at the end loses that too.
        assert(stickSafePathSegment("ab. cd", "x", 4) == "ab");

        assert(stickSafeFileName("01 Song.mp3") == "01 Song.mp3");
        assert(stickSafeFileName("AC/DC: Live?.FLAC") == "AC_DC_ Live_.FLAC");
        assert(stickSafeFileName("noextension") == "noextension");
        assert(stickSafeFileName(".hidden") == "hidden");
        assert(stickSafeFileName("???.mp3") == "___.mp3");
        assert(stickSafeFileName(" .mp3") == "track.mp3");
        assert(stickSafeFileName("aux.mp3") == "_aux.mp3");
        assert(stickSafeFileName("Song feat. Someone") == "Song feat. Someone");
        // The stem is cut, the extension stays.
        assert(stickSafeFileName(std::string(300, 'a') + ".flac") == std::string(115, 'a') + ".flac");

        Track t;
        t.filePath = "/in/sub/AC_DC - T.N.T..mp3";
        t.artist = "AC/DC";
        t.album = "T.N.T.";
        assert(importDestination(t) == "Contents/AC_DC/T.N.T/AC_DC - T.N.T.mp3");
        std::cout << "case 10 (names the stick's filesystems and Windows take) OK\n";
    }

    // 11. A place a library row names is taken although no file is there:
    //     a file copied to it would give that row, its cues and its grid,
    //     another recording to play.
    {
        ImportStick s = stick();
        s.libraryTracks = {row("rekordbox", "/media/STICK/Contents/Someone/X/SONG.mp3", "Old", "Other", 300.0)};
        const auto plan = planTrackImport({offer("/in/song.mp3", 1000, "New", "Someone", "X", 200.0)}, s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::Import);
        assert(plan.entries[0].destination == "Contents/Someone/X/song (2).mp3");
        std::cout << "case 11 (a path a row names is taken, file or no file) OK\n";
    }

    // 12. A size nobody read is not a size of nothing: such a file is not
    //     planned, whatever is on the stick.
    {
        ImportStick s = stick();
        s.freeBytes = 0;
        s.files = {{"/media/STICK/Contents/unsized.mp3", 0}};
        g_byteComparisons = 0;
        const auto plan = planTrackImport({offer("/in/unsized.mp3", 0)}, s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::Unreadable);
        assert(g_byteComparisons == 0);
        std::cout << "case 12 (a file of unknown size is not planned) OK\n";
    }

    // 13. No plan without a stick: an empty root, or the filesystem's
    //     own, would make every file of the computer "on the stick".
    {
        const auto refused = [](ImportStick s, ImportPlanTools t) {
            try {
                (void)planTrackImport({offer("/home/dj/in/a.mp3", 1000)}, s, t, {});
            } catch (const std::invalid_argument &) {
                return true;
            }
            return false;
        };
        ImportStick s = stick();
        s.root.clear();
        assert(refused(s, tools()));
        s.root = "/";
        assert(refused(s, tools()));
        ImportPlanTools noKey = tools();
        noKey.pathKey = nullptr;
        assert(refused(stick(), noKey));
        ImportPlanTools noBytes = tools();
        noBytes.sameBytes = nullptr;
        assert(refused(stick(), noBytes));
        assert(!refused(stick(), tools()));
        std::cout << "case 13 (no plan without a stick and its tools) OK\n";
    }

    // 14. One song in three libraries is one song: its rows do not make
    //     its artist and title ambiguous when a library does not know the
    //     length. And a refused file of the same artist and title does
    //     not either.
    {
        ImportStick s = stick();
        s.libraryTracks = {row("rekordbox", "/media/STICK/Contents/A/T.mp3", "T", "A", 0.0),
                           row("onelibrary", "/media/STICK/Contents/a/t.mp3", "T", "A", 0.0),
                           row("engine", "/media/STICK/Contents/A/T.mp3", "T", "A", 0.0)};
        ImportFile refusedTwin = offer("/in/T.txt", 500, "T", "A");
        const auto plan = planTrackImport({refusedTwin, offer("/in/T (new).mp3", 1000, "T", "A")}, s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::UnsupportedFormat);
        assert(plan.entries[1].verdict == ImportVerdict::PossibleDuplicate);
        assert(plan.entries[1].existingPath == "/media/STICK/Contents/A/T.mp3");
        std::cout << "case 14 (one song in three libraries is one song) OK\n";
    }

    // 15. A file offered from the stick is named as the stick's listing
    //     spells it; a copy on the stick under an extension the libraries
    //     do not take cannot carry the row, so the file is imported.
    {
        ImportStick s = stick();
        s.files = {{"/media/STICK/Contents/Akki (De)/x.mp3", 1000}, {"/media/STICK/Contents/copy.ogg", 2000}};
        g_bytes = {{"/in/y.mp3", "y"}, {"/media/STICK/Contents/copy.ogg", "y"}};
        const auto plan = planTrackImport({offer("/media/STICK/contents/AKKI (DE)/X.MP3", 1000),
                                           offer("/in/y.mp3", 2000, "Y", "B", "C")},
                                          s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::AlreadyOnStick);
        assert(plan.entries[0].existingPath == "/media/STICK/Contents/Akki (De)/x.mp3");
        assert(plan.entries[1].verdict == ImportVerdict::Import);
        assert(plan.entries[1].destination == "Contents/B/C/y.mp3");
        g_bytes.clear();
        std::cout << "case 15 (the stick's spelling, and no row on a name the libraries refuse) OK\n";
    }

    // 16. A copy of a file that was left out is decided on its own: the
    //     person chose the copy, and the copy is imported.
    {
        ImportStick s = stick();
        s.libraryTracks = {row("engine", "/media/STICK/Contents/x.mp3", "Song", "Someone")};
        g_bytes = {{"/in/a.mp3", "same"}, {"/in/b.mp3", "same"}};
        ImportChoices choices;
        choices.importAnyway = {"/in/b.mp3"};
        const auto plan = planTrackImport({offer("/in/a.mp3", 1000, "Song", "Someone"),
                                           offer("/in/b.mp3", 1000, "Song", "Someone"),
                                           offer("/in/b.mp3", 1000, "Song", "Someone"),
                                           offer("/in/a.mp3", 1000, "Song", "Someone")},
                                          s, tools(), choices);
        assert(plan.entries[0].verdict == ImportVerdict::PossibleDuplicate);
        // Asked about again, the file left out is the same question
        // again, not "offered twice" behind an import that never was.
        // (It is a copy of b, which is imported: that it is.)
        assert(plan.entries[3].verdict == ImportVerdict::OfferedTwice);
        assert(plan.entries[3].existingPath == "/in/b.mp3");
        assert(plan.entries[1].verdict == ImportVerdict::Import && plan.entries[1].importedAnyway);
        assert(plan.entries[2].verdict == ImportVerdict::OfferedTwice);
        assert(plan.bytesToCopy == 1000);
        g_bytes.clear();
        std::cout << "case 16 (a copy of a file left out is decided on its own) OK\n";
    }

    // 16b. The same file left out twice is the same question twice; two
    //      copies of a file the stick has give that file its rows once;
    //      and a row that names no file is still a song the library has.
    {
        ImportStick s = stick();
        s.libraryTracks = {row("engine", "/media/STICK/Contents/x.mp3", "Song", "Someone")};
        auto plan = planTrackImport({offer("/in/a.mp3", 1000, "Song", "Someone"),
                                     offer("/in/a.mp3", 1000, "Song", "Someone")},
                                    s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::PossibleDuplicate);
        assert(plan.entries[1].verdict == ImportVerdict::PossibleDuplicate);

        s = stick();
        s.files = {{"/media/STICK/loose.mp3", 1000}};
        g_bytes = {{"/media/STICK/loose.mp3", "l"}, {"/home/dj/copy.mp3", "l"}, {"/home/dj/copy2.mp3", "l"}};
        plan = planTrackImport({offer("/home/dj/copy.mp3", 1000), offer("/media/STICK/LOOSE.mp3", 1000),
                                offer("/home/dj/copy2.mp3", 1000)},
                               s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::AlreadyOnStick);
        assert(plan.entries[0].existingPath == "/media/STICK/loose.mp3");
        assert(plan.entries[1].verdict == ImportVerdict::OfferedTwice);
        assert(plan.entries[1].existingPath == "/home/dj/copy.mp3");
        assert(plan.entries[2].verdict == ImportVerdict::OfferedTwice);
        assert(plan.count(ImportVerdict::AlreadyOnStick) == 1);
        g_bytes.clear();

        s = stick();
        Track pathless = row("rekordbox", "", "T", "A", 200.0);
        pathless.filename.clear();
        s.libraryTracks = {pathless};
        plan = planTrackImport({offer("/in/t.mp3", 1000, "T", "A")}, s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::PossibleDuplicate);
        std::cout << "case 16b (left out twice, found on the stick twice, a row without a file) OK\n";
    }

    // 17. Room as the filesystem hands it out: whole clusters a file, one
    //     a new folder. Ten files of 100,000 bytes are 1,000,000 bytes
    //     and 1,310,720 on 128 KiB clusters, plus three folders.
    {
        ImportStick s = stick();
        s.clusterBytes = 131072;
        s.freeBytes = 8 * 131072;
        std::vector<ImportFile> ten;
        for (int i = 0; i < 10; ++i) {
            ten.push_back(offer("/in/" + std::to_string(i) + ".mp3", 100'000 + i));
        }
        const auto plan = planTrackImport(ten, s, tools(), {});
        // Three folders (Contents, UnknownArtist, UnknownAlbum) and five
        // files are the eight clusters.
        assert(plan.count(ImportVerdict::Import) == 5);
        assert(plan.entries[4].verdict == ImportVerdict::Import);
        assert(plan.entries[5].verdict == ImportVerdict::DoesNotFit);

        s.reserveBytes = UINT64_MAX - 5;  // a sum that would wrap
        const auto none = planTrackImport(ten, s, tools(), {});
        assert(none.count(ImportVerdict::DoesNotFit) == 10);
        std::cout << "case 17 (room in clusters, folders included) OK\n";
    }

    // 18. A stick as Windows names it, a path with a doubled separator,
    //     and a taken name with two dots in it.
    {
        ImportStick s = stick();
        s.root = "E:\\";
        s.files = {{"E:\\Contents\\\\Akki (De)\\Old EP\\a.tar.mp3", 900}};
        const auto plan = planTrackImport({offer("C:/in/a.tar.mp3", 1000, "A", "AKKI (DE)", "old ep", 100.0),
                                           offer("E:/contents/akki (de)/old ep/A.TAR.MP3", 900)},
                                          s, tools(), {});
        assert(plan.entries[0].verdict == ImportVerdict::Import);
        assert(plan.entries[0].destination == "Contents/Akki (De)/Old EP/a.tar (2).mp3");
        assert(plan.entries[1].verdict == ImportVerdict::AlreadyOnStick);
        assert(plan.entries[1].existingPath == "E:\\Contents\\\\Akki (De)\\Old EP\\a.tar.mp3");
        std::cout << "case 18 (a Windows root, a doubled separator, a name with two dots) OK\n";
    }

    // 19. More names: nothing in front that hides or is stripped, the
    //     other devices Windows keeps, and a path that stays under what
    //     Windows gives one.
    {
        assert(stickSafePathSegment("...And You Will Know Us", "x") == "And You Will Know Us");
        assert(stickSafePathSegment(" . hidden", "x") == "hidden");
        assert(stickSafePathSegment("CONIN$", "x") == "_CONIN$");
        assert(stickSafePathSegment("conout$.txt", "x") == "_conout$.txt");
        assert(stickSafePathSegment("NUL .txt", "x") == "_NUL .txt");
        assert(stickSafePathSegment("COM\xc2\xb9", "x") == "_COM\xc2\xb9");
        assert(stickSafePathSegment("LPT\xc2\xb3.x", "x") == "_LPT\xc2\xb3.x");
        assert(stickSafePathSegment("COM10", "x") == "COM10");
        assert(stickSafePathSegment("COMPUTER", "x") == "COMPUTER");

        Track t;
        t.filePath = "/in/" + std::string(300, 'n') + ".flac";
        t.artist = std::string(300, 'a');
        t.album = std::string(300, 'b');
        const std::string destination = importDestination(t);
        assert(destination == "Contents/" + std::string(60, 'a') + "/" + std::string(60, 'b') + "/"
                                  + std::string(115, 'n') + ".flac");
        assert(destination.size() == 251);
        assert(maxFileBytesFor("FAT") == 2147483647u && maxFileBytesFor("fat16") == 2147483647u);
        std::cout << "case 19 (more names, and a path of a length Windows takes) OK\n";
    }

    std::cout << "ALL TRACK IMPORT PLAN CASES PASSED\n";
    return 0;
}
