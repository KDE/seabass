// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// stageTrackImport: a plan of files turned into the changes of one save,
// and that save run on a copy of the anonymized fixture as a stick with
// all three libraries. The tags and audio properties come from readers
// of this test's own (the staging is what is tested, not TagLib).
// 1. Files a library could not list are left out with their reason, and
//    the rest are staged; a long comment is cut rather than costing the
//    track.
// 2. The save: a new playlist, the tracks in it in the order offered, in
//    every library.
// 3. A second import into that playlist at its top.
// 4. Undo.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/track_import_staging.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "engine_change_fixture.hpp"

#include <djinterop/djinterop.hpp>

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

// What the test says each source file's tags and audio are.
struct Fake
{
    domain::TrackTags tags;
    application::FileMetadata audio;
    bool readable = true;
};
std::map<std::string, Fake> files;

class FakeTags : public application::TrackTagReader
{
public:
    std::optional<domain::TrackTags> read(const std::string &file) override
    {
        const auto found = files.find(file);
        return found == files.end() || !found->second.readable ? std::nullopt : std::optional(found->second.tags);
    }
};

class FakeProbe : public application::TrackMetadataProbe
{
public:
    std::optional<application::FileMetadata> read(const std::string &file) override
    {
        const auto found = files.find(file);
        return found == files.end() || !found->second.readable ? std::nullopt : std::optional(found->second.audio);
    }
};

// One file on the computer with its made-up tags, and its plan entry.
domain::ImportPlanEntry offer(const fs::path &computer, const std::string &name, const std::string &title,
                              const std::string &destination)
{
    fs::create_directories(computer);
    const fs::path file = computer / name;
    std::ofstream(file, std::ios::binary) << "audio of " << name << std::string(4000, 'x');
    Fake fake;
    fake.tags.title = title;
    fake.tags.artist = "Stager";
    fake.tags.album = "Staged";
    fake.tags.bpm = 124.0;
    fake.audio.durationSeconds = 100.5;
    fake.audio.bitrate = 320;
    fake.audio.sampleRate = 44100;
    files[pathToUtf8(file)] = fake;
    domain::ImportPlanEntry entry;
    entry.offered.file.filePath = pathToUtf8(file);
    entry.offered.file.fileSizeBytes = fs::file_size(file);
    entry.offered.readable = true;
    entry.verdict = domain::ImportVerdict::Import;
    entry.destination = destination;
    return entry;
}

// The files of the playlist `name` in one library, in order, as the
// reader of that library lists them.
template <typename Reader>
std::vector<std::string> playlistOf(Reader &&reader, const std::string &name)
{
    std::map<int, std::string> byPosition;
    for (const auto &track : reader.readAll()) {
        for (const auto &membership : track.playlists) {
            if (membership.name == name) {
                assert(byPosition.emplace(membership.position, track.filePath).second && "one track per place");
            }
        }
    }
    std::vector<std::string> ordered;
    for (const auto &[position, file] : byPosition) {
        ordered.push_back(file);
    }
    return ordered;
}

