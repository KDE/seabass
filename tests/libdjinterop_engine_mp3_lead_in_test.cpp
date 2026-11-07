// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// #50 at the Engine boundary: a cue on an MP3 is stored on Engine's
// timeline and read on rekordbox's. Through a real scratch Engine library
// and the committed lead-in fixtures (tests/fixtures/mp3-lead-in), with
// the raw sample offsets checked through libdjinterop itself:
//
//   - a cue written at rekordbox time T on a LAME (class D) 44.1 kHz file
//     is stored at (T - 51.18 ms) * 44.1 samples, and reads back as T;
//     both ends of a loop and the main cue too;
//   - Create Engine Library stores the same;
//   - a WAV is not moved;
//   - with the setting off, today's behaviour: stored as T, read as T;
//   - fillCues() moves a row by the difference, never twice;
//   - a writer on a scratch copy of the database (FormatWriteSession's)
//     reads the lead-in from the real library's audio, not the copy's.
//   - so does a reader of a copy given the real library as its audio
//     root (a stick backup read beside the live stick).
//
// Usage: libdjinterop_engine_mp3_lead_in_test <fixture dir>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

#include <djinterop/djinterop.hpp>

#include "application/use_cases/scan_library.hpp"
#include "domain/matching_policy.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
using seabass::domain::CuePoint;
using seabass::domain::MatchingPolicy;
using seabass::domain::Track;
namespace fs = std::filesystem;

namespace
{

fs::path g_fixtures;

// LAME's 576-sample delay, the 529 FFmpeg adds, and the Info frame
// rekordbox plays: 2257 samples at 44.1 kHz.
constexpr double LameShiftMs = 2257 * 1000.0 / 44100;

bool near(double a, double b, double tolerance = 1e-6)
{
    return std::abs(a - b) < tolerance;
}

// A fresh library holding one track whose file is a copy of `fixture`,
// named song.<extension of the fixture>; `cues` written by Create Engine
// Library itself.
struct Library
{
    fs::path root;
    std::string file;
    EngineLibraryCreationResult created;
};

Library makeLibrary(const std::string &caseName, const std::string &fixture, std::vector<CuePoint> cues = {})
{
    Library lib;
    lib.root = seabass::testing::scratchRoot() / "seabass_engine_mp3_lead_in_test" / seabass::pathFromUtf8(caseName)
        / "Engine Library";
    fs::remove_all(lib.root.parent_path());
    fs::create_directories(lib.root);
    const fs::path source = g_fixtures / seabass::pathFromUtf8(fixture);
    const fs::path file = lib.root / seabass::pathFromUtf8("song" + seabass::pathToUtf8(source.extension()));
    fs::copy_file(source, file);
    lib.file = seabass::pathToUtf8(file);

    Track t;
    t.sourceId = "r0";
    t.title = "Lead In";
    t.artist = "Artist";
    t.filePath = lib.file;
    t.bpm = 120.0;
    t.durationSeconds = 300.0;
    t.cues = std::move(cues);
    lib.created = EngineLibraryCreator::create(seabass::pathToUtf8(lib.root), {t}, EngineSchemaGeneration::V2);
    assert(lib.created.errorMessage.empty() && lib.created.tracksCreated == 1);
    return lib;
}

Track readOne(const Library &lib, LibdjinteropEngineReader *withReader = nullptr)
{
    LibdjinteropEngineReader own(seabass::pathToUtf8(lib.root));
    LibdjinteropEngineReader &reader = withReader ? *withReader : own;
    auto tracks = seabass::application::ScanLibrary(reader).execute();
    assert(tracks.size() == 1);
    return tracks.front();
}

const CuePoint *find(const std::vector<CuePoint> &cues, CuePoint::Kind kind, int pad, bool loop)
{
    for (const auto &c : cues) {
        if (c.kind == kind && c.hotCueNumber == pad && c.isLoop == loop) {
            return &c;
        }
    }
    return nullptr;
}

// What Engine stores, straight from libdjinterop.
struct Raw
{
    double hotCue = -1;
    double loopStart = -1;
    double loopEnd = -1;
    double mainCue = -1;
};

Raw raw(const Library &lib, const std::string &id)
{
    auto db = djinterop::engine::load_database(seabass::pathToUtf8(lib.root));
    auto track = db.track_by_id(std::stoll(id));
    assert(track);
    Raw r;
    const auto hot = track->hot_cues();
    if (hot.size() > 0 && hot[0]) {
        r.hotCue = hot[0]->sample_offset;
    }
    const auto loops = track->loops();
    if (loops.size() > 1 && loops[1]) {
        r.loopStart = loops[1]->start_sample_offset;
        r.loopEnd = loops[1]->end_sample_offset;
    }
    if (const auto main = track->main_cue()) {
        r.mainCue = *main;
    }
    return r;
}

std::vector<CuePoint> theCues()
{
    CuePoint hot{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"};
    CuePoint loop{CuePoint::Kind::Hot, 2, 10000.0, "#00ff00", "loop"};
    loop.isLoop = true;
    loop.loopEndMs = 14000.0;
    CuePoint memory;
    memory.kind = CuePoint::Kind::Memory;
    memory.positionMs = 8000.0;
    return {hot, loop, memory};
}

void assertReadsBack(const Track &t, double shiftMs)
{
    const CuePoint *hot = find(t.cues, CuePoint::Kind::Hot, 1, false);
    const CuePoint *loop = find(t.cues, CuePoint::Kind::Hot, 2, true);
    const CuePoint *memory = find(t.cues, CuePoint::Kind::Memory, 0, false);
    assert(hot && loop && memory);
    assert(near(hot->positionMs, 5000.0 + shiftMs));
    assert(near(loop->positionMs, 10000.0 + shiftMs));
    assert(near(loop->loopEndMs, 14000.0 + shiftMs));
    assert(near(memory->positionMs, 8000.0 + shiftMs));
}

void assertStored(const Raw &r, double shiftMs)
{
    assert(near(r.hotCue, (5000.0 - shiftMs) * 44.1, 1e-3));
    assert(near(r.loopStart, (10000.0 - shiftMs) * 44.1, 1e-3));
    assert(near(r.loopEnd, (14000.0 - shiftMs) * 44.1, 1e-3));
    assert(near(r.mainCue, (8000.0 - shiftMs) * 44.1, 1e-3));
}

}  // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    g_fixtures = seabass::pathFromUtf8(argv[1]);
    MatchingPolicy::reset();
    assert(MatchingPolicy::correctMp3CueTiming());

