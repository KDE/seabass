// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The playlist changes of a save share one export.pdb writer whose rows are
// held and written once (sharedPdbRows()). Before that, every change opened
// and committed a writer of its own, and that is the file a save has to
// leave.
//
// 1. Saves of several playlist changes, on a copy of the anonymized
//    fixture's rekordbox library: each change in a save of its own, and all
//    of them in one save. The two export.pdb files are the same bytes, the
//    sequence numbers included, and the app's reader reads the same
//    playlists from both. The saves: remove A then add B to one playlist;
//    add then remove one track; reorder then add; delete a playlist and
//    create one of its name; rename then add; add two at the top; changes
//    to two playlists whose entries share a page; a playlist created and
//    300 tracks added to it (new pages started), one taken out again.
// 2. On a stick with the same playlists in all three libraries (rekordbox's
//    own 3-folder.pdb, OneLibrary and Engine made to match):
//    a. a save of Browse's edits (rename, move, remove, delete, add at a
//       place) with a change between them that opens export.pdb itself:
//       that change sees the rows held before it, and the playlists end the
//       same in all three libraries, export.pdb the bytes of the same
//       changes saved one by one;
//    b. the held rows not written, for each of the three reasons the writer
//       gives (the file changed since it was read, the edited copy does not
//       read back, the write fails): the save says which and names the
//       change whose rows were held, before a change that never ran or at
//       the end, and goes back whole without rolling back a change that was
//       not open;
//    c. a change that fails after it changed the held rows: nothing of the
//       save reaches the file, and the next save works;
//    d. a cancel with rows held: the changes before it land;
//    e. Undo Last Save after a save made only of changes that share the
//       rows: the stick as it was;
//    f. after a save that went back, landing the held rows again is a
//       no-op: the writers they were in are gone.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/work_counters.hpp"
#include "engine_change_fixture.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"

// After the project's headers: sqlcipher_dyn.hpp (through the OneLibrary
// writer) declares names sqlite3.h defines as macros.
#include <sqlite3.h>

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;
using infrastructure::rekordbox::PdbRowWriter;

namespace
{

const QStringList RekordboxOnly{QStringLiteral("rekordbox")};
const QStringList All{QStringLiteral("rekordbox"), QStringLiteral("onelibrary"), QStringLiteral("engine")};

using Changes = std::vector<std::shared_ptr<PendingChange>>;

SaveLoopResult save(const Changes &changes, const QString &pioneer, const QString &engine)
{
    application::CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, pioneer, engine);
    return runSaveLoop(changes, ctx);
}

void requireSaved(const SaveLoopResult &saved)
{
    if (!saved.error.isEmpty()) {
        std::cerr << "save: " << saved.error.toStdString() << "\n";
    }
    assert(saved.error.isEmpty());
}

// The file with every sequence number zeroed: the header's and each page's.
std::string withoutSequences(std::string pdb)
{
    const size_t lenPage = static_cast<unsigned char>(pdb[4]) | (static_cast<unsigned char>(pdb[5]) << 8);
    for (size_t at : {size_t(20)}) {
        std::fill_n(pdb.begin() + static_cast<std::ptrdiff_t>(at), 4, '\0');
    }
    for (size_t base = 0; base + lenPage <= pdb.size(); base += lenPage) {
        std::fill_n(pdb.begin() + static_cast<std::ptrdiff_t>(base + 16), 4, '\0');
    }
    return pdb;
}

// What the app's reader says of every playlist: its name and its files in
// their order, each from the stick's root.
std::map<std::string, std::vector<std::string>> playlistsRead(const fs::path &pioneer)
{
    const std::string root = pathToUtf8(pioneer.parent_path());
    std::map<std::string, std::vector<std::pair<int, std::string>>> at;
    for (const auto &t : infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(pioneer)).readAll()) {
        assert(t.filePath.rfind(root, 0) == 0);
        for (const auto &p : t.playlists) {
            at[p.name].emplace_back(p.position, t.filePath.substr(root.size()));
        }
    }
    std::map<std::string, std::vector<std::string>> read;
    for (auto &[name, entries] : at) {
        std::sort(entries.begin(), entries.end());
        for (const auto &[position, file] : entries) {
            read[name].push_back(file);
        }
    }
    return read;
}

std::string exportPdbOf(const fs::path &pioneer)
{
    return testing::fileBytes(pioneer / "rekordbox" / "export.pdb");
}

// A stick holding the fixture's rekordbox library and nothing else.
fs::path rekordboxStick(const fs::path &fixture, const std::string &name)
{
    const fs::path root = testing::scratchRoot() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    std::error_code ec;
    testing::copyPioneerFixture(fixture / "rekordbox", root / "PIONEER", ec);
    assert(!ec);
    fs::remove(root / "PIONEER" / "rekordbox" / "exportLibrary.db");
    fs::remove(root / "PIONEER" / "rekordbox" / "exportLibrary.db-wal");
    return root;
}

