// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// OwnedChange: one page staging another page's changes.
//
// LibraryEditSession takes changes from one editor at a time. Sync after
// Rekordbox Export stages SyncPlanChange ("sync") and DeletePlaylistChange
// ("addcue") in one batch, which the session refuses unwrapped (case 1,
// the red half) and takes once both are wrapped (case 2). The wrapped
// change answers id() with the page's prefix and owner() with the page,
// and every other question with its inner change's answer (case 3). And
// the wrapped pair saves as the bare pair would, on a copy of the
// anonymized fixture: a pad onto an Engine track and an Engine playlist
// deleted, the applied ids the wrapped ones, Undo putting m.db back byte
// for byte (case 4).

#include <QCoreApplication>
#include <QSignalSpy>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include <djinterop/djinterop.hpp>

#include "domain/sync_planning.hpp"
#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/local/file_library_edit_lock_store.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
using seabass::infrastructure::local::FileLibraryEditLockStore;
namespace fs = std::filesystem;

namespace
{

// Analysed in the fixture (isAnalyzed 1), so its sample rate is known.
constexpr std::int64_t EngineTrack = 6;

domain::SyncPlan padOntoEngine(std::int64_t engineId)
{
    domain::Track rekordbox;
    rekordbox.format = "rekordbox";
    rekordbox.sourceId = "1";
    rekordbox.title = "From rekordbox";
    domain::Track engine;
    engine.format = "engine";
    engine.sourceId = std::to_string(engineId);
    engine.title = "On Engine";
    domain::SyncPlan plan;
    plan.kind = domain::SyncPlan::Kind::AOnly;
    plan.match.trackA = rekordbox;
    plan.match.trackB = engine;
    plan.direction = domain::SyncPlan::Direction::ToB;
    plan.cuesToApply = {domain::CuePoint{domain::CuePoint::Kind::Hot, 1, 1000.0, "", "owned"}};
    return plan;
}

std::unique_ptr<PendingChange> syncChange(const testing::EngineChangeStick &stick)
{
    return std::make_unique<SyncPlanChange>(stick.pioneerPath(), stick.enginePath(), padOntoEngine(EngineTrack), 2);
}

// The other way: Engine's cues onto a rekordbox track, which is a write
// on the rekordbox side the wrapped change has to report as its own.
std::unique_ptr<PendingChange> syncOntoRekordbox(const testing::EngineChangeStick &stick)
{
    domain::SyncPlan plan = padOntoEngine(EngineTrack);
    plan.match.trackA.filePath = pathToUtf8(stick.root / "Contents" / "a.mp3");
    plan.direction = domain::SyncPlan::Direction::ToA;
    return std::make_unique<SyncPlanChange>(stick.pioneerPath(), stick.enginePath(), plan, 2);
}

bool sameWrites(const std::vector<RekordboxWrite> &a, const std::vector<RekordboxWrite> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].filePath != b[i].filePath || a[i].rating != b[i].rating || a[i].cues.has_value() != b[i].cues.has_value()) {
            return false;
        }
        if (a[i].cues && (a[i].cues->size() != b[i].cues->size()
                          || !std::equal(a[i].cues->begin(), a[i].cues->end(), b[i].cues->begin(),
                                         [](const domain::CuePoint &x, const domain::CuePoint &y) {
                                             return x.kind == y.kind && x.hotCueNumber == y.hotCueNumber
                                                 && x.positionMs == y.positionMs;
                                         }))) {
            return false;
        }
    }
    return true;
}

// Counts the step before the save, which no real change but the baseline
// record takes, so the forwarding has something to show.
class BeforeSaveProbe : public PendingChange
{
public:
    explicit BeforeSaveProbe(int &calls) : m_calls(calls) {}
    QString id() const override { return QStringLiteral("probe:1"); }
    QString description() const override { return QStringLiteral("probe"); }
    QString unit() const override { return QStringLiteral("probes"); }
    QStringList formatsTouched() const override { return {}; }
    void beforeSave(SaveContext &) override { ++m_calls; }
    ChangeOutcome apply(SaveContext &) override { return ChangeOutcome::success(); }

private:
    int &m_calls;
};