    // Case 1: the cue writer on a LAME file. 5000 ms on rekordbox's
    // timeline is 5000 - 51.18 ms on Engine's: 220500 - 2257 samples.
    {
        const Library lib = makeLibrary("writer-lame", "lame-cbr-44100.mp3");
        const Track before = readOne(lib);
        LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(lib.root));
        writer.writeHotCues(before.sourceId, theCues());

        const Raw r = raw(lib, before.sourceId);
        assert(near(r.hotCue, 220500.0 - 2257.0, 1e-3));
        assertStored(r, LameShiftMs);

        const Track after = readOne(lib);
        assertReadsBack(after, 0.0);
        assert(near(after.cueTimelineShiftMs, LameShiftMs));
        assert(near(after.cueTimelineShiftMs, 51.18, 0.01));

        // Written back as read, nothing moves: read, write, read again.
        LibdjinteropEngineCueWriter again(seabass::pathToUtf8(lib.root));
        again.writeHotCues(after.sourceId, after.cues);
        assertStored(raw(lib, after.sourceId), LameShiftMs);
        assertReadsBack(readOne(lib), 0.0);
        std::cout << "case 1 (writer and reader on a LAME MP3) OK\n";
    }

    // Case 2: Create Engine Library stores the same offsets.
    {
        const Library lib = makeLibrary("creator-lame", "lame-cbr-44100.mp3", theCues());
        const Track t = readOne(lib);
        assertStored(raw(lib, t.sourceId), LameShiftMs);
        assertReadsBack(t, 0.0);
        std::cout << "case 2 (Create Engine Library on a LAME MP3) OK\n";
    }

    // Case 3: a WAV is never moved.
    {
        const Library lib = makeLibrary("writer-wav", "silence.wav");
        const Track before = readOne(lib);
        LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(lib.root));
        writer.writeHotCues(before.sourceId, theCues());
        assertStored(raw(lib, before.sourceId), 0.0);
        const Track after = readOne(lib);
        assertReadsBack(after, 0.0);
        assert(after.cueTimelineShiftMs == 0.0);
        std::cout << "case 3 (a WAV is not moved) OK\n";
    }

    // Case 4: the setting off is today's behaviour: stored as given,
    // read as stored. The same row read with it on again is moved.
    {
        MatchingPolicy::setCorrectMp3CueTiming(false);
        const Library lib = makeLibrary("setting-off", "lame-cbr-44100.mp3", theCues());
        const Track t = readOne(lib);
        assertStored(raw(lib, t.sourceId), 0.0);
        assertReadsBack(t, 0.0);
        assert(t.cueTimelineShiftMs == 0.0);

        LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(lib.root));
        writer.writeHotCues(t.sourceId, theCues());
        assertStored(raw(lib, t.sourceId), 0.0);

        MatchingPolicy::setCorrectMp3CueTiming(true);
        assertReadsBack(readOne(lib), LameShiftMs);
        std::cout << "case 4 (setting off: positions as stored) OK\n";
    }

    // Case 5: a source that cannot say (a Tracks stage the cache knows
    // nothing for) gives no shift; fillCues() with a source that can
    // moves the row by the whole shift, and a second fillCues() by
    // nothing.
    {
        const Library lib = makeLibrary("fill-cues", "lame-cbr-44100.mp3", theCues());
        LibdjinteropEngineReader blind(seabass::pathToUtf8(lib.root));
        blind.setLeadInSource([](const std::string &) { return std::optional<seabass::domain::Mp3LeadIn>(); });
        std::vector<Track> tracks = blind.readTracks();
        assert(tracks.size() == 1);
        assertReadsBack(tracks.front(), -LameShiftMs);
        assert(tracks.front().cueTimelineShiftMs == 0.0);

        LibdjinteropEngineReader seeing(seabass::pathToUtf8(lib.root));
        int asked = 0;
        seeing.setLeadInSource([&asked](const std::string &file) {
            ++asked;
            return seabass::infrastructure::audio::readMp3LeadIn(file);
        });
        seeing.fillCues(tracks);
        assert(asked == 1);
        assertReadsBack(tracks.front(), 0.0);
        assert(near(tracks.front().cueTimelineShiftMs, LameShiftMs));
        seeing.fillCues(tracks);
        assertReadsBack(tracks.front(), 0.0);
        std::cout << "case 5 (fillCues moves by the difference) OK\n";
    }

    // Case 6: the FFmpeg (Lavc) file: 1105 samples, 25.06 ms.
    {
        const Library lib = makeLibrary("lavc", "ffmpeg-lavc-44100.mp3", theCues());
        const Track t = readOne(lib);
        assertStored(raw(lib, t.sourceId), 1105 * 1000.0 / 44100);
        assertReadsBack(t, 0.0);
        std::cout << "case 6 (an FFmpeg Lavc MP3: 25 ms) OK\n";
    }

    // Case 7: the database behind a scratch copy, as FormatWriteSession
    // puts it for a large save: the copy holds Database2 and no audio, so
    // the row's relative path names no file there. The writer is given
    // the real library as its audio root and still shifts the cues.
    {
        const Library lib = makeLibrary("scratch-copy", "lame-cbr-44100.mp3");
        const Track before = readOne(lib);
        Library scratch;
        scratch.root = lib.root.parent_path() / "scratch" / "Engine Library";
        fs::create_directories(scratch.root);
        fs::copy(lib.root / "Database2", scratch.root / "Database2", fs::copy_options::recursive);
        assert(!fs::exists(scratch.root / "song.mp3"));

        LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(scratch.root));
        writer.setAudioRoot(seabass::pathToUtf8(lib.root));
        writer.writeHotCues(before.sourceId, theCues());
        assertStored(raw(scratch, before.sourceId), LameShiftMs);
        std::cout << "case 7 (scratch copy, lead-in from the real library) OK\n";
    }

    // Case 8: a cue inside the lead-in, 30 ms into a LAME file whose
    // shift is 51.18 ms. The hot cue is clamped to Engine's sample 0; the
    // memory cue cannot be (a main cue at 0 is none to Engine), so it is
    // left out, and counted, not lost quietly.
    {
        const Library lib = makeLibrary("inside-lead-in", "lame-cbr-44100.mp3");
        const Track before = readOne(lib);
        CuePoint hot{CuePoint::Kind::Hot, 1, 30.0, "#ff0000", "early"};
        CuePoint memory;
        memory.kind = CuePoint::Kind::Memory;
        memory.positionMs = 30.0;
        LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(lib.root));
        writer.writeHotCues(before.sourceId, {hot, memory});
        const Raw r = raw(lib, before.sourceId);
        assert(r.hotCue == 0.0);
        assert(r.mainCue == -1 && "no main cue");
        assert(writer.memoryCuesDroppedByLeadInShift() == 1);

        // One the shift leaves after 0 is kept and not counted.
        memory.positionMs = 60.0;
        writer.writeHotCues(before.sourceId, {hot, memory});
        assert(near(raw(lib, before.sourceId).mainCue, (60.0 - LameShiftMs) * 44.1, 1e-3));
        assert(writer.memoryCuesDroppedByLeadInShift() == 1);
        std::cout << "case 8 (a memory cue inside the lead-in is left out and counted) OK\n";
    }

    // Case 9: the writer's lead-in source is the one answer it uses. A
    // GUI save hands it the stick's checked metadata cache, the reader's
    // answer; here a source says the LAME file is an FFmpeg one (1105
    // samples), and the cues are moved by that, not by the 2257 the
    // file's own start would give.
    {
        const Library lib = makeLibrary("source-honoured", "lame-cbr-44100.mp3");
        const Track before = readOne(lib);
        const auto lavc = seabass::infrastructure::audio::readMp3LeadIn(
            seabass::pathToUtf8(g_fixtures / "ffmpeg-lavc-44100.mp3"));
        assert(lavc && near(seabass::domain::engineToRekordboxOffsetMs(*lavc), 1105 * 1000.0 / 44100));
        int asked = 0;
        LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(lib.root));
        writer.setLeadInSource([&](const std::string &file) {
            ++asked;
            assert(file == lib.file);
            return lavc;
        });
        writer.writeHotCues(before.sourceId, theCues());
        assert(asked == 1);
        assertStored(raw(lib, before.sourceId), 1105 * 1000.0 / 44100);
        std::cout << "case 9 (the writer moves cues by its source's lead-in, not the file's) OK\n";
    }

    // Case 10: Create Engine Library picks the main cue by the writer's
    // rule: the earliest memory cue Engine can hold, after the shift. A
    // memory cue 30 ms in (inside the 51.18 ms lead-in) and one at 0:00
    // are left out, not taken as the earliest and clamped to the "no
    // cue" offset 0, and they are not counted as copied.
    {
        CuePoint hot{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"};
        CuePoint inside;
        inside.kind = CuePoint::Kind::Memory;
        inside.positionMs = 30.0;
        CuePoint atStart = inside;
        atStart.positionMs = 0.0;
        CuePoint later = inside;
        later.positionMs = 8000.0;
        const Library lib = makeLibrary("creator-main-cue", "lame-cbr-44100.mp3", {hot, inside, atStart, later});
        const Raw r = raw(lib, readOne(lib).sourceId);
        assert(near(r.mainCue, (8000.0 - LameShiftMs) * 44.1, 1e-3) && "the cue Engine can hold");
        assert(lib.created.cuesCopied == 2 && "the hot cue and the one main cue");
        assert(lib.created.memoryCuesDroppedByLeadInShift == 1);
        std::cout << "case 10 (Create Engine Library: the main cue by the writer's rule) OK\n";
    }

    // Case 11: a loop the shift starts before Engine's 0. Its start is
    // clamped to 0 and its end stays at the same audio instant, so it
    // keeps its musical end and is that much shorter. A loop wholly
    // inside the lead-in would be [0, 0]: no loop at all (the waveform
    // draws a loop only when its end is after its start), so it is left
    // out and counted, by the writer and by Create Engine Library.
    {
        CuePoint early{CuePoint::Kind::Hot, 2, 30.0, "#00ff00", "early loop"};
        early.isLoop = true;
        early.loopEndMs = 5000.0;
        CuePoint inside{CuePoint::Kind::Hot, 3, 10.0, "#0000ff", "inside"};
        inside.isLoop = true;
        inside.loopEndMs = 40.0;
        auto loopsOf = [](const Library &lib) {
            auto db = djinterop::engine::load_database(seabass::pathToUtf8(lib.root));
            return db.tracks().front().loops();
        };
        const auto check = [&](const Library &lib) {
            const auto loops = loopsOf(lib);
            assert(loops.size() > 2 && loops[1]);
            assert(loops[1]->start_sample_offset == 0.0);
            assert(near(loops[1]->end_sample_offset, (5000.0 - LameShiftMs) * 44.1, 1e-3));
            assert(!loops[2] && "the loop inside the lead-in is left out");
        };

        const Library written = makeLibrary("writer-loops", "lame-cbr-44100.mp3");
        LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(written.root));
        writer.writeHotCues(readOne(written).sourceId, {early, inside});
        check(written);
        assert(writer.loopsDroppedByLeadInShift() == 1);
        assert(writer.memoryCuesDroppedByLeadInShift() == 0);

        const Library created = makeLibrary("creator-loops", "lame-cbr-44100.mp3", {early, inside});
        check(created);
        assert(created.created.cuesCopied == 1);
        assert(created.created.loopsDroppedByLeadInShift == 1);
        std::cout << "case 11 (a loop the shift starts before 0: clamped, or left out when empty) OK\n";
    }

    // Case 12: reading a copy of a library (a stick backup, a scratch
    // copy) finds no audio beside it, so the lead-in reads as nothing and
    // the cues would come back on Engine's timeline, next to the live
    // stick's shifted ones. With the real library as its audio root the
    // reader shifts them as it does on the stick.
    {
        const Library lib = makeLibrary("reader-copy", "lame-cbr-44100.mp3", theCues());
        const fs::path copy = lib.root.parent_path() / "backup" / "Engine Library";
        fs::create_directories(copy);
        fs::copy(lib.root / "Database2", copy / "Database2", fs::copy_options::recursive);
        assert(!fs::exists(copy / "song.mp3"));

        LibdjinteropEngineReader unrooted(seabass::pathToUtf8(copy));
        const Track alone = seabass::application::ScanLibrary(unrooted).execute().front();
        assert(alone.cueTimelineShiftMs == 0.0 && "no audio beside the copy: Engine's own timeline");

        LibdjinteropEngineReader reader(seabass::pathToUtf8(copy));
        reader.setAudioRoot(seabass::pathToUtf8(lib.root));
        const Track t = seabass::application::ScanLibrary(reader).execute().front();
        assert(near(t.cueTimelineShiftMs, LameShiftMs) && "the copy's cues are shifted by the real file's lead-in");
        assertReadsBack(t, 0.0);
        assert(t.filePath == seabass::pathToUtf8((copy / "song.mp3").lexically_normal())
               && "the row's path still names the library that was read");

        // fillCues() resolves through the same root.
        Track filled = alone;
        LibdjinteropEngineReader again(seabass::pathToUtf8(copy));
        again.setAudioRoot(seabass::pathToUtf8(lib.root));
        std::vector<Track> rows = {filled};
        again.fillCues(rows);
        assert(near(rows.front().cueTimelineShiftMs, LameShiftMs));
        assertReadsBack(rows.front(), 0.0);
        std::cout << "case 12 (a copy of the library, lead-in from the real library's audio) OK\n";
    }

    MatchingPolicy::reset();
    fs::remove_all(seabass::testing::scratchRoot() / "seabass_engine_mp3_lead_in_test");
    std::cout << "all libdjinterop_engine_mp3_lead_in_test cases passed\n";
    return 0;
}
