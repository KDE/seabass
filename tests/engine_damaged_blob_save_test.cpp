// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// One Engine row with a damaged blob does not fail a save (#107): the cue
// writer refuses that row before it writes anything of it
// (DamagedEngineRowError), the save puts back whatever else the change had
// written, skips it with a note naming the track, and saves the rest.
//
// 1. Three tracks of a created 3.x library, a hot cue added to each; the
//    second's quickCues blob is cut short (a stream that does not end:
//    libdjinterop would never come back from it). The first and third get
//    their cue; the second is skipped, named by its title in the save's
//    notes, and its blob is still the bytes it was.
// 2. A restore of one track to rekordbox, Engine and Device Library Plus,
//    one unit, the Engine row's quickCues damaged:
//    a. Engine first: the unit is skipped whole (the other two changes
//       are not applied), a change outside the unit still lands, and
//       PIONEER is byte for byte as before;
//    b. rekordbox first: that part has landed when Engine refuses, and
//       cannot be taken back alone, so the save fails and goes back
//       whole: PIONEER and Engine Library byte for byte as before.
// 3. Clean Up with rekordbox as its own catalog and Engine as the second:
//    it writes rekordbox (merged cues, the removed copy's row, Device
//    Library Plus, the pending deletion) and then meets the damaged
//    Engine survivor row. After the skip PIONEER and the pending deletion
//    list are byte for byte as before. The same plan on an undamaged copy
//    goes through and changes PIONEER, so the put-back is a real one.
// 4. A skipped change that moved a file, then a change that moves another
//    and fails: the save goes back whole, each file where it started,
//    the skipped change's move not undone twice.
//
// Every save runs under a watchdog: a hang fails the test, named.
//
// usage: engine_damaged_blob_save_test <tests/fixtures>

#include <QString>

#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/duplicate_cleanup.hpp"
#include "domain/metadata_restore.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/changes/cleanup_group_change.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_blob_check.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using seabass::domain::CatalogRowRef;
using seabass::domain::CuePoint;
using seabass::domain::Track;
namespace fs = std::filesystem;

namespace
{

// Runs `body`; one that has not come back after `limit` ends the test as
// failed, naming it.
template<typename T>
T within(const std::string &what, std::chrono::seconds limit, const std::function<T()> &body)
{
    std::mutex lock;
    std::condition_variable done;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> held(lock);
        if (!done.wait_for(held, limit, [&] { return finished; })) {
            std::cerr << "FAILED: " << what << " did not finish within " << limit.count() << " s (hung)\n";
            std::cerr.flush();
            std::_Exit(1);
        }
    });
    T result = body();
    {
        std::lock_guard<std::mutex> held(lock);
        finished = true;
    }
    done.notify_one();
    watchdog.join();
    return result;
}

SaveLoopResult save(const std::string &what, const std::vector<std::shared_ptr<PendingChange>> &changes,
                    const QString &rekordboxPath, const QString &enginePath)
{
    return within<SaveLoopResult>(what, std::chrono::seconds(120), [&] {
        auto &noProgress = seabass::application::NullProgressReporter::instance();
        seabass::application::CancellationToken token;
        SaveContext ctx(token, noProgress, {}, rekordboxPath, enginePath);
        SaveLoopResult result = runSaveLoop(changes, ctx);
        std::cout << "  " << what << ": error \"" << result.error.toStdString() << "\"\n";
        for (const QString &note : result.notes) {
            std::cout << "  " << what << ": note: " << note.toStdString() << "\n";
        }
        return result;
    });
}

std::string read(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Every file under `root`, by relative path. -shm is rebuilt from the log,
// and an empty -wal is the state of no -wal (SQLite makes one on open).
std::map<std::string, std::string> snapshot(const fs::path &root)
{
    std::map<std::string, std::string> files;
    if (!fs::exists(root)) {
        return files;
    }
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = seabass::pathToUtf8(entry.path().filename());
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-shm") == 0) {
            continue;
        }
        std::error_code ec;
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-wal") == 0 && fs::file_size(entry.path(), ec) == 0) {
            continue;
        }
        files[seabass::pathToGenericUtf8(fs::relative(entry.path(), root))] = read(entry.path());
    }
    return files;
}

bool same(const std::map<std::string, std::string> &before, const std::map<std::string, std::string> &after,
          const std::string &what)
{
    bool equal = true;
    for (const auto &[path, bytes] : before) {
        const auto found = after.find(path);
        if (found == after.end() || found->second != bytes) {
            std::cerr << what << ": " << path << (found == after.end() ? " removed\n" : " changed\n");
            equal = false;
        }
    }
    for (const auto &[path, bytes] : after) {
        if (!before.count(path)) {
            std::cerr << what << ": " << path << " created\n";
            equal = false;
        }
    }
    return equal;
}

