// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The save loop every LibraryEditSession runs: applies staged changes in
// order, stops between them on cancel or failure, runs the finish hooks
// regardless, and reports exactly which changes landed.

#include <QString>
#include <QStringList>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"
#include "gui/edit/changes/repair_artwork_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/sleep_inhibitor.hpp"

#include "engine_information_fixture.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

enum class Behavior { Ok, Skip, Fail, Throw, CancelDuringApply };

struct Log
{
    QStringList applied;
    QStringList statuses;
};

class FakeChange : public PendingChange
{
public:
    FakeChange(QString id, Behavior behavior, Log &log, std::function<void(SaveContext &)> extra = {})
        : m_id(std::move(id)), m_behavior(behavior), m_log(log), m_extra(std::move(extra))
    {
    }

    QString id() const override { return m_id; }
    QString description() const override { return "apply " + m_id; }
    QString unit() const override { return "tracks"; }
    QStringList formatsTouched() const override { return {"rekordbox"}; }

    ChangeOutcome apply(SaveContext &ctx) override
    {
        if (m_extra) {
            m_extra(ctx);
        }
        switch (m_behavior) {
        case Behavior::Fail:
            return ChangeOutcome::failure("nope");
        case Behavior::Throw:
            throw std::runtime_error("boom");
        case Behavior::CancelDuringApply:
            const_cast<CancellationToken &>(ctx.cancel()).cancel();
            break;
        case Behavior::Skip:
            return ChangeOutcome::skip();
        case Behavior::Ok:
            break;
        }
        m_log.applied << m_id;
        return ChangeOutcome::success();
    }

private:
    QString m_id;
    Behavior m_behavior;
    Log &m_log;
    std::function<void(SaveContext &)> m_extra;
};

struct Counter
{
    int created = 0;
    int uses = 0;
};

fs::path makeStick(const fs::path &root)
{
    fs::remove_all(root);
    fs::create_directories(root / "PIONEER" / "rekordbox");
    std::ofstream(root / "PIONEER" / "rekordbox" / "export.pdb") << "pdb-bytes";
    return root / "PIONEER";
}

// ---- issue #42: the import counter follows Seabass's own pdb writes ----

// A stick with both libraries: export.pdb whose header carries `sequence`
// (the sixth 32-bit word, where the format keeps it) and an Engine m.db
// whose one Information row, at `rowId`, holds `counter`.
struct TwoCatalogs
{
    fs::path pioneer;
    fs::path engine;
};

TwoCatalogs makeTwoCatalogStick(const fs::path &root, std::uint32_t sequence, std::uint32_t counter, int rowId = 1)
{
    fs::remove_all(root);
    TwoCatalogs stick{root / "PIONEER", root / "Engine Library"};
    fs::create_directories(stick.pioneer / "rekordbox");
    {
        std::ofstream pdb(stick.pioneer / "rekordbox" / "export.pdb", std::ios::binary);
        const std::uint32_t header[6] = {0, 4096, 20, 100, 0, sequence};
        pdb.write(reinterpret_cast<const char *>(header), sizeof header);
        pdb << std::string(4096 - sizeof header, '\0');
    }
    fs::create_directories(stick.engine / "Database2");
    seabass::testing::createEngineInformation(stick.engine / "Database2" / "m.db", counter, rowId);
    return stick;
}

std::uint32_t pdbSequence(const fs::path &pioneer)
{
    std::ifstream in(pioneer / "rekordbox" / "export.pdb", std::ios::binary);
    std::uint32_t header[6] = {};
    in.read(reinterpret_cast<char *>(header), sizeof header);
    return header[5];
}

std::int64_t engineCounter(const fs::path &engine)
{
    return seabass::testing::readEngineImportCounter(engine / "Database2" / "m.db");
}

