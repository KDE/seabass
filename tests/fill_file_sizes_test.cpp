// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// application::fillFileSizes: the size of the file on disk for every row
// that has none, 0 for a file that is not there, and a size the catalog
// already gave (OneLibrary's) left as it is.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/fill_file_sizes.hpp"
#include "domain/track.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::Track;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

Track row(const std::string &id, const fs::path &file, std::uint64_t size = 0)
{
    Track t;
    t.sourceId = id;
    t.filePath = file.empty() ? std::string() : seabass::pathToUtf8(file);
    t.fileSizeBytes = size;
    return t;
}

void plant(const fs::path &file, std::uintmax_t size)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).put('x');
    fs::resize_file(file, size);
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "fill_file_sizes_test";
    fs::remove_all(root);
    // A non-ASCII name: the path crosses to the filesystem as UTF-8.
    const fs::path present = root / "Contents" / seabass::pathFromUtf8("Kügler Beat.mp3");
    const fs::path other = root / "Contents" / "other.flac";
    const fs::path missing = root / "Contents" / "missing.mp3";
    plant(present, 4321);
    plant(other, 99);

    std::vector<Track> tracks{
        row("present", present),
        row("same file, second row", present),
        row("other", other),
        row("missing", missing),
        row("no path", fs::path()),
        // A size the catalog gave (OneLibrary) is kept, even when the
        // file on disk says something else.
        row("catalog size", other, 123456),
    };
    seabass::application::fillFileSizes(tracks);

    check(tracks[0].fileSizeBytes == 4321, "present file gets its size");
    check(tracks[1].fileSizeBytes == 4321, "a second row naming the same file gets the same size");
    check(tracks[2].fileSizeBytes == 99, "another file gets its own size");
    check(tracks[3].fileSizeBytes == 0, "a missing file stays 0");
    check(tracks[4].fileSizeBytes == 0, "no path stays 0");
    check(tracks[5].fileSizeBytes == 123456, "a size already known is kept");

    // A missing file that appears later is found on the next call: 0 is
    // "unknown", so it is asked again.
    plant(missing, 7);
    seabass::application::fillFileSizes(tracks);
    check(tracks[3].fileSizeBytes == 7, "a row left at 0 is filled by a later call");
    check(tracks[0].fileSizeBytes == 4321, "a known size is not asked again");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all fill_file_sizes_test checks passed\n";
    return 0;
}