std::string quickCuesOf(const fs::path &mdb, std::int64_t id)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(seabass::pathToUtf8(mdb).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT quickCues FROM PerformanceData WHERE trackId = ?1;", -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, id);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_BLOB) {
        out.assign(static_cast<const char *>(sqlite3_column_blob(stmt, 0)),
                   static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

// Cuts the row's quickCues blob short: its stream no longer ends.
std::string damageQuickCues(const fs::path &mdb, std::int64_t id)
{
    const std::string healthy = quickCuesOf(mdb, id);
    assert(healthy.size() > 12);
    const std::string damaged = healthy.substr(0, healthy.size() / 2);
    sqlite3 *db = nullptr;
    sqlite3_open_v2(seabass::pathToUtf8(mdb).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, "UPDATE PerformanceData SET quickCues = ?1 WHERE trackId = ?2;", -1, &stmt, nullptr);
    sqlite3_bind_blob(stmt, 1, damaged.data(), static_cast<int>(damaged.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, id);
    assert(sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return damaged;
}

// Engine rows with a quickCues blob, in id order.
std::vector<std::int64_t> engineRowsWithCues(const fs::path &mdb)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(seabass::pathToUtf8(mdb).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT trackId FROM PerformanceData WHERE length(quickCues) > 12 AND length(trackData) > 4 "
                       "ORDER BY trackId;",
                       -1, &stmt, nullptr);
    std::vector<std::int64_t> ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ids;
}

// A stick: the anonymized fixture's PIONEER and Engine Library side by side.
struct Stick
{
    fs::path root;
    fs::path pioneer;
    fs::path engine;
    fs::path mdb;
};

Stick freshStick(const fs::path &fixtures, const std::string &name)
{
    Stick s;
    s.root = seabass::testing::scratchRoot() / ("seabass_engine_damaged_blob_save_" + name);
    std::error_code ec;
    fs::remove_all(s.root, ec);
    fs::create_directories(s.root);
    s.pioneer = s.root / "PIONEER";
    s.engine = s.root / "Engine Library";
    seabass::testing::copyPioneerFixture(fixtures / "anonymized_library" / "rekordbox", s.pioneer, ec);
    assert(!ec);
    fs::copy(fixtures / "anonymized_library" / "engine", s.engine, fs::copy_options::recursive);
    s.mdb = s.engine / "Database2" / "m.db";
    return s;
}

// rekordbox tracks with a file, the first with a cue.
std::vector<Track> mirroredTracks(const fs::path &pioneer, std::size_t wanted)
{
    std::vector<Track> picked;
    const std::string root = seabass::pathToUtf8(pioneer);
    // Every rekordbox track of the fixture is in exportLibrary.db too.
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(root);
    for (const Track &track : reader.readAll()) {
        if (track.filePath.empty() || (picked.empty() && track.cues.empty())) {
            continue;
        }
        picked.push_back(track);
        if (picked.size() == wanted) {
            return picked;
        }
    }
    std::cerr << "the fixture offers no " << wanted << " mirrored rekordbox tracks\n";
    std::exit(1);
}

bool notesName(const SaveLoopResult &result, const std::vector<QString> &words)
{
    for (const QString &note : result.notes) {
        bool all = true;
        for (const QString &w : words) {
            all = all && note.contains(w);
        }
        if (all) {
            return true;
        }
    }
    return false;
}

// 1. Three tracks, the second's cues damaged.
void caseThreeTracks()
{
    std::cout << "case 1: three tracks, one with damaged cues\n";
    const fs::path stick = seabass::testing::scratchRoot() / "seabass_engine_damaged_blob_save_three";
    fs::remove_all(stick);
    const fs::path library = stick / "Engine Library";
    fs::create_directories(stick);
    std::vector<std::int64_t> ids;
    {
        auto db = djinterop::engine::create_database(seabass::pathToUtf8(library));
        for (int i = 1; i <= 3; ++i) {
            djinterop::track_snapshot s;
            s.title = "Track " + std::to_string(i);
            s.relative_path = "../Contents/track-" + std::to_string(i) + ".flac";
            s.sample_rate = 44100.0;
            s.sample_count = 44100ull * 60;
            s.hot_cues.assign(8, std::nullopt);
            s.hot_cues[0] = djinterop::hot_cue{"One", 44100.0, djinterop::engine::standard_pad_colors::pad_1};
            ids.push_back(db.create_track(s).id());
        }
    }
    const fs::path mdb = library / "Database2" / "m.db";
    const std::string damaged = damageQuickCues(mdb, ids[1]);

    const QString root = pathToQString(library);
    std::vector<std::shared_ptr<PendingChange>> changes;
    for (int i = 0; i < 3; ++i) {
        changes.push_back(std::make_shared<AddCueChange>(QStringLiteral("engine"), root, QString::number(ids[i]),
                                                         3000.0, QStringLiteral("hot"), 2, QStringLiteral("#00FF00"),
                                                         QStringLiteral("Two"), false, 0.0,
                                                         QStringLiteral("Track %1").arg(i + 1)));
    }
    const SaveLoopResult result = save("case 1", changes, {}, root);
    assert(result.error.isEmpty() && !result.rolledBack && "one damaged row does not fail the save");
    assert(result.appliedIds.size() == 3);
    assert(result.skippedIds == QStringList{changes[1]->id()} && "the damaged row's change is skipped");
    assert(notesName(result, {QStringLiteral("Track 2"), QStringLiteral("quickCues"), QString::number(ids[1])})
           && "the save's notes name the skipped track, its title and the blob");
    assert(quickCuesOf(mdb, ids[1]) == damaged && "nothing of the damaged row was written");

    seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(library));
    for (const Track &t : reader.readAll()) {
        if (t.sourceId == std::to_string(ids[1])) {
            continue;
        }
        bool second = false;
        for (const CuePoint &c : t.cues) {
            second = second || (c.hotCueNumber == 2 && std::abs(c.positionMs - 3000.0) < 1.0);
        }
        assert(second && "the save went on past the damaged row");
    }
    fs::remove_all(stick);
    std::cout << "case 1 OK\n";
}

std::shared_ptr<PendingChange> restoreOf(const QString &format, const QString &path, const std::string &sourceId,
                                         const Track &track)
{
    seabass::domain::MetadataRestoreProposal proposal;
    proposal.stickTrack = track;
    proposal.stickTrack.sourceId = sourceId;
    proposal.cues = track.cues;
    CuePoint extra;
    extra.kind = CuePoint::Kind::Hot;
    extra.hotCueNumber = 7;
    extra.positionMs = 2468.0;
    extra.comment = "Restored";
    proposal.cues.push_back(extra);
    proposal.cuesOffered = true;
    return std::make_shared<RestoreMetadataChange>(format, path, QString::fromStdString(sourceId), proposal, 1);
}

// 2. A restore of one track to all three catalogs, Engine's row damaged.
void caseRestoreUnit(const fs::path &fixtures)
{
    std::cout << "case 2: a restore unit over three catalogs\n";
    for (const bool engineFirst : {true, false}) {
        const std::string what = engineFirst ? "case 2a (Engine first)" : "case 2b (rekordbox first)";
        Stick s = freshStick(fixtures, engineFirst ? "restore-a" : "restore-b");
        const Track track = mirroredTracks(s.pioneer, 1).front();
        const std::vector<std::int64_t> rows = engineRowsWithCues(s.mdb);
        const std::int64_t damagedRow = rows.at(0);
        const std::int64_t otherRow = rows.at(1);
        damageQuickCues(s.mdb, damagedRow);
        const auto pioneerBefore = snapshot(s.pioneer);
        const auto engineBefore = snapshot(s.engine);
        const std::string damagedBefore = quickCuesOf(s.mdb, damagedRow);
        const std::string otherBefore = quickCuesOf(s.mdb, otherRow);

        const QString rb = pathToQString(s.pioneer);
        const QString en = pathToQString(s.engine);
        const auto rekordbox = restoreOf(QStringLiteral("rekordbox"), rb, track.sourceId, track);
        const auto engine = restoreOf(QStringLiteral("engine"), en, std::to_string(damagedRow), track);
        const auto oneLibrary = restoreOf(QStringLiteral("onelibrary"), rb, "1", track);
        assert(rekordbox->unitKeys() == engine->unitKeys() && engine->unitKeys() == oneLibrary->unitKeys()
               && "one restore is one unit");
        // A change outside the unit, to show the rest of the save lands.
        const auto outside =
            std::make_shared<AddCueChange>(QStringLiteral("engine"), en, QString::number(otherRow), 3000.0,
                                           QStringLiteral("hot"), 8, QStringLiteral("#00FF00"), QStringLiteral("Eight"),
                                           false, 0.0, QStringLiteral("Other"));
        std::vector<std::shared_ptr<PendingChange>> changes =
            engineFirst ? std::vector<std::shared_ptr<PendingChange>>{engine, rekordbox, oneLibrary, outside}
                        : std::vector<std::shared_ptr<PendingChange>>{rekordbox, engine, oneLibrary};
        const SaveLoopResult result = save(what, changes, rb, en);
        if (engineFirst) {
            assert(result.error.isEmpty() && !result.rolledBack && "the unit is skipped, the save goes on");
            assert(result.skippedIds.size() == 3 && result.skippedIds.contains(engine->id())
                   && result.skippedIds.contains(rekordbox->id()) && result.skippedIds.contains(oneLibrary->id())
                   && "the whole unit is skipped");
            assert(result.appliedIds.contains(outside->id()) && !result.skippedIds.contains(outside->id()));
            assert(notesName(result, {QStringLiteral("quickCues"), QString::number(damagedRow)}));
            assert(same(pioneerBefore, snapshot(s.pioneer), what) && "rekordbox and Device Library Plus untouched");
            assert(quickCuesOf(s.mdb, damagedRow) == damagedBefore && "the damaged row untouched");
            assert(quickCuesOf(s.mdb, otherRow) != otherBefore && "the outside change landed");
        } else {
            assert(!result.error.isEmpty() && result.error.contains(QStringLiteral("quickCues"))
                   && "part of the unit had landed: the save fails");
            assert(result.rolledBack && "and goes back whole");
            assert(same(pioneerBefore, snapshot(s.pioneer), what) && "rekordbox and Device Library Plus as before");
            assert(same(engineBefore, snapshot(s.engine), what) && "Engine as before");
        }
        std::error_code ec;
        fs::remove_all(s.root, ec);
        std::cout << what << " OK\n";
    }
}

// 3. Clean Up: rekordbox written first, then the damaged Engine survivor.
void caseCleanupPutBack(const fs::path &fixtures)
{
    std::cout << "case 3: Clean Up writes rekordbox, then meets the damaged Engine row\n";
    for (const bool damaged : {false, true}) {
        const std::string what = damaged ? "case 3 (damaged)" : "case 3 (undamaged)";
        Stick s = freshStick(fixtures, damaged ? "cleanup-damaged" : "cleanup-healthy");
        const std::vector<Track> pair = mirroredTracks(s.pioneer, 2);
        const std::vector<std::int64_t> rows = engineRowsWithCues(s.mdb);
        const std::string survivorRow = std::to_string(rows.at(0));
        const std::string doomedRow = std::to_string(rows.at(1));
        if (damaged) {
            damageQuickCues(s.mdb, rows.at(0));
        }

        CuePoint engineCue;
        engineCue.kind = CuePoint::Kind::Hot;
        engineCue.hotCueNumber = 5;
        engineCue.positionMs = 9876.0;
        Track survivor = pair[0];
        survivor.catalogRows = {CatalogRowRef{"rekordbox", pair[0].sourceId, pair[0].cues},
                                CatalogRowRef{"engine", survivorRow, {}}};
        Track doomedCopy = pair[1];
        doomedCopy.catalogRows = {CatalogRowRef{"rekordbox", pair[1].sourceId, pair[1].cues},
                                  CatalogRowRef{"engine", doomedRow, {engineCue}}};
        seabass::domain::DuplicateCleanupPlan plan;
        plan.group.tracks = {survivor, doomedCopy};
        plan.survivor = survivor;
        plan.toRemove = {doomedCopy};
        plan.mergedCuesForSurvivor = survivor.cues;
        CuePoint extra;
        extra.kind = CuePoint::Kind::Memory;
        extra.positionMs = 13579.0;
        plan.mergedCuesForSurvivor.push_back(extra);
        plan.survivor.catalogRows[0].cues = pair[0].cues;
        plan.toRemove[0].catalogRows[0].cues.push_back(extra);

        const fs::path manifest = seabass::infrastructure::paths::stickPendingDeletions(s.root);
        const std::string manifestBefore = read(manifest);
        const auto pioneerBefore = snapshot(s.pioneer);
        const std::string survivorCuesBefore = quickCuesOf(s.mdb, rows.at(0));

        const QString rb = pathToQString(s.pioneer);
        const auto change = std::make_shared<CleanupGroupChange>(QStringLiteral("rekordbox"), rb, plan, 1);
        const SaveLoopResult result = save(what, {change}, rb, pathToQString(s.engine));
        assert(result.error.isEmpty() && !result.rolledBack);
        if (!damaged) {
            assert(result.skippedIds.isEmpty() && "the undamaged clean-up goes through");
            assert(!same(pioneerBefore, snapshot(s.pioneer), what + " (expected to differ)")
                   && "and writes rekordbox, so the damaged case's put-back is a real one");
            assert(quickCuesOf(s.mdb, rows.at(0)) != survivorCuesBefore && "and the Engine survivor's cues");
        } else {
            assert(result.skippedIds == QStringList{change->id()} && "the clean-up is skipped");
            assert(notesName(result, {QStringLiteral("quickCues"), QString::fromStdString(survivorRow)}));
            assert(same(pioneerBefore, snapshot(s.pioneer), what)
                   && "rekordbox and Device Library Plus byte for byte as before");
            assert(read(manifest) == manifestBefore && "the pending deletion taken back out");
            assert(quickCuesOf(s.mdb, rows.at(0)) == survivorCuesBefore && "the damaged row untouched");
        }
        std::error_code ec;
        fs::remove_all(s.root, ec);
        std::cout << what << " OK\n";
    }
}

class ScriptedChange : public PendingChange
{
public:
    ScriptedChange(QString id, std::function<ChangeOutcome(SaveContext &)> run)
        : m_id(std::move(id)), m_run(std::move(run))
    {
    }
    QString id() const override { return m_id; }
    QString description() const override { return "apply " + m_id; }
    QString unit() const override { return "tracks"; }
    QStringList formatsTouched() const override { return {"engine"}; }
    ChangeOutcome apply(SaveContext &ctx) override { return m_run(ctx); }

private:
    QString m_id;
    std::function<ChangeOutcome(SaveContext &)> m_run;
};

// 4. A skipped change with a move, then a failing change with a move.
void caseMoves()
{
    std::cout << "case 4: moves of a skipped change and of a failing one\n";
    const fs::path dir = seabass::testing::scratchRoot() / "seabass_engine_damaged_blob_save_moves";
    fs::remove_all(dir);
    const fs::path library = dir / "Engine Library";
    fs::create_directories(library);
    const fs::path a = dir / "a.mp3", aMoved = dir / "Contents" / "a.mp3";
    const fs::path b = dir / "b.mp3", bMoved = dir / "Contents" / "b.mp3";
    fs::create_directories(aMoved.parent_path());
    std::ofstream(a, std::ios::binary) << "the first file";
    std::ofstream(b, std::ios::binary) << "the second file, longer";
    const auto moveAndNote = [](SaveContext &ctx, const fs::path &from, const fs::path &to) {
        fs::rename(from, to);
        ctx.noteMovedForRollback(seabass::pathToUtf8(from), seabass::pathToUtf8(to), fs::file_size(to));
    };
    std::vector<std::shared_ptr<PendingChange>> changes = {
        std::make_shared<ScriptedChange>(QStringLiteral("moves-then-damaged"),
                                         [&](SaveContext &ctx) -> ChangeOutcome {
                                             moveAndNote(ctx, a, aMoved);
                                             throw seabass::infrastructure::engine::DamagedEngineRowError(
                                                 "track id=1 (\"A\") has a damaged quickCues blob: cues not written");
                                         }),
        std::make_shared<ScriptedChange>(QStringLiteral("moves-then-fails"), [&](SaveContext &ctx) {
            assert(fs::exists(a) && !fs::exists(aMoved) && "the skipped change's move was taken back");
            moveAndNote(ctx, b, bMoved);
            return ChangeOutcome::failure(QStringLiteral("the second change refused"));
        })};
    const SaveLoopResult result = save("case 4", changes, {}, pathToQString(library));
    assert(result.failedId == QStringLiteral("moves-then-fails") && result.rolledBack
           && "the failing change takes the save back, cleanly");
    assert(read(a) == "the first file" && !fs::exists(aMoved) && "the skipped change's file where it started");
    assert(read(b) == "the second file, longer" && !fs::exists(bMoved) && "the failed change's file moved back");
    fs::remove_all(dir);
    std::cout << "case 4 OK\n";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_damaged_blob_save_test <tests/fixtures>\n";
        return 2;
    }
    const fs::path fixtures = seabass::pathFromUtf8(argv[1]);
    caseThreeTracks();
    caseRestoreUnit(fixtures);
    caseCleanupPutBack(fixtures);
    caseMoves();
    std::cout << "engine_damaged_blob_save_test: all cases passed\n";
    return 0;
}
