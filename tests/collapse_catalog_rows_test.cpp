// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A file is what duplicates; a catalog row is one catalog's record of
// one. Everything here is a case where confusing the two would have made
// a real stick's cleanup wrong.
#include <cassert>
#include <iostream>
#include <string>

#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/duplicate_cleanup.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/track_scope.hpp"

using namespace seabass;
using seabass::application::collapseCatalogRows;
using seabass::domain::Track;

namespace
{

Track row(std::string format, std::string sourceId, std::string path, std::string title, double duration,
           int bitrate)
{
    Track t;
    t.format = std::move(format);
    t.sourceId = std::move(sourceId);
    t.filePath = std::move(path);
    t.filename = "song.mp3";
    t.title = std::move(title);
    t.artist = "Kollektiv Turmstrasse";
    t.durationSeconds = duration;
    t.bitrate = bitrate;
    t.fileSizeBytes = 8'000'000;
    return t;
}

bool listsRow(const Track &file, const std::string &format, const std::string &sourceId)
{
    for (const auto &r : file.catalogRows) {
        if (r.format == format && r.sourceId == sourceId) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main()
{
    // The shape of a real stick: three catalogs, one file. It must come
    // out as one copy that knows all three of its rows -- not as three
    // copies, which is what makes a file look like a duplicate of
    // itself.
    {
        std::vector<Track> rows = {
            row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 256),
            row("engine", "22", "/stick/Contents/a.mp3", "Sorry", 200.0, 0),
            row("onelibrary", "33", "/stick/Contents/a.mp3", "Sorry", 200.0, 256),
        };
        auto files = collapseCatalogRows(rows);
        assert(files.size() == 1);
        assert(files[0].catalogRows.size() == 3);
        assert(listsRow(files[0], "rekordbox", "11"));
        assert(listsRow(files[0], "engine", "22"));
        assert(listsRow(files[0], "onelibrary", "33"));
        std::cout << "case 1 (three catalogs listing one file collapse to one copy) OK\n";
    }

    // Engine stores no bitrate at all. Uncollapsed, a file catalogued
    // there scores 0 kbps and loses to anything -- which on a real stick
    // meant the survivor rule firing on all 435 groups from the Engine
    // side. Collapsed, the file knows the bitrate rekordbox recorded.
    {
        std::vector<Track> rows = {
            row("engine", "22", "/stick/Contents/a.mp3", "Sorry", 200.0, 0),
            row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 256),
        };
        auto files = collapseCatalogRows(rows);
        assert(files.size() == 1);
        assert(files[0].bitrate == 256);
        // The base row stays the first one given: stable output for a
        // caller that passes its catalogs in a fixed order.
        assert(files[0].format == "engine" && files[0].sourceId == "22");
        std::cout << "case 2 (a gap is filled from another catalog's row for the same file) OK\n";
    }

    // Two genuinely different files of the same recording stay two
    // copies -- this is exactly what cleanup is for, and collapsing them
    // would hide the duplicate instead of finding it.
    {
        std::vector<Track> rows = {
            row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 256),
            row("rekordbox", "12", "/stick/Contents/a-1.mp3", "Sorry", 200.0, 320),
        };
        auto files = collapseCatalogRows(rows);
        assert(files.size() == 2);
        std::cout << "case 3 (two different files of one recording stay two copies) OK\n";
    }

    // Case and separator spellings of one path name one file: exFAT and
    // NTFS are case-insensitive, and a stick written on Windows is read
    // on Linux.
    {
        std::vector<Track> rows = {
            row("rekordbox", "11", "/stick/Contents/A/Song.mp3", "Sorry", 200.0, 256),
            row("engine", "22", "\\stick\\contents\\a\\song.mp3", "Sorry", 200.0, 0),
        };
        auto files = collapseCatalogRows(rows);
        assert(files.size() == 1 && files[0].catalogRows.size() == 2);
        std::cout << "case 4 (case and backslash spellings are the same file) OK\n";
    }

    // A row with no resolvable path cannot be shown to be the same file
    // as anything, so it stands alone. Folding them together on a blank
    // key would merge unrelated files -- the one error here that deletes
    // music.
    {
        std::vector<Track> rows = {
            row("engine", "22", "", "Sorry", 200.0, 0),
            row("engine", "23", "", "Sorry", 200.0, 0),
        };
        auto files = collapseCatalogRows(rows);
        assert(files.size() == 2);
        assert(files[0].catalogRows.empty() && files[1].catalogRows.empty());
        std::cout << "case 5 (rows with no path are never folded together) OK\n";
    }

    // A streaming row's path names a cache on another machine, so it is
    // never evidence that two rows are one file.
    {
        Track streaming = row("engine", "22", "/home/dj/tidal-cache/x.flac", "Sorry", 200.0, 0);
        streaming.streamingSource = "TIDAL";
        Track other = row("engine", "23", "/home/dj/tidal-cache/x.flac", "Sorry", 200.0, 0);
        other.streamingSource = "TIDAL";
        auto files = collapseCatalogRows({streaming, other});
        assert(files.size() == 2);
        std::cout << "case 6 (streaming rows are never folded together) OK\n";
    }