const QStringList AllThree{QStringLiteral("rekordbox"), QStringLiteral("onelibrary"), QStringLiteral("engine")};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: track_import_staging_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "track_import_staging_home");
    const fs::path computer = testing::scratchRoot() / "track_import_staging_computer";
    fs::remove_all(computer);
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "track_import_staging_stick");
    testing::levelImportCounter(stick);
    ImportTarget target{pathToQString(stick.root), stick.pioneerPath(), stick.enginePath(), AllThree};
    FakeTags tags;
    FakeProbe probe;
    const auto onStick = [&](const std::string &destination) { return pathToUtf8(stick.root / pathFromUtf8(destination)); };

    // 1. Six entries: three good ones, one the plan already settled, one
    //    that cannot be read, one whose title no page holds; and one with
    //    a comment far too long, which is cut.
    domain::ImportPlan plan;
    plan.entries.push_back(offer(computer, "one.mp3", "One", "Contents/Stager/Staged/one.mp3"));
    plan.entries.push_back(offer(computer, "two.mp3", "Two", "Contents/Stager/Staged/two.mp3"));
    plan.entries.push_back(offer(computer, "settled.mp3", "Settled", "Contents/Stager/Staged/settled.mp3"));
    plan.entries.back().verdict = domain::ImportVerdict::AlreadyOnStick;
    plan.entries.push_back(offer(computer, "broken.mp3", "Broken", "Contents/Stager/Staged/broken.mp3"));
    files[plan.entries.back().offered.file.filePath].readable = false;
    plan.entries.push_back(offer(computer, "endless.mp3", "", "Contents/Stager/Staged/endless.mp3"));
    for (int i = 0; i < 2500; ++i) {
        files[plan.entries.back().offered.file.filePath].tags.title += "\xc3\xa9";
    }
    plan.entries.push_back(offer(computer, "wordy.mp3", "Wordy", "Contents/Stager/Staged/wordy.mp3"));
    for (int i = 0; i < 2500; ++i) {
        files[plan.entries.back().offered.file.filePath].tags.comment += "\xc3\xa9";
    }
    plan.entries.push_back(offer(computer, "norate.mp3", "No Rate", "Contents/Stager/Staged/norate.mp3"));
    files[plan.entries.back().offered.file.filePath].audio.sampleRate = 0;

    const StagedImport staged =
        stageTrackImport(plan, target, tags, probe, "2026-11-25", ImportPlaylist{"Staged Import", true, std::nullopt});
    assert(staged.staged.size() == 3 && staged.leftOut.size() == 3);
    assert(staged.leftOut[0].sourceFile == pathToUtf8(computer / "broken.mp3")
           && staged.leftOut[0].why.find("could not be read") != std::string::npos);
    assert(staged.leftOut[1].sourceFile == pathToUtf8(computer / "endless.mp3")
           && staged.leftOut[1].why.find("do not fit a page") != std::string::npos);
    assert(staged.leftOut[2].sourceFile == pathToUtf8(computer / "norate.mp3")
           && staged.leftOut[2].why.find("sample rate") != std::string::npos);
    // The playlist first, then import and entry per file, tied together.
    assert(staged.changes.size() == 1 + 3 * 2);
    assert(std::dynamic_pointer_cast<CreatePlaylistChange>(staged.changes[0]) && staged.refusal.empty());
    // The new playlist is one unit with the first import: a cancel
    // before it would otherwise leave the playlist behind, empty.
    assert(staged.changes[0]->unitKeys().contains(staged.changes[1]->unitKeys().last())
           && !staged.changes[0]->unitKeys().contains(staged.changes[3]->unitKeys().last()));
    for (size_t i = 1; i < staged.changes.size(); i += 2) {
        const auto import = std::dynamic_pointer_cast<ImportTrackChange>(staged.changes[i]);
        const auto entry = std::dynamic_pointer_cast<AddToPlaylistChange>(staged.changes[i + 1]);
        assert(import && entry && entry->playlistEdited() == QStringLiteral("Staged Import"));
        bool shared = false;
        for (const QString &key : import->unitKeys()) {
            shared = shared || (key != import->id() && entry->unitKeys().contains(key));
        }
        assert(shared && "a cancel keeps a track and its playlist entry together");
        assert(import->owner() == entry->owner() && "one page's save");
    }
    std::cout << "case 1 (three staged with their playlist entries; an unreadable file, a title no page holds and a "
                 "file without a sample rate left out, each with its reason) OK\n";

    // 2. The save.
    const std::vector<std::string> expected = {onStick("Contents/Stager/Staged/one.mp3"), onStick("Contents/Stager/Staged/two.mp3"),
                                               onStick("Contents/Stager/Staged/wordy.mp3")};
    const SaveLoopResult saved = testing::saveChanges(staged.changes, stick.pioneerPath(), stick.enginePath());
    if (!saved.error.isEmpty()) {
        std::cerr << "save: " << saved.error.toStdString() << "\n";
    }
    assert(saved.error.isEmpty() && saved.skippedIds.isEmpty() && saved.appliedIds.size() == 7);
    assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "Staged Import") == expected);
    assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "Staged Import") == expected);
    assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "Staged Import") == expected);
    assert(!fs::exists(stick.root / "Contents" / "Stager" / "Staged" / "broken.mp3")
           && !fs::exists(stick.root / "Contents" / "Stager" / "Staged" / "endless.mp3")
           && !fs::exists(stick.root / "Contents" / "Stager" / "Staged" / "settled.mp3"));
    {
        // The long comment: its first 250 characters in the row.
        const auto tracks = infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll();
        std::string comment;
        for (const auto &t : tracks) {
            if (t.filePath == expected[2]) {
                comment = t.comment;
            }
        }
        assert(comment.size() == 500 && "250 characters of two bytes each");
    }
    std::cout << "case 2 (one save: the playlist made, three tracks in it in order in all three libraries) OK\n";

    // 3. One more, dropped at the top of the playlist that now exists.
    domain::ImportPlan more;
    more.entries.push_back(offer(computer, "zero.mp3", "Zero", "Contents/Stager/Staged/zero.mp3"));
    const StagedImport second = stageTrackImport(more, target, tags, probe, "2026-11-25", ImportPlaylist{"Staged Import", false, 0});
    assert(second.changes.size() == 3 && second.leftOut.empty() && second.note.empty());
    const SaveLoopResult savedSecond = testing::saveChanges(second.changes, stick.pioneerPath(), stick.enginePath());
    if (!savedSecond.error.isEmpty()) {
        std::cerr << "save: " << savedSecond.error.toStdString() << "\n";
    }
    assert(savedSecond.error.isEmpty() && savedSecond.appliedIds.size() == 3);
    std::vector<std::string> withZero = expected;
    withZero.insert(withZero.begin(), onStick("Contents/Stager/Staged/zero.mp3"));
    assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "Staged Import") == withZero);
    assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "Staged Import") == withZero);
    assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "Staged Import") == withZero);
    std::cout << "case 3 (a file dropped at the top of the playlist is first in it, in all three libraries) OK\n";

    // 4. Undo of that second save: the playlist is the three again.
    const SaveLoopResult undone = testing::undoSave(savedSecond, stick.pioneerPath(), stick.enginePath());
    assert(undone.error.isEmpty());
    assert(!fs::exists(pathFromUtf8(withZero[0])));
    assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "Staged Import") == expected);
    assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "Staged Import") == expected);
    assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "Staged Import") == expected);
    std::cout << "case 4 (Undo: the dropped file and its entry are gone, the playlist as before) OK\n";

    // 4b. Three files dropped between the first and the second track:
    //     they stand there in the order offered.
    {
        domain::ImportPlan middle;
        for (const char *name : {"m1", "m2", "m3"}) {
            middle.entries.push_back(offer(computer, std::string(name) + ".mp3", name,
                                           std::string("Contents/Stager/Staged/") + name + ".mp3"));
        }
        const StagedImport dropped =
            stageTrackImport(middle, target, tags, probe, "2026-11-25", ImportPlaylist{"Staged Import", false, 1});
        // Three imports, each with its entry at the end, and one move of
        // the run: not a rewrite of the playlist per file.
        assert(dropped.refusal.empty() && dropped.note.empty() && dropped.changes.size() == 7);
        for (size_t i = 1; i < 6; i += 2) {
            const auto entry = std::dynamic_pointer_cast<AddToPlaylistChange>(dropped.changes[i]);
            assert(entry && !entry->toIndex());
        }
        const auto run = std::dynamic_pointer_cast<PlaceInPlaylistChange>(dropped.changes[6]);
        assert(run && run->toIndex() == 1 && run->files().size() == 3);
        const SaveLoopResult savedDrop = testing::saveChanges(dropped.changes, stick.pioneerPath(), stick.enginePath());
        assert(savedDrop.error.isEmpty() && savedDrop.appliedIds.size() == 7);
        const std::vector<std::string> order = {expected[0],
                                                onStick("Contents/Stager/Staged/m1.mp3"),
                                                onStick("Contents/Stager/Staged/m2.mp3"),
                                                onStick("Contents/Stager/Staged/m3.mp3"),
                                                expected[1],
                                                expected[2]};
        assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "Staged Import") == order);
        assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "Staged Import") == order);
        assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "Staged Import") == order);
    }
    std::cout << "case 4b (three files dropped at a place stand there in the order offered) OK\n";

    // 4c. The playlist asked of the stick: nothing is staged for one
    //     that is not in every library, for a new one in a folder, or for
    //     libraries that are not the stick's.
    {
        domain::ImportPlan one;
        one.entries.push_back(offer(computer, "p.mp3", "P", "Contents/Stager/Staged/p.mp3"));
        const auto refusalOf = [&](const ImportTarget &where, const ImportPlaylist &playlist) {
            const StagedImport result = stageTrackImport(one, where, tags, probe, "2026-11-25", playlist);
            assert((result.refusal.empty()) == (!result.changes.empty()));
            return result.refusal;
        };
        assert(refusalOf(target, {"Never Made", false, std::nullopt}).find("is not in every library") != std::string::npos);
        assert(refusalOf(target, {"A Folder/New", true, std::nullopt}).find("names a folder") != std::string::npos);
        ImportTarget two = target;
        two.libraries = QStringList{QStringLiteral("rekordbox"), QStringLiteral("engine")};
        assert(refusalOf(two, {"Staged Import", false, std::nullopt}).find("are not the ones") != std::string::npos);
        // A playlist only Engine has.
        auto engineOnly = std::make_shared<CreatePlaylistChange>(stick.pioneerPath(), stick.enginePath(), "Engine Alone",
                                                                 QStringList{QStringLiteral("engine")});
        assert(testing::saveChanges({engineOnly}, stick.pioneerPath(), stick.enginePath()).error.isEmpty());
        const std::string alone = refusalOf(target, {"Engine Alone", false, std::nullopt});
        assert(alone.find("is not in every library") != std::string::npos && alone.find("rekordbox") != std::string::npos
               && alone.find("onelibrary") != std::string::npos);
        // Made new, it is made where it is missing, and the tracks listed everywhere.
        assert(refusalOf(target, {"Engine Alone", true, std::nullopt}).empty());
        assert(refusalOf(target, {"Staged Import", false, std::nullopt}).empty());
    }
    std::cout << "case 4c (a playlist not in every library, a new one in a folder, other libraries than the stick's: "
                 "nothing staged, with the reason) OK\n";

    // 4d. Undo of a save that made the playlist, on a stick of its own:
    //     the playlist, the files and the rows are gone.
    {
        const testing::EngineChangeStick other = testing::makeEngineChangeStick(fixture, "track_import_staging_undo");
        testing::levelImportCounter(other);
        ImportTarget there{pathToQString(other.root), other.pioneerPath(), other.enginePath(), AllThree};
        domain::ImportPlan two;
        two.entries.push_back(offer(computer, "u1.mp3", "U1", "Contents/Stager/Undo/u1.mp3"));
        two.entries.push_back(offer(computer, "u2.mp3", "U2", "Contents/Stager/Undo/u2.mp3"));
        const size_t rowsBefore = infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(other.pioneer)).readAll().size();
        const StagedImport made = stageTrackImport(two, there, tags, probe, "2026-11-25", ImportPlaylist{"To Undo", true, std::nullopt});
        const SaveLoopResult savedMade = testing::saveChanges(made.changes, other.pioneerPath(), other.enginePath());
        assert(savedMade.error.isEmpty() && savedMade.appliedIds.size() == 5);
        assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(other.engineUtf8()), "To Undo").size() == 2);
        const SaveLoopResult undoneMade = testing::undoSave(savedMade, other.pioneerPath(), other.enginePath());
        assert(undoneMade.error.isEmpty());
        assert(!fs::exists(other.root / "Contents" / "Stager" / "Undo" / "u1.mp3")
               && !fs::exists(other.root / "Contents" / "Stager" / "Undo" / "u2.mp3"));
        assert(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(other.pioneer)).readAll().size() == rowsBefore);
        const PlaylistWhereabouts gone = playlistWhereabouts(other.pioneerPath(), other.enginePath(), "To Undo");
        assert(gone.having.isEmpty() && gone.onStick.size() == 3 && "the playlist is in no library again");
    }
    std::cout << "case 4d (Undo of the save that made the playlist: playlist, files and rows gone) OK\n";

    // 4e. An export.pdb whose track table cannot start a new page (its
    //     empty candidate is a page in use, as some real sticks have it):
    //     the batch is rehearsed, the tracks the last page has no room
    //     for are left out, and the save of the rest goes through instead
    //     of failing at the first that does not fit and taking the ones
    //     before it back.
    {
        const testing::EngineChangeStick full = testing::makeEngineChangeStick(fixture, "track_import_staging_full");
        testing::levelImportCounter(full);
        const fs::path pdbFile = full.pioneer / "rekordbox" / "export.pdb";
        std::string pdb = testing::fileBytes(pdbFile);
        const auto u32 = [&](size_t at) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(pdb[at]) | (static_cast<unsigned char>(pdb[at + 1]) << 8)
                                              | (static_cast<unsigned char>(pdb[at + 2]) << 16)
                                              | (static_cast<unsigned char>(pdb[at + 3]) << 24));
        };
        for (std::uint32_t table = 0; table < u32(8); ++table) {
            const size_t entry = 28 + 16 * table;
            if (u32(entry) == 0) {
                // empty_candidate: the table's own first page.
                for (int i = 0; i < 4; ++i) {
                    pdb[entry + 4 + i] = pdb[entry + 8 + i];
                }
            }
        }
        std::ofstream(pdbFile, std::ios::binary | std::ios::trunc) << pdb;
        ImportTarget there{pathToQString(full.root), full.pioneerPath(), full.enginePath(), AllThree};
        domain::ImportPlan many;
        for (int i = 0; i < 30; ++i) {
            const std::string name = "f" + std::to_string(i) + ".mp3";
            many.entries.push_back(offer(computer, name, "F" + std::to_string(i), "Contents/Stager/Full/" + name));
        }
        const StagedImport fitted = stageTrackImport(many, there, tags, probe, "2026-11-25", std::nullopt);
        assert(fitted.staged.size() + fitted.leftOut.size() == 30);
        assert(!fitted.leftOut.empty() && fitted.staged.size() < 30 && "a page holds some ten track rows, not thirty");
        for (const LeftOut &left : fitted.leftOut) {
            assert(left.why.find("page") != std::string::npos);
        }
        assert(fitted.changes.size() == fitted.staged.size() && !fitted.staged.empty() && "some fit the last page");
        {
            const SaveLoopResult savedFitted = testing::saveChanges(fitted.changes, full.pioneerPath(), full.enginePath());
            if (!savedFitted.error.isEmpty()) {
                std::cerr << "save: " << savedFitted.error.toStdString() << "\n";
            }
            assert(savedFitted.error.isEmpty() && savedFitted.appliedIds.size() == static_cast<qsizetype>(fitted.changes.size()));
        }
        std::cout << "case 4e (an export.pdb that cannot grow: " << fitted.staged.size() << " of 30 staged and saved, "
                  << fitted.leftOut.size() << " left out before the save) OK\n";
    }

    // 4f. A folder is not a playlist: nothing is staged into its path,
    //     whether it is asked for as it is or as one to make.
    {
        const testing::EngineChangeStick foldered = testing::makeEngineChangeStick(fixture, "track_import_staging_folder");
        testing::levelImportCounter(foldered);
        {
            infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(foldered.pioneer / "rekordbox" / "export.pdb"));
            rows.createPlaylist(0, "Sets", true);
            assert(rows.commit());
            infrastructure::onelibrary::OneLibraryCueWriter writer(pathToUtf8(foldered.pioneer));
            writer.createPlaylist("", "Sets", true);
            infrastructure::engine::createEnginePlaylist(foldered.engineUtf8(), "Sets/Inner");
        }
        ImportTarget there{pathToQString(foldered.root), foldered.pioneerPath(), foldered.enginePath(), AllThree};
        assert(playlistWhereabouts(foldered.pioneerPath(), foldered.enginePath(), "Sets").having.size() == 3
               && "the lookup alone takes the folder for a playlist");
        domain::ImportPlan one;
        one.entries.push_back(offer(computer, "s.mp3", "S", "Contents/Stager/Sets/s.mp3"));
        for (const bool create : {false, true}) {
            const StagedImport into =
                stageTrackImport(one, there, tags, probe, "2026-11-25", ImportPlaylist{"Sets", create, std::nullopt});
            assert(into.changes.empty() && into.refusal.find("is a folder") != std::string::npos);
        }
    }
    std::cout << "case 4f (a folder at the playlist's path: nothing staged, with the reason) OK\n";

    // 4i. createWhereMissing (Browse's Own Recordings): made in the
    //     libraries that lack the playlist and only there, nothing made
    //     where none does, refused for a folder or a name held twice; the
    //     staged set without it is what it was.
    {
        const testing::EngineChangeStick spread = testing::makeEngineChangeStick(fixture, "track_import_staging_where_missing");
        testing::levelImportCounter(spread);
        const QString pioneerQ = spread.pioneerPath();
        const QString engineQ = spread.enginePath();
        const auto plant = [&](const std::string &name, const QStringList &libraries) {
            const auto made = std::make_shared<CreatePlaylistChange>(pioneerQ, engineQ, name, libraries);
            assert(testing::saveChanges({made}, pioneerQ, engineQ).error.isEmpty());
        };
        plant("Everywhere", AllThree);
        plant("Two Have", QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")});
        plant("Foldered", QStringList{QStringLiteral("onelibrary"), QStringLiteral("engine")});
        {
            infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(spread.pioneer / "rekordbox" / "export.pdb"));
            rows.createPlaylist(0, "Foldered", true);
            assert(rows.commit());
            // "Odd/Twice" spelled by two of Engine's playlists: one of that
            // name, and "Twice" inside "Odd".
            auto db = djinterop::engine::load_database(spread.engineUtf8());
            db.create_root_playlist("Odd/Twice");
            db.create_root_playlist("Odd").create_sub_playlist("Twice");
        }
        ImportTarget there{pathToQString(spread.root), pioneerQ, engineQ, AllThree};
        domain::ImportPlan one;
        one.entries.push_back(offer(computer, "w.mp3", "W", "Contents/Stager/Spread/w.mp3"));
        const auto whereMissing = [](const std::string &name) {
            ImportPlaylist playlist{name, false, std::nullopt};
            playlist.createWhereMissing = true;
            return playlist;
        };
        const auto stage = [&](const ImportPlaylist &playlist) {
            return stageTrackImport(one, there, tags, probe, "2026-11-25", playlist);
        };
        const auto idsOf = [](const StagedImport &staged) {
            std::vector<QString> ids;
            for (const auto &change : staged.changes) {
                ids.push_back(change->id());
            }
            return ids;
        };
        const auto creates = [](const StagedImport &staged) {
            return std::count_if(staged.changes.begin(), staged.changes.end(), [](const auto &change) {
                return std::dynamic_pointer_cast<CreatePlaylistChange>(change) != nullptr;
            });
        };
        // Lacking in none: nothing made, the set as without the flag.
        const StagedImport everywhere = stage(whereMissing("Everywhere"));
        assert(everywhere.refusal.empty() && everywhere.createdIn.isEmpty() && creates(everywhere) == 0);
        assert(idsOf(everywhere) == idsOf(stage({"Everywhere", false, std::nullopt})));
        // Lacking in all: made in all three, as a new playlist is.
        const StagedImport nowhere = stage(whereMissing("Nowhere"));
        assert(nowhere.refusal.empty() && nowhere.createdIn == AllThree && creates(nowhere) == 1);
        assert(idsOf(nowhere) == idsOf(stage({"Nowhere", true, std::nullopt})));
        // Lacking in Engine: made there only; without the flag refused as
        // before.
        const StagedImport two = stage(whereMissing("Two Have"));
        assert(two.refusal.empty() && (two.createdIn == QStringList{QStringLiteral("engine")}) && creates(two) == 1);
        const auto made = std::dynamic_pointer_cast<CreatePlaylistChange>(two.changes.front());
        assert(made && (made->formatsTouched() == QStringList{QStringLiteral("engine")}));
        const StagedImport plain = stage({"Two Have", false, std::nullopt});
        assert(plain.changes.empty() && plain.createdIn.isEmpty()
               && plain.refusal.find("is not in every library") != std::string::npos);
        // A folder in rekordbox, a name Engine holds twice: nothing.
        const StagedImport foldered = stage(whereMissing("Foldered"));
        assert(foldered.changes.empty() && foldered.createdIn.isEmpty()
               && foldered.refusal.find("is a folder in rekordbox") != std::string::npos);
        const StagedImport twice = stage(whereMissing("Odd/Twice"));
        assert(twice.changes.empty() && twice.createdIn.isEmpty()
               && twice.refusal.find("More than one playlist") != std::string::npos);
        // Saved: the playlist once in every library, the track in it.
        const SaveLoopResult saved = testing::saveChanges(two.changes, pioneerQ, engineQ);
        assert(saved.error.isEmpty() && saved.skippedIds.isEmpty());
        for (const char *catalog : {"rekordbox", "onelibrary", "engine"}) {
            const auto paths = playlistPathsIn(catalog, pathToUtf8(std::string(catalog) == "engine" ? spread.engine
                                                                                               : spread.pioneer));
            assert(std::count(paths.begin(), paths.end(), "Two Have") == 1 && "once in each library");
        }
        const std::vector<std::string> w{pathToUtf8(spread.root / "Contents" / "Stager" / "Spread" / "w.mp3")};
        assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(spread.pioneer)), "Two Have") == w);
        assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(spread.pioneer)), "Two Have") == w);
        assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(spread.engineUtf8()), "Two Have") == w);
    }
    std::cout << "case 4i (made where missing: only in the libraries that lack it, nowhere when none does; a folder "
                 "or a name held twice refused; the set without it unchanged) OK\n";

    // 4g. An export.pdb whose playlist-entries table cannot start a new
    //     page: the move of the run to its place would rewrite the
    //     playlist onto pages the table cannot have. Rehearsed, it is not
    //     staged, the tracks go to the end and the save goes through.
    {
        const testing::EngineChangeStick tight = testing::makeEngineChangeStick(fixture, "track_import_staging_entries");
        testing::levelImportCounter(tight);
        const fs::path pdbFile = tight.pioneer / "rekordbox" / "export.pdb";
        std::string pdb = testing::fileBytes(pdbFile);
        const auto u32 = [&](size_t at) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(pdb[at]) | (static_cast<unsigned char>(pdb[at + 1]) << 8)
                                              | (static_cast<unsigned char>(pdb[at + 2]) << 16)
                                              | (static_cast<unsigned char>(pdb[at + 3]) << 24));
        };
        bool found = false;
        for (std::uint32_t table = 0; table < u32(8); ++table) {
            const size_t entry = 28 + 16 * table;
            if (u32(entry) == 8) {
                // playlist entries: empty_candidate made the table's own first page.
                for (int i = 0; i < 4; ++i) {
                    pdb[entry + 4 + i] = pdb[entry + 8 + i];
                }
                found = true;
            }
        }
        assert(found);
        std::ofstream(pdbFile, std::ios::binary | std::ios::trunc) << pdb;
        // The fixture's longest playlist that all three libraries have.
        std::map<std::string, size_t> sizes;
        for (const auto &track : infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(tight.pioneer)).readAll()) {
            for (const auto &membership : track.playlists) {
                ++sizes[membership.name];
            }
        }
        std::string longest;
        for (const auto &[name, size] : sizes) {
            const PlaylistWhereabouts where = playlistWhereabouts(tight.pioneerPath(), tight.enginePath(), name);
            if (where.having.size() == 3 && where.several.isEmpty() && (longest.empty() || size > sizes[longest])) {
                longest = name;
            }
        }
        assert(!longest.empty());
        const std::vector<std::string> before =
            playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(tight.pioneer)), longest);
        // Engine's copy of it, which need not be as long as rekordbox's.
        const std::vector<std::string> engineBefore =
            playlistOf(infrastructure::engine::LibdjinteropEngineReader(tight.engineUtf8()), longest);
        ImportTarget there{pathToQString(tight.root), tight.pioneerPath(), tight.enginePath(), AllThree};
        domain::ImportPlan two;
        two.entries.push_back(offer(computer, "t1.mp3", "T1", "Contents/Stager/Tight/t1.mp3"));
        two.entries.push_back(offer(computer, "t2.mp3", "T2", "Contents/Stager/Tight/t2.mp3"));
        const StagedImport atTop = stageTrackImport(two, there, tags, probe, "2026-11-25", ImportPlaylist{longest, false, 0});
        assert(atTop.refusal.empty() && atTop.staged.size() == 2);
        assert(atTop.note.find("end of") != std::string::npos && atTop.changes.size() == 4 && "no move is staged");
        const SaveLoopResult savedTight = testing::saveChanges(atTop.changes, tight.pioneerPath(), tight.enginePath());
        if (!savedTight.error.isEmpty()) {
            std::cerr << "save: " << savedTight.error.toStdString() << "\n";
        }
        assert(savedTight.error.isEmpty() && savedTight.appliedIds.size() == 4);
        std::vector<std::string> after = before;
        after.push_back(pathToUtf8(tight.root / "Contents" / "Stager" / "Tight" / "t1.mp3"));
        after.push_back(pathToUtf8(tight.root / "Contents" / "Stager" / "Tight" / "t2.mp3"));
        assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(tight.pioneer)), longest) == after);
        std::vector<std::string> engineAfter = engineBefore;
        engineAfter.insert(engineAfter.end(), after.end() - 2, after.end());
        assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(tight.engineUtf8()), longest) == engineAfter);
        std::cout << "case 4g (a playlist of " << before.size()
                  << " whose entries cannot be rewritten: the two tracks at its end, no move staged, saved) OK\n";
    }

    // 4h. A file where a folder of the destination has to be: left out.
    {
        std::ofstream(stick.root / "Contents" / "Blocked") << "a file, not a folder";
        domain::ImportPlan under;
        under.entries.push_back(offer(computer, "b.mp3", "B", "Contents/Blocked/Album/b.mp3"));
        const StagedImport blocked = stageTrackImport(under, target, tags, probe, "2026-11-25", std::nullopt);
        assert(blocked.changes.empty() && blocked.leftOut.size() == 1
               && blocked.leftOut[0].why.find("a file is where the folder") != std::string::npos);
        fs::remove(stick.root / "Contents" / "Blocked");
    }
    std::cout << "case 4h (a file where the destination's folder has to be: left out) OK\n";

    // 5. Nothing to stage is no change at all, not a lone playlist.
    domain::ImportPlan none;
    none.entries.push_back(offer(computer, "settled2.mp3", "Settled", "Contents/Stager/Staged/settled2.mp3"));
    none.entries.back().verdict = domain::ImportVerdict::PossibleDuplicate;
    assert(stageTrackImport(none, target, tags, probe, "2026-11-25", ImportPlaylist{"Never Made", true, std::nullopt}).changes.empty());
    {
        bool refused = false;
        try {
            (void)stageTrackImport(plan, target, tags, probe, "2026-11-25", ImportPlaylist{"Staged Import", false, -1});
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused && "a place below 0 is not a place");
        // A file whose place was taken since the plan was made is left
        // out, not staged to fail.
        domain::ImportPlan taken;
        taken.entries.push_back(offer(computer, "one again.mp3", "One Again", "Contents/Stager/Staged/one.mp3"));
        const StagedImport none2 = stageTrackImport(taken, target, tags, probe, "2026-11-25", std::nullopt);
        assert(none2.changes.empty() && none2.leftOut.size() == 1
               && none2.leftOut[0].why.find("on the stick already") != std::string::npos);
    }
    for (const char *date : {"", "25-11-2026"}) {
        bool refused = false;
        try {
            (void)stageTrackImport(plan, target, tags, probe, date, std::nullopt);
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused);
    }
    // Without a playlist: the imports alone.
    domain::ImportPlan loose;
    loose.entries.push_back(offer(computer, "loose.mp3", "Loose", "Contents/Stager/Staged/loose.mp3"));
    assert(stageTrackImport(loose, target, tags, probe, "2026-11-25", std::nullopt).changes.size() == 1);
    std::cout << "track_import_staging_test: all cases passed\n";
    return 0;
}
