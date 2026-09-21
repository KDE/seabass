// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A change lands whole or not at all. On RV2 a Clean Up change removed a
// file's Engine row, then failed on Device Library Plus, and the save
// committed the Engine half anyway: the stick was left listing the file in
// rekordbox and nowhere in Engine. Each case here fails a change after it
// has written something and checks that exactly the changes before it
// remain -- on a plain file, through a scratch copy, and in a WAL database
// whose writer is still open.

#include <QString>
#include <QStringList>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <system_error>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
using seabass::application::CancellationToken;
using namespace seabass::infrastructure::onelibrary;
namespace fs = std::filesystem;

namespace
{

class ScriptedChange : public PendingChange
{
public:
    ScriptedChange(QString id, std::vector<std::string> declared, std::function<ChangeOutcome(SaveContext &)> run)
        : m_id(std::move(id)), m_declared(std::move(declared)), m_run(std::move(run))
    {
    }
    QString id() const override { return m_id; }
    QString description() const override { return "apply " + m_id; }
    QString unit() const override { return "tracks"; }
    QStringList formatsTouched() const override { return {"rekordbox"}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &) const override
    {
        std::vector<BackupTarget> targets;
        for (const auto &file : m_declared) {
            targets.push_back({file, "rollback-test"});
        }
        return targets;
    }
    ChangeOutcome apply(SaveContext &ctx) override { return m_run(ctx); }

private:
    QString m_id;
    std::vector<std::string> m_declared;
    std::function<ChangeOutcome(SaveContext &)> m_run;
};

std::string read(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

int64_t cueCount(const fs::path &dbPath, int64_t contentId)
{
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbPath.string(), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement count(db, "SELECT count(*) FROM cue WHERE content_id = ?");
    count.bindInt64(1, contentId);
    count.step();
    return count.columnInt64(0);
}

}  // namespace

// A case's scratch stick: emptied when the case starts, removed when its
// block ends. Declared first in the block so it is destroyed last -- after
// the SaveContext, whose operation log keeps Seabass/seabass.log open for
// as long as it lives. POSIX removes an open file; Windows refuses ("used
// by another process"), and a remove_all() inside the block threw there.
// The minimal WAL exportLibrary.db both rollback cases write through.
void makeWalFixture(const std::filesystem::path &dbPath)
{
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbPath.string(), /*readOnly=*/false);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    db.exec("PRAGMA journal_mode = WAL;");
    db.exec("CREATE TABLE content(content_id integer primary key, title varchar, path varchar);");
    db.exec("CREATE TABLE cue(cue_id integer primary key, content_id integer, kind integer, "
            "colorTableIndex integer, cueComment varchar, isActiveLoop integer, inUsec integer, "
            "outUsec integer);");
    db.exec("CREATE TABLE hotCueBankList_cue(hotCueBankList_id integer, cue_id integer, sequenceNo integer);");
    db.exec("INSERT INTO content VALUES (1, 'One', '/Contents/One.mp3'), (2, 'Two', '/Contents/Two.mp3');");
}

struct ScratchStick
{
    fs::path path;
    explicit ScratchStick(fs::path p) : path(std::move(p)) { fs::remove_all(path); }
    ~ScratchStick()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec) {
            std::cerr << "warning: could not remove " << path.string() << ": " << ec.message() << "\n";
        }
    }
    ScratchStick(const ScratchStick &) = delete;
    ScratchStick &operator=(const ScratchStick &) = delete;
};

