// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// AddEngineTrackChange through the save loop, on copies of the anonymized
// fixture with both catalogs, a WAV planted in the copy's Music folder and
// a hand-built rekordbox row for it.
//
// 1. As the fixture ships (export.pdb at sequence 15132, Engine's import
//    counter at 14204): the row lands as id 1575, isAnalyzed 0 with NULL
//    analysis blobs, pdbImportKey 0, the path relative to the Engine
//    Library, its two cues at the file's own 48 kHz sample offsets, its
//    cover in. Nothing else in m.db changed but what the add and the save
//    are for: one Track row, one PerformanceData row, the Track and
//    AlbumArt sequences and one AlbumArt row for the cover, and the
//    import counter, which every save with both catalogs levels with
//    export.pdb (runSaveLoop's keepImportLevel).
// 2. Undo of that save: the backup puts m.db back byte for byte, and the
//    undo is itself a save with both catalogs, which levels the counter
//    again. So after Undo m.db is the fixture's in every row but the
//    Information row's counter. The cover file the save wrote under
//    Artwork/ is gone again: it was declared absent before the save
//    (BackupTarget::removeOnRestoreIfAbsent), so Undo removes it, and
//    Artwork/ holds what it held before.
// 3. On a copy levelled first (a stick after an earlier Seabass save),
//    save and Undo leave m.db byte for byte as it was; and again through
//    a scratch copy of m.db (a save with a large item count).
// 4. A probe that cannot read the file refuses the change, naming the
//    file: the save stops there with nothing applied and m.db byte for
//    byte as it was, and the change staged after it stays pending. A rate
//    of 0 is refused the same way.
// 5. An Engine 1.x library (m.db at the root) is refused, with no backup
//    target declared.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "gui/edit/changes/add_engine_track_change.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#if defined(SEABASS_HAVE_TAGLIB)
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

// The file's own header, through TagLib as the page will; without TagLib
// the rate the test wrote.
class FileProbe : public application::TrackMetadataProbe
{
public:
    std::optional<application::FileMetadata> read(const std::string &file) override
    {
#if defined(SEABASS_HAVE_TAGLIB)
        return infrastructure::audio::TagLibMetadataProbe().read(file);
#else
        application::FileMetadata m;
        m.sampleRate = 48000;
        m.durationSeconds = 4.0;
        return fs::exists(pathFromUtf8(file)) ? std::optional(m) : std::nullopt;
#endif
    }
};

// Answers what it was built with, for every file.
class FixedProbe : public application::TrackMetadataProbe
{
public:
    explicit FixedProbe(std::optional<int> rate) : m_rate(rate) {}
    std::optional<application::FileMetadata> read(const std::string &) override
    {
        if (!m_rate) {
            return std::nullopt;
        }
        application::FileMetadata m;
        m.sampleRate = *m_rate;
        return m;
    }

private:
    std::optional<int> m_rate;
};

// A 1x1 PNG.
void writePng(const fs::path &file)
{
    static const unsigned char Png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d,
        0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char *>(Png), sizeof(Png));
    assert(out.good());
}

// The rekordbox row a sync would copy, for a file planted on the stick.
domain::Track rekordboxRow(const testing::EngineChangeStick &stick, const std::string &name, bool withCover)
{
    const fs::path wav = stick.root / "Music" / (name + ".wav");
    testing::writeSilentWav(wav, 48000, 4);
    domain::Track t;
    t.format = "rekordbox";
    t.sourceId = "9001";
    t.title = name;
    t.artist = "Hand Built";
    t.bpm = 124.0;
    t.key = "Am";
    t.durationSeconds = 4.0;
    t.rating = 3;
    t.comment = "planted";
    t.filePath = pathToUtf8(wav);
    t.fileSizeBytes = static_cast<std::int64_t>(fs::file_size(wav));
    domain::CuePoint hot{domain::CuePoint::Kind::Hot, 1, 1000.0, "", "one"};
    domain::CuePoint memory{domain::CuePoint::Kind::Memory, 0, 2500.0, "", ""};
    t.cues = {hot, memory};
    if (withCover) {
        const fs::path png = stick.root / "Music" / (name + ".png");
        writePng(png);
        t.artworkPath = pathToUtf8(png);
    }
    return t;
}