std::unique_ptr<PendingChange> deleteChange(const testing::EngineChangeStick &stick)
{
    return std::make_unique<DeletePlaylistChange>(QString(), stick.enginePath(), "Playlist 001",
                                                  QStringList{QStringLiteral("engine")});
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: owned_change_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path scratch = testing::scratchRoot() / "owned_change_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "owned_change_stick");
    EditSessionRegistry registry(std::make_unique<FileLibraryEditLockStore>(scratch / "locks"));

    // 1. Unwrapped, the second page's change is refused: the batch is the
    //    first one's.
    {
        LibraryEditSession *session = registry.openSession(QStringLiteral("lib-unwrapped"), QStringLiteral("U"));
        QSignalSpy conflict(session, &LibraryEditSession::editorConflict);
        assert(session->stage(syncChange(stick)));
        assert(session->editorOwner() == QStringLiteral("sync"));
        assert(!session->stage(deleteChange(stick)) && "another page's change is refused");
        assert(conflict.count() == 1);
        assert(conflict.at(0).at(0).toString() == QStringLiteral("sync"));
        assert(conflict.at(0).at(1).toString() == QStringLiteral("addcue"));
        assert(session->pendingCount() == 1);
        session->discard();
        registry.closeSession(QStringLiteral("lib-unwrapped"));
        std::cout << "case 1 (unwrapped, \"sync\" then \"addcue\": the second is refused) OK\n";
    }

    // 2. Wrapped, both stage into one batch owned by the page.
    {
        LibraryEditSession *session = registry.openSession(QStringLiteral("lib-wrapped"), QStringLiteral("W"));
        QSignalSpy conflict(session, &LibraryEditSession::editorConflict);
        assert(session->stage(ownedByRekordboxExportSync(syncChange(stick))));
        assert(session->stage(ownedByRekordboxExportSync(deleteChange(stick))));
        assert(conflict.count() == 0);
        assert(session->pendingCount() == 2);
        assert(session->editorOwner() == QStringLiteral("rekordbox-export-sync"));
        // And a bare change of either page is now the one refused.
        assert(!session->stage(syncChange(stick)));
        assert(conflict.count() == 1);
        session->discard();
        registry.closeSession(QStringLiteral("lib-wrapped"));
        std::cout << "case 2 (wrapped, both stage under \"rekordbox-export-sync\"; a bare one is refused) OK\n";
    }

    // 3. The id and the owner are the page's; everything else is the
    //    inner change's. Each virtual of PendingChange is asked.
    {
        application::CancellationToken token;
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, QString(), stick.enginePath());
        const auto make = [&](int which) {
            return which == 0 ? syncChange(stick) : which == 1 ? deleteChange(stick) : syncOntoRekordbox(stick);
        };
        for (const int which : {0, 1, 2}) {
            std::unique_ptr<PendingChange> bare = make(which);
            const OwnedChange owned(rekordboxExportSyncOwner(), make(which));
            assert(owned.id() == QStringLiteral("rekordbox-export-sync:") + bare->id());
            assert(owned.owner() == QStringLiteral("rekordbox-export-sync"));
            assert(owned.id().section(QLatin1Char(':'), 0, 0) == owned.owner() && "the id's prefix is the owner");
            assert(owned.description() == bare->description());
            assert(owned.subject() == bare->subject());
            assert(owned.unit() == bare->unit());
            assert(owned.unitsWritten() == bare->unitsWritten());
            assert(owned.unitsSkipped() == bare->unitsSkipped());
            assert(owned.verb() == bare->verb());
            assert(owned.formatsTouched() == bare->formatsTouched());
            assert(sameWrites(owned.rekordboxWrites(), bare->rekordboxWrites()));
            const auto ownedTargets = owned.filesToBackup(ctx);
            const auto bareTargets = bare->filesToBackup(ctx);
            assert(ownedTargets.size() == bareTargets.size() && !ownedTargets.empty());
            for (size_t i = 0; i < ownedTargets.size(); ++i) {
                assert(ownedTargets[i].file == bareTargets[i].file && ownedTargets[i].label == bareTargets[i].label);
            }
        }
        // The rekordbox-side write, as the wrapped change reports it: one
        // track, its file, the plan's one pad, no rating.
        {
            const auto writes = OwnedChange(rekordboxExportSyncOwner(), syncOntoRekordbox(stick)).rekordboxWrites();
            assert(writes.size() == 1);
            assert(writes[0].filePath == pathToUtf8(stick.root / "Contents" / "a.mp3"));
            assert(writes[0].cues && writes[0].cues->size() == 1 && writes[0].cues->front().hotCueNumber == 1
                   && writes[0].cues->front().positionMs == 1000.0);
            assert(!writes[0].rating);
            assert(OwnedChange(rekordboxExportSyncOwner(), syncChange(stick)).rekordboxWrites().empty()
                   && "a sync onto Engine wrote nothing on the rekordbox side");
        }
        {
            int calls = 0;
            OwnedChange probe(rekordboxExportSyncOwner(), std::make_unique<BeforeSaveProbe>(calls));
            probe.beforeSave(ctx);
            assert(calls == 1 && "the step before the save reaches the wrapped change");
        }
        assert(OwnedChange(rekordboxExportSyncOwner(), syncChange(stick)).id()
               == QStringLiteral("rekordbox-export-sync:sync:engine:6"));
        assert(OwnedChange(rekordboxExportSyncOwner(), deleteChange(stick)).verb() == QStringLiteral("deleted"));
        assert(OwnedChange(rekordboxExportSyncOwner(), syncChange(stick)).verb() == QStringLiteral("synchronised"));
        bool refused = false;
        try {
            OwnedChange none(rekordboxExportSyncOwner(), nullptr);
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused && "no change to wrap is refused");
        refused = false;
        try {
            OwnedChange nobody(QString(), syncChange(stick));
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused && "no owner is refused");
        refused = false;
        try {
            OwnedChange colon(QStringLiteral("a:b"), syncChange(stick));
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused && "an owner with a ':' is refused");
        std::cout << "case 3 (id and owner the page's, every other answer the inner change's) OK\n";
    }

    // 4. The wrapped pair saves: Engine only, so the save leaves the
    //    import counter alone and Undo is byte for byte.
    {
        const std::string before = testing::fileBytes(pathFromUtf8(stick.db));
        assert(infrastructure::engine::enginePlaylistCountAtPath(stick.engineUtf8(), "Playlist 001") == 1);
        const std::vector<std::shared_ptr<PendingChange>> changes{
            std::shared_ptr<PendingChange>(ownedByRekordboxExportSync(syncChange(stick))),
            std::shared_ptr<PendingChange>(ownedByRekordboxExportSync(deleteChange(stick)))};
        const SaveLoopResult saved = testing::saveChanges(changes, QString(), stick.enginePath());
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty() && saved.skippedIds.isEmpty());
        assert((saved.appliedIds
                == QStringList{QStringLiteral("rekordbox-export-sync:sync:engine:6"),
                               QStringLiteral("rekordbox-export-sync:addcue:delete-playlist:Playlist 001")}));
        assert(infrastructure::engine::enginePlaylistCountAtPath(stick.engineUtf8(), "Playlist 001") == 0);
        {
            auto db = djinterop::engine::load_database(stick.engineUtf8());
            const auto track = db.track_by_id(EngineTrack);
            assert(track);
            const auto hot = track->hot_cues();
            assert(hot[0] && hot[0]->label == "owned");
        }
        assert(testing::fileBytes(pathFromUtf8(stick.db)) != before);

        const SaveLoopResult undone = testing::undoSave(saved, QString(), stick.enginePath());
        assert(undone.error.isEmpty() && undone.appliedIds.size() == 1);
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == before && "Undo puts m.db back byte for byte");
        std::cout << "case 4 (the wrapped pair saves under the wrapped ids; Undo restores m.db byte for byte) OK\n";
    }

    std::cout << "owned_change_test: all cases passed\n";
    return 0;
}
