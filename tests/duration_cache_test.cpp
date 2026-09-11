// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infrastructure/local/duration_cache.hpp"

#include "scratch_path.hpp"

using seabass::infrastructure::local::DurationCache;
namespace fs = std::filesystem;

namespace
{
std::string writeFile(const fs::path &p, const std::string &data)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << data;
    return p.string();
}
}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_duration_cache_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string audio = writeFile(root / "Contents" / "a" / "track.mp3", "not really audio, but a real file");

    // Case 1: an empty cache misses, then round-trips through save/load.
    {
        DurationCache cache(root.string());
        assert(!cache.lookup(audio).has_value());
        cache.store(audio, 266.376);
        assert(cache.dirty());
        assert(cache.save());
        assert(fs::exists(root / "Seabass" / "caches" / "durations.jsonl"));

        DurationCache reloaded(root.string());
        auto got = reloaded.lookup(audio);
        assert(got.has_value());
        assert(std::abs(*got - 266.376) < 0.001);
        std::cout << "case 1 (store -> save -> reload -> hit) OK\n";
    }

    // Case 2: a file whose size changed is stale, never a hit -- a wrong
    // length here would feed duplicate detection.
    {
        writeFile(root / "Contents" / "a" / "track.mp3", "not really audio, but a real file -- now longer");
        DurationCache reloaded(root.string());
        assert(!reloaded.lookup(audio).has_value());
        std::cout << "case 2 (size changed -> stale) OK\n";
    }

    // Case 3: paths are stored relative, so the same stick read at a
    // different mount point still hits. (Re-store first -- case 2 above
    // deliberately invalidated the entry by changing the file.)
    {
        DurationCache cache(root.string());
        cache.store(audio, 311.5);
        assert(cache.save());

        fs::path moved = seabass::testing::scratchRoot() / "seabass_duration_cache_test_moved";
        fs::remove_all(moved);
        fs::rename(root, moved);
        DurationCache movedCache(moved.string());
        auto got = movedCache.lookup((moved / "Contents" / "a" / "track.mp3").string());
        assert(got.has_value());
        assert(std::abs(*got - 311.5) < 0.001);
        std::cout << "case 3 (relative paths survive a different mount point) OK\n";
        fs::rename(moved, root);
    }

    // Case 4: a path outside the stick root is simply not this stick's
    // business -- no hit, and store() records nothing.
    {
        DurationCache cache(root.string());
        cache.store("/somewhere/else/other.mp3", 100.0);
        assert(!cache.dirty());
        assert(!cache.lookup("/somewhere/else/other.mp3").has_value());
        std::cout << "case 4 (paths outside the stick are ignored) OK\n";
    }

    // Case 5: a torn or hand-edited line costs that one entry, not the
    // whole cache.
    {
        std::ofstream out(root / "Seabass" / "caches" / "durations.jsonl", std::ios::app);
        out << "{not json at all\n";
        out.close();
        DurationCache cache(root.string());
        assert(cache.size() >= 1);
        std::cout << "case 5 (malformed line doesn't poison the cache) OK\n";
    }

    // Case 6: the cache must round-trip under a locale whose decimal
    // separator is a comma. Regression: the reader used std::stod, which
    // honours the global C locale -- and QCoreApplication sets that from
    // the environment, so on a nl_NL/de_DE machine the Qt-linked CLI
    // silently parsed "377.207000" as 377 and rejected every entry as
    // malformed, re-probing all 1213 files on every scan while this same
    // test passed in a non-Qt binary under the C locale.
    {
        const char *applied = std::setlocale(LC_ALL, "nl_NL.UTF-8");
        if (!applied) {
            applied = std::setlocale(LC_ALL, "nl_NL.utf8");
        }
        if (!applied) {
            std::cout << "case 6 SKIPPED (no comma-decimal locale installed)\n";
        } else {
            const std::string other = writeFile(root / "Contents" / "b" / "second.mp3", "another file");
            DurationCache cache(root.string());
            cache.store(other, 123.456);
            assert(cache.save());

            DurationCache reloaded(root.string());
            auto got = reloaded.lookup(other);
            assert(got.has_value());
            assert(std::abs(*got - 123.456) < 0.001);
            std::setlocale(LC_ALL, "C");
            std::cout << "case 6 (comma-decimal locale round-trips) OK\n";
        }
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
