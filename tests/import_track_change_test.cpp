// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// ImportTrackChange through the save loop, on a copy of the anonymized
// fixture as a stick with all three libraries (export.pdb,
// exportLibrary.db, Engine's m.db) and a file on "the computer" beside
// it:
// 1. the file lands on the stick, byte for byte, and every library's own
//    reader lists the track with what the one description said;
// 2. Undo Last Save takes the rows and the file away again;
// 3. a library that refuses its row after the file was copied and
//    export.pdb written fails the change, and the save loop puts all of
//    it back: no file, no row anywhere;
// 4. a refusal known before anything is written writes nothing; a file
//    already at the destination is not replaced.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "domain/imported_track.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

// A file on the computer: 300 KB that are no two the same in a row, so a
// copy that lost or repeated a block is not the same bytes.
fs::path makeSource(const fs::path &folder, const std::string &name)
{
    fs::create_directories(folder);
    const fs::path file = folder / name;
    std::string bytes;
    for (int i = 0; i < 300 * 1024; ++i) {
        bytes.push_back(static_cast<char>((i * 31 + i / 251) & 0xFF));
    }
    std::ofstream(file, std::ios::binary) << bytes;
    return file;
}

domain::ImportedTrack description(const std::string &stickPath)
{
    domain::TrackTags tags;
    tags.title = "Imported S\xc3\xb6ng";
    tags.artist = "Hand Built";
    tags.album = "From Disk";
    tags.genre = "Techno";
    tags.label = "A Label";
    tags.key = "Am";
    tags.comment = "from a tag";
    tags.bpm = 126.5;
    tags.year = 2024;
    tags.trackNumber = 4;
    domain::AudioFormat audio;
    audio.durationSeconds = 200.5;
    audio.bitrate = 900;
    audio.sampleRateHz = 48000;
    audio.bitsPerSample = 24;
    return domain::describeImportedTrack(tags, audio, stickPath, "2026-11-25");
}

template <typename Reader>
const domain::Track *findByPath(const std::vector<domain::Track> &tracks, const std::string &file)
{
    const domain::Track *found = nullptr;
    for (const auto &t : tracks) {
        if (t.filePath == file) {
            assert(found == nullptr && "one row for the file");
            found = &t;
        }
    }
    return found;
}

// The file_size field of the present track row with this id, read off
// export.pdb's pages; 0 when there is no such row.
std::uint32_t pdbFileSizeOf(const std::string &pdb, std::uint32_t id)
{
    const auto u16 = [&](size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(pdb[at]) | (static_cast<unsigned char>(pdb[at + 1]) << 8));
    };
    const auto u32 = [&](size_t at) { return u16(at) | (u16(at + 2) << 16); };
    const size_t lenPage = u32(4);
    for (std::uint32_t t = 0; t < u32(8); ++t) {
        const size_t entry = 28 + 16 * t;
        if (u32(entry) != 0) {
            continue;
        }
        const std::uint32_t last = u32(entry + 12);
        for (std::uint32_t page = u32(entry + 8);; page = u32(lenPage * page + 12)) {
            const size_t base = lenPage * page;
            const bool dataPage = (static_cast<unsigned char>(pdb[base + 27]) & 0x40) == 0;
            const std::uint32_t offsets = u32(base + 24) & 0x1FFF;
            for (std::uint32_t r = 0; dataPage && r < offsets; ++r) {
                const size_t group = base + lenPage - (r / 16) * 0x24;
                const size_t row = base + 40 + u16(group - 6 - 2 * (r % 16));
                if (((u16(group - 4) >> (r % 16)) & 1) && u32(row + 72) == id) {
                    return u32(row + 16);
                }
            }
            if (page == last) {
                break;
            }
        }
    }
    return 0;
}

struct Counts
{
    size_t deviceLibrary = 0;
    size_t oneLibrary = 0;
    size_t engine = 0;
    bool operator==(const Counts &) const = default;
};

Counts countsOf(const testing::EngineChangeStick &stick)
{
    Counts counts;
    counts.deviceLibrary = infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll().size();
    counts.oneLibrary = infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)).readAll().size();
    counts.engine = infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()).readAll().size();
    return counts;
}

