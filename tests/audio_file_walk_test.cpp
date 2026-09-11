// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "infrastructure/cleanup/audio_file_walk.hpp"

#include "scratch_path.hpp"

using seabass::application::CancellationToken;
using seabass::infrastructure::cleanup::isAudioExtension;
using seabass::infrastructure::cleanup::walkAudioFiles;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &p, const std::string &data = "x")
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << data;
}

bool found(const seabass::infrastructure::cleanup::AudioFileWalkResult &r, const fs::path &p)
{
    const std::string want = p.generic_string();
    for (const auto &f : r.files) {
        if (f.filePath == want) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main()
{
    // Case 1: the extension list is closed, and closed in the safe
    // direction -- everything it admits becomes a deletion candidate.
    {
        assert(isAudioExtension("/a/b.mp3"));
        assert(isAudioExtension("/a/b.M4A"));      // case-insensitive
        assert(isAudioExtension("/a/b.FLAC"));
        assert(!isAudioExtension("/a/b.txt"));
        assert(!isAudioExtension("/a/b.pdb"));     // a rekordbox database
        assert(!isAudioExtension("/a/b.mp3.bak")); // only the real extension counts
        assert(!isAudioExtension("/a/b"));         // no extension at all
        assert(!isAudioExtension("/a.mp3/b"));     // a dot in a DIRECTORY name is not one
        std::cout << "case 1 (audio extensions, closed list) OK\n";
    }

    fs::path root = seabass::testing::scratchRoot() / "seabass_audio_file_walk_test";
    fs::remove_all(root);

    // Case 2: a real nested tree. Audio at several depths is found with
    // its size; non-audio is ignored entirely.
    {
        writeFile(root / "Contents" / "Artist" / "Album" / "01_track.mp3", "0123456789");
        writeFile(root / "Contents" / "Artist" / "Album" / "02_track.m4a", "01234");
        writeFile(root / "Contents" / "Other" / "deep" / "deeper" / "03_track.MP3", "012");
        writeFile(root / "Contents" / "Artist" / "cover.jpg", "not audio");
        writeFile(root / "Contents" / "notes.txt", "not audio");

        auto result = walkAudioFiles((root / "Contents").generic_string(), CancellationToken());
        assert(!result.incomplete);
        assert(result.files.size() == 3);
        assert(found(result, root / "Contents" / "Artist" / "Album" / "01_track.mp3"));
        assert(found(result, root / "Contents" / "Artist" / "Album" / "02_track.m4a"));
        assert(found(result, root / "Contents" / "Other" / "deep" / "deeper" / "03_track.MP3"));
        for (const auto &f : result.files) {
            assert(f.fileSizeBytes > 0);  // a real stat, not a placeholder
        }
        assert(result.directoriesVisited >= 5);
        std::cout << "case 2 (nested tree, sizes read, non-audio ignored) OK\n";
    }

    // Case 3: a missing root is reported as incomplete rather than as an
    // empty stick -- "nothing here" and "could not look" must not be the
    // same answer when the next step is proposing deletions.
    {
        auto result = walkAudioFiles((root / "does-not-exist").generic_string(), CancellationToken());
        assert(result.files.empty());
        assert(result.incomplete);

        auto empty = walkAudioFiles(std::string(), CancellationToken());
        assert(empty.files.empty());
        std::cout << "case 3 (missing root -> incomplete, not empty) OK\n";
    }

    // Case 4: symlinks are neither followed nor reported. A link's target
    // can sit outside the stick, and nothing should offer to delete a
    // file it reached by leaving.
    {
        fs::path outside = root / "outside";
        writeFile(outside / "elsewhere.mp3", "0123456789");
        std::error_code ec;
        fs::create_directory_symlink(outside, root / "Contents" / "linked-dir", ec);
        fs::create_symlink(outside / "elsewhere.mp3", root / "Contents" / "linked.mp3", ec);
        if (ec) {
            std::cout << "case 4 (symlinks) SKIPPED -- cannot create symlinks here\n";
        } else {
            auto result = walkAudioFiles((root / "Contents").generic_string(), CancellationToken());
            assert(!found(result, root / "Contents" / "linked.mp3"));
            assert(!found(result, root / "Contents" / "linked-dir" / "elsewhere.mp3"));
            assert(result.files.size() == 3);  // the same three as case 2
            std::cout << "case 4 (symlinks neither followed nor reported) OK\n";
        }
    }

    // Case 5: cancellation propagates out rather than returning a
    // half-walk that a caller could mistake for the whole stick.
    {
        CancellationToken cancel;
        cancel.cancel();
        bool threw = false;
        try {
            walkAudioFiles((root / "Contents").generic_string(), cancel);
        } catch (const seabass::application::OperationCancelled &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 5 (cancellation throws, never a partial list) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all audio_file_walk_test cases passed\n";
    return 0;
}