// Rewrites export.pdb the way every real writer does: through the save's
// shared rekordbox session, into its writeRoot -- a scratch copy when
// the hint is large enough, the stick itself when it is not.
class MovesPdbSequence : public PendingChange
{
public:
    MovesPdbSequence(fs::path pioneer, std::uint32_t to, int itemCountHint, bool *usedScratch = nullptr,
                     std::function<void()> afterWrite = {})
        : m_pioneer(std::move(pioneer)), m_to(to), m_hint(itemCountHint), m_usedScratch(usedScratch),
          m_afterWrite(std::move(afterWrite))
    {
    }
    QString id() const override { return QStringLiteral("cleanup:moves"); }
    QString description() const override { return QStringLiteral("rewrite export.pdb"); }
    QString unit() const override { return QStringLiteral("tracks"); }
    QStringList formatsTouched() const override { return {QStringLiteral("rekordbox")}; }
    ChangeOutcome apply(SaveContext &ctx) override
    {
        FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", m_pioneer.string(), m_hint, "cleanup");
        if (m_usedScratch != nullptr) {
            *m_usedScratch = session.usesScratch();
        }
        const fs::path pdb = fs::path(session.writeRoot()) / "rekordbox" / "export.pdb";
        std::fstream out(pdb, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(20);
        out.write(reinterpret_cast<const char *>(&m_to), sizeof m_to);
        out.close();
        session.noteItemApplied();
        if (m_afterWrite) {
            m_afterWrite();
        }
        return ChangeOutcome::success();
    }

private:
    fs::path m_pioneer;
    std::uint32_t m_to;
    int m_hint;
    bool *m_usedScratch;
    std::function<void()> m_afterWrite;
};

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_edit_session_save_loop_test";
    fs::path pioneer = makeStick(root);
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    QString rb = QString::fromStdString(pioneer.string());

