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
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "engine_change_fixture.hpp"

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
    assert(std::dynamic_pointer_cast<CreatePlaylistChange>(staged.changes[0]));
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
    assert(second.changes.size() == 2 && second.leftOut.empty());
    const SaveLoopResult savedSecond = testing::saveChanges(second.changes, stick.pioneerPath(), stick.enginePath());
    if (!savedSecond.error.isEmpty()) {
        std::cerr << "save: " << savedSecond.error.toStdString() << "\n";
    }
    assert(savedSecond.error.isEmpty() && savedSecond.appliedIds.size() == 2);
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