const QStringList AllThree{QStringLiteral("rekordbox"), QStringLiteral("onelibrary"), QStringLiteral("engine")};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: import_track_change_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "import_track_home");
    const fs::path computer = testing::scratchRoot() / "import_track_computer";
    fs::remove_all(computer);
    const fs::path source = makeSource(computer, "song from disk.flac");
    const std::string sourceBytes = testing::fileBytes(source);
    const std::string stickPath = "/Contents/Hand Built/From Disk/04 Imported S\xc3\xb6ng.flac";

    // 1 and 2: the import, and its Undo.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "import_track_stick");
        testing::levelImportCounter(stick);
        const fs::path pdb = stick.pioneer / "rekordbox" / "export.pdb";
        const std::string pdbBefore = testing::fileBytes(pdb);
        const Counts before = countsOf(stick);
        assert(before.deviceLibrary > 1000 && before.oneLibrary > 1000 && before.engine > 1000);
        const fs::path destination = stick.root / "Contents" / "Hand Built" / "From Disk" / pathFromUtf8("04 Imported S\xc3\xb6ng.flac");
        assert(!fs::exists(destination));

        auto change = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                          stick.enginePath(), description(stickPath), AllThree, 1);
        assert(change->id() == QStringLiteral("addcue:import-track:") + QString::fromStdString(stickPath));
        assert(change->owner() == QStringLiteral("addcue") && "staged beside Browse's playlist edits");
        assert(change->unit() == QStringLiteral("tracks") && change->verb() == QStringLiteral("imported"));
        assert(change->description() == QString::fromUtf8("Import \"Imported S\xc3\xb6ng, Hand Built\""));
        assert(change->unitKeys().contains(ImportTrackChange::unitKeyFor(stickPath)));
        assert(change->formatsTouched() == AllThree);
        assert(change->destinationFile() == pathToUtf8(destination));

        const SaveLoopResult saved = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty() && saved.skippedIds.isEmpty() && saved.appliedIds == QStringList{change->id()});
        assert(testing::fileBytes(destination) == sourceBytes && "the file is on the stick, byte for byte");
        assert(testing::fileBytes(source) == sourceBytes && "and the one on the computer is as it was");
        assert(change->deviceLibraryId() > 0 && change->oneLibraryId() > 0 && change->engineId() > 0);

        const Counts after = countsOf(stick);
        assert(after.deviceLibrary == before.deviceLibrary + 1 && after.oneLibrary == before.oneLibrary + 1
               && after.engine == before.engine + 1);
        const std::string file = pathToUtf8(destination);
        {
            const auto tracks = infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll();
            const domain::Track *t = findByPath<void>(tracks, file);
            assert(t && t->title == "Imported S\xc3\xb6ng" && t->artist == "Hand Built" && t->album == "From Disk");
            assert(t->bpm == 126.5 && t->key == "Am" && t->durationSeconds == 200.0 && t->comment == "from a tag");
            assert(t->sourceId == std::to_string(change->deviceLibraryId()));
            assert(pdbFileSizeOf(testing::fileBytes(pdb), change->deviceLibraryId()) == sourceBytes.size()
                   && "the size is the copy's");
        }
        {
            const auto tracks = infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)).readAll();
            const domain::Track *t = findByPath<void>(tracks, file);
            assert(t && t->title == "Imported S\xc3\xb6ng" && t->artist == "Hand Built" && t->album == "From Disk");
            assert(t->bpm == 126.5 && t->durationSeconds == 200.0);
            assert(t->sourceId == std::to_string(change->oneLibraryId()));
            infrastructure::onelibrary::SqlCipherLibrary lib;
            infrastructure::onelibrary::SqlCipherDb raw(lib, pathToUtf8(stick.pioneer / "rekordbox" / "exportLibrary.db"), true);
            raw.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
            infrastructure::onelibrary::SqlCipherStatement size(raw, "SELECT fileSize FROM content WHERE content_id = ?");
            size.bindInt64(1, change->oneLibraryId());
            assert(size.step() && size.columnInt64(0) == static_cast<int64_t>(sourceBytes.size()));
        }
        {
            const auto tracks = infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()).readAll();
            const domain::Track *t = findByPath<void>(tracks, file);
            assert(t && t->title == "Imported S\xc3\xb6ng" && t->artist == "Hand Built" && t->album == "From Disk");
            // Engine keeps a length in whole seconds.
            assert(t->bpm == 126.5 && t->key == "Am" && t->durationSeconds == 200.0 && t->comment == "from a tag");
            const std::string id = std::to_string(change->engineId());
            const auto row = testing::sqlRows(stick.db, "SELECT genre, label, year, playOrder, isAnalyzed, fileBytes, path "
                                                        "FROM Track WHERE id = " + id + ";");
            assert(row.size() == 1 && row[0][0] == "Techno" && row[0][1] == "A Label" && row[0][2] == "2024" && row[0][3] == "4");
            assert(row[0][4] == "0" && "left for the player to analyse");
            assert(row[0][5] == std::to_string(sourceBytes.size()) && "the size is the copy's, not a tag's");
            assert(row[0][6] == "../Contents/Hand Built/From Disk/04 Imported S\xc3\xb6ng.flac");
        }
        std::cout << "case 1 (the file on the stick byte for byte; the track in all three libraries, the same in each) OK\n";

        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        assert(undone.error.isEmpty());
        assert(!fs::exists(destination) && "Undo takes the copied file away again");
        assert(countsOf(stick) == before);
        assert(testing::fileBytes(pdb) == pdbBefore && "export.pdb is its own bytes again");
        assert(testing::fileBytes(source) == sourceBytes);
        std::cout << "case 2 (Undo: no file on the stick, no row in any library) OK\n";
    }

    // 3. OneLibrary refuses its row (a schema this build does not know),
    //    after the copy and after export.pdb's row: the change fails and
    //    all of it is put back.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "import_track_refused");
        testing::levelImportCounter(stick);
        {
            infrastructure::onelibrary::SqlCipherLibrary lib;
            infrastructure::onelibrary::SqlCipherDb raw(
                lib, pathToUtf8(stick.pioneer / "rekordbox" / "exportLibrary.db"), false);
            raw.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
            raw.exec("ALTER TABLE content ADD COLUMN somethingNew integer;");
        }
        const fs::path pdb = stick.pioneer / "rekordbox" / "export.pdb";
        const std::string pdbBefore = testing::fileBytes(pdb);
        const Counts before = countsOf(stick);
        const fs::path destination = stick.root / "Contents" / "Hand Built" / "From Disk" / pathFromUtf8("04 Imported S\xc3\xb6ng.flac");
        auto change = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                          stick.enginePath(), description(stickPath), AllThree, 1);
        const SaveLoopResult saved = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
        assert(!saved.error.isEmpty() && saved.appliedIds.isEmpty());
        assert(saved.error.contains(QStringLiteral("content table")) && "the refusal is OneLibrary's own");
        assert(!fs::exists(destination) && "the copied file is taken back");
        assert(testing::fileBytes(pdb) == pdbBefore && "export.pdb's row is taken back");
        assert(countsOf(stick) == before);
        std::cout << "case 3 (a library that refuses its row fails the change: no file, no row anywhere) OK\n";
    }

    // 4. Refused before anything is written; and a file at the
    //    destination is not replaced.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "import_track_early");
        testing::levelImportCounter(stick);
        const Counts before = countsOf(stick);
        const fs::path destination = stick.root / "Contents" / "Hand Built" / "From Disk" / pathFromUtf8("04 Imported S\xc3\xb6ng.flac");
        domain::ImportedTrack noRate = description(stickPath);
        noRate.sampleRateHz = 0;
        auto unprobed = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                            stick.enginePath(), noRate, AllThree, 1);
        const SaveLoopResult early = testing::saveChanges({unprobed}, stick.pioneerPath(), stick.enginePath());
        assert(!early.error.isEmpty() && early.error.contains(QStringLiteral("sample rate")));
        assert(!fs::exists(destination) && countsOf(stick) == before);
        // Refused before the copy, not copied and taken back: the folder
        // the copy would have made was never there.
        assert(!fs::exists(stick.root / "Contents" / "Hand Built") && "nothing was copied");

        // What a library's writer would refuse the row for is asked
        // before the copy too: a comment that does not fit export.pdb's
        // page, a file type the Pioneer libraries have no number for.
        domain::ImportedTrack wordy = description(stickPath);
        for (int i = 0; i < 2500; ++i) {
            wordy.comment += "\xc3\xa9";
        }
        auto tooLong = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                           stick.enginePath(), wordy, AllThree, 1);
        const SaveLoopResult long1 = testing::saveChanges({tooLong}, stick.pioneerPath(), stick.enginePath());
        assert(!long1.error.isEmpty() && long1.error.contains(QStringLiteral("do not fit a page")));
        assert(!fs::exists(stick.root / "Contents" / "Hand Built") && countsOf(stick) == before);
        auto ogg = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                       stick.enginePath(), description("/Contents/Hand Built/x.ogg"), AllThree, 1);
        const SaveLoopResult ogg1 = testing::saveChanges({ogg}, stick.pioneerPath(), stick.enginePath());
        assert(!ogg1.error.isEmpty() && ogg1.error.contains(QStringLiteral("no file type known")));
        assert(!fs::exists(stick.root / "Contents" / "Hand Built") && countsOf(stick) == before);

        // The libraries named have to be the stick's: one left out, and
        // the two written would say what the third does not.
        auto partial = std::make_shared<ImportTrackChange>(
            pathToQString(source), pathToQString(stick.root), stick.pioneerPath(), stick.enginePath(), description(stickPath),
            QStringList{QStringLiteral("rekordbox"), QStringLiteral("engine")}, 1);
        const SaveLoopResult part = testing::saveChanges({partial}, stick.pioneerPath(), stick.enginePath());
        assert(!part.error.isEmpty() && part.error.contains(QStringLiteral("not the ones this import was planned for")));
        assert(!fs::exists(stick.root / "Contents" / "Hand Built") && countsOf(stick) == before);

        fs::create_directories(destination.parent_path());
        std::ofstream(destination, std::ios::binary) << "somebody's own file";
        auto change = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                          stick.enginePath(), description(stickPath), AllThree, 1);
        const SaveLoopResult occupied = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
        assert(!occupied.error.isEmpty() && occupied.error.contains(QStringLiteral("not replaced")));
        assert(testing::fileBytes(destination) == "somebody's own file" && countsOf(stick) == before);

        // A file that is not there on the computer.
        fs::remove(destination);
        auto missing = std::make_shared<ImportTrackChange>(pathToQString(computer / "never was.flac"), pathToQString(stick.root),
                                                           stick.pioneerPath(), stick.enginePath(), description(stickPath),
                                                           AllThree, 1);
        const SaveLoopResult gone = testing::saveChanges({missing}, stick.pioneerPath(), stick.enginePath());
        assert(!gone.error.isEmpty() && !fs::exists(destination) && countsOf(stick) == before);

        // Not a change at all: no library, a library without its folder,
        // an unknown one, a track without a place.
        const auto refusedAtStaging = [&](const QStringList &libraries, const QString &pioneer, const QString &engine,
                                          const std::string &path) {
            domain::ImportedTrack t = description(stickPath);
            t.stickPath = path;
            try {
                ImportTrackChange c(pathToQString(source), pathToQString(stick.root), pioneer, engine, t, libraries, 1);
            } catch (const std::invalid_argument &) {
                return true;
            }
            return false;
        };
        assert(refusedAtStaging({}, stick.pioneerPath(), stick.enginePath(), stickPath));
        assert(refusedAtStaging({QStringLiteral("engine")}, stick.pioneerPath(), QString(), stickPath));
        assert(refusedAtStaging({QStringLiteral("rekordbox")}, QString(), stick.enginePath(), stickPath));
        assert(refusedAtStaging({QStringLiteral("traktor")}, stick.pioneerPath(), stick.enginePath(), stickPath));
        assert(refusedAtStaging(AllThree, stick.pioneerPath(), stick.enginePath(), "Contents/a.flac"));
        // A place that would be beside the stick, or spelled two ways.
        assert(refusedAtStaging(AllThree, stick.pioneerPath(), stick.enginePath(), "/../outside.flac"));
        assert(refusedAtStaging(AllThree, stick.pioneerPath(), stick.enginePath(), "//Contents/a.flac"));
        assert(refusedAtStaging(AllThree, stick.pioneerPath(), stick.enginePath(), "/Contents\\a.flac"));
        assert(!refusedAtStaging(AllThree, stick.pioneerPath(), stick.enginePath(), stickPath));
    }
    std::cout << "case 4 (no sample rate, an occupied destination, a missing source: nothing written; a change that "
                 "could not be one is refused when staged) OK\n";

    // 5. Twenty files in one save, which is enough for the save to write
    //    scratch copies of the databases and put them in place at the
    //    end: all of them in every library, the second with a Camelot key
    //    that Engine holds as the same key; then Undo.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "import_track_many");
        testing::levelImportCounter(stick);
        const Counts before = countsOf(stick);
        std::vector<std::shared_ptr<PendingChange>> changes;
        std::vector<fs::path> destinations;
        for (int i = 0; i < 20; ++i) {
            const std::string name = "many " + std::to_string(i) + ".flac";
            domain::ImportedTrack t = description("/Contents/Many/" + name);
            t.title = "Many " + std::to_string(i);
            if (i == 1) {
                t.key = "8A";
            }
            destinations.push_back(stick.root / "Contents" / "Many" / name);
            changes.push_back(std::make_shared<ImportTrackChange>(pathToQString(makeSource(computer, name)),
                                                                  pathToQString(stick.root), stick.pioneerPath(),
                                                                  stick.enginePath(), t, AllThree, 20));
        }
        const SaveLoopResult saved = testing::saveChanges(changes, stick.pioneerPath(), stick.enginePath());
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty() && saved.appliedIds.size() == 20);
        const Counts after = countsOf(stick);
        assert(after.deviceLibrary == before.deviceLibrary + 20 && after.oneLibrary == before.oneLibrary + 20
               && after.engine == before.engine + 20);
        for (const fs::path &destination : destinations) {
            assert(testing::fileBytes(destination) == sourceBytes);
        }
        {
            const auto engine = infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()).readAll();
            const domain::Track *camelot = findByPath<void>(engine, pathToUtf8(destinations[1]));
            assert(camelot && camelot->key == "Am" && "8A is A minor, and Engine holds it");
            const auto pioneer = infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll();
            const domain::Track *asTagged = findByPath<void>(pioneer, pathToUtf8(destinations[1]));
            assert(asTagged && asTagged->key == "8A" && "rekordbox's row keeps the tag's spelling");
        }
        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        assert(undone.error.isEmpty() && countsOf(stick) == before);
        for (const fs::path &destination : destinations) {
            assert(!fs::exists(destination));
        }
    }
    std::cout << "case 5 (twenty imports in one save, through scratch copies; a Camelot key in Engine; Undo) OK\n";

    // 6. A change that fails takes the save back: the track imported
    //    before it is gone again, file and rows. The second file's place
    //    is taken between staging and saving.
    {
        const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "import_track_second_fails");
        testing::levelImportCounter(stick);
        const Counts before = countsOf(stick);
        const fs::path first = stick.root / "Contents" / "Two" / "first.flac";
        const fs::path second = stick.root / "Contents" / "Two" / "second.flac";
        auto one = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                       stick.enginePath(), description("/Contents/Two/first.flac"), AllThree, 2);
        // Occupied by a folder: nothing a copy could replace.
        fs::create_directories(second);
        auto two = std::make_shared<ImportTrackChange>(pathToQString(source), pathToQString(stick.root), stick.pioneerPath(),
                                                       stick.enginePath(), description("/Contents/Two/second.flac"), AllThree, 2);
        const SaveLoopResult saved = testing::saveChanges({one, two}, stick.pioneerPath(), stick.enginePath());
        assert(!saved.error.isEmpty() && saved.error.contains(QStringLiteral("not replaced")));
        assert(!fs::exists(first) && "the first track's file went with the save");
        // What was at the second place is not the save's to remove.
        assert(fs::is_directory(second) && countsOf(stick) == before);
    }
    std::cout << "case 6 (the second import fails: the first is taken back with it) OK\n";

    std::cout << "import_track_change_test: all cases passed\n";
    return 0;
}
