// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// copyFileVerified: one audio file copied onto a stick for the track
// import. What arrives, and what is left behind when it does not.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/track_import/verified_file_copy.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::track_import;
using seabass::infrastructure::hashing::Sha256;
namespace fs = std::filesystem;

namespace
{

void write(const fs::path &path, const std::string &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
}

std::string read(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Bytes that differ from place to place, so a piece written twice or in
// the wrong order is another file.
std::string pattern(std::size_t size)
{
    std::string bytes(size, '\0');
    std::uint32_t state = 12345;
    for (char &byte : bytes) {
        state = state * 1664525u + 1013904223u;
        byte = static_cast<char>(state >> 24);
    }
    return bytes;
}

// Every file under `folder`, relative, sorted.
std::vector<std::string> filesUnder(const fs::path &folder)
{
    std::vector<std::string> found;
    if (!fs::exists(folder)) {
        return found;
    }
    for (const auto &entry : fs::recursive_directory_iterator(folder)) {
        if (!entry.is_directory()) {
            found.push_back(entry.path().lexically_relative(folder).generic_string());  // narrow-ok: ASCII test names
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_verified_file_copy_test";
    fs::remove_all(root);
    const fs::path computer = root / "computer";
    const fs::path stick = root / "stick";
    fs::create_directories(stick);

    // 1. The file arrives whole in a folder that was not there, in
    //    pieces smaller than itself: bytes, size, digest, modification
    //    time, and no temporary file beside it. Progress ends at the size.
    {
        const std::string bytes = pattern(10'000);
        write(computer / "song.mp3", bytes);
        const auto modified = fs::file_time_type::clock::now() - std::chrono::hours(24 * 400);
        fs::last_write_time(computer / "song.mp3", modified);

        FileCopyOptions options;
        options.chunkSize = 4096;
        std::vector<std::uint64_t> seen;
        options.progress = [&](std::uint64_t soFar) { seen.push_back(soFar); };
        const fs::path destination = stick / "Contents" / "Someone" / "An Album" / "song.mp3";
        const FileCopyResult result = copyFileVerified(computer / "song.mp3", destination, options);
        assert(result.copied && !result.cancelled && result.error.empty());
        assert(result.bytes == 10'000);
        assert(result.sha256 == Sha256::of(bytes));
        assert(read(destination) == bytes);
        assert(fs::last_write_time(destination) == fs::last_write_time(computer / "song.mp3"));
        assert((seen == std::vector<std::uint64_t>{4096, 8192, 10'000}));
        assert((filesUnder(stick) == std::vector<std::string>{"Contents/Someone/An Album/song.mp3"}));
        assert(read(computer / "song.mp3") == bytes);
        std::cout << "case 1 (the file arrives whole, with its time, and nothing beside it) OK\n";
    }

    // 2. A file that is there is never replaced, and is as it was.
    {
        write(computer / "other.mp3", pattern(500));
        const fs::path destination = stick / "Contents" / "Someone" / "An Album" / "song.mp3";
        const std::string before = read(destination);
        const FileCopyResult result = copyFileVerified(computer / "other.mp3", destination, {});
        assert(!result.copied && !result.cancelled);
        assert(result.error.find("already") != std::string::npos);
        assert(read(destination) == before);
        assert(filesUnder(stick).size() == 1);
        std::cout << "case 2 (a file on the stick is not replaced) OK\n";
    }

    // 3. What an interrupted copy left under the temporary name is not
    //    the start of the next copy: longer than the new file, and the
    //    new file still arrives as itself.
    {
        const fs::path destination = stick / "Contents" / "left.mp3";
        fs::path leftover = destination;
        leftover += ImportTempSuffix;
        write(leftover, pattern(9'000));
        const std::string bytes = pattern(3'000);
        write(computer / "left.mp3", bytes);
        const FileCopyResult result = copyFileVerified(computer / "left.mp3", destination, {});
        assert(result.copied);
        assert(read(destination) == bytes);
        assert(!fs::exists(leftover));
        std::cout << "case 3 (a leftover temporary file is not part of the next copy) OK\n";
    }

    // 4. A cancel between two pieces: nothing under the name, nothing
    //    under the temporary name, and it is a cancel, not an error.
    {
        write(computer / "long.wav", pattern(50'000));
        FileCopyOptions options;
        options.chunkSize = 4096;
        std::uint64_t furthest = 0;
        options.progress = [&](std::uint64_t soFar) {
            furthest = soFar;
            if (soFar >= 8192) {
                options.cancel.cancel();
            }
        };
        const auto before = filesUnder(stick);
        const FileCopyResult result = copyFileVerified(computer / "long.wav", stick / "Contents" / "long.wav", options);
        assert(result.cancelled && !result.copied && result.error.empty());
        assert(furthest == 8192);  // it stopped there, not after writing the rest
        assert(filesUnder(stick) == before);

        // The folders made for it go again; one that was there stays.
        const FileCopyResult deep =
            copyFileVerified(computer / "long.wav", stick / "Contents" / "New Artist" / "New Album" / "long.wav", options);
        assert(deep.cancelled);
        assert(!fs::exists(stick / "Contents" / "New Artist"));
        assert(fs::is_directory(stick / "Contents"));
        std::cout << "case 4 (a cancel leaves nothing) OK\n";
    }

    // 5. What cannot be copied says so and leaves nothing: no source, a
    //    folder offered as the source, a file in the way of the
    //    destination's folder.
    {
        const auto before = filesUnder(stick);
        FileCopyResult result = copyFileVerified(computer / "missing.mp3", stick / "Contents" / "missing.mp3", {});
        assert(!result.copied && !result.cancelled && !result.error.empty());
        result = copyFileVerified(computer, stick / "Contents" / "folder.mp3", {});
        assert(!result.copied && !result.error.empty());

        write(stick / "Blocked", "a file where a folder is wanted");
        result = copyFileVerified(computer / "song.mp3", stick / "Blocked" / "Artist" / "song.mp3", {});
        assert(!result.copied && !result.cancelled && !result.error.empty());
        auto after = filesUnder(stick);
        after.erase(std::remove(after.begin(), after.end(), "Blocked"), after.end());
        assert(after == before);
        std::cout << "case 5 (what cannot be copied says so and leaves nothing) OK\n";
    }

    // 6. A file of no bytes is a file: it arrives as one, and its digest
    //    is that of nothing.
    {
        write(computer / "empty.mp3", "");
        const FileCopyResult result = copyFileVerified(computer / "empty.mp3", stick / "Contents" / "empty.mp3", {});
        assert(result.copied && result.bytes == 0);
        assert(result.sha256 == Sha256::of(std::string_view{}));
        assert(fs::file_size(stick / "Contents" / "empty.mp3") == 0);
        std::cout << "case 6 (an empty file arrives as one) OK\n";
    }

    // 7. A source written to while it is read, at the same size, is not
    //    one file's bytes: not imported, and nothing left.
    {
        write(computer / "tagged.mp3", pattern(20'000));
        FileCopyOptions options;
        options.chunkSize = 4096;
        bool rewritten = false;
        options.progress = [&](std::uint64_t soFar) {
            if (soFar >= 8192 && !rewritten) {
                rewritten = true;
                std::fstream in(computer / "tagged.mp3", std::ios::binary | std::ios::in | std::ios::out);
                // Ahead of what has been read, so the copy would carry
                // bytes of both files.
                in.seekp(15'000);
                in.write("ANOTHER TAG", 11);
                in.close();
                assert(in.good());
                // Another time whatever the filesystem's clock resolves.
                fs::last_write_time(computer / "tagged.mp3", fs::file_time_type::clock::now() + std::chrono::hours(1));
            }
        };
        const FileCopyResult result =
            copyFileVerified(computer / "tagged.mp3", stick / "Tagged" / "tagged.mp3", options);
        assert(rewritten && fs::file_size(computer / "tagged.mp3") == 20'000);
        assert(!result.copied && !result.cancelled);
        assert(result.error.find("changed") != std::string::npos);
        assert(!fs::exists(stick / "Tagged"));
        std::cout << "case 7 (a source rewritten in place during the copy is not imported) OK\n";
    }

    // 8. A file offered under the temporary name of its own destination
    //    is refused before it is emptied.
    {
        const fs::path destination = stick / "Contents" / "self.mp3";
        fs::path leftover = destination;
        leftover += ImportTempSuffix;
        const std::string bytes = pattern(700);
        write(leftover, bytes);
        const FileCopyResult result = copyFileVerified(leftover, destination, {});
        assert(!result.copied && !result.error.empty());
        assert(read(leftover) == bytes);
        assert(!fs::exists(destination));
        fs::remove(leftover);
        std::cout << "case 8 (a leftover is not copied onto itself) OK\n";
    }

    // 9. sweepImportLeftovers takes the leftovers of interrupted copies
    //    out of the folder named: files with exactly that suffix, directly
    //    in it. Not a file whose name only has the suffix inside it, not a
    //    folder of that name, not one below, not a folder outside the
    //    stick; never through a link.
    {
        const fs::path album = stick / "Contents" / "Swept" / "Album";
        const auto leftoverOf = [](const fs::path &file) {
            fs::path leftover = file;
            leftover += ImportTempSuffix;
            return leftover;
        };
        write(leftoverOf(album / "one.mp3"), pattern(900));
        write(leftoverOf(album / "two.flac"), pattern(300));
        write(album / "kept.mp3", pattern(100));
        write(album / "kept.seabass-import-tmp.mp3", pattern(100));
        write(album / "kept.seabass-import-tmpx", pattern(100));
        fs::create_directories(leftoverOf(album / "a folder"));
        write(leftoverOf(album / "Deeper" / "below.mp3"), pattern(100));
        const fs::path outside = root / "outside";
        write(leftoverOf(outside / "elsewhere.mp3"), pattern(100));
        // The suffix alone is no name of a copy: kept.
        write(album / ImportTempSuffix, pattern(10));

        // Outside the stick, and a path that leaves it: nothing.
        assert(sweepImportLeftovers(stick, outside).empty());
        assert(sweepImportLeftovers(stick, stick / ".." / "outside").empty());
        assert(fs::exists(leftoverOf(outside / "elsewhere.mp3")));
        // Not there: nothing, and no error.
        assert(sweepImportLeftovers(stick, stick / "Contents" / "Nobody").empty());

#if !defined(_WIN32)
        // A link to a leftover outside is not removed, nor its target; a
        // folder reached through a link is not looked into.
        fs::create_symlink(leftoverOf(outside / "elsewhere.mp3"), leftoverOf(album / "link.mp3"));
        fs::create_directory_symlink(outside, stick / "Contents" / "Linked");
        assert(sweepImportLeftovers(stick, stick / "Contents" / "Linked").empty());
        assert(fs::exists(leftoverOf(outside / "elsewhere.mp3")));
#endif

        const std::vector<fs::path> removed = sweepImportLeftovers(stick, album);
        assert(removed.size() == 2);
        assert(!fs::exists(leftoverOf(album / "one.mp3")) && !fs::exists(leftoverOf(album / "two.flac")));
        assert(read(album / "kept.mp3") == pattern(100));
        assert(fs::exists(album / "kept.seabass-import-tmp.mp3") && fs::exists(album / "kept.seabass-import-tmpx"));
        assert(fs::is_directory(leftoverOf(album / "a folder")));
        assert(fs::exists(leftoverOf(album / "Deeper" / "below.mp3")));
        assert(fs::exists(album / ImportTempSuffix));
        assert(fs::exists(leftoverOf(outside / "elsewhere.mp3")));
#if !defined(_WIN32)
        assert(fs::is_symlink(fs::symlink_status(leftoverOf(album / "link.mp3"))));
#endif
        std::cout << "case 9 (leftovers swept from the folder named, and nothing else) OK\n";
    }

    fs::remove_all(root);
    std::cout << "ALL VERIFIED FILE COPY CASES PASSED\n";
    return 0;
}
