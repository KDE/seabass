// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>

#include <sqlite3.h>

#include "application/use_cases/scan_library.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"

namespace
{

// Presses Cancel after the first track of the build phase.
class CancelOnFirstTick : public seabass::application::ProgressReporter
{
public:
    explicit CancelOnFirstTick(seabass::application::CancellationToken token) : m_token(std::move(token)) {}
    void start(const std::string &, size_t) override {}
    void tick(size_t) override { m_token.cancel(); }
    void finish() override {}
    void warn(const std::string &) override {}

private:
    seabass::application::CancellationToken m_token;
};

}  // namespace
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
using seabass::domain::CuePoint;
using seabass::domain::Track;
namespace fs = std::filesystem;

namespace
{

Track makeTrack(std::string id, std::string title, std::string artist, std::string filePath, double bpm = 128.0,
                 std::string key = "F#m", double duration = 300.0)
{
    Track t;
    t.sourceId = std::move(id);
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.filePath = std::move(filePath);
    t.bpm = bpm;
    t.key = std::move(key);
    t.durationSeconds = duration;
    t.bitrate = 320;
    t.rating = 4;
    t.comment = "banger";
    t.fileSizeBytes = 12345;
    t.cues = {
        {CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"},
        {CuePoint::Kind::Memory, 0, 500.0, "", ""},
    };
    return t;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_engine_library_creator_test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path enginePath = root / "Engine Library";

    // Case 1: creates tracks + cues, verified by reading them back via
    // the same, already-proven LibdjinteropEngineReader.
    {
        std::vector<Track> tracks = {
            makeTrack("r1", "Song One", "Artist One", (root / "song1.mp3").string()),
            makeTrack("r2", "Song Two", "Artist Two", (root / "song2.mp3").string(), 140.0, "Gbm", 200.0),
        };
        auto result = EngineLibraryCreator::create(enginePath.string(), tracks, EngineSchemaGeneration::V2);
        assert(result.errorMessage.empty());
        assert(result.tracksCreated == 2);
        assert(result.tracksSkipped == 0);
        assert(result.cuesCopied == 4);

        LibdjinteropEngineReader reader(enginePath.string());
        auto readBack = seabass::application::ScanLibrary(reader).execute();
        assert(readBack.size() == 2);

        bool foundOne = false, foundTwo = false;
        for (const auto &t : readBack) {
            if (t.title == "Song One") {
                foundOne = true;
                assert(t.artist == "Artist One");
                assert(t.bitrate == 320);
                assert(t.rating.has_value() && *t.rating == 4);
                assert(t.comment == "banger");
                assert(t.cues.size() == 2);
            }
            if (t.title == "Song Two") {
                foundTwo = true;
                // "Gbm" and "F#m" are enharmonically the same key --
                // both tracks should read back with a non-empty key
                // string (exact spelling is libdjinterop's own choice).
                assert(!t.key.empty());
            }
        }
        assert(foundOne && foundTwo);
        std::cout << "case 1 (create + read back tracks/cues/metadata) OK\n";
    }

    // Case 2: refuses to overwrite an existing Engine Library.
    {
        std::vector<Track> tracks = {makeTrack("r3", "Song Three", "Artist Three", (root / "song3.mp3").string())};
        auto result = EngineLibraryCreator::create(enginePath.string(), tracks, EngineSchemaGeneration::V2);
        assert(!result.errorMessage.empty());
        assert(result.tracksCreated == 0);
        std::cout << "case 2 (refuses to overwrite an existing library) OK\n";
    }

    // Case 3: a track with no resolved local file is skipped, not
    // fabricated into the new library.
    {
        fs::path secondEnginePath = root / "Engine Library 2";
        Track noFile = makeTrack("r4", "No File", "Nobody", "");
        std::vector<Track> tracks = {noFile};
        auto result = EngineLibraryCreator::create(secondEnginePath.string(), tracks, EngineSchemaGeneration::V2);
        assert(result.errorMessage.empty());
        assert(result.tracksCreated == 0);
        assert(result.tracksSkipped == 1);
        std::cout << "case 3 (track with no local file is skipped) OK\n";
    }

    // Case 4: the Information row is where Engine keeps it -- exactly one,
    // at id 1 -- at every schema generation this feature offers.
    //
    // Not a detail. That table is where the schema version lives, so it is
    // the first thing anything reading the library looks at, and a Prime 4
    // rejected a real stick as corrupt over it. libdjinterop's 3.0.2
    // creator used to seed the table's AUTOINCREMENT counter before
    // inserting the row, landing it at id 2, and this project corrected it
    // afterwards; the fix is upstream now (17ea4f70) and the correction is
    // gone. What is left guards the vendored checkout itself: if a bump
    // ever brings the bug back, or a new schema generation grows its own
    // version of it, this fails before any hardware sees it.
    {
        const EngineSchemaGeneration generations[] = {EngineSchemaGeneration::V1, EngineSchemaGeneration::V2,
                                                       EngineSchemaGeneration::V3};
        int which = 0;
        for (EngineSchemaGeneration generation : generations) {
            fs::path path = root / ("Engine Library gen" + std::to_string(++which));
            std::vector<Track> tracks = {makeTrack("r1", "Song One", "Artist One", (root / "song1.mp3").string())};
            auto result = EngineLibraryCreator::create(path.string(), tracks, generation);
            if (!result.errorMessage.empty()) {
                // Say which generation and why: an assertion that hides the
                // reason costs an hour every time this fires. On cerr, because
                // abort() throws away whatever is still sitting in cout.
                std::cerr << "generation " << which << " failed: " << result.errorMessage << "\n";
            }
            assert(result.errorMessage.empty());
            assert(result.tracksCreated == 1);

            // 1.x keeps m.db at the library root, 2.x and 3.x under Database2.
            fs::path dbFile = path / "Database2" / "m.db";
            if (!fs::exists(dbFile)) {
                dbFile = path / "m.db";
            }
            assert(fs::exists(dbFile));

            sqlite3 *db = nullptr;
            const std::string dbPath = dbFile.string();
            assert(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
            sqlite3_stmt *stmt = nullptr;
            assert(sqlite3_prepare_v2(db, "SELECT count(*), coalesce(min(id), 0) FROM Information;", -1, &stmt,
                                       nullptr)
                   == SQLITE_OK);
            assert(sqlite3_step(stmt) == SQLITE_ROW);
            const int rows = sqlite3_column_int(stmt, 0);
            const int firstId = sqlite3_column_int(stmt, 1);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            assert(rows == 1);
            assert(firstId == 1);
        }
        std::cout << "case 4 (Information row at id 1, every schema generation) OK\n";
    }

    // Case 5: playlists come across, with their folder structure and in
    // the order the source recorded -- a playlist is not a set, and a DJ
    // notices immediately when it is treated as one.
    {
        fs::path path = root / "Engine Library playlists";
        Track first = makeTrack("r1", "Opener", "Artist One", (root / "song1.mp3").string());
        Track second = makeTrack("r2", "Closer", "Artist Two", (root / "song2.mp3").string(), 140.0, "Gbm", 200.0);
        Track third = makeTrack("r3", "Middle", "Artist Three", (root / "song3.mp3").string());
        // Deliberately out of order in the input, and in two playlists
        // that share one folder, so both the ordering and the folder
        // reuse are actually exercised.
        first.playlists = {{"Techno/Peak Time", 0}};
        third.playlists = {{"Techno/Peak Time", 1}};
        second.playlists = {{"Techno/Peak Time", 2}, {"Techno/Warm Up", 0}};
        std::vector<Track> tracks = {second, third, first};

        auto result = EngineLibraryCreator::create(path.string(), tracks, EngineSchemaGeneration::V2);
        assert(result.errorMessage.empty());
        assert(result.tracksCreated == 3);
        // Two playlists plus the "Techno" folder they share.
        assert(result.playlistsCreated == 3);

        LibdjinteropEngineReader reader(path.string());
        auto readBack = seabass::application::ScanLibrary(reader).execute();
        assert(readBack.size() == 3);

        std::map<int, std::string> peakTime;
        bool warmUpHasCloser = false;
        for (const auto &t : readBack) {
            for (const auto &membership : t.playlists) {
                if (membership.name == "Techno/Peak Time") {
                    peakTime[membership.position] = t.title;
                }
                if (membership.name == "Techno/Warm Up" && t.title == "Closer") {
                    warmUpHasCloser = true;
                }
            }
        }
        assert(peakTime.size() == 3);
        assert(peakTime[0] == "Opener");
        assert(peakTime[1] == "Middle");
        assert(peakTime[2] == "Closer");
        assert(warmUpHasCloser);
        std::cout << "case 5 (playlists, their folders and their order) OK\n";
    }

    // The player is asked to do the analysis itself. This project has no
    // waveform to give -- no rekordbox->Engine waveform conversion exists
    // yet -- and libdjinterop marks every track it creates as analysed,
    // which leaves a player believing there is nothing to do and drawing
    // an empty waveform for ever. A created library therefore goes out in
    // the state Engine's own rekordbox import leaves behind: unanalysed,
    // with the analysis columns empty and the cues still there.
    {
        const EngineSchemaGeneration modern[] = {EngineSchemaGeneration::V2, EngineSchemaGeneration::V3};
        int which = 0;
        for (EngineSchemaGeneration generation : modern) {
            fs::path path = root / ("Engine Library analysis" + std::to_string(++which));
            std::vector<Track> tracks = {makeTrack("r1", "Song One", "Artist One", (root / "song1.mp3").string())};
            auto result = EngineLibraryCreator::create(path.string(), tracks, generation);
            assert(result.errorMessage.empty());
            assert(result.tracksCreated == 1);
            assert(result.tracksLeftForDeviceAnalysis == result.tracksCreated);

            sqlite3 *db = nullptr;
            const std::string dbPath = (path / "Database2" / "m.db").string();
            assert(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
            const auto count = [db](const char *sql) {
                sqlite3_stmt *stmt = nullptr;
                assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
                assert(sqlite3_step(stmt) == SQLITE_ROW);
                const int value = sqlite3_column_int(stmt, 0);
                sqlite3_finalize(stmt);
                return value;
            };
            assert(count("SELECT count(*) FROM Track;") == 1);
            assert(count("SELECT count(*) FROM Track WHERE isAnalyzed <> 0;") == 0);
            assert(count("SELECT count(*) FROM PerformanceData WHERE trackData IS NOT NULL "
                          "OR overviewWaveFormData IS NOT NULL OR beatData IS NOT NULL;")
                   == 0);
            // The cues are not analysis output and must survive it.
            assert(count("SELECT count(*) FROM PerformanceData WHERE quickCues IS NOT NULL;") == 1);
            sqlite3_close(db);

            // And the library still reads back, NULL analysis columns and all,
            // exactly as a Denon-written stick full of them does.
            LibdjinteropEngineReader reader(path.string());
            auto readBack = seabass::application::ScanLibrary(reader).execute();
            assert(readBack.size() == 1);
            assert(readBack.front().title == "Song One");
            assert(readBack.front().cues.size() == 2);
        }

        // A 1.x library keeps this flag somewhere else entirely, and no 1.x
        // hardware has been available to check it, so it is left alone --
        // and says so rather than claiming tracks it did not touch.
        fs::path v1Path = root / "Engine Library analysis v1";
        std::vector<Track> v1Tracks = {makeTrack("r1", "Song One", "Artist One", (root / "song1.mp3").string())};
        auto v1Result = EngineLibraryCreator::create(v1Path.string(), v1Tracks, EngineSchemaGeneration::V1);
        assert(v1Result.errorMessage.empty());
        assert(v1Result.tracksCreated == 1);
        assert(v1Result.tracksLeftForDeviceAnalysis == 0);
        std::cout << "case (tracks handed to the player for analysis) OK\n";
    }

    // Cover art: pointed at on EVERY schema generation, and named from
    // the bytes rather than from whatever the source file was called.
    //
    // Both halves shipped broken. The UPDATE said `albumArtId`, which is
    // the 2.x/3.x spelling; a V1 library declares [idAlbumArt], so on V1
    // the image was copied and the AlbumArt row inserted and then the
    // UPDATE threw into a per-track catch -- covers under Artwork/ that
    // nothing referenced, orphan rows, and artworkCopied == 0, which
    // reads as "this library had no art to copy". And the extension came
    // from the source's own name, so a PNG called cover.jpg was written
    // as <hash>.jpg: a file whose name promises a format it does not
    // hold, to a player that has only the name to go on.
    {
        // A real PNG and a real JPEG, each deliberately misnamed.
        const std::string pngBytes = std::string("\x89PNG\r\n\x1a\n", 8) + "not really a png body";
        const std::string jpegBytes = std::string("\xFF\xD8\xFF", 3) + "not really a jpeg body";
        const auto write = [](const fs::path &at, const std::string &bytes) {
            std::ofstream out(at, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        write(root / "cover.jpg", pngBytes);   // PNG bytes, .jpg name
        write(root / "cover.PNG", jpegBytes);  // JPEG bytes, .PNG name
        write(root / "cover.webp", "RIFFxxxxWEBPVP8 nonsense");  // neither

        const EngineSchemaGeneration all[] = {EngineSchemaGeneration::V1, EngineSchemaGeneration::V2,
                                              EngineSchemaGeneration::V3};
        int which = 0;
        for (EngineSchemaGeneration generation : all) {
            fs::path path = root / ("Engine Library artwork" + std::to_string(++which));
            std::vector<Track> tracks = {
                makeTrack("r1", "Song One", "Artist One", (root / "song1.mp3").string()),
                makeTrack("r2", "Song Two", "Artist Two", (root / "song2.mp3").string()),
                makeTrack("r3", "Song Three", "Artist Three", (root / "song3.mp3").string()),
            };
            tracks[0].artworkPath = (root / "cover.jpg").string();
            tracks[1].artworkPath = (root / "cover.PNG").string();
            tracks[2].artworkPath = (root / "cover.webp").string();

            auto result = EngineLibraryCreator::create(path.string(), tracks, generation);
            assert(result.errorMessage.empty());

            // Two of the three: the third is neither JPEG nor PNG, so it
            // is skipped rather than named for a player that cannot be
            // promised it reads it.
            assert(result.artworkCopied == 2);

            // Named for what they are, not for what they were called.
            int png = 0;
            int jpg = 0;
            int other = 0;
            for (const auto &entry : fs::directory_iterator(path / "Artwork")) {
                const std::string ext = entry.path().extension().string();
                if (ext == ".png") {
                    ++png;
                } else if (ext == ".jpg") {
                    ++jpg;
                } else {
                    ++other;
                }
            }
            assert(png == 1);
            assert(jpg == 1);
            assert(other == 0);

            // And the rows actually point at them -- the part that was
            // silently false on V1.
            // A 1.x library keeps m.db at the library root; 2.x and 3.x
            // put it under Database2/.
            sqlite3 *db = nullptr;
            const fs::path modern = path / "Database2" / "m.db";
            const std::string dbPath = (fs::exists(modern) ? modern : path / "m.db").string();
            assert(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
            const std::string column = generation == EngineSchemaGeneration::V1 ? "idAlbumArt" : "albumArtId";
            const auto countOf = [db](const std::string &sql) {
                sqlite3_stmt *stmt = nullptr;
                assert(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
                assert(sqlite3_step(stmt) == SQLITE_ROW);
                const int value = sqlite3_column_int(stmt, 0);
                sqlite3_finalize(stmt);
                return value;
            };
            // Pointing at a row is not the test: libdjinterop gives every
            // track an empty AlbumArt row (hash '', blob NULL) to start
            // with, and the skipped webp must stay on it. What matters is
            // how many tracks reach a row carrying a real hash -- the
            // rows this function inserted and named a file after.
            const int withRealArt = countOf("SELECT count(*) FROM Track JOIN AlbumArt ON AlbumArt.id = Track."
                                            + column + " WHERE length(AlbumArt.hash) > 0;");
            const int realRows = countOf("SELECT count(*) FROM AlbumArt WHERE length(hash) > 0;");
            sqlite3_close(db);

            assert(withRealArt == 2);
            // And no row was inserted that nothing points at, which is
            // what the V1 failure left behind.
            assert(realRows == 2);
        }
        std::cout << "case (cover art is pointed at on every generation, and named from its bytes) OK\n";
    }

    // Cancel between two tracks: nothing at all lands on the target
    // (the scratch build is thrown away), and the result says so.
    {
        fs::path cancelledPath = root / "cancelled" / "Engine Library";
        fs::create_directories(cancelledPath.parent_path());
        std::vector<Track> tracks = {
            makeTrack("r1", "Song One", "Artist One", (root / "song1.mp3").string()),
            makeTrack("r2", "Song Two", "Artist Two", (root / "song2.mp3").string(), 140.0, "Gbm", 200.0),
        };
        seabass::application::CancellationToken cancel;
        CancelOnFirstTick reporter(cancel);
        auto result = EngineLibraryCreator::create(cancelledPath.string(), tracks, EngineSchemaGeneration::V2,
                                                   reporter, cancel);
        assert(result.cancelled);
        assert(result.errorMessage.empty());
        assert(result.tracksTotal == 2);
        assert(result.tracksCreated == 1);
        assert(!fs::exists(cancelledPath));
        std::cout << "case (cancelled build: nothing created on the target) OK\n";
    }

    fs::remove_all(root);
    std::cout << "All libdjinterop_engine_library_creator tests passed.\n";

    return 0;
}
