// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The save loop every LibraryEditSession runs: applies staged changes in
// order, stops between them on cancel or failure, runs the finish hooks
// regardless, and reports exactly which changes landed.

#include <QString>
#include <QStringList>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"

#include "scratch_path.hpp"

using namespace seabass::gui;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

enum class Behavior { Ok, Fail, Throw, CancelDuringApply };

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

    fs::remove_all(root);
    std::cout << "edit_session_save_loop_test: all cases passed\n";
    return 0;
}
