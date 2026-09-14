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

int main()
{
    auto &noProgress = application::NullProgressReporter::instance();

    // 1. Plain files. The change before the failing one keeps its write;
    //    the failing one's writes are undone, including a file it created
    //    and one it wrote without declaring (protected on the way).
    {
        fs::path stick = seabass::testing::scratchRoot() / "seabass_failed_change_rollback_plain";
        fs::remove_all(stick);
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
        fs::remove_all(stick);
        std::cout << "case 1 (a failed change's writes are put back, earlier changes kept) OK\n";
    }

    // 2. A change that throws is rolled back the same way.
    {
        fs::path stick = seabass::testing::scratchRoot() / "seabass_failed_change_rollback_throw";
        fs::remove_all(stick);
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
        fs::remove_all(stick);
        std::cout << "case 2 (a throwing change is put back too) OK\n";
    }

    // 3. Through a scratch copy. Both changes write the copy, not the stick;
    //    the commit at the end must carry the first change and not the
    //    second, and say it applied one update, not two.
    {
        fs::path stick = seabass::testing::scratchRoot() / "seabass_failed_change_rollback_scratch";
        fs::remove_all(stick);
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
        fs::remove_all(stick);
        std::cout << "case 3 (a scratch copy commits the changes that landed, not the one that failed) OK\n";
    }

    // 4. A WAL database with its writer still open. Putting the file back
    //    while the connection lives would let its close write the failed
    //    change's WAL frames straight back in, so this is the case that
    //    proves the writers are closed first.
    {
        fs::path stick = seabass::testing::scratchRoot() / "seabass_failed_change_rollback_wal";
        fs::remove_all(stick);
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
        fs::remove_all(stick);
        std::cout << "case 4 (a WAL database is put back with its writer closed first) OK\n";
    }

    std::cout << "failed_change_rollback_test: all cases passed\n";
    return 0;
}