std::set<fs::path> artworkFiles(const testing::EngineChangeStick &stick)
{
    std::set<fs::path> files;
    if (fs::exists(stick.engine / "Artwork")) {
        for (const auto &e : fs::recursive_directory_iterator(stick.engine / "Artwork")) {
            if (e.is_regular_file()) {
                files.insert(e.path());
            }
        }
    }
    return files;
}

std::string information(const std::string &db)
{
    const auto r = testing::sqlRows(db, "SELECT lastRekordBoxLibraryImportReadCounter FROM Information;");
    assert(r.size() == 1);
    return r[0][0];
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: add_engine_track_change_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "add_engine_track_home");
    const auto probe = std::make_shared<FileProbe>();

    // 1 and 2: the fixture as it ships.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "add_engine_track_shipped");
        const std::string originalBytes = testing::fileBytes(pathFromUtf8(stick.db));
        const testing::DatabaseDump original = testing::dumpDatabase(stick.db);
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Track;") == 1564);
        assert(testing::sqlScalar(stick.db, "SELECT max(id) FROM Track;") == 1574);
        assert(information(stick.db) == "14204");
        const std::set<fs::path> artworkBefore = artworkFiles(stick);

        auto change = std::make_shared<AddEngineTrackChange>(stick.enginePath(), rekordboxRow(stick, "Added Track", true),
                                                             probe, 1);
        assert(change->id() == QStringLiteral("rekordbox-export-sync:engine-add:") + QString::fromStdString(pathToUtf8(stick.root / "Music" / "Added Track.wav")));
        assert(change->owner() == QStringLiteral("rekordbox-export-sync"));
        assert(change->unit() == QStringLiteral("tracks") && change->verb() == QStringLiteral("added"));
        assert(change->description() == QStringLiteral("Add \"Added Track, Hand Built\" to Engine"));
        const SaveLoopResult saved = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty() && saved.warning.isEmpty() && saved.skippedIds.isEmpty());
        assert(saved.appliedIds == QStringList{change->id()});
        assert(change->createdId() == 1575 && "the next id after the fixture's 1574");
        assert(change->coverProblem().empty());

        const std::string id = "1575";
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Track;") == 1565);
        const auto row = testing::sqlRows(stick.db, "SELECT path, filename, title, artist, isAnalyzed, pdbImportKey, rating, "
                                                    "albumArtId IS NOT NULL FROM Track WHERE id = " + id + ";");
        assert(row.size() == 1);
        assert(row[0][0] == "../Music/Added Track.wav" && row[0][1] == "Added Track.wav");
        assert(row[0][2] == "Added Track" && row[0][3] == "Hand Built");
        assert(row[0][4] == "0" && "left for the player to analyse");
        assert(row[0][5] == "0" && "no player import is faked");
        assert(row[0][6] == "60" && "three stars, Engine's scale times 20");
        const auto blobs = testing::sqlRows(stick.db, "SELECT trackData, overviewWaveFormData, beatData FROM PerformanceData "
                                                      "WHERE trackId = " + id + ";");
        assert(blobs.size() == 1 && blobs[0][0] == "<null>" && blobs[0][1] == "<null>" && blobs[0][2] == "<null>");
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Track WHERE isAnalyzed = 1;") == 350
               && "the player's analysed rows stay analysed");
        {
            // The cues at the file's own rate: 1000 ms is 48000 samples at
            // 48 kHz (44100 if the rate had been guessed), the memory cue
            // on the first free pad and as the main cue.
            auto engine = djinterop::engine::load_database(stick.engineUtf8());
            const auto t = engine.track_by_id(1575);
            assert(t);
            const auto hot = t->hot_cues();
            assert(hot[0] && hot[0]->sample_offset == 48000.0 && hot[0]->label == "one");
            assert(hot[1] && hot[1]->sample_offset == 120000.0 && "2500 ms at 48 kHz, on pad 2");
            for (size_t i = 2; i < hot.size(); ++i) {
                assert(!hot[i]);
            }
            assert(t->main_cue() == std::optional<double>(120000.0));
        }
        {
            // And Seabass reads the row back with both cues on their pads.
            infrastructure::engine::LibdjinteropEngineReader reader(stick.engineUtf8());
            const auto tracks = reader.readAll();
            assert(tracks.size() == 1565);
            const domain::Track *added = nullptr;
            for (const auto &t : tracks) {
                if (t.sourceId == id) {
                    added = &t;
                }
            }
            assert(added && added->title == "Added Track" && added->filePath == pathToUtf8(stick.root / "Music" / "Added Track.wav"));
            int pads = 0;
            for (const auto &c : added->cues) {
                pads += c.kind == domain::CuePoint::Kind::Hot && (c.hotCueNumber == 1 || c.hotCueNumber == 2) ? 1 : 0;
            }
            assert(pads == 2);
        }
        // The cover: one file under the real library's Artwork/.
        std::vector<fs::path> covers;
        for (const auto &file : artworkFiles(stick)) {
            if (!artworkBefore.count(file)) {
                covers.push_back(file);
            }
        }
        assert(covers.size() == 1 && "the cover went into Artwork/ on the stick");

        // Nothing else changed: every difference is one of these.
        const auto diffs = testing::differences(original, testing::dumpDatabase(stick.db));
        std::map<std::string, std::pair<size_t, size_t>> counts;
        for (const auto &d : diffs) {
            counts[d.table] = {d.onlyBefore.size(), d.onlyAfter.size()};
        }
        if (counts != std::map<std::string, std::pair<size_t, size_t>>{{"AlbumArt", {0, 1}},
                                                                        {"Information", {1, 1}},
                                                                        {"PerformanceData", {0, 1}},
                                                                        {"Track", {0, 1}},
                                                                        {"sqlite_sequence", {2, 2}}}) {
            testing::printDifferences(diffs);
            assert(false && "only the new row, its cover, the sequences and the counter changed");
        }
        assert(information(stick.db) == "15132" && "levelled with export.pdb by the save");
        std::cout << "case 1 (the row lands as id 1575, unanalysed, cues at 48 kHz, cover in; nothing else changed "
                     "but the counter the save levels) OK\n";

        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        assert(undone.error.isEmpty() && undone.appliedIds.size() == 1);
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Track;") == 1564);
        const auto afterUndo = testing::differences(original, testing::dumpDatabase(stick.db));
        if (afterUndo.size() != 1 || afterUndo[0].table != "Information") {
            testing::printDifferences(afterUndo);
            assert(false && "after Undo only the Information row differs");
        }
        assert(information(stick.db) == "15132" && "the undo's own save levelled the counter again");
        assert(testing::fileBytes(pathFromUtf8(stick.db)) != originalBytes);
        if (artworkFiles(stick) != artworkBefore) {
            for (const auto &file : artworkFiles(stick)) {
                if (!artworkBefore.count(file)) {
                    std::cerr << "  left under Artwork/: " << pathToUtf8(file) << "\n";
                }
            }
            assert(false && "after Undo no file under Artwork/ that was not there before");
        }
        std::cout << "case 2 (Undo: every row back but the counter, which the undo's own save levels; the cover "
                     "file it wrote is gone) OK\n";
    }

    // 3. A stick already level: save and Undo, byte for byte.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "add_engine_track_level");
        std::string error;
        assert(infrastructure::engine::markRekordboxLibraryImported(stick.engineUtf8(), 15132, &error));
        const std::string before = testing::fileBytes(pathFromUtf8(stick.db));
        auto change = std::make_shared<AddEngineTrackChange>(stick.enginePath(), rekordboxRow(stick, "Level", false),
                                                             probe, 1);
        const SaveLoopResult saved = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
        assert(saved.error.isEmpty() && saved.appliedIds.size() == 1 && change->createdId() == 1575);
        assert(testing::fileBytes(pathFromUtf8(stick.db)) != before);
        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        assert(undone.error.isEmpty());
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == before && "Undo puts m.db back byte for byte");
        std::cout << "case 3 (on a level stick, Undo restores m.db byte for byte) OK\n";

        // A save big enough to go through a scratch copy of m.db: the row
        // is written there, committed with the save, and read back on the
        // stick's own path; Undo is byte for byte again.
        auto big = std::make_shared<AddEngineTrackChange>(stick.enginePath(), rekordboxRow(stick, "Scratch", false),
                                                          probe, 5000);
        const SaveLoopResult viaScratch = testing::saveChanges({big}, stick.pioneerPath(), stick.enginePath());
        if (!viaScratch.error.isEmpty()) {
            std::cerr << "save: " << viaScratch.error.toStdString() << "\n";
        }
        // 1575 again: Undo put sqlite_sequence back with the rest of the
        // file, so an id an undone save handed out is handed out again.
        assert(viaScratch.error.isEmpty() && big->createdId() == 1575);
        const std::string log = testing::fileBytes(infrastructure::paths::stickOperationLog(stick.root));
        assert(log.find("rekordbox-export-sync: applying up to 5000 engine update(s) to a local scratch copy first")
                   != std::string::npos
               && "the save wrote a scratch copy");
        assert(testing::sqlRows(stick.db, "SELECT path FROM Track WHERE id = 1575;")
               == std::vector<std::vector<std::string>>{{"../Music/Scratch.wav"}});
        assert(testing::undoSave(viaScratch, stick.pioneerPath(), stick.enginePath()).error.isEmpty());
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == before);
        std::cout << "case 3b (through a scratch copy of m.db, the row lands on the stick; Undo byte for byte) OK\n";
    }

    // 4. The probe cannot say: refused, naming the file, nothing written.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "add_engine_track_unprobed");
        const std::string before = testing::fileBytes(pathFromUtf8(stick.db));
        const domain::Track unreadable = rekordboxRow(stick, "Unreadable", false);
        auto refused = std::make_shared<AddEngineTrackChange>(stick.enginePath(), unreadable,
                                                              std::make_shared<FixedProbe>(std::nullopt), 2);
        auto after = std::make_shared<AddEngineTrackChange>(stick.enginePath(), rekordboxRow(stick, "After", false),
                                                            probe, 2);
        const SaveLoopResult saved = testing::saveChanges({refused, after}, QString(), stick.enginePath());
        assert(saved.failedId == refused->id());
        assert(saved.error.contains(QString::fromStdString(unreadable.filePath)) && "the message names the file");
        assert(saved.error.contains(QStringLiteral("sample rate")));
        assert(saved.appliedIds.isEmpty() && "nothing landed, and the change after it is still pending");
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == before && "m.db as it was");
        assert(refused->createdId() == -1 && after->createdId() == -1);

        auto zero = std::make_shared<AddEngineTrackChange>(stick.enginePath(), unreadable,
                                                           std::make_shared<FixedProbe>(0), 1);
        const SaveLoopResult zeroSaved = testing::saveChanges({zero}, QString(), stick.enginePath());
        assert(zeroSaved.failedId == zero->id() && zeroSaved.error.contains(QStringLiteral("sample rate")));
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == before);
        std::cout << "case 4 (no sample rate: refused naming the file, nothing written, the next change pending) OK\n";

        bool threw = false;
        try {
            AddEngineTrackChange none(stick.enginePath(), unreadable, nullptr, 1);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        assert(threw && "a probe is required");
    }

    // 5. Engine 1.x: m.db at the library's root, no Database2.
    {
        const fs::path root = testing::scratchRoot() / "add_engine_track_v1";
        fs::remove_all(root);
        fs::create_directories(root / "Engine Library");
        fs::copy_file(fixture / "engine" / "Database2" / "m.db", root / "Engine Library" / "m.db");
        const QString library = pathToQString(root / "Engine Library");
        auto change = std::make_shared<AddEngineTrackChange>(library, domain::Track{}, probe, 1);
        application::CancellationToken token;
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, QString(), library);
        assert(change->filesToBackup(ctx).empty());
        const SaveLoopResult saved = runSaveLoop({change}, ctx);
        assert(saved.failedId == change->id() && saved.error.contains(QStringLiteral("Engine 1.x")));
        std::cout << "case 5 (an Engine 1.x library is refused) OK\n";
    }

    std::cout << "add_engine_track_change_test: all cases passed\n";
    return 0;
}
