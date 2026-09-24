// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The duration source the scans use. Its own test because the scans pick
// it over the Qt probe: on macOS QMediaPlayer never finishes loading, so
// a probe that answers synchronously is not a preference there.
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infrastructure/audio/taglib_duration_probe.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "mp3_fixture.hpp"
#include "scratch_path.hpp"

using seabass::infrastructure::audio::TagLibDurationProbe;
using namespace seabass::test_fixture::mp3;
namespace fs = std::filesystem;

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_taglib_duration_probe_test";
    fs::remove_all(root);
    fs::create_directories(root);
    TagLibDurationProbe probe;

    {
        const int frames = 40;
        const fs::path path = root / "xing.mp3";
        writeMp3(path, frames, true);
        const auto seconds = probe.durationSeconds(seabass::pathToUtf8(path));
        assert(seconds.has_value());
        assert(std::abs(*seconds - expectedSeconds(frames)) < 0.01);
        std::cout << "case 1 (a real file's length, in seconds) OK\n";
    }

    // The answers this port owes for files a real stick carries: a
    // catalog row pointing at a file that is gone, and something that is
    // not audio at all. Both are "don't know", never a throw.
    {
        assert(!probe.durationSeconds(seabass::pathToUtf8(root / "not-here.mp3")).has_value());
        const fs::path text = root / "notes.txt";
        std::ofstream(text) << "not audio";
        assert(!probe.durationSeconds(seabass::pathToUtf8(text)).has_value());
        assert(!probe.durationSeconds("").has_value());
        std::cout << "case 2 (missing file, non-audio file and an empty path are all \"don't know\") OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