    // Cues are the union: a cue that only one catalog knows about is
    // still a cue on that file, and cleanup must not decide otherwise.
    {
        Track rb = row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        rb.cues = {domain::CuePoint{domain::CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}};
        Track en = row("engine", "22", "/stick/Contents/a.mp3", "Sorry", 200.0, 0);
        en.cues = {domain::CuePoint{domain::CuePoint::Kind::Hot, 2, 40000.0, "#00FF00", "outro"}};
        auto files = collapseCatalogRows({rb, en});
        assert(files.size() == 1);
        assert(files[0].cues.size() == 2);
        std::cout << "case 7 (cues are the union across a file's rows) OK\n";
    }

    // The whole point, end to end: three catalogs, two real files of one
    // recording. Uncollapsed this is a group of six "copies" whose
    // removal list holds rows for the survivor's own file. Collapsed it
    // is what it actually is -- two copies, one to keep, one to remove,
    // and the removal carries the rows of all three catalogs.
    {
        std::vector<Track> rows = {
            row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 320),
            row("engine", "21", "/stick/Contents/a.mp3", "Sorry", 200.0, 0),
            row("onelibrary", "31", "/stick/Contents/a.mp3", "Sorry", 200.0, 320),
            row("rekordbox", "12", "/stick/Contents/a-1.mp3", "Sorry", 200.0, 128),
            row("engine", "22", "/stick/Contents/a-1.mp3", "Sorry", 200.0, 0),
            row("onelibrary", "32", "/stick/Contents/a-1.mp3", "Sorry", 200.0, 128),
        };
        auto files = collapseCatalogRows(rows);
        assert(files.size() == 2);