    // 1. Every change applies: all ids reported, hooks ran with ok=true,
    //    the status line named each change.
    {
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, [&](const QString &s) { log.statuses << s; }, rb, {});
        bool hookOk = false, hookRan = false;
        ctx.onFinish([&](bool ok) { hookRan = true; hookOk = ok; });
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log),
            std::make_shared<FakeChange>("b", Behavior::Ok, log),
            std::make_shared<FakeChange>("c", Behavior::Ok, log),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds == (QStringList{"a", "b", "c"}));
        assert(result.error.isEmpty() && !result.cancelled && result.failedId.isEmpty());
        assert(hookRan && hookOk);
        assert(log.statuses.contains("apply b"));
        std::cout << "case 1 (all applied) OK\n";
    }

    // 2. Cancel lands between changes: the change that was running when
    //    the request came in completes, nothing after it starts.
    {
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, rb, {});
        bool hookOk = true;
        ctx.onFinish([&](bool ok) { hookOk = ok; });
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log),
            std::make_shared<FakeChange>("b", Behavior::CancelDuringApply, log),
            std::make_shared<FakeChange>("c", Behavior::Ok, log),
            std::make_shared<FakeChange>("d", Behavior::Ok, log),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds == (QStringList{"a", "b"}));
        assert(result.cancelled && result.error.isEmpty());
        assert(log.applied == (QStringList{"a", "b"}));
        assert(!hookOk);
        std::cout << "case 2 (cancel stops between changes) OK\n";
    }

    // 3. A failing change stops the loop; it and the rest stay pending.
    //    A throwing one is reported the same way, with its message.
    {
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, rb, {});
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log),
            std::make_shared<FakeChange>("b", Behavior::Fail, log),
            std::make_shared<FakeChange>("c", Behavior::Ok, log),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds == (QStringList{"a"}));
        assert(result.failedId == "b" && result.error == "nope" && !result.cancelled);

        SaveContext ctx2(token, noProgress, {}, rb, {});
        std::vector<std::shared_ptr<PendingChange>> throwing = {
            std::make_shared<FakeChange>("x", Behavior::Throw, log),
        };
        auto r2 = runSaveLoop(throwing, ctx2);
        assert(r2.appliedIds.isEmpty() && r2.failedId == "x" && r2.error == "boom");
        std::cout << "case 3 (failure stops the loop) OK\n";
    }

    // 4. An already-cancelled token applies nothing at all.
    {
        Log log;
        CancellationToken token;
        token.cancel();
        SaveContext ctx(token, noProgress, {}, rb, {});
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds.isEmpty() && result.cancelled && log.applied.isEmpty());
        std::cout << "case 4 (pre-cancelled) OK\n";
    }

    // 5. backupOnce() backs a file up once per save however many changes
    //    ask, and the backup is what the loop hands back for undo.
    {
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, rb, {});
        std::string pdb = (pioneer / "rekordbox" / "export.pdb").string();
        int madeNow = 0;
        auto backup = [&](SaveContext &c) { madeNow += c.backupOnce(pdb, "test") ? 1 : 0; };
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log, backup),
            std::make_shared<FakeChange>("b", Behavior::Ok, log, backup),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(madeNow == 1);
        assert(result.backups.size() == 1);
        assert(fs::is_directory(result.backups[0].backupDir.toStdString()));
        assert(result.backups[0].backupDir.toStdString() == (root / "Seabass" / "backups").string());
        assert(fs::exists(root / "Seabass" / "seabass.log"));
        std::cout << "case 5 (backupOnce dedups and feeds undo) OK\n";
    }

    // 6. shared<T>() creates a per-save resource once and hands the same
    //    one to every change.
    {
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, rb, {});
        int created = 0;
        Counter *seen = nullptr;
        auto use = [&](SaveContext &c) {
            Counter &counter = c.shared<Counter>("counter", [&]() {
                created++;
                return std::make_unique<Counter>();
            });
            counter.uses++;
            assert(seen == nullptr || seen == &counter);
            seen = &counter;
        };
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log, use),
            std::make_shared<FakeChange>("b", Behavior::Ok, log, use),
            std::make_shared<FakeChange>("c", Behavior::Ok, log, use),
        };
        runSaveLoop(changes, ctx);
        assert(created == 1 && seen && seen->uses == 3);
        std::cout << "case 6 (shared resources) OK\n";
    }

    // 7. A finish hook that throws (a scratch commit that failed) turns the
    //    whole save into "nothing landed": every change stays pending so a
    //    retry re-applies it, and the later hooks still run.
    {
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, rb, {});
        bool laterRan = false;
        ctx.onFinish([](bool) { throw std::runtime_error("commit failed"); });
        ctx.onFinish([&](bool) { laterRan = true; });
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log),
            std::make_shared<FakeChange>("b", Behavior::Ok, log),
        };
        auto result = runSaveLoop(changes, ctx);
        assert(result.appliedIds.isEmpty());
        assert(result.error == "commit failed");
        assert(laterRan);
        std::cout << "case 7 (finish hook failure) OK\n";
    }

    // 8. The system is kept awake for the whole save, and let go after it --
    //    failed or not. A suspend halfway through leaves a stick half-written.
    {
        struct Counts
        {
            int acquires = 0;
            int releases = 0;
            bool held = false;
        };
        class FakeSleep : public SleepInhibitor::Backend
        {
        public:
            explicit FakeSleep(std::shared_ptr<Counts> counts) : m_counts(std::move(counts)) {}
            bool acquire(const QString &) override
            {
                ++m_counts->acquires;
                m_counts->held = true;
                return true;
            }
            void release() override
            {
                ++m_counts->releases;
                m_counts->held = false;
            }

        private:
            std::shared_ptr<Counts> m_counts;
        };
        auto counts = std::make_shared<Counts>();
        SleepInhibitor::setBackendForTesting(std::make_unique<FakeSleep>(counts));

        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, {}, rb, {});
        bool heldDuringApply = false;
        std::vector<std::shared_ptr<PendingChange>> changes = {
            std::make_shared<FakeChange>("a", Behavior::Ok, log, [&](SaveContext &) { heldDuringApply = counts->held; }),
            std::make_shared<FakeChange>("b", Behavior::Fail, log),
        };
        runSaveLoop(changes, ctx);
        assert(heldDuringApply && "the system must be kept awake while a change is applied");
        assert(counts->acquires == 1 && counts->releases == 1);
        assert(!counts->held && "and let go once the save is over");
        assert(SleepInhibitor::activeHolds() == 0);
        SleepInhibitor::setBackendForTesting(nullptr);
        std::cout << "case 8 (kept awake for the save, let go after) OK\n";
    }

    // ---- #42: Seabass's own pdb write does not re-arm the import prompt ----
    //
    // Level before, moved by the save: Engine follows, in the same save,
    // and the follow-up is backed up with it so Undo Last Save puts both
    // numbers back together. Through a scratch copy (the rekordbox writes
    // are not on the stick until the finish hooks commit them, so the new
    // sequence has to be read from where they are) and straight to the
    // stick.
    for (const int hint : {100000, 1}) {
        const TwoCatalogs stick = makeTwoCatalogStick(root / "import-level", 500, 500);
        bool usedScratch = false;
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        auto result = runSaveLoop({std::make_shared<MovesPdbSequence>(stick.pioneer, 501, hint, &usedScratch)}, ctx);
        assert(result.error.isEmpty() && result.warning.isEmpty());
        assert(usedScratch == (hint > 1) && "both write paths are exercised");
        assert(pdbSequence(stick.pioneer) == 501);
        assert(engineCounter(stick.engine) == 501 && "the player stays quiet: Seabass's edit is not a new export");
        assert(result.appliedIds == QStringList{QStringLiteral("cleanup:moves")}
               && "the follow-up is the save's own step, not a change the page staged");
        bool engineBackedUp = false;
        for (const UndoableBackup &backup : result.backups) {
            engineBackedUp = engineBackedUp || backup.id.endsWith(QStringLiteral("-engine-import-counter"));
        }
        assert(engineBackedUp && "and it is backed up with the save, so Undo Last Save puts it back too");
    }
    std::cout << "case 9 (a save that moves the pdb sequence carries it into Engine, scratch and direct) OK\n";

    // Apart before: the stick already had an import offer pending, which
    // means the rekordbox library really did move on before Seabass
    // touched it. Not swallowed.
    {
        const TwoCatalogs stick = makeTwoCatalogStick(root / "import-apart", 500, 400);
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        auto result = runSaveLoop({std::make_shared<MovesPdbSequence>(stick.pioneer, 501, 1)}, ctx);
        assert(result.error.isEmpty());
        assert(engineCounter(stick.engine) == 400 && "a pending offer is left for the DJ to see");
        std::cout << "case 10 (an import offer that was already pending is not swallowed) OK\n";
    }

    // Level, and the save does not move the sequence: Engine is not
    // written, not even backed up.
    {
        const TwoCatalogs stick = makeTwoCatalogStick(root / "import-still", 500, 500);
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        auto result = runSaveLoop({std::make_shared<FakeChange>("addcue:a", Behavior::Ok, log)}, ctx);
        assert(result.error.isEmpty() && result.backups.empty());
        assert(engineCounter(stick.engine) == 500);
        std::cout << "case 11 (a save that leaves the pdb alone leaves Engine alone) OK\n";
    }

    // Engine's own libraries number the Information row 1, a library
    // libdjinterop created numbers it 2. Neither is assumed.
    {
        const TwoCatalogs stick = makeTwoCatalogStick(root / "import-row2", 500, 500, /*rowId=*/2);
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        auto result = runSaveLoop({std::make_shared<MovesPdbSequence>(stick.pioneer, 502, 1)}, ctx);
        assert(result.error.isEmpty() && engineCounter(stick.engine) == 502);
        std::cout << "case 12 (the Information row's id is not assumed) OK\n";
    }

    // No Engine library on the stick: nothing is created, nothing written.
    {
        TwoCatalogs stick = makeTwoCatalogStick(root / "import-noengine", 500, 500);
        fs::remove_all(stick.engine);
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        auto result = runSaveLoop({std::make_shared<MovesPdbSequence>(stick.pioneer, 501, 1)}, ctx);
        assert(result.error.isEmpty() && result.warning.isEmpty());
        assert(!fs::exists(stick.engine) && "no Engine library is conjured up");
        std::cout << "case 13 (no Engine library, no Engine write) OK\n";
    }

    // Cancelled after the pdb moved: what landed is committed, so the
    // counter still follows it.
    {
        const TwoCatalogs stick = makeTwoCatalogStick(root / "import-cancel", 500, 500);
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        auto result = runSaveLoop({std::make_shared<MovesPdbSequence>(stick.pioneer, 501, 100000),
                                   std::make_shared<FakeChange>("stop", Behavior::CancelDuringApply, log),
                                   std::make_shared<FakeChange>("never", Behavior::Ok, log)},
                                  ctx);
        assert(result.cancelled);
        assert(pdbSequence(stick.pioneer) == 501 && engineCounter(stick.engine) == 501);
        std::cout << "case 14 (a cancelled save still carries the sequence it moved) OK\n";
    }

    // The rekordbox scratch copy does not make it back onto the stick.
    // The counter was written before the finish hooks ran; left there, it
    // would be ahead of the pdb that is really on the stick, and Seabass
    // itself would have armed the prompt. It is put back to match.
