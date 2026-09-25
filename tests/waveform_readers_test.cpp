// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The last two files on issue #6's list, and the two whose failure mode
// is the mildest: a waveform is drawn, never written back, so the worst
// a bug here does is draw the wrong picture. That is also why they had
// no test -- and why the contract they share is worth pinning, because
// it is the part a caller depends on:
//
//   both are best-effort. A track that is not there, an analysis file
//   that is not there, a database that is not there, an id that is not
//   a number at all: every one of those is an empty result, never an
//   exception. A missing waveform must never block playback, and both
//   readers are called from the GUI thread's display path.
//
// Plus the arithmetic, which nothing else can see: Engine's true
// three-band entries averaged per bucket and normalised to 0..1, and
// rekordbox's single byte unpacked into a 5-bit height and a 3-bit
// whiteness.

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include "infrastructure/work_counters.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
namespace engine = seabass::infrastructure::engine;
namespace rekordbox = seabass::infrastructure::rekordbox;
using seabass::domain::Track;
using seabass::domain::WaveformColumn;

namespace
{

bool inRange(const WaveformColumn &column)
{
    const double values[] = {column.low, column.mid, column.high};
    for (double value : values) {
        if (!(value >= 0.0 && value <= 1.0)) {
            return false;
        }
    }
    return true;
}

bool nearly(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

// The two ways a bucket could be reduced to one column. The reader
// averages; sampling the first of each bucket is the mistake worth
// being able to tell apart, so both are built here and the cases below
// assert that the data can in fact tell them apart before asserting
// which one the reader did.
std::vector<WaveformColumn> reduce(const std::vector<djinterop::waveform_entry> &entries, bool average)
{
    std::vector<WaveformColumn> columns;
    if (entries.empty()) {
        return columns;
    }
    const size_t bucket = std::max<size_t>(1, entries.size() / 400);
    for (size_t start = 0; start < entries.size(); start += bucket) {
        const size_t end = std::min(entries.size(), start + bucket);
        double low = 0.0, mid = 0.0, high = 0.0;
        if (average) {
            for (size_t i = start; i < end; ++i) {
                low += entries[i].low.value;
                mid += entries[i].mid.value;
                high += entries[i].high.value;
            }
            const auto count = static_cast<double>(end - start);
            low /= count;
            mid /= count;
            high /= count;
        } else {
            low = entries[start].low.value;
            mid = entries[start].mid.value;
            high = entries[start].high.value;
        }
        columns.push_back({low / 255.0, mid / 255.0, high / 255.0});
    }
    return columns;
}

bool same(const std::vector<WaveformColumn> &a, const std::vector<WaveformColumn> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!nearly(a[i].low, b[i].low) || !nearly(a[i].mid, b[i].mid) || !nearly(a[i].high, b[i].high)) {
            return false;
        }
    }
    return true;
}

// Writes a waveform onto the one track of a freshly created library and
// hands back the library path and that track's id as the reader wants
// it: a string, because that is what a catalog row carries.
struct EngineFixture
{
    fs::path libraryPath;
    std::string trackId;
    // What libdjinterop ACTUALLY stored, which is not what was handed
    // to it: Engine keeps a waveform against the track's own extents
    // and resamples to them on write. The thing under test here is the
    // downsampling this project does on the way out, so its input is
    // this, not the vector above.
    std::vector<djinterop::waveform_entry> stored;

