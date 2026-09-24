// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include "infrastructure/local/silence_cache.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::domain::AudioContentSpan;
using seabass::infrastructure::local::CachedAudioContentProbe;
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

// Counts how often it was actually asked, which is the whole point of
// the thing it is wrapped in.
class CountingProbe : public seabass::domain::AudioContentProbe
{
public:
    int calls = 0;
    bool answer = true;
    AudioContentSpan span{266.376, 0.512, 1.880};

    std::optional<AudioContentSpan> measure(const std::string &) override
    {
        ++calls;
        if (!answer) {
            return std::nullopt;
        }
        return span;
    }
};

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_silence_cache_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string audio = writeFile(root / "Contents" / "a" / "track.mp3", "not really audio, but a real file");
    const fs::path cacheFile = root / "Seabass" / "caches" / "silence.jsonl";

    // Case 1: measured once, then answered from memory, then from disk
    // after a reload. The inner probe is a full decode in the real
    // thing, so "asked twice" is the failure that makes the feature
    // unusable on a real library rather than merely slow.
    {
        auto inner = std::make_unique<CountingProbe>();
        CountingProbe *counter = inner.get();
        CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::move(inner));

        auto first = probe.measure(audio);
        assert(first.has_value());
        assert(std::abs(first->leadingSilenceSeconds - 0.512) < 0.001);
        assert(std::abs(first->trailingSilenceSeconds - 1.880) < 0.001);
        assert(std::abs(first->contentSeconds() - (266.376 - 0.512 - 1.880)) < 0.001);
        assert(counter->calls == 1);

        assert(probe.measure(audio).has_value());
        assert(counter->calls == 1 && "the second ask came from memory");
        assert(probe.decodedCount() == 1);

        assert(probe.dirty());
        assert(probe.save());
        assert(fs::exists(cacheFile));
        std::cout << "case 1 (measured once, then remembered) OK\n";
    }

    // Case 2: and remembered across a restart, which is what makes the
    // second scan of a stick free.
    {
        auto inner = std::make_unique<CountingProbe>();
        CountingProbe *counter = inner.get();
        CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::move(inner));
        assert(probe.size() == 1);
        auto got = probe.measure(audio);
        assert(got.has_value());
        assert(std::abs(got->totalSeconds - 266.376) < 0.001);
        assert(counter->calls == 0 && "nothing was decoded");
        assert(probe.decodedCount() == 0);
        std::cout << "case 2 (reloaded from the stick, nothing decoded) OK\n";
    }

    // Case 3: a cache-only probe (no decoder in this build) still
    // answers from what a build that had one wrote. That is why null
    // inner is allowed rather than refused.
    {
        CachedAudioContentProbe probe(seabass::pathToUtf8(root), nullptr);
        assert(probe.measure(audio).has_value());
        assert(!probe.measure(seabass::pathToUtf8(root / "Contents" / "a" / "missing.mp3")).has_value());
        std::cout << "case 3 (no decoder still reads the cache) OK\n";
    }

    // Case 4: a file that changed is stale, never a hit. The same
    // strictness DurationCache applies, and for the same reason: this
    // feeds a caller that offers to delete things, so a silence figure
    // belonging to the previous contents of a filename would be worse
    // than no figure at all.
    {
        writeFile(root / "Contents" / "a" / "track.mp3", "re-ripped, and a different length now");
        auto inner = std::make_unique<CountingProbe>();
        CountingProbe *counter = inner.get();
        CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::move(inner));
        assert(probe.measure(audio).has_value());
        assert(counter->calls == 1 && "it was re-measured, not answered from the stale entry");
        std::cout << "case 4 (a changed file is re-measured) OK\n";
    }

    // Case 5: a failure is not written to the cache file -- an
    // unplugged stick or a backend that was not ready is about this
    // run, and writing it down would make one bad moment permanent for
    // that file -- but it IS remembered in memory for the rest of the
    // run. DuplicateTrackFinder asks once per *pair*, so a file in a
    // group of five copies is asked four times, and without this each
    // ask paid the full cost of failing again. Found by /code-review,
    // 2026-09-19.
    {
        fs::remove_all(root / "Seabass");
        auto inner = std::make_unique<CountingProbe>();
        CountingProbe *counter = inner.get();
        counter->answer = false;
        CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::move(inner));
        assert(!probe.measure(audio).has_value());
        assert(!probe.dirty() && "nothing to write down");
        assert(!probe.measure(audio).has_value());
        assert(!probe.measure(audio).has_value());
        assert(counter->calls == 1 && "asked once, then remembered for this run only");
        assert(probe.failedCount() == 1);
        // And the successes counter stays honest: nothing was compared.
        assert(probe.decodedCount() == 0 && "a file that gave no answer was not compared");

        // A new probe (the next scan) tries again rather than inheriting
        // the failure, which is the half that must NOT be remembered.
        auto retryInner = std::make_unique<CountingProbe>();
        CountingProbe *retryCounter = retryInner.get();
        CachedAudioContentProbe retry(seabass::pathToUtf8(root), std::move(retryInner));
        assert(retry.measure(audio).has_value());
        assert(retryCounter->calls == 1);
        assert(retry.decodedCount() == 1 && "a real answer is a real comparison");
        std::cout << "case 5 (a failure is remembered for this run, and no longer) OK\n";
    }

    // Case 6: a file outside the stick root is measured but not cached
    // -- nothing outside the stick belongs in this stick's cache, and
    // the entry's key would not resolve on another machine anyway.
    {
        const std::string outside = writeFile(seabass::testing::scratchRoot() / "seabass_silence_outside.mp3",
                                               "elsewhere entirely");
        auto inner = std::make_unique<CountingProbe>();
        CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::move(inner));
        assert(probe.measure(outside).has_value());
        assert(!probe.dirty());
        std::cout << "case 6 (a file off the stick is not cached) OK\n";
    }

    // Case 7: a torn or hand-edited line costs one entry, never the
    // cache, and a nonsense measurement is dropped rather than trusted.
    {
        fs::create_directories(cacheFile.parent_path());
        std::ofstream out(cacheFile, std::ios::binary);
        out << "{not json at all\n";
        out << R"({"lead":"0.100","mtime":"1","path":"Contents/a/bad.mp3","size":"1","total":"0.000","trail":"0.000"})"
            << "\n";
        out << R"({"lead":"-5.000","mtime":"1","path":"Contents/a/negative.mp3","size":"1","total":"10.000","trail":"0.000"})"
            << "\n";
        out << R"({"lead":"0.512","mtime":"1","path":"Contents/a/good.mp3","size":"1","total":"266.376","trail":"1.880"})"
            << "\n";
        out.close();

        CachedAudioContentProbe probe(seabass::pathToUtf8(root), nullptr);
        assert(probe.size() == 1 && "only the one usable line survived");
        std::cout << "case 7 (bad lines are dropped, the cache survives) OK\n";
    }

    // Case 8: the destructor saves, so a caller that forgets still
    // keeps what it paid for.
    {
        fs::remove_all(root / "Seabass");
        const std::string audio2 = writeFile(root / "Contents" / "b" / "two.mp3", "another file");
        {
            CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::make_unique<CountingProbe>());
            assert(probe.measure(audio2).has_value());
            assert(probe.dirty());
        }  // no save() call
        CachedAudioContentProbe reloaded(seabass::pathToUtf8(root), nullptr);
        assert(reloaded.size() == 1);
        assert(reloaded.measure(audio2).has_value());
        std::cout << "case 8 (the destructor saves) OK\n";
    }

    // Case 9: entries whose file is gone are dropped when the cache is
    // written, so silence.jsonl does not grow without bound on the
    // stick across re-rips and deletions, being re-parsed in full on
    // every scan. Found by /code-review, 2026-09-19.
    {
        fs::remove_all(root / "Seabass");
        const std::string keep = writeFile(root / "Contents" / "c" / "keep.mp3", "still here");
        const std::string gone = writeFile(root / "Contents" / "c" / "gone.mp3", "not for long");
        {
            CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::make_unique<CountingProbe>());
            assert(probe.measure(keep).has_value());
            assert(probe.measure(gone).has_value());
            assert(probe.save());
        }
        assert(CachedAudioContentProbe(seabass::pathToUtf8(root), nullptr).size() == 2);

        fs::remove(seabass::pathFromUtf8(gone));
        {
            // A save only rewrites when something was measured, so
            // something has to be.
            const std::string fresh = writeFile(root / "Contents" / "c" / "fresh.mp3", "new arrival");
            CachedAudioContentProbe probe(seabass::pathToUtf8(root), std::make_unique<CountingProbe>());
            assert(probe.measure(fresh).has_value());
            assert(probe.save());
        }
        CachedAudioContentProbe reloaded(seabass::pathToUtf8(root), nullptr);
        assert(reloaded.size() == 2 && "keep and fresh survived, gone was dropped");
        assert(reloaded.measure(keep).has_value());
        std::cout << "case 9 (entries for deleted files are pruned) OK\n";
    }

    // Case 10: a stick mounted at a drive root must not send the cache
    // somewhere else entirely. Stripping the trailing separator before
    // building the path turns "H:/" into "H:", and "H:" / "Seabass/..."
    // is drive-RELATIVE on Windows, resolved against the process's
    // current directory rather than the stick. Checked here through the
    // shape that reproduces on any platform: a root given with a
    // trailing slash must land in the same place as one without.
    // Found by /code-review, 2026-09-19.
    {
        fs::remove_all(root / "Seabass");
        const std::string withSlash = seabass::pathToUtf8(root) + "/";
        {
            CachedAudioContentProbe probe(withSlash, std::make_unique<CountingProbe>());
            assert(probe.measure(audio).has_value());
            assert(probe.save());
        }
        assert(fs::exists(cacheFile) && "the cache landed under the stick root, not beside the process");
        CachedAudioContentProbe reloaded(seabass::pathToUtf8(root), nullptr);
        assert(reloaded.size() >= 1 && "and a root without the slash reads the same file");
        assert(reloaded.measure(audio).has_value());
        std::cout << "case 10 (a trailing separator does not move the cache) OK\n";
    }

    fs::remove_all(root);
    std::cout << "All silence_cache tests passed.\n";
    return 0;
}