#if !defined(_WIN32)
    {
        const TwoCatalogs stick = makeTwoCatalogStick(root / "import-commit-fails", 500, 500);
        const fs::path rekordboxDir = stick.pioneer / "rekordbox";
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        bool usedScratch = false;
        auto result = runSaveLoop({std::make_shared<MovesPdbSequence>(
                                      stick.pioneer, 501, 100000, &usedScratch,
                                      [&] { fs::permissions(rekordboxDir, fs::perms::owner_read | fs::perms::owner_exec); })},
                                  ctx);
        fs::permissions(rekordboxDir, fs::perms::owner_all);
        assert(usedScratch && "the pdb went through a scratch copy, whose commit is what fails here");
        assert(!result.error.isEmpty() && "the failed commit is reported");
        assert(pdbSequence(stick.pioneer) == 500 && "the new pdb never reached the stick");
        assert(engineCounter(stick.engine) == 500 && "and Engine is not left ahead of it");
        std::cout << "case 15 (a pdb commit that fails does not leave Engine ahead of it) OK\n";
    }
#endif

    // "Mark as imported" staged together with a repair that rewrites the
    // pdb: the mark carries the sequence from when it was staged, the
    // repair moves past it. The two were apart before the save, but the
    // user asked in this very save for them to be level.
    {
        const TwoCatalogs stick = makeTwoCatalogStick(root / "import-mark-and-move", 500, 400);
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, QString::fromStdString(stick.pioneer.string()),
                        QString::fromStdString(stick.engine.string()));
        auto result = runSaveLoop(
            {std::make_shared<MarkRekordboxImportedChange>(QString::fromStdString(stick.engine.string()), 500),
             std::make_shared<MovesPdbSequence>(stick.pioneer, 501, 1)},
            ctx);
        assert(result.error.isEmpty());
        assert(engineCounter(stick.engine) == 501 && "marked imported means imported as the save left it");
        std::cout << "case 16 (a mark staged with a pdb rewrite ends level) OK\n";
    }

    // 17. A skip settles the change without counting it as done. It leaves
    //     the pending list with the applied ones (appliedIds: nothing of it
    //     is left to retry) and is named again in skippedIds, which the
    //     summary subtracts -- before, a skip was a success, and a save in
    //     which every image had become unreadable since the scan reported
    //     every track repaired. A finish-hook failure takes skippedIds down
    //     with appliedIds: "report every change as still pending".
    {
        Log log;
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, rb, {});
        auto result = runSaveLoop({std::make_shared<FakeChange>("a", Behavior::Ok, log),
                                   std::make_shared<FakeChange>("b", Behavior::Skip, log),
                                   std::make_shared<FakeChange>("c", Behavior::Ok, log)},
                                  ctx);
        assert(result.error.isEmpty());
        assert(result.appliedIds == (QStringList{"a", "b", "c"}));
        assert(result.skippedIds == QStringList{"b"});

        SaveContext failing(token, noProgress, nullptr, rb, {});
        failing.onFinish([](bool) { throw std::runtime_error("commit failed"); });
        auto lost = runSaveLoop({std::make_shared<FakeChange>("s", Behavior::Skip, log)}, failing);
        assert(lost.appliedIds.isEmpty() && lost.skippedIds.isEmpty());
        std::cout << "case 17 (a skip is settled, and counted apart) OK\n";
    }

    // 18. The real artwork change: one track repaired, one deleted since
    //     the scan, one whose image stopped reading. Only the first is a
    //     repair. Before, all three came back success() and the summary
    //     said "3 tracks repaired".
    {
        const fs::path stickRoot = root / "artwork-skip";
        const fs::path library = stickRoot / "Engine Library";
        fs::create_directories(library / "Database2");
        fs::create_directories(library / "Artwork");
        seabass::testing::createEngineArtworkTables(library / "Database2" / "m.db", {1, 3});
        const fs::path image = stickRoot / "PIONEER" / "Artwork" / "00001" / "a1.jpg";
        fs::create_directories(image.parent_path());
        std::ofstream(image, std::ios::binary) << std::string("\xFF\xD8\xFF", 3) << "COVER";

        auto entry = [&](std::int64_t trackId, const fs::path &imageOnStick) {
            seabass::infrastructure::engine::ArtworkEntry e;
            e.trackId = trackId;
            e.imageOnStick = imageOnStick.string();
            return e;
        };
        const QString enginePath = QString::fromStdString(library.string());
        CancellationToken token;
        SaveContext ctx(token, noProgress, nullptr, {}, enginePath);
        auto result = runSaveLoop(
            {std::make_shared<RepairArtworkChange>(enginePath, entry(1, image), 3, true, nullptr),
             std::make_shared<RepairArtworkChange>(enginePath, entry(2, image), 3, false, nullptr),
             std::make_shared<RepairArtworkChange>(enginePath, entry(3, stickRoot / "gone.jpg"), 3, false, nullptr)},
            ctx);
        assert(result.error.isEmpty());
        assert(result.appliedIds.size() == 3);
        assert(result.skippedIds
               == (QStringList{RepairArtworkChange::idFor(2), RepairArtworkChange::idFor(3)}));
        assert(!seabass::testing::engineTrackArtworkHash(library / "Database2" / "m.db", 1).empty()
               && "the one repair did land");
        assert(seabass::testing::engineTrackArtworkHash(library / "Database2" / "m.db", 3).empty());
        std::cout << "case 18 (a cover that could not be given is not counted as given) OK\n";
    }

    fs::remove_all(root);
    std::cout << "edit_session_save_loop_test: all cases passed\n";
    return 0;
}