    EngineFixture(const fs::path &root, const std::vector<djinterop::waveform_entry> &entries,
                  unsigned long long sampleCount = 44100ULL * 300)
    {
        std::error_code ec;
        fs::create_directories(root / "Contents", ec);
        libraryPath = engine::engineLibraryPath(root);
        Track track;
        track.sourceId = "1";
        track.title = "One";
        track.artist = "An Artist";
        track.filePath = seabass::pathToUtf8(root / "Contents" / "one.mp3");
        track.bpm = 128.0;
        track.durationSeconds = 300.0;
        const auto created = engine::EngineLibraryCreator::create(seabass::pathToUtf8(libraryPath), {track},
                                                                   engine::EngineSchemaGeneration::V2);
        if (!created.errorMessage.empty()) {
            std::cerr << "creator said: " << created.errorMessage << "\n";
        }
        assert(created.errorMessage.empty());
        assert(created.tracksCreated == 1);

        auto db = djinterop::engine::load_database(seabass::pathToUtf8(libraryPath));
        auto tracks = db.tracks();
        assert(tracks.size() == 1);
        trackId = std::to_string(tracks[0].id());
        if (!entries.empty()) {
            // Through a snapshot, with a sample count and rate set.
            // set_waveform() on its own writes nothing that reads back:
            // Engine stores a waveform against the track's extents, and
            // a track with no samples has none to store it against.
            auto snapshot = tracks[0].snapshot();
            snapshot.sample_rate = 44100.0;
            snapshot.sample_count = sampleCount;
            snapshot.waveform = entries;
            tracks[0].update(snapshot);
            stored = tracks[0].waveform();
            assert(!stored.empty() && "libdjinterop stored no waveform, so there is nothing to downsample");
        }
    }
};

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "waveform_readers_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);

    // ---- Engine: nothing there, in three different ways --------------
    //
    // None of these may throw, and none may come back with a waveform.
    {
        assert(engine::readWaveformPreview(seabass::pathToUtf8(scratch / "no-such-library"), "1").empty());
        assert(engine::readTrackAnalysis(seabass::pathToUtf8(scratch / "no-such-library"), "1").waveform.empty());

        EngineFixture fixture(scratch / "engine-empty", {});
        // A real library, but no waveform was ever written.
        assert(engine::readWaveformPreview(seabass::pathToUtf8(fixture.libraryPath), fixture.trackId).empty());
        // A track id nothing answers to.
        assert(engine::readWaveformPreview(seabass::pathToUtf8(fixture.libraryPath), "999999").empty());
        // And an id that is not a number: std::stoll throws on it, and
        // the display path must not.
        assert(engine::readWaveformPreview(seabass::pathToUtf8(fixture.libraryPath), "not-a-number").empty());
        assert(engine::readWaveformPreview(seabass::pathToUtf8(fixture.libraryPath), "").empty());
        std::cout << "case 1 (Engine: a missing library, track or waveform is empty, never a throw) OK\n";
    }

    // ---- Engine: the arithmetic --------------------------------------
    //
    // 0 and 255 are the ends of libdjinterop's range, so they pin both
    // the division by 255 and which band went where. A wrong band here
    // draws a track's bass as its treble.
    {
        std::vector<djinterop::waveform_entry> entries(800);
        for (auto &entry : entries) {
            entry.low = {255, 255};
            entry.mid = {0, 255};
            entry.high = {51, 255};  // 51/255 = 0.2 exactly
        }
        EngineFixture fixture(scratch / "engine-bands", entries);

        const auto waveform = engine::readWaveformPreview(seabass::pathToUtf8(fixture.libraryPath), fixture.trackId);
        assert(!waveform.empty());
        for (const auto &column : waveform) {
            assert(inRange(column));
            assert(nearly(column.low, 1.0));
            assert(nearly(column.mid, 0.0));
            assert(nearly(column.high, 0.2));
        }
        std::cout << "case 2 (Engine: each band averaged on its own and scaled to 0..1) OK\n";
    }

    // Averaging within a bucket, rather than taking the first of each.
    // Alternating values so the two differ, then asserted in that order:
    // first that this data CAN tell them apart, then which one happened.
    {
        std::vector<djinterop::waveform_entry> entries(4000);
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto value = static_cast<std::uint8_t>((i % 2 == 0) ? 0 : 200);
            entries[i].low = {value, 255};
            entries[i].mid = {value, 255};
            entries[i].high = {value, 255};
        }
        EngineFixture fixture(scratch / "engine-average", entries);

        const auto waveform = engine::readWaveformPreview(seabass::pathToUtf8(fixture.libraryPath), fixture.trackId);
        const auto averaged = reduce(fixture.stored, true);
        const auto sampled = reduce(fixture.stored, false);
        assert(!same(averaged, sampled) && "this waveform cannot tell averaging from sampling, so nothing below means anything");
        assert(same(waveform, averaged));
        assert(!same(waveform, sampled));
        std::cout << "case 3 (Engine: a bucket is averaged, not sampled) OK\n";
    }

    // What an Engine waveform's size actually is, which is not what the
    // header implies. libdjinterop hands back 1024 entries whatever was
    // written and whatever the track's length -- 800 in, 4000 in, 37 in,
    // two seconds long or five minutes: 1024 out, every time. The bucket
    // size is integer division, so 1024 / 400 is 2, and this always
    // produces 512 columns. "~400 points" in the header is the target
    // the arithmetic aims at, not a number that has ever come out of it.
    //
    // Pinned rather than changed, because 512 columns is a fine preview
    // and nothing downstream depends on the count. What is worth
    // noticing is a future libdjinterop returning something else.
    {
        std::vector<djinterop::waveform_entry> entries(37);
        for (auto &entry : entries) {
            entry.low = {255, 255};
            entry.mid = {128, 255};
            entry.high = {0, 255};
        }
        EngineFixture fixture(scratch / "engine-short", entries, 44100ULL * 2);

        assert(fixture.stored.size() == 1024);
        const auto waveform = engine::readWaveformPreview(seabass::pathToUtf8(fixture.libraryPath), fixture.trackId);
        assert(waveform.size() == 512);
        assert(same(waveform, reduce(fixture.stored, true)));
        for (const auto &column : waveform) {
            assert(inRange(column));
            assert(column.low > column.mid && column.mid > column.high);
        }
        std::cout << "case 4 (Engine: a stored waveform is 1024 entries, so a preview is 512 columns) OK\n";
    }

    // ---- rekordbox: nothing there ------------------------------------
    {
        const fs::path nothing = scratch / "no-such-stick";
        assert(rekordbox::readWaveformPreview(seabass::pathToUtf8(nothing), "1").empty());
        assert(rekordbox::readTrackAnalysis(seabass::pathToUtf8(nothing), "1").beats.empty());
        assert(rekordbox::readWaveformPreview(seabass::pathToUtf8(nothing), "not-a-number").empty());
        assert(rekordbox::readWaveformPreview("", "1").empty());
        std::cout << "case 5 (rekordbox: a missing stick or an unparseable id is empty, never a throw) OK\n";
    }

    // ---- rekordbox: a real export ------------------------------------
    //
    // The committed fixture, because a hand-built ANLZ file would only
    // prove this reads what this test wrote. The invariant asserted is
    // the one the unpacking guarantees: this tag carries a height and a
    // whiteness, not three independent bands, so low is the height and
    // mid and high are that height scaled down by the whiteness --
    // low >= mid >= high, always, for every column of every track.
    {
        const fs::path pioneer = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library"
            / "rekordbox";
        rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneer));
        const auto tracks = reader.readAll();
        assert(!tracks.empty());

        size_t withWaveform = 0;
        size_t withBeats = 0;
        size_t looked = 0;
        for (const auto &track : tracks) {
            if (looked++ >= 30) {
                break;
            }
            const auto analysis = rekordbox::readTrackAnalysis(seabass::pathToUtf8(pioneer), track.sourceId);
            if (!analysis.beats.empty()) {
                withBeats++;
            }
            if (analysis.waveform.empty()) {
                continue;
            }
            withWaveform++;
            for (const auto &column : analysis.waveform) {
                assert(inRange(column));
                assert(column.low >= column.mid - 1e-9);
                assert(column.mid >= column.high - 1e-9);
            }
        }
        // The fixture is anonymized, and anonymizing drops the detailed
        // colour waveform -- but not this tag. If that ever changes,
        // this says so rather than passing over an empty loop, which is
        // how a test like this goes green while reading nothing.
        assert(withWaveform > 0 && "no track in the fixture has a PWAV tag any more");
        std::cout << "case 6 (rekordbox: " << withWaveform << " of " << looked
                  << " fixture tracks unpack to a height and a whiteness, " << withBeats << " with a grid) OK\n";
    }

    // ---- rekordbox: an index answers what the database did ----------
    //
    // A list of previews hands the reader one AnlzPathIndex instead of
    // letting it parse export.pdb per track (the UI thread's cost: about
    // 20 ms a row on this fixture). The index must change nothing but
    // the cost: every track reads the same analysis either way, and with
    // the index the database is parsed by nobody but the index.
    {
        const fs::path pioneer = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library"
            / "rekordbox";
        const std::string root = seabass::pathToUtf8(pioneer);
        rekordbox::KaitaiRekordboxReader reader(root);
        const auto tracks = reader.readAll();
        const rekordbox::AnlzPathIndex index(root);

        auto &counters = seabass::infrastructure::WorkCounters::instance();
        size_t compared = 0;
        size_t withWaveform = 0;
        std::uint64_t parsesWithIndex = 0;
        for (const auto &track : tracks) {
            if (compared >= 30) {
                break;
            }
            const auto direct = rekordbox::readTrackAnalysis(root, track.sourceId);
            const auto before = counters.snapshot().trackDatabaseParses;
            const auto indexed = rekordbox::readTrackAnalysis(root, track.sourceId, nullptr, &index);
            parsesWithIndex += counters.snapshot().trackDatabaseParses - before;
            assert(same(direct.waveform, indexed.waveform));
            assert(direct.beats.size() == indexed.beats.size());
            withWaveform += direct.waveform.empty() ? 0 : 1;
            compared++;
        }
        assert(compared == 30);
        assert(withWaveform > 0 && "compared nothing but empty previews");
        assert(parsesWithIndex == 0 && "a reader handed an index parsed export.pdb anyway");
        // And an id the database does not hold is still empty, not a throw.
        assert(rekordbox::readTrackAnalysis(root, "999999", nullptr, &index).waveform.empty());
        std::cout << "case 7 (rekordbox: " << compared << " tracks read the same through an index, "
                  << withWaveform << " with a preview, no export.pdb parse) OK\n";
    }

    fs::remove_all(scratch, ec);
    std::cout << "all cases passed\n";
    return 0;
}
