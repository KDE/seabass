// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/find_unreferenced_files.hpp"

using seabass::application::AudioFileOnDisk;
using seabass::application::CatalogTracks;
using seabass::application::findUnreferencedFiles;
using seabass::domain::Track;

namespace
{

Track rowFor(const std::string &format, const std::string &path)
{
    Track t;
    t.format = format;
    t.sourceId = path;
    t.filePath = path;
    return t;
}

AudioFileOnDisk fileAt(const std::string &path, std::uint64_t bytes = 1000)
{
    return AudioFileOnDisk{path, bytes};
}

bool listed(const std::vector<AudioFileOnDisk> &files, const std::string &path)
{
    for (const auto &f : files) {
        if (f.filePath == path) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main()
{
    // Case 1: a file referenced by NO catalog is unreferenced; one
    // referenced by any single catalog is not. This is the whole feature.
    {
        std::vector<AudioFileOnDisk> disk = {fileAt("/stick/Contents/a/kept-by-rekordbox.mp3"),
                                              fileAt("/stick/Contents/a/kept-by-engine.mp3"),
                                              fileAt("/stick/Contents/a/kept-by-onelibrary.mp3"),
                                              fileAt("/stick/Contents/a/stray.mp3", 5000)};
        CatalogTracks catalogs;
        catalogs.rekordbox = {rowFor("rekordbox", "/stick/Contents/a/kept-by-rekordbox.mp3")};
        catalogs.engine = {rowFor("engine", "/stick/Contents/a/kept-by-engine.mp3")};
        catalogs.oneLibrary = {rowFor("onelibrary", "/stick/Contents/a/kept-by-onelibrary.mp3")};

        auto scan = findUnreferencedFiles(disk, catalogs);
        assert(scan.usable);
        assert(scan.unreferenced.size() == 1);
        assert(scan.unreferenced[0].filePath == "/stick/Contents/a/stray.mp3");
        assert(scan.unreferenced[0].fileSizeBytes == 5000);
        assert(scan.catalogsConsulted.size() == 3);
        assert(scan.audioFilesSeen == 4);
        std::cout << "case 1 (referenced by any catalog is not a stray) OK\n";
    }

    // Case 2: the mistake this API exists to prevent -- checking only one
    // catalog. The Engine-only file must NOT be reported as unreferenced
    // when Engine was never consulted... which it cannot be, because a
    // caller that omits Engine gets an answer that says so.
    {
        std::vector<AudioFileOnDisk> disk = {fileAt("/stick/Contents/a/engine-only.mp3")};
        CatalogTracks rekordboxOnly;
        rekordboxOnly.rekordbox = std::vector<Track>{};

        auto scan = findUnreferencedFiles(disk, rekordboxOnly);
        assert(scan.usable);
        assert(scan.unreferenced.size() == 1);  // it genuinely looks unreferenced...
        assert(scan.catalogsConsulted.size() == 1);
        assert(scan.catalogsConsulted[0] == "rekordbox");  // ...and the answer says on what basis
        std::cout << "case 2 (a partial check reports which catalogs it saw) OK\n";
    }

    // Case 3: no catalog at all must never read as "nothing is
    // referenced" -- that would propose deleting every file on the stick.
    {
        std::vector<AudioFileOnDisk> disk = {fileAt("/stick/Contents/a/one.mp3"),
                                              fileAt("/stick/Contents/a/two.mp3")};
        auto scan = findUnreferencedFiles(disk, CatalogTracks{});
        assert(!scan.usable);
        assert(scan.unreferenced.empty());
        assert(scan.catalogsConsulted.empty());
        std::cout << "case 3 (no catalogs -> unusable, not 'delete everything') OK\n";
    }

    // Case 4: separators and case. exFAT and NTFS are case-insensitive,
    // so a catalog row and a directory entry that differ only in case
    // name the same physical file -- calling that file unreferenced
    // would offer a referenced file for deletion.
    {
        std::vector<AudioFileOnDisk> disk = {fileAt("/stick/Contents/Artist/Track.mp3"),
                                              fileAt("/stick/Contents/Artist/other.mp3")};
        CatalogTracks catalogs;
        catalogs.rekordbox = {rowFor("rekordbox", "\\stick\\contents\\artist\\TRACK.MP3"),
                              rowFor("rekordbox", "/stick/Contents/./Artist/../Artist/other.mp3")};

        auto scan = findUnreferencedFiles(disk, catalogs);
        assert(scan.unreferenced.empty());
        std::cout << "case 4 (backslashes, case and . / .. all normalize) OK\n";
    }

    // Case 5: an empty filePath references nothing and must not make
    // every file look referenced; a broken row (no resolved path) simply
    // contributes no protection.
    {
        std::vector<AudioFileOnDisk> disk = {fileAt("/stick/Contents/a/stray.mp3")};
        CatalogTracks catalogs;
        Track broken = rowFor("rekordbox", "");
        broken.title = "A row whose file could not be resolved";
        catalogs.rekordbox = {broken};

        auto scan = findUnreferencedFiles(disk, catalogs);
        assert(scan.referencedPathsSeen == 0);
        assert(listed(scan.unreferenced, "/stick/Contents/a/stray.mp3"));
        std::cout << "case 5 (rows with no resolved path protect nothing) OK\n";
    }

    // Case 6: a present-but-empty catalog is not the same as an absent
    // one. Both are ordinary; only the first is evidence.
    {
        CatalogTracks absent;
        absent.rekordbox = std::vector<Track>{};
        assert(absent.present().size() == 1);

        CatalogTracks none;
        assert(none.present().empty());
        std::cout << "case 6 (empty catalog counts as consulted, absent does not) OK\n";
    }

    std::cout << "all find_unreferenced_files_test cases passed\n";
    return 0;
}
