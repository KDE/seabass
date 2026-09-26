// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// application::fillFileSizes: the size of the file on disk for every row
// that has none, 0 for a file that is not there, and a size the catalog
// already gave (OneLibrary's) left as it is. And completeTracks(), which
// the catalog cache's Full stage, ScanLibrary and the CLI all run.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/fill_file_sizes.hpp"
#include "application/use_cases/scan_library.hpp"
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

    // fileSizeOnDisk(): the one rule both size stages share.
    check(seabass::application::fileSizeOnDisk(seabass::pathToUtf8(present)) == std::optional<std::uint64_t>(4321),
          "fileSizeOnDisk sizes a file with a non-ASCII name");
    check(!seabass::application::fileSizeOnDisk(seabass::pathToUtf8(root / "nothing.mp3")),
          "fileSizeOnDisk has nothing for a missing file");
    check(!seabass::application::fileSizeOnDisk(""), "fileSizeOnDisk has nothing for no path");

    // Cancellation: checked before every file, and a tick after each one,
    // so a cancel landing during the second file's tick stops the pass
    // before the third file is looked at. The same for the artwork check.
    {
        struct CancelAt : seabass::application::ProgressReporter
        {
            seabass::application::CancellationToken token;
            size_t at = 0;
            int ticks = 0;
            void start(const std::string &, size_t) override {}
            void finish() override {}
            void warn(const std::string &) override {}
            void tick(size_t current) override
            {
                ++ticks;
                if (current == at) {
                    token.cancel();
                }
            }
        };
        std::vector<Track> many;
        for (int i = 0; i < 6; ++i) {
            const fs::path file = root / "Contents" / ("many" + std::to_string(i) + ".mp3");
            plant(file, 10 + i);
            many.push_back(row("many" + std::to_string(i), file));
            many.back().artworkPath = seabass::pathToUtf8(file);
        }
        CancelAt reporter;
        reporter.at = 2;
        bool threw = false;
        try {
            seabass::application::fillFileSizes(many, reporter.token, reporter);
        } catch (const seabass::application::OperationCancelled &) {
            threw = true;
        }
        check(threw, "a cancelled size pass throws OperationCancelled");
        check(reporter.ticks == 2, "no file is stat'd after the cancel");
        check(many[1].fileSizeBytes == 11 && many[2].fileSizeBytes == 0, "the pass stopped where it was cancelled");

        CancelAt art;
        art.at = 3;
        threw = false;
        try {
            seabass::application::dropMissingArtwork(many, art.token, art);
        } catch (const seabass::application::OperationCancelled &) {
            threw = true;
        }
        check(threw && art.ticks == 3, "the artwork check stops within one image of a cancel");
    }

    // completeTracks(): the sizes, and a cover that is not on disk
    // dropped on the Engine and OneLibrary rows, never on a rekordbox row
    // (its reader never checked). ScanLibrary::execute() runs it after
    // readAll(), so the CLI gets what the GUI's Full stage gets.
    {
        const fs::path cover = root / "Engine Library" / "art" / "here.jpg";
        plant(cover, 10);
        const std::string gone = seabass::pathToUtf8(root / "Engine Library" / "art" / "gone.jpg");
        const auto withArt = [&](const std::string &format, const std::string &art) {
            Track t = row(format, present);
            t.format = format;
            t.artworkPath = art;
            return t;
        };
        const std::vector<Track> read{withArt("engine", seabass::pathToUtf8(cover)), withArt("engine", gone),
                                      withArt("onelibrary", gone), withArt("rekordbox", gone)};

        std::vector<Track> tracks = read;
        seabass::application::completeTracks(tracks);
        check(tracks[0].fileSizeBytes == 4321, "completeTracks fills the sizes");
        check(tracks[0].artworkPath == seabass::pathToUtf8(cover), "a cover on disk is kept");
        check(tracks[1].artworkPath.empty(), "a missing Engine cover is dropped");
        check(tracks[2].artworkPath.empty(), "a missing OneLibrary cover is dropped");
        check(tracks[3].artworkPath == gone, "a rekordbox row keeps the cover its catalog names");

        class FixedReader : public seabass::application::LibraryReader
        {
        public:
            explicit FixedReader(std::vector<Track> tracks) : m_tracks(std::move(tracks)) {}
            std::vector<Track> readAll() override { return m_tracks; }

        private:
            std::vector<Track> m_tracks;
        };
        FixedReader reader(read);
        const std::vector<Track> scanned = seabass::application::ScanLibrary(reader).execute();
        check(scanned.size() == read.size() && scanned[0].fileSizeBytes == 4321, "ScanLibrary fills the sizes");
        check(scanned[1].artworkPath.empty() && scanned[2].artworkPath.empty(),
              "ScanLibrary drops the missing covers, as the GUI's Full stage does");
        check(scanned[3].artworkPath == gone, "and leaves rekordbox's alone");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all fill_file_sizes_test checks passed\n";
    return 0;
}
