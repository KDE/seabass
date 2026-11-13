// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The #73 hardware round of 2026-11-13 (stick TESTSHARK, rekordbox 7.2.18,
// Prime 4, Prime Go+), replayed on what the stick held at each step
// (tests/fixtures/testshark_t73, README.md there). Each case a ctest of
// its own, by name:
//
// fixture   The audio heads read as the stick's whole files did: TagLib's
//           sample rate and length, and the MP3 lead-in, as the stick's
//           metadata cache recorded them. And the chain checker
//           (engine_chain_check.hpp) finds what it is there to find: the
//           fixture's lists whole, a broken copy of them broken.
//
// argv[1]: tests/fixtures/testshark_t73, argv[2]: the case.

#include <QCoreApplication>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "engine_chain_check.hpp"
#include "engine_change_fixture.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif
#include "scratch_path.hpp"

using seabass::pathFromUtf8;
using seabass::pathToUtf8;
namespace fs = std::filesystem;
namespace testing = seabass::testing;
namespace d = seabass::domain;

namespace
{

fs::path g_fixture;

// An Engine state of the fixture as a library of its own, for reading
// with libdjinterop and SQLite without touching the fixture.
fs::path engineCopy(const std::string &engineDb, const std::string &name)
{
    const fs::path library = testing::scratchRoot() / name / "Engine Library";
    fs::remove_all(library.parent_path());
    fs::create_directories(library / "Database2");
    fs::copy_file(g_fixture / "engine" / engineDb, library / "Database2" / "m.db");
    return library;
}

// --- fixture -----------------------------------------------------------

void testFixture()
{
    // What the stick's Seabass/caches/metadata.jsonl recorded for the
    // whole files after save 1 (N3 was never probed there: 44.1 kHz,
    // 240 s by how it was made).
    struct Expected
    {
        const char *file;
        int rate;
        double seconds;
    };
    const Expected expected[] = {
        {"Seabass Test/T73 Base/T73-B01-click-44k-102bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B02-click-44k-104bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B03-click-44k-106bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B04-click-44k-108bpm.mp3", 44100, 180.036},
        {"Seabass Test/T73 Base/T73-B07-click-44k-114bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B08-click-44k-116bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B12-click-44k-124bpm.flac", 44100, 180.0},
        {"Seabass Test/T73/T73-N1-click-48k-120bpm.flac", 48000, 240.0},
        {"Seabass Test/T73/T73-N3-click-44k-128bpm.flac", 44100, 240.0},
        {"UnknownArtist/UnknownAlbum/T73-B11-click-48k-122bpm.aiff", 48000, 180.0},
        {"UnknownArtist/UnknownAlbum/T73-N2-click-48k-125bpm.aiff", 48000, 240.0},
    };
    std::size_t onDisk = 0;
    for (auto it = fs::recursive_directory_iterator(g_fixture / "stick" / "Contents");
         it != fs::recursive_directory_iterator(); ++it) {
        onDisk += it->is_regular_file() ? 1 : 0;
    }
    assert(onDisk == std::size(expected) && "the eleven files on the stick at export 128, no others");

#ifdef SEABASS_HAVE_TAGLIB
    seabass::infrastructure::audio::TagLibMetadataProbe probe;
#else
    // Never a pass without the probe the save itself needs: an add to
    // Engine reads the file's sample rate through TagLib.
    std::cerr << "built without TagLib: the fixture's audio cannot be checked, and the save cannot add tracks\n";
    std::exit(1);
    seabass::application::NullTrackMetadataProbe probe;
#endif
    for (const auto &e : expected) {
        const fs::path file = g_fixture / "stick" / "Contents" / pathFromUtf8(e.file);
        const auto read = probe.read(pathToUtf8(file));
        if (!read) {
            std::cerr << "  TagLib cannot read " << e.file << "\n";
        }
        assert(read);
        std::cout << "  " << e.file << ": " << read->sampleRate << " Hz, " << read->durationSeconds << " s\n";
        assert(read->sampleRate == e.rate);
        assert(std::abs(read->durationSeconds - e.seconds) < 0.0005);
        assert(!read->durationIsEstimated);
    }
    // B04's lead-in, as the cache had it for the whole file:
    // "4 1 C 1 576 1008 0 0 44100 1152 1 1".
    {
        const auto read =
            probe.read(pathToUtf8(g_fixture / "stick" / "Contents" / "Seabass Test" / "T73 Base"
                                  / "T73-B04-click-44k-108bpm.mp3"));
        assert(read && read->mp3LeadInRead && read->mp3LeadIn);
        const d::Mp3LeadIn &l = *read->mp3LeadIn;
        assert(l.infoFrame && l.tag == d::Mp3LeadIn::Tag::Lavc && l.lameCrcValid);
        assert(l.encoderDelay == 576 && l.encoderPadding == 1008);
        assert(!l.infoFrameModeMatchesAudio && !l.vbri);
        assert(l.sampleRate == 44100 && l.samplesPerFrame == 1152);
        assert(l.tagFrameFirst && l.tagFrameHasCounts);
    }
    std::cout << "fixture: the audio heads read as the whole files did OK\n";

    // The chain checker: every Engine state of the fixture walks whole.
    for (const char *state : {"m-before-R3.db", "m-after-R3c-save1.db", "m-after-R3d.db", "m-after-R1b.db"}) {
        const fs::path library = engineCopy(state, std::string("chains_") + state);
        const auto report = testing::checkEngineChains(pathToUtf8(library / "Database2" / "m.db"));
        if (!report.whole()) {
            std::cerr << state << ":\n" << report.text();
        }
        assert(report.whole());
        assert(report.children.at(0).size() == 3 && "three playlists at the top, in every state");
    }
    // And finds each way a chain breaks, on copies of save 1's m.db.
    // Engine's delete trigger relinks round a deleted entry, so the breaks
    // are made by UPDATE, as a writer that got it wrong would.
    const auto broken = [](const char *name, const std::string &sql, const std::string &finding) {
        const fs::path library = engineCopy("m-after-R3c-save1.db", std::string("broken_") + name);
        const std::string db = pathToUtf8(library / "Database2" / "m.db");
        assert(testing::sqlExec(db, sql));
        const auto report = testing::checkEngineChains(db);
        std::cout << "  " << name << ":\n" << report.text();
        assert(!report.whole());
        bool found = false;
        for (const auto &p : report.problems) {
            found = found || p.find(finding) != std::string::npos;
        }
        assert(found && "the checker names the break");
    };
    // "T73 Renamed" (list 5) cut after its head: two heads, two tails.
    broken("cut", "UPDATE PlaylistEntity SET nextEntityId = 0 WHERE listId = 5 AND id NOT IN "
                  "(SELECT nextEntityId FROM PlaylistEntity WHERE listId = 5);",
           "list 5: 2 head(s) and 2 tail(s)");
    // Its tail pointed back at its head.
    broken("loop", "UPDATE PlaylistEntity SET nextEntityId = (SELECT id FROM PlaylistEntity WHERE listId = 5 AND "
                   "id NOT IN (SELECT nextEntityId FROM PlaylistEntity WHERE listId = 5)) WHERE listId = 5 AND "
                   "nextEntityId = 0;",
           "0 head(s) and 0 tail(s)");
    // Its tail pointed into "T73 Order" (list 2).
    broken("into another list", "UPDATE PlaylistEntity SET nextEntityId = (SELECT min(id) FROM PlaylistEntity WHERE "
                                "listId = 2) WHERE listId = 5 AND nextEntityId = 0;",
           "which is not in this chain");
    // An entry of a track that does not exist.
    broken("no track", "UPDATE PlaylistEntity SET trackId = 999 WHERE id = (SELECT min(id) FROM PlaylistEntity);",
           "names track 999, which does not exist");
    // The last root playlist pointing at one that does not exist.
    broken("siblings", "UPDATE Playlist SET nextListId = 999 WHERE parentListId = 0 AND nextListId = 0;",
           "points at 999, which is not in this chain");
    std::cout << "fixture: the chain checker finds the fixture whole and every planted break OK\n";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: testshark_t73_test <tests/fixtures/testshark_t73> fixture\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    // Every line out before an assert ends the process.
    std::cout << std::unitbuf;
    g_fixture = pathFromUtf8(argv[1]);
    const std::string which = argv[2];
    const fs::path scratch = testing::scratchRoot() / ("testshark_t73_" + which);
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    if (which == "fixture") {
        testFixture();
    } else {
        std::cerr << "unknown case " << which << "\n";
        return 2;
    }
    std::cout << "testshark_t73_test " << which << ": OK\n";
    return 0;
}