// The fixture's track files, from the stick's root, and its top-level
// playlists by size, with their tracks' files.
struct Library
{
    std::vector<std::string> files;
    std::vector<std::pair<std::string, std::vector<std::string>>> playlists;
};
Library libraryOf(const fs::path &stick)
{
    Library library;
    PdbRowWriter w(pathToUtf8(stick / "PIONEER" / "rekordbox" / "export.pdb"));
    std::map<std::uint32_t, std::string> fileOf;
    for (const auto &row : w.trackPathRows()) {
        fileOf[row.id] = row.filePath.substr(1);
        library.files.push_back(fileOf[row.id]);
    }
    // Only names the top level holds once: a change names a playlist by
    // its path, and refuses one that names several.
    std::map<std::string, int> named;
    for (const auto &n : w.playlistTree()) {
        named[n.name] += n.parentId == 0 ? 1 : 0;
    }
    for (const auto &n : w.playlistTree()) {
        if (n.parentId == 0 && !n.isFolder && named[n.name] == 1) {
            std::vector<std::string> files;
            for (const std::uint32_t id : w.playlistTrackIds(n.id)) {
                files.push_back(fileOf.at(id));
            }
            library.playlists.emplace_back(n.name, files);
        }
    }
    std::stable_sort(library.playlists.begin(), library.playlists.end(),
                     [](const auto &a, const auto &b) { return a.second.size() > b.second.size(); });
    return library;
}

// A change of the test's own that shares the held rows: `act` runs with
// the save's writer and says whether the change succeeded.
class Sharing : public PendingChange
{
public:
    Sharing(QString pioneer, QString name, std::function<bool(SaveContext &, FormatWriteSession &, PdbRowWriter &)> act)
        : m_pioneer(std::move(pioneer)), m_name(std::move(name)), m_act(std::move(act))
    {
    }
    QString id() const override { return QStringLiteral("test:") + m_name; }
    QString description() const override { return m_name; }
    QString unit() const override { return QStringLiteral("edits"); }
    QStringList formatsTouched() const override { return RekordboxOnly; }
    std::vector<BackupTarget> filesToBackup(SaveContext &) const override
    {
        return {{pathToUtf8(pathFromUtf8(m_pioneer.toStdString()) / "rekordbox" / "export.pdb"), "test-sharing"}};
    }
    bool sharesHeldWrites() const override { return true; }
    ChangeOutcome apply(SaveContext &ctx) override
    {
        FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", m_pioneer.toStdString(), 1, "test-sharing");
        if (!m_act(ctx, session, sharedPdbRows(ctx, session))) {
            return ChangeOutcome::failure(m_name + QStringLiteral(" failed on purpose"));
        }
        return ChangeOutcome::success();
    }

private:
    QString m_pioneer;
    QString m_name;
    std::function<bool(SaveContext &, FormatWriteSession &, PdbRowWriter &)> m_act;
};

// A change that opens export.pdb with a writer of its own, as most changes
// other than the playlist edits and the import do: `act` reads and writes
// through it, and says whether it found what it expected.
class OpensItself : public PendingChange
{
public:
    OpensItself(QString pioneer, std::function<bool(PdbRowWriter &)> act, bool *ran)
        : m_pioneer(std::move(pioneer)), m_act(std::move(act)), m_ran(ran)
    {
    }
    QString id() const override { return QStringLiteral("test:opens-itself"); }
    QString description() const override { return QStringLiteral("Opens export.pdb itself"); }
    QString unit() const override { return QStringLiteral("edits"); }
    QStringList formatsTouched() const override { return RekordboxOnly; }
    std::vector<BackupTarget> filesToBackup(SaveContext &) const override
    {
        return {{pathToUtf8(pathFromUtf8(m_pioneer.toStdString()) / "rekordbox" / "export.pdb"), "test-own"}};
    }
    ChangeOutcome apply(SaveContext &ctx) override
    {
        *m_ran = true;
        FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", m_pioneer.toStdString(), 1, "test-own");
        PdbRowWriter rows(pathToUtf8(pathFromUtf8(session.writeRoot()) / "rekordbox" / "export.pdb"));
        if (!m_act(rows)) {
            return ChangeOutcome::failure(QStringLiteral("export.pdb is not as the changes before left it"));
        }
        if (rows.hasEdits() && !rows.commit()) {
            return ChangeOutcome::failure(QString::fromStdString(rows.commitRefusal()));
        }
        session.noteItemApplied();
        return ChangeOutcome::success();
    }

private:
    QString m_pioneer;
    std::function<bool(PdbRowWriter &)> m_act;
    bool *m_ran;
};