int main()
{
    auto &noProgress = application::NullProgressReporter::instance();

    // 1. Plain files. The change before the failing one keeps its write;
    //    the failing one's writes are undone, including a file it created
    //    and one it wrote without declaring (protected on the way).
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_plain");
        const fs::path &stick = scratch.path;
        fs::path pdb = stick / "PIONEER" / "rekordbox" / "export.pdb";
        fs::path anlz = stick / "PIONEER" / "USBANLZ" / "P001" / "ANLZ0000.EXT";
        fs::path extra = stick / "PIONEER" / "USBANLZ" / "P002" / "ANLZ0000.EXT";
        fs::path created = stick / "Seabass" / "pending-deletions.jsonl";
        write(pdb, "pdb-original");
        write(anlz, "anlz-original");
        write(extra, "extra-original");

        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, QString::fromStdString((stick / "PIONEER").string()), {});
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<ScriptedChange>("a", std::vector<std::string>{pdb.string()}, [&](SaveContext &) {
                write(pdb, "pdb-from-a");
                return ChangeOutcome::success();
            }),
            std::make_shared<ScriptedChange>("b", std::vector<std::string>{pdb.string(), anlz.string()},
                                             [&](SaveContext &c) {
                                                 write(pdb, "pdb-from-b");
                                                 write(anlz, "anlz-from-b");
                                                 c.protectForThisChange(extra.string());
                                                 write(extra, "extra-from-b");
                                                 c.protectForThisChange(created.string());
                                                 write(created, "line from b\n");
                                                 return ChangeOutcome::failure("the second catalog refused");
                                             }),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds == QStringList{"a"});
        assert(result.failedId == "b");
        assert(result.error == "the second catalog refused");
        assert(read(pdb) == "pdb-from-a");
        assert(read(anlz) == "anlz-original");
        assert(read(extra) == "extra-original");
        assert(!fs::exists(created));
        assert(read(stick / "Seabass" / "seabass.log").find("put back 4 file(s)") != std::string::npos);
        std::cout << "case 1 (a failed change's writes are put back, earlier changes kept) OK\n";
    }

    // 2. A change that throws is rolled back the same way.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_throw");
        const fs::path &stick = scratch.path;
        fs::path pdb = stick / "PIONEER" / "rekordbox" / "export.pdb";
        write(pdb, "pdb-original");
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, QString::fromStdString((stick / "PIONEER").string()), {});
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<ScriptedChange>("a", std::vector<std::string>{pdb.string()}, [&](SaveContext &) {
                write(pdb, "pdb-from-a");
                throw std::runtime_error("boom");
                return ChangeOutcome::success();
            }),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds.isEmpty() && result.error == "boom");
        assert(read(pdb) == "pdb-original");
        std::cout << "case 2 (a throwing change is put back too) OK\n";
    }

    // 3. Through a scratch copy. Both changes write the copy, not the stick;
    //    the commit at the end must carry the first change and not the
    //    second, and say it applied one update, not two.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_scratch");
        const fs::path &stick = scratch.path;
        fs::path engineLibrary = stick / "Engine Library";
        fs::path mdb = engineLibrary / "Database2" / "m.db";
        write(mdb, std::string(1 << 20, 'o'));

        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, {}, QString::fromStdString(engineLibrary.string()));
        auto writeThroughSession = [&](SaveContext &c, char fill) {
            FormatWriteSession &session =
                sharedFormatWriteSession(c, "engine", engineLibrary.string(), 100000, "rollback-test");
            assert(session.usesScratch());
            write(fs::path(session.writeRoot()) / "Database2" / "m.db", std::string(1 << 20, fill));
            session.noteItemApplied();
        };
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<ScriptedChange>("a", std::vector<std::string>{mdb.string()}, [&](SaveContext &c) {
                writeThroughSession(c, 'a');
                return ChangeOutcome::success();
            }),
            std::make_shared<ScriptedChange>("b", std::vector<std::string>{mdb.string()}, [&](SaveContext &c) {
                writeThroughSession(c, 'b');
                return ChangeOutcome::failure("no Engine track with id=546");
            }),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds == QStringList{"a"});
        assert(read(mdb) == std::string(1 << 20, 'a'));
        const std::string log = read(stick / "Seabass" / "seabass.log");
        assert(log.find("committed the engine scratch copy (1 update(s))") != std::string::npos);
        std::cout << "case 3 (a scratch copy commits the changes that landed, not the one that failed) OK\n";
    }

    // 4. A WAL database with its writer still open. Putting the file back
    //    while the connection lives would let its close write the failed
    //    change's WAL frames straight back in, so this is the case that
    //    proves the writers are closed first.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_wal");
        const fs::path &stick = scratch.path;
        fs::path pioneer = stick / "PIONEER";
        fs::create_directories(pioneer / "rekordbox");
        const fs::path dbPath = OneLibraryCueWriter::dbPathFor(pioneer.string());
        {
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, dbPath.string(), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
            db.exec("PRAGMA journal_mode = WAL;");
            db.exec("CREATE TABLE content(content_id integer primary key, title varchar, path varchar);");
            db.exec("CREATE TABLE cue(cue_id integer primary key, content_id integer, kind integer, "
                    "colorTableIndex integer, cueComment varchar, isActiveLoop integer, inUsec integer, "
                    "outUsec integer);");
            db.exec("CREATE TABLE hotCueBankList_cue(hotCueBankList_id integer, cue_id integer, sequenceNo integer);");
            db.exec("INSERT INTO content VALUES (1, 'One', '/Contents/One.mp3'), (2, 'Two', '/Contents/Two.mp3');");
        }
        const std::vector<domain::CuePoint> cues = {
            domain::CuePoint{domain::CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
            domain::CuePoint{domain::CuePoint::Kind::Hot, 2, 2000.0, "#00FF00", ""},
        };

        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, QString::fromStdString(pioneer.string()), {});
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<ScriptedChange>("a", std::vector<std::string>{dbPath.string()}, [&](SaveContext &c) {
                sharedOneLibraryWriter(c, pioneer.string()).writeCuesForPath((stick / "Contents" / "One.mp3").string(), cues);
                return ChangeOutcome::success();
            }),
            std::make_shared<ScriptedChange>("b", std::vector<std::string>{dbPath.string()}, [&](SaveContext &c) {
                sharedOneLibraryWriter(c, pioneer.string()).writeCuesForPath((stick / "Contents" / "Two.mp3").string(), cues);
                return ChangeOutcome::failure("Device Library Plus refused the next write");
            }),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds == QStringList{"a"});
        assert(cueCount(dbPath, 1) == 2);
        assert(cueCount(dbPath, 2) == 0);
        std::cout << "case 4 (a WAL database is put back with its writer closed first) OK\n";
    }

    // 5. The change that fails never touched the WAL database, but an
    //    earlier one wrote it. The rollback's fold used to be keyed to the
    //    FAILING change's protected files, so nothing folded here: change
    //    a's rows were reported applied while they sat in a -wal that a
    //    player reading exportLibrary.db alone would not see.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_wal_other");
        const fs::path &stick = scratch.path;
        fs::path pioneer = stick / "PIONEER";
        fs::create_directories(pioneer / "rekordbox");
        const fs::path dbPath = OneLibraryCueWriter::dbPathFor(pioneer.string());
        makeWalFixture(dbPath);
        const fs::path unrelated = stick / "unrelated.txt";
        write(unrelated, "before");

        const std::vector<domain::CuePoint> cues = {
            domain::CuePoint{domain::CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
        };
        // A reader with an open read transaction for the whole save. WAL
        // allows one writer alongside readers, so change a still writes --
        // but the writer's close-time checkpoint is PASSIVE, takes no busy
        // handler and gives up, which is the state finding 1 is about: in a
        // quiet test SQLite folds the log on close by itself and the check
        // would pass with the fold deleted.
        SqlCipherLibrary readerLib;
        SqlCipherDb reader(readerLib, dbPath.string(), /*readOnly=*/true);
        reader.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
        SqlCipherStatement holdOpen(reader, "SELECT content_id, title FROM content;");
        assert(holdOpen.step() && "the reader holds a read transaction open across the save");

        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, QString::fromStdString(pioneer.string()), {});
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<ScriptedChange>("a", std::vector<std::string>{dbPath.string()}, [&](SaveContext &c) {
                sharedOneLibraryWriter(c, pioneer.string()).writeCuesForPath((stick / "Contents" / "One.mp3").string(), cues);
                return ChangeOutcome::success();
            }),
            // Declares only the unrelated file: exportLibrary.db is nowhere
            // in this change's checkpoints.
            std::make_shared<ScriptedChange>("b", std::vector<std::string>{unrelated.string()}, [&](SaveContext &) {
                write(unrelated, "after");
                return ChangeOutcome::failure("something else refused");
            }),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds == QStringList{"a"});
        assert(read(unrelated) == "before");

        // The rollback looked at the database this save WROTE, not at what
        // the failing change protected, and said what it found. Without that
        // the log is silent and a's rows sit in a -wal while the summary
        // reports them applied.
        std::error_code ec;
        const fs::path stickLog = stick / "Seabass" / "seabass.log";
        assert(fs::exists(stickLog, ec) && "the save logs to the stick");
        const std::string logText = read(stickLog);
        // The fold's own line, not "write-ahead log": the move-aside and
        // could-not-tell lines say that too, and a regression into either
        // branch would still have passed.
        const bool folded = logText.find("after the rollback exportLibrary.db kept") != std::string::npos;
        if (!folded) {
            std::cerr << "case 5: the rollback did not report the fold:\n" << logText << "\n";
        }
        assert(folded && "the rollback measures the database the save wrote, whatever the failure touched");
        assert(cueCount(dbPath, 1) == 1);
        std::cout << "case 5 (a database an earlier change wrote is folded even when the failure is elsewhere) OK\n";
    }

    // 6. The database itself could not be put back. Folding then writes one
    //    generation's log over another's file and mixes them; leaving both
    //    alone keeps the state inert and recoverable. The fold must not run.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_wal_stuck");
        const fs::path &stick = scratch.path;
        fs::path pioneer = stick / "PIONEER";
        fs::create_directories(pioneer / "rekordbox");
        const fs::path dbPath = OneLibraryCueWriter::dbPathFor(pioneer.string());
        makeWalFixture(dbPath);

        const std::vector<domain::CuePoint> cues = {
            domain::CuePoint{domain::CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
        };
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, QString::fromStdString(pioneer.string()), {});
        // Set from inside change "b" once it knows whether the sabotage
        // below actually took: a Windows-only sharing violation the rest
        // of this case's own assertions have to be skipped for, not
        // something to find out about from an unrelated assert() lower
        // down.
        bool sabotaged = false;
        // Kept apart from `sabotaged`: only a platform refusing the removal
        // may skip this case. Change "a" failing, so that "b" never ran,
        // or the sabotage missing for any other reason, is a failure.
        bool bRan = false;
        bool removalRefused = false;
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<ScriptedChange>("a", std::vector<std::string>{dbPath.string()}, [&](SaveContext &c) {
                sharedOneLibraryWriter(c, pioneer.string()).writeCuesForPath((stick / "Contents" / "One.mp3").string(), cues);
                return ChangeOutcome::success();
            }),
            std::make_shared<ScriptedChange>("b", std::vector<std::string>{dbPath.string()}, [&](SaveContext &c) {
                sharedOneLibraryWriter(c, pioneer.string()).writeCuesForPath((stick / "Contents" / "Two.mp3").string(), cues);
                // Only the database's put-back must fail -- its -wal has to go
                // back fine, so the stick ends up with an old log beside a
                // database of another generation. A non-empty directory where
                // the file was makes the rename onto it fail, and nothing else.
                // (A read-only directory fails both, which leaves a pair that
                // is still self-consistent and proves nothing.)
                //
                // This case's own header names the precondition: "a WAL
                // database whose writer is still open" -- sharedOneLibraryWriter
                // above is that writer, and it is still holding dbPath open
                // right here. POSIX lets you unlink a file out from under an
                // open handle (the old inode lives on until the last close);
                // Windows does not, unless that handle was opened with
                // FILE_SHARE_DELETE, so fs::remove() below fails outright
                // with ERROR_SHARING_VIOLATION on this platform -- confirmed
                // directly -- and dbPath is still the same ordinary file
                // afterwards, never a directory at all.
                bRan = true;
                std::error_code ec;
                fs::remove(dbPath, ec);
                removalRefused = static_cast<bool>(ec);
                fs::create_directory(dbPath, ec);
                write(dbPath / "occupied", "x");
                sabotaged = fs::is_directory(dbPath, ec) && fs::exists(dbPath / "occupied", ec);
                return ChangeOutcome::failure("Device Library Plus refused the next write");
            }),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(bRan && "change b never ran, so nothing in this case was checked");
        assert((sabotaged || removalRefused) && "the sabotage failed for a reason other than the platform refusing it");
        if (!sabotaged) {
            std::cout << "case 6 SKIPPED (this filesystem would not let the database be replaced by a "
                         "directory while its writer still had it open, so the put-back failure this "
                         "case exists to check for could not be constructed)\n";
        } else {
            std::error_code ec;
            assert(!result.error.isEmpty());
            // The asymmetry this case exists for really happened.
            assert(fs::is_directory(dbPath, ec) && "the database did not go back");
            // Not folded: the log and the database are from different
            // generations, and checkpointing one into the other mixes them.
            // Asserted, not printed: the stick's own log is where the skip is
            // observable, and a fold that quietly did nothing would look the
            // same as one that was correctly skipped.
            const fs::path stickLog = stick / "Seabass" / "seabass.log";
            assert(fs::exists(stickLog, ec) && "the save logs to the stick");
            const std::string logText = read(stickLog);
            // Out of SQLite's reach, not merely unfolded: a live -wal beside the
            // wrong generation is replayed into it on the next open.
            const fs::path wal = fs::path(dbPath.string() + "-wal");
            const fs::path stale = fs::path(dbPath.string() + "-wal.seabass-stale");
            if (fs::exists(wal, ec) || !fs::exists(stale, ec)) {
                std::cerr << "case 6: wal live=" << fs::exists(wal, ec) << " stale=" << fs::exists(stale, ec)
                          << "\n" << logText << "\n";
            }
            assert(!fs::exists(wal, ec) && "no live log is left beside a database of another generation");
            assert(fs::exists(stale, ec) && "the log is kept, renamed, not destroyed");
            assert(logText.find("aside as .seabass-stale") != std::string::npos
                   && "the stick log says what was moved");
            std::cout << "case 6 (a database that could not be put back is left unfolded) OK\n";
        }
    }

    // 7. A file the failed change created whose existence the rollback
    //    cannot establish. fs::exists() answers EIO on a dying stick, a
    //    Windows sharing violation and an unsearchable parent directory
    //    the same way it answers "no": false, with an error code set.
    //    Read as a no, the file is counted as already gone -- the
    //    rollback removes nothing, reports nothing, and says it is
    //    complete while the file the change created is still there.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_blind");
        const fs::path &stick = scratch.path;
        const fs::path pdb = stick / "PIONEER" / "rekordbox" / "export.pdb";
        const fs::path locked = stick / "Seabass" / "staging";
        const fs::path created = locked / "pending-deletions.jsonl";
        write(pdb, "pdb-original");

        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, QString::fromStdString((stick / "PIONEER").string()), {});
        bool bRan = false;
        bool blinded = false;
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<ScriptedChange>("a", std::vector<std::string>{pdb.string()}, [&](SaveContext &) {
                write(pdb, "pdb-from-a");
                return ChangeOutcome::success();
            }),
            std::make_shared<ScriptedChange>("b", std::vector<std::string>{pdb.string()}, [&](SaveContext &c) {
                bRan = true;
                write(pdb, "pdb-from-b");
                c.protectForThisChange(created.string());
                write(created, "line from b\n");
                // Take away the right to search the directory the file is
                // in: the file is untouched, only the answer about it is.
                std::error_code ec;
                fs::permissions(locked, fs::perms::none, ec);
                const bool answered = fs::exists(created, ec);
                blinded = !answered && static_cast<bool>(ec);
                return ChangeOutcome::failure("the second catalog refused");
            }),
        };
        auto result = runSaveLoop(changes, ctx);
        std::error_code ec;
        fs::permissions(locked, fs::perms::owner_all, ec);
        assert(bRan && "change b never ran, so nothing in this case was checked");
        if (!blinded) {
            // Running as root, or on a platform where directory
            // permissions do not gate a stat: the state this case exists
            // to check for could not be constructed.
            std::cout << "case 7 SKIPPED (this platform still answered whether a file in an unsearchable "
                         "directory exists, so the blind rollback could not be constructed)\n";
        } else {
            assert(read(pdb) == "pdb-from-a" && "the rest of the rollback still ran");
            assert(fs::exists(created, ec) && "the file really was left behind");
            // Said out loud, in the save's own error and in the stick's
            // log: a rollback that could not tell is not a rollback that
            // succeeded, and the backup is what to go back to.
            assert(result.error.contains("putting back what it had already written failed")
                   && "the save says the rollback failed");
            if (!result.error.contains("could not tell whether")) {
                std::cerr << "case 7: " << result.error.toStdString() << "\n";
            }
            assert(result.error.contains("could not tell whether") && "and says it is because it could not tell");
            const std::string logText = read(stick / "Seabass" / "seabass.log");
            assert(logText.find("putting back what the failed change had written FAILED") != std::string::npos
                   && "the stick's log says so too");
            assert(logText.find("put back 2 file(s)") == std::string::npos
                   && "and never claims it put the file back");
            std::cout << "case 7 (a rollback that cannot tell whether a file is there says so) OK\n";
        }
    }

    // 8. What the save's own backup is for, and when it is not for
    //    anything. A save takes a backup before it writes; if it then
    //    writes nothing and puts everything back, that backup is a copy
    //    of a stick that never changed. Round 5 found one on a stick too
    //    full for the save to proceed: a complete record, backup.zip and
    //    manifest, taking the space the save had just been refused for.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_failed_change_rollback_records");
        const fs::path &stick = scratch.path;
        const fs::path pdb = stick / "PIONEER" / "rekordbox" / "export.pdb";
        const fs::path records = stick / "Seabass" / "backups";
        const auto recordsNow = [&] {
            int n = 0;
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator(records, ec)) {
                if (entry.is_directory()) {
                    ++n;
                }
            }
            return n;
        };

        {
            write(pdb, "pdb-original");
            CancellationToken token;
            SaveContext ctx(token, noProgress, {}, QString::fromStdString((stick / "PIONEER").string()), {});
            // Counted from INSIDE the change, which runs after the save
            // has taken its backup: without this, the case passes just as
            // happily against a save that never backed anything up, since
            // "no records afterwards" is true either way. That is the
            // agreeable-counter shape this round kept meeting.
            int recordsDuringTheSave = -1;
            std::vector<std::shared_ptr<PendingChange>> changes = {
                std::make_shared<ScriptedChange>("only", std::vector<std::string>{pdb.string()}, [&](SaveContext &) {
                    recordsDuringTheSave = recordsNow();
                    write(pdb, "pdb-from-the-change");
                    return ChangeOutcome::failure("the catalog refused");
                }),
            };
            auto result = runSaveLoop(changes, ctx);
            assert(recordsDuringTheSave == 1 && "the save did take its backup before writing");
            assert(result.appliedIds.isEmpty() && "nothing applied, which is the case this is about");
            assert(read(pdb) == "pdb-original" && "and the file went back");
            assert(recordsNow() == 0 && "the backup of a stick that never changed does not stay");
            assert(result.backups.empty() && "and the save does not offer it as something to undo from");
            std::cout << "case 8 (a save that wrote nothing keeps no backup of it) OK\n";
        }

        {
            // The other half, and the more important one: a change that
            // DID apply needs its backup, because that backup is the way
            // back from it. Undo lives on exactly this.
            write(pdb, "pdb-original");
            CancellationToken token;
            SaveContext ctx(token, noProgress, {}, QString::fromStdString((stick / "PIONEER").string()), {});
            std::vector<std::shared_ptr<PendingChange>> changes = {
                std::make_shared<ScriptedChange>("a", std::vector<std::string>{pdb.string()}, [&](SaveContext &) {
                    write(pdb, "pdb-from-a");
                    return ChangeOutcome::success();
                }),
                std::make_shared<ScriptedChange>("b", std::vector<std::string>{pdb.string()}, [&](SaveContext &) {
                    write(pdb, "pdb-from-b");
                    return ChangeOutcome::failure("the second catalog refused");
                }),
            };
            auto result = runSaveLoop(changes, ctx);
            assert(result.appliedIds == QStringList{"a"});
            assert(read(pdb) == "pdb-from-a");
            assert(recordsNow() > 0 && "a save that applied something keeps the backup to undo from");
            assert(!result.backups.empty() && "and offers it");
            std::cout << "case 8b (a save that applied something keeps its backup) OK\n";
        }
    }

    std::cout << "failed_change_rollback_test: all cases passed\n";
    return 0;
}
