// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The stick's record of pairs already lined up (local/alignment_cache):
// what it keeps, what it never keeps, when it stops trusting a line, and
// that a cache it cannot read or write never stops anything. The probe
// behind it counts what it is asked: a warm cache asks it nothing.

#include <cassert>
#include <functional>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "infrastructure/file_clock.hpp"
#include "infrastructure/local/alignment_cache.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::domain::AudioAlignmentProbe;
using seabass::infrastructure::local::CachedAudioAlignmentProbe;
using Answer = AudioAlignmentProbe::Answer;
using Kind = Answer::Kind;
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

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// Answers from a table, either way round, and counts every question: in
// the real thing each align() is two full decodes and each sameContent()
// reads both files.
class CountingProbe : public AudioAlignmentProbe
{
public:
    std::map<std::pair<std::string, std::string>, Answer> answers;
    std::set<std::pair<std::string, std::string>> same;
    int aligned = 0;
    int compared = 0;

    Answer align(const std::string &a, const std::string &b) override
    {
        ++aligned;
        if (same.count({a, b}) > 0 || same.count({b, a}) > 0) {
            return Answer::linedUp(0.0);
        }
        if (const auto it = answers.find({a, b}); it != answers.end()) {
            return it->second;
        }
        if (const auto it = answers.find({b, a}); it != answers.end()) {
            Answer reversed = it->second;
            reversed.shiftMs = -reversed.shiftMs;
            std::swap(reversed.unreadableA, reversed.unreadableB);
            return reversed;
        }
        return Answer::noOpinion();
    }

    bool sameContent(const std::string &a, const std::string &b) override
    {
        ++compared;
        return same.count({a, b}) > 0 || same.count({b, a}) > 0;
    }
};

struct Probe
{
    CountingProbe *counter = nullptr;
    std::unique_ptr<CachedAudioAlignmentProbe> cached;
};