// The stick of playlist_edit_changes_test: rekordbox's own 3-folder.pdb
// (Q1 with a01..a05, folder F1 holding F1A with b01 and c01), OneLibrary
// and Engine made to hold the same.
struct File
{
    const char *relative;  // under the stick's Contents/
};
const File A01{"Tone Artist 01/Tone Album 01/a01.mp3"};
const File A02{"Tone Artist 02/Tone Album 02/a02.mp3"};
const File A03{"Tone Artist 03/Tone Album 03/a03.mp3"};
const File A04{"Tone Artist 04/Tone Album 04/a04.mp3"};
const File A05{"Tone Artist 05/Tone Album 01/a05.mp3"};
const File B01{"Nouvel Artiste/Nouvel Album/b01.mp3"};
const File C01{"Tone Artist 01/Tone Album 01/c01.mp3"};
const File A10{"Tone Artist 05/Tone Album 02/a10.mp3"};

struct ThreeStick
{
    fs::path root;
    fs::path pioneer;
    fs::path library;
    std::map<std::string, std::int64_t> engineId;  // by relative path
    QString pioneerQ() const { return pathToQString(pioneer); }
    QString libraryQ() const { return pathToQString(library); }
    std::string file(const File &f) const { return pathToUtf8(root / "Contents" / f.relative); }
};

ThreeStick threeStick(const fs::path &pdbFixtures, const fs::path &olFixture, const std::string &name)
{
    ThreeStick s;
    s.root = testing::scratchRoot() / name;
    fs::remove_all(s.root);
    s.pioneer = s.root / "PIONEER";
    s.library = s.root / "Engine Library";
    fs::create_directories(s.pioneer / "rekordbox");
    fs::copy_file(pdbFixtures / "3-folder.pdb", s.pioneer / "rekordbox" / "export.pdb");
    for (const char *db : {"exportLibrary.db", "exportLibrary.db-wal"}) {
        fs::copy_file(olFixture / "0-base" / "PIONEER" / "rekordbox" / db, s.pioneer / "rekordbox" / db);
    }
    {
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(s.pioneer));
        w.createPlaylist("", "Q1", false);
        for (const File &f : {A01, A02, A03, A04, A05}) {
            w.addToPlaylist("Q1", s.file(f));
        }
        w.createPlaylist("", "F1", true);
        w.createPlaylist("F1", "F1A", false);
        w.addToPlaylist("F1/F1A", s.file(B01));
        w.addToPlaylist("F1/F1A", s.file(C01));
    }
    {
        auto db = djinterop::engine::create_database(pathToUtf8(s.library));
        auto q1 = db.create_root_playlist("Q1");
        auto f1 = db.create_root_playlist("F1");
        auto f1a = f1.create_sub_playlist("F1A");
        for (const File &f : {A01, A02, A03, A04, A05, B01, C01, A10}) {
            djinterop::track_snapshot snapshot;
            snapshot.relative_path = std::string("../Contents/") + f.relative;
            auto t = db.create_track(snapshot);
            s.engineId[f.relative] = t.id();
            if (std::string(f.relative).find("b01") != std::string::npos || std::string(f.relative).find("c01") != std::string::npos) {
                f1a.add_track_back(t);
            } else if (std::string(f.relative).find("a10") == std::string::npos) {
                q1.add_track_back(t);
            }
        }
    }
    return s;
}

