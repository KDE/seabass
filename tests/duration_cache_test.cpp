// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/fill_missing_durations.hpp"
#include "infrastructure/local/duration_cache.hpp"

#include "infrastructure/paths/utf8_path.hpp"
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
    return seabass::pathToUtf8(p);
}
// Answers from a table and counts what it was asked.
class TableProbe : public seabass::application::TrackDurationProbe
{
public:
    std::map<std::string, double> answers;
    std::map<std::string, int> calls;
    std::optional<double> durationSeconds(const std::string &path) override
    {
        calls[path]++;
        const auto found = answers.find(path);
        return found == answers.end() ? std::nullopt : std::optional<double>(found->second);
    }
};
}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_duration_cache_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string audio = writeFile(root / "Contents" / "a" / "track.mp3", "not really audio, but a real file");

    // Case 1: an empty cache misses, then round-trips through save/load.
    {
        DurationCache cache(seabass::pathToUtf8(root));
        assert(!cache.lookup(audio).has_value());
        cache.store(audio, 266.376);
        assert(cache.dirty());
        assert(cache.save());
        assert(fs::exists(root / "Seabass" / "caches" / "durations.jsonl"));

        DurationCache reloaded(seabass::pathToUtf8(root));
        auto got = reloaded.lookup(audio);
        assert(got.has_value());
        assert(std::abs(*got - 266.376) < 0.001);
        std::cout << "case 1 (store -> save -> reload -> hit) OK\n";
    }

    // Case 2: a file whose size changed is stale, never a hit -- a wrong
    // length here would feed duplicate detection.
    {
        writeFile(root / "Contents" / "a" / "track.mp3", "not really audio, but a real file, now longer");
        DurationCache reloaded(seabass::pathToUtf8(root));
        assert(!reloaded.lookup(audio).has_value());
        std::cout << "case 2 (size changed -> stale) OK\n";
    }

    // Case 3: paths are stored relative, so the same stick read at a
    // different mount point still hits. (Re-store first -- case 2 above
    // deliberately invalidated the entry by changing the file.)
    {
        DurationCache cache(seabass::pathToUtf8(root));
        cache.store(audio, 311.5);
        assert(cache.save());

        fs::path moved = seabass::testing::scratchRoot() / "seabass_duration_cache_test_moved";
        fs::remove_all(moved);
        fs::rename(root, moved);
        DurationCache movedCache(seabass::pathToUtf8(moved));
        auto got = movedCache.lookup(seabass::pathToUtf8(moved / "Contents" / "a" / "track.mp3"));
        assert(got.has_value());
        assert(std::abs(*got - 311.5) < 0.001);
        std::cout << "case 3 (relative paths survive a different mount point) OK\n";
        fs::rename(moved, root);
    }

    // Case 4: a path outside the stick root is simply not this stick's
    // business -- no hit, and store() records nothing.
    {
        DurationCache cache(seabass::pathToUtf8(root));
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
        DurationCache cache(seabass::pathToUtf8(root));
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
            DurationCache cache(seabass::pathToUtf8(root));
            cache.store(other, 123.456);
            assert(cache.save());

            DurationCache reloaded(seabass::pathToUtf8(root));
            auto got = reloaded.lookup(other);
            assert(got.has_value());
            assert(std::abs(*got - 123.456) < 0.001);
            std::setlocale(LC_ALL, "C");
            std::cout << "case 6 (comma-decimal locale round-trips) OK\n";
        }
    }

    // Case 7: lookupUnverified() answers by path alone. It never looks at
    // the file, so it answers for a file that changed and for one that is
    // gone, where lookup() answers for neither; outside the root it has
    // nothing. A non-ASCII name goes through the UTF-8 path helpers.
    {
        const fs::path root7 = root / "case7";
        const std::string kept = writeFile(root7 / "Contents" / seabass::pathFromUtf8("Kügler.mp3"), "kept");
        const std::string grown = writeFile(root7 / "Contents" / "grown.mp3", "short");
        const std::string gone = writeFile(root7 / "Contents" / "gone.mp3", "soon gone");
        {
            DurationCache cache(seabass::pathToUtf8(root7));
            cache.store(kept, 101.0);
            cache.store(grown, 202.0);
            cache.store(gone, 303.0);
            assert(cache.save());
        }
        writeFile(root7 / "Contents" / "grown.mp3", "a good deal longer than it was");
        fs::remove(root7 / "Contents" / "gone.mp3");

        DurationCache cache(seabass::pathToUtf8(root7));
        assert(cache.lookup(kept) && *cache.lookup(kept) == 101.0);
        assert(cache.lookupUnverified(kept) && *cache.lookupUnverified(kept) == 101.0);
        assert(!cache.lookup(grown) && cache.lookupUnverified(grown) && *cache.lookupUnverified(grown) == 202.0);
        assert(!cache.lookup(gone) && cache.lookupUnverified(gone) && *cache.lookupUnverified(gone) == 303.0);
        assert(!cache.lookupUnverified("/somewhere/else/kept.mp3"));
        std::cout << "case 7 (lookupUnverified answers by path, without the file) OK\n";
    }

    // Case 8: the two halves of a staged read against the real cache. The
    // Tracks stage fills Unverified: every cached length is taken, even for
    // a file already deleted (which proves it was not looked at), and the
    // files are listed. The Full stage then verifies them: the unchanged
    // file stands, the changed one is probed again and its rows and the
    // cache get the new length, the deleted one becomes unknown, and a
    // row whose length the catalog gave is left alone.
    {
        using seabass::application::DurationFill;
        using seabass::domain::Track;
        const fs::path root8 = root / "case8";
        const std::string same = writeFile(root8 / "Contents" / "same.mp3", "same");
        const std::string edited = writeFile(root8 / "Contents" / "edited.mp3", "before");
        const std::string deleted = writeFile(root8 / "Contents" / "deleted.mp3", "deleted");
        {
            DurationCache cache(seabass::pathToUtf8(root8));
            cache.store(same, 100.0);
            cache.store(edited, 200.0);
            cache.store(deleted, 300.0);
            assert(cache.save());
        }
        fs::remove(root8 / "Contents" / "deleted.mp3");

        const auto row = [](const std::string &path, double seconds = 0.0) {
            Track track;
            track.filePath = path;
            track.durationSeconds = seconds;
            return track;
        };
        std::vector<Track> tracks{row(same), row(edited), row(edited), row(deleted), row(edited, 199.0)};
        TableProbe probe;
        probe.answers[edited] = 250.0;
        {
            DurationCache cache(seabass::pathToUtf8(root8));
            const auto filled = seabass::application::fillMissingDurations(tracks, probe, &cache,
                                                                           DurationFill::CachedByPathOnly);
            assert(filled.fromCache == 4 && filled.fromCacheUnverified == 4);
            assert(filled.alreadyKnown == 1 && filled.probed == 0 && filled.unreadable == 0);
            assert((filled.unverifiedPaths == std::vector<std::string>{same, edited, deleted}));
            assert(tracks[3].durationSeconds == 300.0 && "a deleted file's length is taken: nothing looked at it");
            assert(probe.calls.empty());
        }
        // The file is re-tagged between the two stages.
        writeFile(root8 / "Contents" / "edited.mp3", "after a re-tag, and longer");
        {
            DurationCache cache(seabass::pathToUtf8(root8));
            const auto verified = seabass::application::verifyCachedDurations(tracks, {same, edited, deleted}, probe, cache);
            assert(verified.confirmed == 1 && verified.reprobed == 1 && verified.unreadable == 1);
            assert(verified.tracksUpdated == 3);
            assert(tracks[0].durationSeconds == 100.0);
            assert(tracks[1].durationSeconds == 250.0 && tracks[2].durationSeconds == 250.0);
            assert(tracks[3].durationSeconds == 0.0 && "a file gone since it was cached reads unknown");
            assert(tracks[4].durationSeconds == 199.0 && "the catalog's own length is left alone");
            assert(probe.calls[edited] == 1 && probe.calls[deleted] == 1 && probe.calls.count(same) == 0);
            assert(cache.lookup(edited) && *cache.lookup(edited) == 250.0 && "the cache has the new length");
            assert(cache.save());
        }
        // A Verified fill afterwards agrees with what the Full stage made.
        std::vector<Track> fresh{row(same), row(edited), row(deleted)};
        TableProbe second;
        DurationCache cache(seabass::pathToUtf8(root8));
        const auto again = seabass::application::fillMissingDurations(fresh, second, &cache);
        assert(again.fromCacheUnverified == 0 && again.unverifiedPaths.empty());
        assert(fresh[0].durationSeconds == 100.0 && fresh[1].durationSeconds == 250.0 && fresh[2].durationSeconds == 0.0);
        std::cout << "case 8 (Unverified at Tracks, verified at Full: a changed file is caught) OK\n";
    }

    // Case 9: a file that changed and then gives no length when probed
    // again (re-encoded into something unreadable, or deleted) is
    // forgotten, not kept with its stale length. Kept, the next insertion
    // would take the stale length at its Tracks stage and turn it back
    // into 0 at its Full stage, every time.
    {
        using seabass::application::DurationFill;
        using seabass::domain::Track;
        const fs::path root9 = root / "case9";
        const std::string broken = writeFile(root9 / "Contents" / "broken.mp3", "audio");
        const std::string deleted = writeFile(root9 / "Contents" / "deleted.mp3", "audio");
        const std::string fine = writeFile(root9 / "Contents" / "fine.mp3", "audio");
        {
            DurationCache cache(seabass::pathToUtf8(root9));
            cache.store(broken, 400.0);
            cache.store(deleted, 500.0);
            cache.store(fine, 600.0);
            assert(cache.save());
        }
        const auto rows = [&] {
            std::vector<Track> tracks(3);
            tracks[0].filePath = broken;
            tracks[1].filePath = deleted;
            tracks[2].filePath = fine;
            return tracks;
        };
        // One insertion: Tracks takes the three by path, then the files
        // change, then Full checks them.
        std::vector<Track> tracks = rows();
        TableProbe probe;  // answers nothing for broken.mp3
        std::vector<std::string> taken;
        {
            DurationCache cache(seabass::pathToUtf8(root9));
            taken = seabass::application::fillMissingDurations(tracks, probe, &cache, DurationFill::CachedByPathOnly)
                        .unverifiedPaths;
            assert(taken.size() == 3 && tracks[0].durationSeconds == 400.0);
        }
        writeFile(root9 / "Contents" / "broken.mp3", "no longer audio, and a different size");
        fs::remove(root9 / "Contents" / "deleted.mp3");
        {
            DurationCache cache(seabass::pathToUtf8(root9));
            const auto verified = seabass::application::verifyCachedDurations(tracks, taken, probe, cache);
            assert(verified.unreadable == 2 && verified.confirmed == 1);
            assert(tracks[0].durationSeconds == 0.0 && tracks[1].durationSeconds == 0.0);
            assert(!cache.lookupUnverified(broken) && "a changed file that gives no length is forgotten");
            assert(!cache.lookupUnverified(deleted) && "and so is a deleted one");
            assert(cache.lookupUnverified(fine) && *cache.lookupUnverified(fine) == 600.0);
            assert(cache.save());
        }
        // The next insertion's Tracks stage: no stale length comes back,
        // so it reads what the Full stage read (0), and the good file
        // still reads from the cache.
        std::vector<Track> next = rows();
        DurationCache cache(seabass::pathToUtf8(root9));
        const auto again = seabass::application::fillMissingDurations(next, probe, &cache, DurationFill::CachedByPathOnly);
        assert(next[0].durationSeconds == 0.0 && "not the stale 400");
        assert(next[1].durationSeconds == 0.0 && "not the stale 500");
        assert(next[2].durationSeconds == 600.0);
        assert(again.deferred == 2 && again.fromCache == 1);
        std::cout << "case 9 (a changed file that no longer gives a length is forgotten, not kept stale) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