Probe makeProbe(const fs::path &root, const std::function<void(CountingProbe &)> &fill)
{
    auto inner = std::make_unique<CountingProbe>();
    fill(*inner);
    Probe probe;
    probe.counter = inner.get();
    probe.cached = std::make_unique<CachedAudioAlignmentProbe>(seabass::pathToUtf8(root), std::move(inner), true,
                                                                "test decoder");
    return probe;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

void touchLater(const std::string &path)
{
    const fs::path p = seabass::pathFromUtf8(path);
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(10));
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_alignment_cache_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path cacheFile = root / "Seabass" / "caches" / "alignment.jsonl";
    const std::string a = writeFile(root / "Contents" / "a" / "Song.mp3", "copy a");
    const std::string b = writeFile(root / "Contents" / "b" / "Song.mp3", "copy b, longer");
    const std::string c = writeFile(root / "Contents" / "c" / "Song.flac", "copy c, longer still");
    const std::string d = writeFile(root / "Contents" / "d" / "Song.mp3", "copy d");
    const std::string e = writeFile(root / "Contents" / "e" / "Song.mp3", "copy e");
    const std::string twin = writeFile(root / "Contents" / "f" / "Song.mp3", "copy a");

    auto table = [&](CountingProbe &probe) {
        probe.answers[{a, b}] = Answer::linedUp(21.3);
        probe.answers[{a, c}] = Answer::notLinedUp();
        probe.answers[{a, d}] = Answer::noOpinion(false, true);
        probe.answers[{a, e}] = Answer::noOpinion();
        probe.same.insert({a, twin});
    };

    // Cold: everything asked of the decoder once, and kept.
    {
        auto probe = makeProbe(root, table);
        const auto shift = probe.cached->align(a, b);
        assert(shift.kind == Kind::LinedUp && near(shift.shiftMs, 21.3));
        assert(probe.cached->align(a, c).kind == Kind::NotLinedUp);
        assert(probe.cached->align(a, d).kind == Kind::NoOpinion && probe.cached->align(a, d).unreadableB
               && "no opinion passes through as it came");
        assert(probe.cached->align(a, e).kind == Kind::NoOpinion);
        assert(probe.cached->sameContent(a, twin));
        assert(probe.counter->aligned == 5);
        assert(probe.cached->align(a, b).kind == Kind::LinedUp && probe.counter->aligned == 5
               && "asked again in the same scan, from memory");
        assert(probe.cached->save());
        assert(fs::exists(cacheFile));
    }

    // Warm: nothing decoded, nothing compared, every kept answer the
    // same, either way round. No opinion was never kept, so it is asked.
    {
        auto probe = makeProbe(root, table);
        assert(probe.cached->size() == 3 && "a shift, a pair that does not line up, a pair of the same bytes");
        assert(probe.cached->answersAtOnce(a, b) && probe.cached->answersAtOnce(b, a));
        assert(!probe.cached->answersAtOnce(a, d) && !probe.cached->answersAtOnce(a, e));
        auto shift = probe.cached->align(a, b);
        assert(shift.kind == Kind::LinedUp && near(shift.shiftMs, 21.3) && "a hit");
        shift = probe.cached->align(b, a);
        assert(shift.kind == Kind::LinedUp && near(shift.shiftMs, -21.3) && "the other way round is negated");
        assert(probe.cached->align(c, a).kind == Kind::NotLinedUp && "does not line up, kept");
        assert(probe.cached->sameContent(twin, a) && "the same bytes, kept, either way round");
        assert(probe.cached->align(twin, a).kind == Kind::LinedUp);
        assert(probe.counter->aligned == 0 && probe.counter->compared == 0 && "a warm cache asks nothing");
        assert(probe.cached->cachedAnswerCount() == 4);
        assert(probe.cached->align(a, d).kind == Kind::NoOpinion && probe.counter->aligned == 1
               && "an unreadable file was never written down");
        assert(probe.cached->align(a, e).kind == Kind::NoOpinion && probe.counter->aligned == 2
               && "no decoder, a timeout or a cancel was never written down");
        assert(probe.cached->linedUpCount() == 3 && probe.cached->unknownCount() == 3);
    }
    {
        const std::string text = readFile(cacheFile);
        assert(text.find("\"result\":\"shift\"") != std::string::npos);
        assert(text.find("\"result\":\"no\"") != std::string::npos);
        assert(text.find("\"result\":\"same\"") != std::string::npos);
        assert(text.find("Contents/d/") == std::string::npos && text.find("Contents/e/") == std::string::npos
               && "no line for a pair with no opinion");
        assert(text.find(seabass::pathToUtf8(root)) == std::string::npos && "paths are relative to the stick");
    }

    // Stale: either file's size or mtime moved, and the pair is measured
    // again (and the new answer kept).
    {
        writeFile(seabass::pathFromUtf8(b), "copy b, re-encoded at another size");
        auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{a, b}] = Answer::linedUp(-4.0); });
        const auto shift = probe.cached->align(b, a);
        assert(shift.kind == Kind::LinedUp && near(shift.shiftMs, 4.0) && probe.counter->aligned == 1
               && "the second file's size changed: asked again");
        assert(probe.cached->align(a, c).kind == Kind::NotLinedUp && probe.counter->aligned == 1);
        assert(probe.cached->save());
    }
    {
        auto probe = makeProbe(root, [](CountingProbe &) {});
        assert(probe.cached->align(a, b).kind == Kind::LinedUp && near(probe.cached->align(a, b).shiftMs, -4.0)
               && probe.counter->aligned == 0 && "the re-measured pair was kept");
        touchLater(c);
        assert(probe.cached->align(a, c).kind == Kind::NoOpinion && probe.counter->aligned == 1
               && "the second file's mtime changed: asked again");
        touchLater(a);
        assert(probe.cached->align(a, twin).kind == Kind::NoOpinion && probe.counter->aligned == 2
               && "the first file's mtime changed: asked again");
        assert(!probe.cached->answersAtOnce(b, a) && "and every pair of it");
    }

    // A path off the stick is not kept: nothing outside it belongs here.
    {
        const fs::path elsewhere = seabass::testing::scratchRoot() / "seabass_alignment_cache_test_elsewhere";
        fs::remove_all(elsewhere);
        const std::string outside = writeFile(elsewhere / "Song.mp3", "elsewhere");
        auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{a, outside}] = Answer::linedUp(1.0); });
        assert(probe.cached->align(a, outside).kind == Kind::LinedUp);
        probe.cached->save();
        assert(readFile(cacheFile).find("elsewhere") == std::string::npos);
        fs::remove_all(elsewhere);
    }

    // A file gone from the stick takes its pairs with it at the next save.
    {
        auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{d, e}] = Answer::linedUp(3.0); });
        assert(probe.cached->align(d, e).kind == Kind::LinedUp);
        fs::remove(seabass::pathFromUtf8(e));
        assert(probe.cached->save());
        assert(readFile(cacheFile).find("Contents/e/") == std::string::npos && "a pair with a file gone is dropped");
        writeFile(seabass::pathFromUtf8(e), "copy e");
    }

    // Another version of the method: every line of it ignored, and gone
    // from the file at the next save.
    {
        std::string text = readFile(cacheFile);
        const std::string current = "\"method\":\"" + std::to_string(CachedAudioAlignmentProbe::MethodVersion) + "\"";
        const std::string older = "\"method\":\"" + std::to_string(CachedAudioAlignmentProbe::MethodVersion - 1) + "\"";
        for (size_t at = text.find(current); at != std::string::npos; at = text.find(current)) {
            text.replace(at, current.size(), older);
        }
        writeFile(cacheFile, text);
        auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{a, b}] = Answer::linedUp(-4.0); });
        assert(probe.cached->size() == 0 && "lines of another method are not trusted");
        assert(probe.cached->align(a, b).kind == Kind::LinedUp && probe.counter->aligned == 1);
        assert(probe.cached->save());
        assert(readFile(cacheFile).find(older) == std::string::npos && "and are dropped");
    }

    // A damaged cache is an empty one: garbage lines cost their pair, a
    // good line beside them still counts.
    {
        const std::string good = readFile(cacheFile);
        writeFile(cacheFile, "not json at all\n{\"a\":\"Contents/a/Song.mp3\",\"b\":\n\x01\x02\x03\n" + good
                                 + "{\"a\":\"z\",\"b\":\"a\",\"method\":\"" + std::to_string(CachedAudioAlignmentProbe::MethodVersion)
                                 + "\",\"result\":\"shift\",\"shift\":\"x\"}\n");
        auto probe = makeProbe(root, [](CountingProbe &) {});
        assert(probe.cached->size() == 1 && "only the good line");
        assert(probe.cached->align(a, b).kind == Kind::LinedUp && probe.counter->aligned == 0);
    }

    // A folder where the file should be: nothing read, every pair asked,
    // the save refused, and no exception anywhere.
    {
        fs::remove(cacheFile);
        fs::create_directories(cacheFile / "in the way");
        auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{a, b}] = Answer::linedUp(-4.0); });
        assert(probe.cached->size() == 0);
        assert(probe.cached->align(a, b).kind == Kind::LinedUp && probe.counter->aligned == 1);
        assert(!probe.cached->save() && "a save that cannot happen says so");
        probe.cached.reset();  // the destructor's save fails quietly
        assert(fs::is_directory(cacheFile));
        fs::remove_all(cacheFile);
    }

    // No stick root: every pair is measured, nothing is written.
    {
        auto inner = std::make_unique<CountingProbe>();
        inner->answers[{a, b}] = Answer::linedUp(1.0);
        CountingProbe *counter = inner.get();
        CachedAudioAlignmentProbe probe({}, std::move(inner), true, "test decoder");
        assert(probe.align(a, b).kind == Kind::LinedUp && probe.align(a, b).kind == Kind::LinedUp);
        assert(counter->aligned == 2 && probe.save() && !fs::exists(cacheFile));
    }

    // Without a decoder, only pairs of the same bytes are answered from the
    // stick: a shift is on the timeline of the decoder that measured it.
    {
        auto probe = makeProbe(root, [&](CountingProbe &p) {
            p.answers[{a, b}] = Answer::linedUp(-4.0);
            p.same.insert({a, twin});
        });
        probe.cached->align(a, b);
        assert(probe.cached->sameContent(a, twin));
        probe.cached->save();
        CachedAudioAlignmentProbe cacheOnly(seabass::pathToUtf8(root), nullptr, true, "none");
        assert(!cacheOnly.hasDecoder());
        assert(cacheOnly.sameContent(twin, a) && cacheOnly.align(a, twin).kind == Kind::LinedUp
               && "the same bytes hold for every decoder");
        assert(cacheOnly.align(a, b).kind == Kind::NoOpinion && "another decoder's shift is not trusted");
        assert(cacheOnly.align(a, e).kind == Kind::NoOpinion);
    }

    // Another decoder may start a file elsewhere: what one wrote, another
    // does not trust, and keeps for it.
    {
        auto inner = std::make_unique<CountingProbe>();
        inner->answers[{a, b}] = Answer::linedUp(-3.0);
        CountingProbe *counter = inner.get();
        {
            CachedAudioAlignmentProbe other(seabass::pathToUtf8(root), std::move(inner), true, "another decoder");
            assert(other.align(a, b).kind == Kind::LinedUp && counter->aligned == 1 && "not trusted from another");
            assert(other.save());
        }
        CachedAudioAlignmentProbe again(seabass::pathToUtf8(root), nullptr, true, "test decoder");
        const auto kept = again.align(a, b);
        assert(kept.kind == Kind::LinedUp && near(kept.shiftMs, -4.0) && "and the first decoder's line is still there");
        assert(readFile(cacheFile).find("another decoder") != std::string::npos);
    }

    // A line whose file changed is dropped at the next save, whichever
    // decoder wrote it: nobody will trust it again.
    {
        {
            auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{d, e}] = Answer::linedUp(3.0); });
            probe.cached->align(d, e);
            assert(probe.cached->save() && readFile(cacheFile).find("Contents/e/") != std::string::npos);
        }
        writeFile(seabass::pathFromUtf8(e), "copy e, re-encoded");
        auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{a, c}] = Answer::linedUp(1.0); });
        probe.cached->align(a, c);
        assert(probe.cached->save());
        assert(readFile(cacheFile).find("Contents/e/") == std::string::npos && "the stale line is gone");
    }

    // Another machine's lines are its own to judge: one whose times do not
    // match here (another clock, another time zone on FAT) is kept by a
    // save here. And a "bytes" line may only say the same bytes.
    {
        const std::string foreign =
            "{\"a\":\"Contents/a/Song.mp3\",\"amtime\":\"1\",\"asize\":\"6\",\"b\":\"Contents/c/Song.flac\","
            "\"bmtime\":\"1\",\"bsize\":\"21\",\"decoder\":\"elsewhere\",\"method\":\""
            + std::to_string(CachedAudioAlignmentProbe::MethodVersion) + "\",\"result\":\"shift\",\"shift\":\"5.0\"}\n";
        // With the real files' sizes and times, so only its saying a shift
        // keeps it out.
        auto state = [](const std::string &path) {
            const fs::path p = seabass::pathFromUtf8(path);
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                seabass::infrastructure::toSystemClock(fs::last_write_time(p)).time_since_epoch());
            return std::pair{std::to_string(fs::file_size(p)), std::to_string(seconds.count())};
        };
        const auto [sizeA, timeA] = state(a);
        const auto [sizeD, timeD] = state(d);
        const std::string forged = "{\"a\":\"Contents/a/Song.mp3\",\"amtime\":\"" + timeA + "\",\"asize\":\"" + sizeA
            + "\",\"b\":\"Contents/d/Song.mp3\",\"bmtime\":\"" + timeD + "\",\"bsize\":\"" + sizeD
            + "\",\"decoder\":\"bytes\",\"method\":\"" + std::to_string(CachedAudioAlignmentProbe::MethodVersion)
            + "\",\"result\":\"shift\",\"shift\":\"5.0\"}\n";
        writeFile(cacheFile, readFile(cacheFile) + foreign + forged);
        {
            CachedAudioAlignmentProbe reader(seabass::pathToUtf8(root), nullptr, true, "none");
            assert(reader.align(a, d).kind == Kind::NoOpinion && "a bytes line saying a shift is not trusted");
        }
        auto probe = makeProbe(root, [&](CountingProbe &p) { p.answers[{b, c}] = Answer::linedUp(2.0); });
        probe.cached->align(b, c);
        assert(probe.cached->save());
        const std::string text = readFile(cacheFile);
        assert(text.find("\"decoder\":\"elsewhere\"") != std::string::npos && "another machine's line is kept");
        assert(text.find("\"decoder\":\"bytes\",\"method\":\"" + std::to_string(CachedAudioAlignmentProbe::MethodVersion)
                         + "\",\"result\":\"shift\"")
                   == std::string::npos
               && "a bytes line saying a shift is not read, and so not written back");
    }

    // Read-only (the command line's scan): what is on the stick is read,
    // what is measured is kept in memory, and nothing is written.
    {
        const std::string before = readFile(cacheFile);
        auto inner = std::make_unique<CountingProbe>();
        inner->answers[{a, e}] = Answer::linedUp(9.0);
        CountingProbe *counter = inner.get();
        {
            CachedAudioAlignmentProbe readOnly(seabass::pathToUtf8(root), std::move(inner), true, "test decoder",
                                               CachedAudioAlignmentProbe::Writes::ReadOnly);
            assert(readOnly.size() > 0 && "the stick's cache is read");
            assert(readOnly.align(a, e).kind == Kind::LinedUp && readOnly.align(e, a).kind == Kind::LinedUp
                   && counter->aligned == 1 && "measured once, kept in memory");
            assert(readOnly.save() && "a save that writes nothing is no failure");
        }
        assert(readFile(cacheFile) == before && "and nothing was written, by save() or the destructor");
    }

    fs::remove_all(root);
    std::cout << "alignment_cache_test: all assertions passed\n";
    return 0;
}
