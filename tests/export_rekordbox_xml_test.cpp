// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The reason this use case exists: a stick's rekordbox catalog holds a
// fraction of the cues its Engine catalog does, because the DJ cues on
// Denon gear. Exporting rekordbox's own view to XML would throw those
// away. These cases run against the committed anonymized fixture, whose
// shape is recorded (rekordbox: 1161 tracks / 314 cues, engine: 1564 /
// 1688), so "the merge carried Engine's cues over" is an assertion rather
// than a hope.
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/export_rekordbox_xml.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace fs = std::filesystem;

using seabass::application::ExportRekordboxXml;
using seabass::application::ExportRekordboxXmlOptions;
using seabass::domain::CuePoint;
using seabass::domain::PlaylistMembership;
using seabass::domain::Track;

namespace
{

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

size_t countOf(const std::string &haystack, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

Track row(std::string format, std::string sourceId, std::string path, std::string title)
{
    Track t;
    t.format = std::move(format);
    t.sourceId = std::move(sourceId);
    t.filePath = std::move(path);
    t.filename = "song.mp3";
    t.title = std::move(title);
    t.artist = "Kollektiv Turmstrasse";
    t.durationSeconds = 400.0;
    t.bitrate = 320;
    t.fileSizeBytes = 16'000'000;
    return t;
}

// The headline case, in miniature: rekordbox knows one cue for a file,
// Engine knows four more. All five have to reach the XML.
void testEngineCuesReachRekordbox()
{
    Track rb = row("rekordbox", "19", "/stick/Contents/a.mp3", "Sleepless");
    rb.cues.push_back(CuePoint{CuePoint::Kind::Hot, 1, 30765.0, "#FF0017", "", false, 0.0});

    Track engine = row("engine", "19", "/stick/Contents/a.mp3", "Sleepless");
    engine.cues.push_back(CuePoint{CuePoint::Kind::Hot, 1, 30765.0, "#FF0017", "", false, 0.0});
    engine.cues.push_back(CuePoint{CuePoint::Kind::Hot, 2, 75259.0, "#00C4FF", "", false, 0.0});
    engine.cues.push_back(CuePoint{CuePoint::Kind::Hot, 3, 105259.0, "#33FF00", "", false, 0.0});
    engine.cues.push_back(CuePoint{CuePoint::Kind::Hot, 4, 150259.0, "#4D00FF", "", false, 0.0});

    ExportRekordboxXml useCase;
    const auto result = useCase.execute({rb, engine});

    assert(result.tracksWritten == 1);  // one file, not two tracks
    assert(result.cuesWritten == 4);
    // Three of the four were only ever in Engine.
    assert(result.cuesFromOtherCatalogs == 3);
    assert(countOf(result.xml, "<POSITION_MARK") == 4);
}

// A cue in the first second is noise whatever kind it is -- the track
// already starts there, so neither a memory cue nor a hot pad at 7 ms is
// anything to navigate by. A loop is the exception, wherever it starts:
// an intro loop on the first bar is real work, and it carries an end as
// well as a start, which no stray press does.
void testJunkCuesDroppedExceptLoops()
{
    Track t = row("engine", "1", "/stick/Contents/b.mp3", "Drifter");
    t.cues.push_back(CuePoint{CuePoint::Kind::Memory, 0, 0.0, "", "", false, 0.0});
    t.cues.push_back(CuePoint{CuePoint::Kind::Memory, 0, 539.0, "", "", false, 0.0});  // Engine's own main_cue drift
    t.cues.push_back(CuePoint{CuePoint::Kind::Hot, 1, 250.0, "#FF0017", "", false, 0.0});
    t.cues.push_back(CuePoint{CuePoint::Kind::Hot, 2, 100.0, "#00C4FF", "intro", true, 4200.0});  // a loop
    t.cues.push_back(CuePoint{CuePoint::Kind::Memory, 0, 40000.0, "", "", false, 0.0});

    ExportRekordboxXml useCase;
    const auto result = useCase.execute({t});
    assert(result.junkMemoryCuesDropped == 3);  // two memory cues and the hot pad
    assert(result.cuesWritten == 2);
    assert(contains(result.xml, "Type=\"4\" Start=\"0.100\" End=\"4.200\""));  // the loop survived
    assert(contains(result.xml, "Start=\"40.000\" Num=\"-1\""));                 // the real memory cue survived
    assert(!contains(result.xml, "Start=\"0.250\""));                            // the hot pad at 250 ms did not

    ExportRekordboxXmlOptions keepThem;
    keepThem.dropJunkMemoryCues = false;
    assert(useCase.execute({t}, keepThem).cuesWritten == 5);
}

void testExtensionExclusionAndPathMapping()
{
    Track mp3 = row("rekordbox", "1", "/stick/Contents/a.mp3", "Keeper");
    Track flac = row("rekordbox", "2", "/stick/Contents/b.flac", "Dropped");

    ExportRekordboxXmlOptions options;
    options.excludeExtensions = {"flac"};
    options.pathPrefixMap = {{"/stick/Contents", "/Users/sebas/Music/WhalesharkLibrary/Tracks"}};

    ExportRekordboxXml useCase;
    const auto result = useCase.execute({mp3, flac}, options);

    assert(result.tracksWritten == 1);
    assert(result.tracksExcludedByExtension == 1);
    assert(contains(result.xml, "file://localhost/Users/sebas/Music/WhalesharkLibrary/Tracks/a.mp3"));
    assert(!contains(result.xml, "/stick/Contents"));
}

void testLongestPrefixWins()
{
    Track t = row("rekordbox", "1", "/stick/Contents/sub/a.mp3", "Keeper");
    ExportRekordboxXmlOptions options;
    options.pathPrefixMap = {{"/stick", "/wrong"}, {"/stick/Contents/sub", "/right"}};
    const auto result = ExportRekordboxXml().execute({t}, options);
    assert(contains(result.xml, "file://localhost/right/a.mp3"));
}

// Two catalogs putting the same hot cue in different places is a real
// defect on a real stick. A hot slot holds exactly one cue on the
// hardware, so the merge cannot keep both: the FIRST catalog in the list
// wins the slot (domain::LocalRestorePlanner::mergeCues) and the other
// position is dropped. That is precisely why it gets reported -- the loss
// is silent otherwise, and the caller's catalog order decides it.
void testConflictingCuesAreReportedAndOrderDecides()
{
    Track rb = row("rekordbox", "1", "/stick/Contents/c.mp3", "Disagreement");
    rb.cues.push_back(CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0017", "", false, 0.0});
    Track engine = row("engine", "1", "/stick/Contents/c.mp3", "Disagreement");
    engine.cues.push_back(CuePoint{CuePoint::Kind::Hot, 1, 45000.0, "#FF0017", "", false, 0.0});

    const auto rekordboxFirst = ExportRekordboxXml().execute({rb, engine});
    assert(rekordboxFirst.conflicts.size() == 1);
    assert(rekordboxFirst.conflicts.front().slot == "hot 1");
    assert(rekordboxFirst.conflicts.front().positionMsA == 30000.0);
    assert(rekordboxFirst.conflicts.front().positionMsB == 45000.0);
    assert(rekordboxFirst.cuesWritten == 1);
    assert(contains(rekordboxFirst.xml, "Start=\"30.000\""));

    // Same two rows, other order: the Engine position wins instead. This
    // is the knob a DJ who cues on Denon gear actually wants.
    const auto engineFirst = ExportRekordboxXml().execute({engine, rb});
    assert(engineFirst.cuesWritten == 1);
    assert(contains(engineFirst.xml, "Start=\"45.000\""));
    assert(engineFirst.conflicts.size() == 1);

    // Rounding across formats is not a conflict. The bar is the project's
    // own PositionToleranceMs (500 ms), which has to be this generous:
    // rekordbox's legacy and PCO2 cue lists hold the same pad up to 514 ms
    // apart inside one file.
    Track close = row("engine", "1", "/stick/Contents/c.mp3", "Disagreement");
    close.cues.push_back(CuePoint{CuePoint::Kind::Hot, 1, 30400.0, "#FF0017", "", false, 0.0});
    assert(ExportRekordboxXml().execute({rb, close}).conflicts.empty());
}

void testStreamingAndPathlessRowsAreSkipped()
{
    Track streaming = row("engine", "1", "/cache/tidal/x.mp3", "Streamed");
    streaming.streamingSource = "TIDAL";
    Track pathless = row("rekordbox", "2", "", "No path");

    const auto result = ExportRekordboxXml().execute({streaming, pathless});
    assert(result.tracksWritten == 0);
    assert(result.tracksStreaming == 1);
    assert(result.tracksWithoutPath == 1);
    assert(contains(result.xml, "<COLLECTION Entries=\"0\">"));
}

void testPlaylistsSurviveTheMerge()
{
    Track rb = row("rekordbox", "1", "/stick/Contents/d.mp3", "Shared");
    rb.playlists.push_back(PlaylistMembership{"Whaleshark/Peaktime", 3});
    Track engine = row("engine", "1", "/stick/Contents/d.mp3", "Shared");
    engine.playlists.push_back(PlaylistMembership{"Whaleshark/Peaktime", 3});

    const auto result = ExportRekordboxXml().execute({rb, engine});
    assert(result.playlistsWritten == 1);
    // One file in one playlist -- not the same track listed twice because
    // two catalogs mentioned it.
    assert(countOf(result.xml, "<TRACK Key=") == 1);
}

// The same run against the real committed fixture. Loose bounds on
// purpose: the exact numbers belong to the fixture and are recorded in
// its SET-EXPECTATIONS.txt, while what must hold for any library is the
// relationship -- the merged export carries more cues than the rekordbox
// catalog had on its own.
void testAgainstFixture(const fs::path &fixture)
{
    if (!fs::exists(fixture / "rekordbox" / "rekordbox" / "export.pdb")) {
        std::cerr << "fixture not found at " << fixture << ", skipping\n";
        return;
    }

    seabass::infrastructure::rekordbox::KaitaiRekordboxReader rekordbox(seabass::pathToUtf8(fixture / "rekordbox"));
    seabass::infrastructure::engine::LibdjinteropEngineReader engine(seabass::pathToUtf8(fixture / "engine"));

    std::vector<Track> rows = rekordbox.readAll();
    const size_t rekordboxRows = rows.size();

    // The same export, rekordbox alone: the baseline the merge has to
    // beat. Compared like for like -- both runs drop junk cues -- because
    // comparing a cleaned merge against a raw catalog count measures the
    // cleanup, not the merge.
    const auto rekordboxOnly = ExportRekordboxXml().execute(rows);

    const std::vector<Track> engineRows = engine.readAll();
    rows.insert(rows.end(), engineRows.begin(), engineRows.end());

    const auto result = ExportRekordboxXml().execute(rows);

    std::cout << "  fixture: " << rekordboxRows << " rekordbox rows (" << rekordboxOnly.cuesWritten
              << " cues after cleanup) + " << engineRows.size() << " engine rows -> " << result.tracksWritten
              << " tracks, " << result.cuesWritten << " cues (" << result.cuesFromOtherCatalogs
              << " not in rekordbox), " << result.junkMemoryCuesDropped << " junk dropped, "
              << result.playlistsWritten << " playlists, " << result.conflicts.size() << " conflicts\n";

    assert(result.tracksWritten > 0);
    // The whole point: more cues than rekordbox had by itself.
    assert(result.cuesWritten > rekordboxOnly.cuesWritten);
    assert(result.cuesFromOtherCatalogs > 0);
    assert(result.playlistsWritten > 0);
    // Collapsing means files, not rows: fewer tracks than rows in.
    assert(result.tracksWritten < rows.size());
    assert(contains(result.xml, "<DJ_PLAYLISTS Version=\"1.0.0\">"));
    assert(contains(result.xml, "<COLLECTION Entries=\"" + std::to_string(result.tracksWritten) + "\">"));
    assert(countOf(result.xml, "<POSITION_MARK") == result.cuesWritten);
    // No junk survived the drop.
    assert(!contains(result.xml, "Start=\"0.000\" Num=\"-1\""));
}

}  // namespace

int main(int argc, char **argv)
{
    testEngineCuesReachRekordbox();
    testJunkCuesDroppedExceptLoops();
    testExtensionExclusionAndPathMapping();
    testLongestPrefixWins();
    testConflictingCuesAreReportedAndOrderDecides();
    testStreamingAndPathlessRowsAreSkipped();
    testPlaylistsSurviveTheMerge();
    testAgainstFixture(argc > 1 ? fs::path(argv[1]) : fs::path("tests/fixtures/anonymized_library"));
    std::cout << "export_rekordbox_xml_test passed\n";
    return 0;
}