        auto groups = domain::DuplicateTrackFinder::find(files);
        assert(groups.size() == 1);
        auto plan = domain::DuplicateCleanupPlanner::plan(groups[0]);
        assert(plan.survivor.filePath == "/stick/Contents/a.mp3");
        assert(plan.toRemove.size() == 1);
        assert(plan.toRemove[0].filePath == "/stick/Contents/a-1.mp3");
        // Not one row for the file being kept is in the removal list.
        for (const auto &r : plan.toRemove[0].catalogRows) {
            assert(!listsRow(plan.survivor, r.format, r.sourceId));
        }
        assert(plan.toRemove[0].catalogRows.size() == 3);
        std::cout << "case 8 (six rows, two files: one group, one removal, three rows to drop) OK\n";
    }

    // Playlist membership is the union across formats. A cleanup scoped
    // to a playlist has to see a file whose membership was only recorded
    // in the format it is not reading -- otherwise scoping silently
    // changes which files are candidates, and the formats are meant to
    // carry the same playlists anyway.
    {
        Track rb = row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        rb.playlists = {{"Techno/Peak Time", 3}};
        Track en = row("engine", "22", "/stick/Contents/a.mp3", "Sorry", 200.0, 0);
        en.playlists = {{"Techno/Peak Time", 9}, {"Warmup", 1}};
        auto files = collapseCatalogRows({rb, en});
        assert(files.size() == 1);
        assert(files[0].playlists.size() == 2);
        // First position wins, like every other field here.
        assert(files[0].playlists[0].name == "Techno/Peak Time" && files[0].playlists[0].position == 3);
        assert(domain::TrackScope::playlist("Warmup").matches(files[0]));
        std::cout << "case 9 (playlists are the union across a file's formats) OK\n";
    }

    // A selection names a file, not a row. Picking the track by its
    // Engine row must still select the file when the collapse happened
    // to make rekordbox's row the base.
    {
        std::vector<Track> rows = {
            row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 256),
            row("engine", "22", "/stick/Contents/a.mp3", "Sorry", 200.0, 0),
        };
        auto files = collapseCatalogRows(rows);
        assert(files.size() == 1 && files[0].format == "rekordbox");
        auto byEngineRow = domain::TrackScope::arbitrary({{"engine", "22"}});
        assert(byEngineRow.matches(files[0]));
        auto byRekordboxRow = domain::TrackScope::arbitrary({{"rekordbox", "11"}});
        assert(byRekordboxRow.matches(files[0]));
        auto bySomethingElse = domain::TrackScope::arbitrary({{"engine", "99"}});
        assert(!bySomethingElse.matches(files[0]));
        std::cout << "case 10 (a selection by any format's row selects the file) OK\n";
    }

    // Collapse then scope, never the other way round: scoping rows first
    // hands the collapse a partial set, and the file comes out missing
    // the rows that would have had to go with it.
    {
        Track rb = row("rekordbox", "11", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        rb.playlists = {{"Techno", 1}};
        Track en = row("engine", "22", "/stick/Contents/a.mp3", "Sorry", 200.0, 0);
        // Engine never recorded this file's membership -- the formats
        // have diverged, which is exactly when order starts to matter.
        auto scope = domain::TrackScope::playlist("Techno");

        auto collapsedThenScoped = domain::filterByScope(collapseCatalogRows({rb, en}), scope);
        assert(collapsedThenScoped.size() == 1);
        assert(collapsedThenScoped[0].catalogRows.size() == 2);  // both rows still go with it

        auto scopedThenCollapsed = collapseCatalogRows(domain::filterByScope({rb, en}, scope));
        assert(scopedThenCollapsed.size() == 1);
        assert(scopedThenCollapsed[0].catalogRows.size() == 1);  // Engine's row silently lost
        std::cout << "case 11 (collapse then scope keeps every row that must go with the file) OK\n";
    }

    // An album one catalog recorded and another did not. Same rule as
    // every other gap-fill: the formats describe one library, so a value
    // missing from one of them is one another already holds about that
    // same track.
    {
        Track rb = row("rekordbox", "rb1", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        Track en = row("engine", "en1", "/stick/Contents/a.mp3", "Sorry", 200.0, 0);
        en.album = "Sounds From The Deep";
        auto files = collapseCatalogRows({rb, en});
        assert(files.size() == 1);
        assert(files[0].album == "Sounds From The Deep");
        std::cout << "case 16 (an album only one catalog recorded survives the collapse) OK\n";
    }

    // --- collapseForCleanupScan: the scan's collapse-or-not decision ---

    // Every catalog readable: rows from all of them fold into files, so a
    // plan can carry each catalog's row and one save can remove them all.
    {
        Track rb = row("rekordbox", "rb1", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        Track en = row("engine", "en1", "/stick/Contents/a.mp3", "Sorry", 200.0, 0);
        application::CatalogTracks catalogs;
        catalogs.rekordbox = std::vector<Track>{rb};
        catalogs.engine = std::vector<Track>{en};

        auto scan = application::collapseForCleanupScan(catalogs, {}, {rb});
        assert(scan.collapsedAcrossCatalogs);
        assert(scan.rows.size() == 1);
        assert(scan.rows[0].catalogRows.size() == 2);
        std::cout << "case 12 (all catalogs readable: rows fold into files) OK\n";
    }

    // A catalog that could not be read: NOT collapsed. A file whose Engine
    // row was never seen would look Engine-free, the save would remove
    // only the rekordbox row, and Engine would be left listing a file
    // rekordbox no longer does -- deduplication manufacturing the split
    // state it exists to prevent. Finding less is the safe failure.
    {
        Track rb = row("rekordbox", "rb1", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        Track en = row("engine", "en1", "/stick/Contents/a.mp3", "Sorry", 200.0, 0);
        application::CatalogTracks catalogs;
        catalogs.rekordbox = std::vector<Track>{rb};
        catalogs.engine = std::vector<Track>{en};

        auto scan = application::collapseForCleanupScan(catalogs, {"onelibrary"}, {rb});
        assert(!scan.collapsedAcrossCatalogs);
        assert(scan.rows.size() == 1);
        // The caller's own row, uncollapsed: no catalogRows, so nothing
        // downstream believes this file has an Engine row to remove.
        assert(scan.rows[0].catalogRows.empty());
        assert(scan.rows[0].sourceId == "rb1");
        std::cout << "case 13 (an unreadable catalog forbids collapsing) OK\n";
    }

    // Streaming rows never reach grouping, from either branch: their path
    // names a cache on another machine, so they must never be a survivor
    // or a doomed copy.
    {
        Track rb = row("rekordbox", "rb1", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        Track stream = row("engine", "en2", "/cache/elsewhere.mp3", "Streamed", 200.0, 0);
        stream.streamingSource = "tidal";
        application::CatalogTracks catalogs;
        catalogs.rekordbox = std::vector<Track>{rb};
        catalogs.engine = std::vector<Track>{stream};

        auto collapsed = application::collapseForCleanupScan(catalogs, {}, {rb});
        assert(collapsed.collapsedAcrossCatalogs);
        assert(collapsed.rows.size() == 1);
        assert(collapsed.rows[0].streamingSource.empty());

        auto fallback = application::collapseForCleanupScan(catalogs, {"engine"}, {rb, stream});
        assert(!fallback.collapsedAcrossCatalogs);
        assert(fallback.rows.size() == 1);
        assert(fallback.rows[0].streamingSource.empty());
        std::cout << "case 14 (streaming rows are dropped on both branches) OK\n";
    }

    // Nothing supplied at all falls back rather than reporting a
    // cross-catalog scan that saw nothing -- which would read as "no
    // duplicates on this stick" instead of "this scan was handed nothing".
    {
        Track rb = row("rekordbox", "rb1", "/stick/Contents/a.mp3", "Sorry", 200.0, 256);
        application::CatalogTracks empty;
        auto scan = application::collapseForCleanupScan(empty, {}, {rb});
        assert(!scan.collapsedAcrossCatalogs);
        assert(scan.rows.size() == 1);
        std::cout << "case 15 (no catalog supplied anything: falls back, does not claim a collapse) OK\n";
    }

    std::cout << "all collapse_catalog_rows_test cases passed\n";
    return 0;
}