// A playlist's files in their order, in each of the three libraries; a
// library without the playlist gives {"<none>"}.
struct InThree
{
    std::vector<std::string> rekordbox;
    std::vector<std::string> oneLibrary;
    std::vector<std::string> engine;
    bool operator==(const InThree &) const = default;
};
InThree playlistInThree(const ThreeStick &s, const std::string &path)
{
    InThree in;
    {
        PdbRowWriter rows(pathToUtf8(s.pioneer / "rekordbox" / "export.pdb"));
        std::map<std::uint32_t, std::string> fileOf;
        for (const auto &row : rows.trackPathRows()) {
            fileOf[row.id] = pathToUtf8(pathFromUtf8(row.filePath).filename());
        }
        std::optional<std::uint32_t> node;
        std::uint32_t parent = 0;
        size_t start = 0;
        while (start <= path.size()) {
            const size_t slash = path.find('/', start);
            const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            node.reset();
            for (const auto &n : rows.playlistTree()) {
                if (n.parentId == parent && n.name == part) {
                    node = n.id;
                }
            }
            if (!node || slash == std::string::npos) {
                break;
            }
            parent = *node;
            start = slash + 1;
        }
        if (!node) {
            in.rekordbox = {"<none>"};
        } else {
            for (const std::uint32_t id : rows.playlistTrackIds(*node)) {
                in.rekordbox.push_back(fileOf.at(id));
            }
        }
    }
    infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(s.pioneer));
    std::map<std::int64_t, std::string> contentFile;
    for (const auto &[id, file] : w.contentPaths()) {
        contentFile[id] = pathToUtf8(pathFromUtf8(file).filename());
    }
    if (w.playlistCountAtPath(path) == 0) {
        in.oneLibrary = {"<none>"};
    } else {
        auto content = w.playlistContent(path);
        std::sort(content.begin(), content.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
        for (const auto &[id, number] : content) {
            in.oneLibrary.push_back(contentFile.at(id));
        }
    }
    std::map<std::int64_t, std::string> engineFile;
    for (const auto &[relative, id] : s.engineId) {
        engineFile[id] = pathToUtf8(pathFromUtf8(relative).filename());
    }
    auto db = djinterop::engine::load_database(pathToUtf8(s.library));
    std::optional<djinterop::playlist> list;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        list = list ? list->sub_playlist_by_name(part) : db.root_playlist_by_name(part);
        if (!list || slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    if (!list) {
        in.engine = {"<none>"};
    } else {
        for (const auto &t : list->tracks()) {
            in.engine.push_back(engineFile.at(t.id()));
        }
    }
    return in;
}

std::string logOf(const fs::path &root)
{
    std::error_code ec;
    return fs::exists(root / "Seabass" / "seabass.log", ec) ? testing::fileBytes(root / "Seabass" / "seabass.log") : std::string();
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: held_playlist_saves_test <tests/fixtures/anonymized_library> <tests/fixtures/pdb_playlist_tree> "
                     "<tests/fixtures/onelibrary_playlist_tree>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path anonymized = pathFromUtf8(argv[1]);
    const fs::path pdbFixtures = pathFromUtf8(argv[2]);
    const fs::path olFixture = pathFromUtf8(argv[3]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "held_playlist_saves_home");
    // SEABASS_HELD_CASES ("1 2c", say) runs only those cases, for a red
    // check of one; every case runs without it.
    const QStringList only = qEnvironmentVariable("SEABASS_HELD_CASES").split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto wanted = [&](const char *name) { return only.isEmpty() || only.contains(QString::fromLatin1(name)); };

    // 1. Each save one change at a time and all at once.
    if (wanted("1")) {
        const fs::path probe = rekordboxStick(anonymized, "held_saves_probe");
        const Library library = libraryOf(probe);
        assert(library.playlists.size() >= 2 && library.files.size() > 300);
        const std::string p1 = library.playlists[0].first;
        const std::string p2 = library.playlists[1].first;
        const auto &in1 = library.playlists[0].second;
        const auto &in2 = library.playlists[1].second;
        std::vector<std::string> outside;
        for (const std::string &f : library.files) {
            if (std::find(in1.begin(), in1.end(), f) == in1.end() && std::find(in2.begin(), in2.end(), f) == in2.end()) {
                outside.push_back(f);
            }
        }
        assert(in1.size() >= 2 && in2.size() >= 2 && outside.size() >= 2);
        const std::string b = outside[0];
        const std::string d = outside[1];
        // Each change made for the stick it is saved on, by its root; a file
        // named from the root.
        using Make = std::function<std::shared_ptr<PendingChange>(const fs::path &root)>;
        const auto pioneerOf = [](const fs::path &root) { return pathToQString(root / "PIONEER"); };
        const auto fileOn = [](const fs::path &root, const std::string &file) { return pathToUtf8(root / pathFromUtf8(file)); };
        const auto add = [=](std::string playlist, std::string file, std::optional<int> at = std::nullopt) -> Make {
            return [=](const fs::path &root) {
                return std::make_shared<AddToPlaylistChange>(pioneerOf(root), QString(), playlist, fileOn(root, file),
                                                             QStringLiteral("t"), RekordboxOnly, at);
            };
        };
        const auto remove = [=](std::string playlist, std::string file) -> Make {
            return [=](const fs::path &root) {
                return std::make_shared<RemoveFromPlaylistChange>(pioneerOf(root), QString(), playlist, fileOn(root, file),
                                                                  QStringLiteral("t"), RekordboxOnly);
            };
        };
        const auto move = [=](std::string playlist, std::string file, int to) -> Make {
            return [=](const fs::path &root) {
                return std::make_shared<MoveInPlaylistChange>(pioneerOf(root), QString(), playlist, fileOn(root, file),
                                                              QStringLiteral("t"), to, RekordboxOnly);
            };
        };
        const auto create = [=](std::string playlist) -> Make {
            return [=](const fs::path &root) {
                return std::make_shared<CreatePlaylistChange>(pioneerOf(root), QString(), playlist, RekordboxOnly);
            };
        };
        const auto erase = [=](std::string playlist) -> Make {
            return [=](const fs::path &root) {
                return std::make_shared<DeletePlaylistChange>(pioneerOf(root), QString(), playlist, RekordboxOnly);
            };
        };
        const auto rename = [=](std::string playlist, std::string to) -> Make {
            return [=](const fs::path &root) {
                return std::make_shared<RenamePlaylistChange>(pioneerOf(root), QString(), playlist, to, RekordboxOnly);
            };
        };
        std::vector<std::pair<std::string, std::vector<Make>>> scenarios{
            {"remove A then add B", {remove(p1, in1.front()), add(p1, b)}},
            {"add then remove one track", {add(p1, b), remove(p1, b)}},
            {"reorder then add", {move(p1, in1.back(), 0), add(p1, b)}},
            {"delete a playlist and create one of its name", {erase(p1), create(p1)}},
            {"rename then add", {rename(p1, "Renamed One"), add("Renamed One", b)}},
            {"add two at the top", {add(p1, b, 0), add(p1, d, 1)}},
            {"two playlists whose entries share a page",
             {remove(p1, in1.front()), add(p2, b), remove(p2, in2.front()), add(p1, d), move(p1, in1.back(), 0), add(p2, in1.front())}},
        };
        {
            std::vector<Make> big{create("Three Hundred")};
            for (size_t i = 0; i < 300; ++i) {
                big.push_back(add("Three Hundred", library.files[i]));
            }
            big.push_back(remove("Three Hundred", library.files[3]));
            big.push_back(add(p1, b));
            scenarios.emplace_back("a playlist created, 300 added, one taken out", big);
        }
        // Every scenario's answer is said before any is held against it, so
        // a failing run shows all that differ.
        struct Found
        {
            bool same = false;
            bool sameBut = false;
            bool readSame = false;
        };
        std::vector<Found> found;
        int k = 0;
        for (const auto &[name, makes] : scenarios) {
            const fs::path one = rekordboxStick(anonymized, "held_saves_each_" + std::to_string(k));
            const fs::path all = rekordboxStick(anonymized, "held_saves_all_" + std::to_string(k));
            ++k;
            const QString oneQ = pathToQString(one / "PIONEER");
            const QString allQ = pathToQString(all / "PIONEER");
            for (const Make &make : makes) {
                requireSaved(save({make(one)}, oneQ, QString()));
            }
            Changes changes;
            for (const Make &make : makes) {
                changes.push_back(make(all));
            }
            const auto before = infrastructure::WorkCounters::instance().snapshot();
            const SaveLoopResult saved = save(changes, allQ, QString());
            requireSaved(saved);
            assert(saved.appliedIds.size() == static_cast<int>(changes.size()));
            const auto commits = infrastructure::WorkCounters::instance().snapshot().pdbWriterCommits - before.pdbWriterCommits;
            if (commits != 1) {
                std::cerr << "  " << name << ": export.pdb written " << commits << " times, skipped "
                          << saved.skippedIds.join(QStringLiteral(", ")).toStdString() << "\n" << logOf(all) << "\n";
            }
            assert(commits == 1 && "the save wrote export.pdb once");
            const std::string eachBytes = exportPdbOf(one / "PIONEER");
            const std::string allBytes = exportPdbOf(all / "PIONEER");
            assert(eachBytes != exportPdbOf(probe / "PIONEER") && "the changes changed the file");
            const bool same = allBytes == eachBytes;
            const bool sameBut = withoutSequences(allBytes) == withoutSequences(eachBytes);
            const bool readSame = playlistsRead(one / "PIONEER") == playlistsRead(all / "PIONEER");
            std::cerr << "  " << name << ": the same bytes " << same << ", but for the sequence numbers " << sameBut
                      << ", read the same " << readSame << "\n";
            found.push_back({same, sameBut, readSame});
        }
        for (const Found &f : found) {
            assert(f.readSame && "the reader reads the same playlists from both");
            assert(f.sameBut && "nothing differs but the sequence numbers");
            assert(f.same && "and not those either");
        }
        std::cout << "case 1 (eight saves of several playlist changes: export.pdb the bytes of the same changes saved "
                     "one by one, and read the same) OK\n";
    }

    // 2a. Browse's edits with a change between them that opens the file.
    if (wanted("2a")) {
        const ThreeStick held = threeStick(pdbFixtures, olFixture, "held_saves_browse_all");
        const ThreeStick each = threeStick(pdbFixtures, olFixture, "held_saves_browse_each");
        bool ranHeld = false;
        bool ranEach = false;
        const auto changesFor = [&](const ThreeStick &s, bool *ran) {
            const QString p = s.pioneerQ();
            const QString e = s.libraryQ();
            return Changes{
                std::make_shared<RenamePlaylistChange>(p, e, "Q1", "Q One", All),
                std::make_shared<MoveInPlaylistChange>(p, e, "Q One", s.file(A05), QStringLiteral("a05"), 0, All),
                std::make_shared<RemoveFromPlaylistChange>(p, e, "Q One", s.file(A03), QStringLiteral("a03"), All),
                // Sees the held rows: "Q One" is there, a05 first, a03 gone;
                // and rates a01 through a writer of its own.
                std::make_shared<OpensItself>(
                    p,
                    [](PdbRowWriter &rows) {
                        std::optional<std::uint32_t> qOne;
                        for (const auto &n : rows.playlistTree()) {
                            if (n.name == "Q One") {
                                qOne = n.id;
                            }
                        }
                        const auto a01 = rows.trackIdsWithFilePath(std::string("/Contents/") + A01.relative);
                        const auto a05 = rows.trackIdsWithFilePath(std::string("/Contents/") + A05.relative);
                        if (!qOne || a01.size() != 1 || a05.size() != 1) {
                            return false;
                        }
                        const auto order = rows.playlistTrackIds(*qOne);
                        if (order.size() != 4 || order.front() != a05.front()) {
                            return false;
                        }
                        return rows.setTrackRating(a01.front(), 4);
                    },
                    ran),
                std::make_shared<DeletePlaylistChange>(p, e, "F1/F1A", All),
                std::make_shared<AddToPlaylistChange>(p, e, "Q One", s.file(A10), QStringLiteral("a10"), All, 1),
            };
        };
        const Changes all = changesFor(held, &ranHeld);
        const auto before = infrastructure::WorkCounters::instance().snapshot();
        const SaveLoopResult saved = save(all, held.pioneerQ(), held.libraryQ());
        requireSaved(saved);
        assert(ranHeld && saved.appliedIds.size() == static_cast<int>(all.size()));
        // The held rows before the change that opens the file, its own
        // writer, the held rows of the two after it.
        assert(infrastructure::WorkCounters::instance().snapshot().pdbWriterCommits - before.pdbWriterCommits == 3);
        for (const auto &change : changesFor(each, &ranEach)) {
            requireSaved(save({change}, each.pioneerQ(), each.libraryQ()));
        }
        const InThree expected{{"a05.mp3", "a10.mp3", "a01.mp3", "a02.mp3", "a04.mp3"},
                               {"a05.mp3", "a10.mp3", "a01.mp3", "a02.mp3", "a04.mp3"},
                               {"a05.mp3", "a10.mp3", "a01.mp3", "a02.mp3", "a04.mp3"}};
        const InThree got = playlistInThree(held, "Q One");
        for (const auto *list : {&got.rekordbox, &got.oneLibrary, &got.engine}) {
            for (const auto &f : *list) {
                std::cerr << f << " ";
            }
            std::cerr << "\n";
        }
        assert(got == expected && "Q One alike in all three libraries");
        assert(playlistInThree(held, "F1/F1A") == (InThree{{"<none>"}, {"<none>"}, {"<none>"}}) && "F1A gone from all three");
        assert(playlistInThree(held, "Q1") == (InThree{{"<none>"}, {"<none>"}, {"<none>"}}) && "Q1 renamed in all three");
        assert(playlistInThree(each, "Q One") == expected);
        assert(exportPdbOf(held.pioneer) == exportPdbOf(each.pioneer) && "export.pdb the bytes of the changes saved one by one");
        bool rated = false;
        for (const auto &t : infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(held.pioneer)).readAll()) {
            rated = rated || (t.filePath == held.file(A01) && t.rating.value_or(0) == 4);
        }
        assert(rated && "the change between kept its rating");
        std::cout << "case 2a (Browse's edits with a change between them that opens export.pdb: it sees the held rows, "
                     "the playlists alike in all three libraries, export.pdb the bytes of the changes saved one by one) OK\n";
    }

    // 2b. The held rows not written, for each of the writer's three
    //     reasons, before a change that opens the file and at the end.
    if (wanted("2b")) {
        struct Reason
        {
            std::string name;
            std::function<bool(SaveContext &, FormatWriteSession &, PdbRowWriter &)> act;
            std::string said;
        };
        const std::vector<Reason> reasons{
            {"the file changed",
             [](SaveContext &, FormatWriteSession &session, PdbRowWriter &) {
                 // Something else writes the file after the writer read it.
                 const fs::path pdb = pathFromUtf8(session.writeRoot()) / "rekordbox" / "export.pdb";
                 std::string bytes = testing::fileBytes(pdb);
                 bytes.back() = static_cast<char>(bytes.back() ^ 0x5A);
                 std::ofstream(pdb, std::ios::binary | std::ios::trunc) << bytes;
                 return true;
             },
             "export.pdb changed on the stick after Seabass read it"},
            {"the copy does not read back",
             [](SaveContext &, FormatWriteSession &, PdbRowWriter &rows) {
                 // As a bug in the writer could leave it: the tracks table's
                 // first page past the end of the file.
                 std::string &buffer = rows.bufferForTesting();
                 for (std::uint32_t t = 0; t < 20; ++t) {
                     const size_t entry = 28 + 16 * t;
                     if (buffer[entry] == 0 && buffer[entry + 1] == 0 && buffer[entry + 2] == 0 && buffer[entry + 3] == 0) {
                         buffer[entry + 8] = static_cast<char>(0xF0);
                         buffer[entry + 10] = static_cast<char>(0x0F);
                         break;
                     }
                 }
                 return true;
             },
             "export.pdb as edited does not read back with Seabass's own parser"},
            {"the write fails",
             [](SaveContext &, FormatWriteSession &session, PdbRowWriter &) {
                 // A folder where the new copy is written beside the file.
                 fs::create_directories(pathFromUtf8(session.writeRoot()) / "rekordbox" / "export.pdb.tmp-seabass-write" / "in the way");
                 return true;
             },
             "export.pdb could not be written to the stick"},
        };
        int k = 0;
        for (const Reason &reason : reasons) {
            for (const bool beforeAChange : {true, false}) {
                const ThreeStick s = threeStick(pdbFixtures, olFixture, "held_saves_reason_" + std::to_string(k++));
                const std::string pdbBefore = exportPdbOf(s.pioneer);
                bool ran = false;
                Changes changes{
                    std::make_shared<CreatePlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", All),
                    std::make_shared<AddToPlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", s.file(A02), QStringLiteral("a02"), All),
                    std::make_shared<Sharing>(s.pioneerQ(), QString::fromStdString(reason.name), reason.act),
                };
                if (beforeAChange) {
                    changes.push_back(std::make_shared<OpensItself>(s.pioneerQ(), [](PdbRowWriter &) { return true; }, &ran));
                }
                const SaveLoopResult saved = save(changes, s.pioneerQ(), s.libraryQ());
                std::cerr << "  " << reason.name << (beforeAChange ? " (before a change)" : " (at the end)") << ": "
                          << saved.error.toStdString() << "\n";
                assert(saved.error.contains(QString::fromStdString(reason.said)) && "the reason, in its own words");
                assert(saved.error.contains(QStringLiteral("the rows 3 changes held for export.pdb"))
                       && saved.error.contains(QString::fromStdString(reason.name)) && "whose rows they were");
                assert(saved.failedId == changes[2]->id() && "the last change whose rows were held");
                assert(!ran && "the change after them never ran");
                assert(saved.rolledBack && saved.appliedIds.isEmpty());
                fs::remove_all(s.pioneer / "rekordbox" / "export.pdb.tmp-seabass-write");
                assert(exportPdbOf(s.pioneer) == pdbBefore && "nothing of the save on the stick");
                assert(playlistInThree(s, "Fresh") == (InThree{{"<none>"}, {"<none>"}, {"<none>"}}));
                const std::string log = logOf(s.root);
                assert(log.find(beforeAChange ? "save: stopped before \"Opens export.pdb itself\"" : "save: stopped at the end")
                       != std::string::npos);
                assert(log.find("the failed change had written") == std::string::npos
                       && "no change rolled back on its own: none was open");
            }
        }
        std::cout << "case 2b (the held rows not written, for each of the three reasons: said in its own words, the "
                     "changes whose rows they were named, the save back whole) OK\n";
    }

    // 2c. A change that fails after it changed the held rows.
    if (wanted("2c")) {
        const ThreeStick s = threeStick(pdbFixtures, olFixture, "held_saves_fails");
        const std::string pdbBefore = exportPdbOf(s.pioneer);
        const auto commitsBefore = infrastructure::WorkCounters::instance().snapshot().pdbWriterCommits;
        const SaveLoopResult failed = save(
            {std::make_shared<CreatePlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", All),
             std::make_shared<AddToPlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", s.file(A02), QStringLiteral("a02"), All),
             std::make_shared<Sharing>(s.pioneerQ(), QStringLiteral("Ghost then fail"),
                                       [](SaveContext &, FormatWriteSession &, PdbRowWriter &rows) {
                                           rows.createPlaylist(0, "Ghost", false);
                                           rows.endEdit();
                                           return false;
                                       })},
            s.pioneerQ(), s.libraryQ());
        assert(!failed.error.isEmpty() && failed.rolledBack && failed.failedId == QStringLiteral("test:Ghost then fail"));
        assert(infrastructure::WorkCounters::instance().snapshot().pdbWriterCommits == commitsBefore && "never written");
        assert(exportPdbOf(s.pioneer) == pdbBefore);
        assert(playlistInThree(s, "Fresh") == (InThree{{"<none>"}, {"<none>"}, {"<none>"}}));
        requireSaved(save({std::make_shared<CreatePlaylistChange>(s.pioneerQ(), s.libraryQ(), "Later", All)}, s.pioneerQ(),
                          s.libraryQ()));
        assert(playlistInThree(s, "Later") == (InThree{{}, {}, {}}) && "the next save works");
        assert(playlistsRead(s.pioneer).count("Ghost") == 0);
        {
            PdbRowWriter rows(pathToUtf8(s.pioneer / "rekordbox" / "export.pdb"));
            for (const auto &n : rows.playlistTree()) {
                assert(n.name != "Ghost" && n.name != "Fresh");
            }
        }
        std::cout << "case 2c (a change that fails after changing the held rows: none of the save written, the next "
                     "save works) OK\n";
    }

    // 2d. A cancel with rows held: the changes before it land.
    if (wanted("2d")) {
        const ThreeStick s = threeStick(pdbFixtures, olFixture, "held_saves_cancel");
        application::CancellationToken token;
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, s.pioneerQ(), s.libraryQ());
        const SaveLoopResult saved = runSaveLoop(
            {std::make_shared<CreatePlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", All),
             std::make_shared<AddToPlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", s.file(A02), QStringLiteral("a02"), All),
             std::make_shared<Sharing>(s.pioneerQ(), QStringLiteral("cancel"),
                                       [&token](SaveContext &, FormatWriteSession &, PdbRowWriter &) {
                                           token.cancel();
                                           return true;
                                       }),
             std::make_shared<AddToPlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", s.file(A04), QStringLiteral("a04"), All)},
            ctx);
        assert(saved.cancelled && saved.error.isEmpty() && saved.appliedIds.size() == 3);
        assert(playlistInThree(s, "Fresh") == (InThree{{"a02.mp3"}, {"a02.mp3"}, {"a02.mp3"}}) && "what came before the cancel");
        std::cout << "case 2d (a cancel with rows held: the changes before it land in all three) OK\n";
    }

    // 2e. Undo Last Save after a save made only of changes that share the
    //     rows.
    if (wanted("2e")) {
        const ThreeStick s = threeStick(pdbFixtures, olFixture, "held_saves_undo");
        const std::string pdbBefore = exportPdbOf(s.pioneer);
        const InThree q1Before = playlistInThree(s, "Q1");
        const SaveLoopResult saved =
            save({std::make_shared<RemoveFromPlaylistChange>(s.pioneerQ(), s.libraryQ(), "Q1", s.file(A03), QStringLiteral("a03"), All),
                  std::make_shared<AddToPlaylistChange>(s.pioneerQ(), s.libraryQ(), "Q1", s.file(A10), QStringLiteral("a10"), All, 0),
                  std::make_shared<RenamePlaylistChange>(s.pioneerQ(), s.libraryQ(), "Q1", "Q Renamed", All)},
                 s.pioneerQ(), s.libraryQ());
        requireSaved(saved);
        assert(exportPdbOf(s.pioneer) != pdbBefore);
        requireSaved(testing::undoSave(saved, s.pioneerQ(), s.libraryQ()));
        assert(exportPdbOf(s.pioneer) == pdbBefore && "export.pdb the bytes it was");
        assert(playlistInThree(s, "Q1") == q1Before && "Q1 as it was in all three");
        assert(playlistInThree(s, "Q Renamed") == (InThree{{"<none>"}, {"<none>"}, {"<none>"}}));
        std::cout << "case 2e (Undo Last Save after a save of shared rows only: the stick as it was) OK\n";
    }

    // 2f. After a save that went back, landing the held rows is a no-op.
    if (wanted("2f")) {
        const ThreeStick s = threeStick(pdbFixtures, olFixture, "held_saves_after_rollback");
        const std::string pdbBefore = exportPdbOf(s.pioneer);
        application::CancellationToken token;
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, s.pioneerQ(), s.libraryQ());
        const SaveLoopResult failed = runSaveLoop(
            {std::make_shared<CreatePlaylistChange>(s.pioneerQ(), s.libraryQ(), "Fresh", All),
             std::make_shared<Sharing>(s.pioneerQ(), QStringLiteral("fail"),
                                       [](SaveContext &, FormatWriteSession &, PdbRowWriter &) { return false; })},
            ctx);
        assert(failed.rolledBack);
        const auto commitsBefore = infrastructure::WorkCounters::instance().snapshot().pdbWriterCommits;
        ctx.landHeldWrites();
        ctx.landHeldWrites();
        assert(infrastructure::WorkCounters::instance().snapshot().pdbWriterCommits == commitsBefore);
        assert(exportPdbOf(s.pioneer) == pdbBefore);
        std::cout << "case 2f (after a save that went back, landing the held rows does nothing) OK\n";
    }

    std::cout << "held_playlist_saves_test: all cases passed\n";
    return 0;
}
