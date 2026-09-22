// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Syncing a track Device Library Plus does not list must not fail the
// save.
//
// Issue #14 asked whether Sync's OneLibrary mirror can be verified at
// all, since a best-effort write whose failure is invisible cannot be.
// Running the question turned up the opposite problem. Sync wrote its
// mirror inline instead of through mirrorCuesOrExplain(), so it never
// asked hasTrackAtPath() first -- and contentIdsAt() throws
// OneLibraryRowMissing for a path the database has no row for. The catch
// block took that for a mirror failure and failed the change, so the
// save loop rolled back the rekordbox cue write that had already gone
// through. A track with no Device Library Plus copy has nothing to keep
// in step and nothing to disagree; refusing to sync it is wrong twice
// over.
//
// 635 of 1118 tracks on #14's hardware capture have no OneLibrary row.
// Nothing caught it because every rekordbox track in the committed
// fixture does have one, and corpus_test's sampled plans happened to
// pick listed tracks on the real sticks too.
//
// Two tracks, one save each, and both are asserted -- "did not fail" is
// trivially true of a Sync that stopped mirroring altogether, and that
// is the regression this has to tell apart from a fix.
//
//   a track OneLibrary DOES list      -> the save succeeds, logs that it
//                                        wrote the mirror, and the cue is
//                                        readable back out of
//                                        exportLibrary.db
//   a track OneLibrary does NOT list  -> the save still succeeds, and the
//                                        log records that there was
//                                        nothing to mirror rather than a
//                                        write it did not make
//
// Driven through runSaveLoop and a real SyncPlanChange. The engine path
// is empty on purpose: a rekordbox target never reads it (see
// SyncPlanChange::filesToBackup), and giving it one would invite the test
// to pass by touching a catalog the case is not about.

#include <QString>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

namespace
{

fs::path freshCopy(const std::string &name)
{
    const fs::path scratch = seabass::testing::scratchRoot() / name;
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "rekordbox";
    const fs::path pioneerRoot = scratch / "PIONEER";
    fs::copy(source, pioneerRoot, fs::copy_options::recursive);
    return pioneerRoot;
}

// A rekordbox track that OneLibrary lists, or one it does not, whichever
// was asked for. Both have to exist in the fixture or the pair of cases
// below stops being a pair.
std::optional<domain::Track> trackOfKind(const fs::path &pioneerRoot, bool listedByOneLibrary)
{
    const std::string root = pioneerRoot.string();
    assert(infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root));
    infrastructure::onelibrary::OneLibraryCueWriter mirror(root);
    infrastructure::rekordbox::KaitaiRekordboxReader reader(root);
    for (const domain::Track &track : reader.readAll()) {
        if (track.filePath.empty()) {
            continue;
        }
        if (mirror.hasTrackAtPath(track.filePath) == listedByOneLibrary) {
            return track;
        }
    }
    return std::nullopt;
}

std::string readOperationLog(const fs::path &pioneerRoot)
{
    const std::string stickRoot = infrastructure::backup::stickRootForCatalogPath(pioneerRoot.string());
    std::ifstream in(infrastructure::backup::operationLogForStickRoot(stickRoot));
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// One hot cue at a position the track does not already carry, so the
// write has something to do and the read-back has something to find.
domain::CuePoint aNewHotCue(const domain::Track &track)
{
    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Hot;
    cue.hotCueNumber = 7;
    cue.positionMs = 61234.0;
    for (const auto &existing : track.cues) {
        if (existing.kind == domain::CuePoint::Kind::Hot && existing.hotCueNumber == cue.hotCueNumber) {
            cue.hotCueNumber = 6;  // the fixture rarely fills both
        }
    }
    return cue;
}

SaveLoopResult syncOnto(const fs::path &pioneerRoot, const domain::Track &target, const domain::CuePoint &cue)
{
    domain::SyncPlan plan;
    plan.direction = domain::SyncPlan::Direction::ToB;
    plan.match.trackB = target;
    // The source is read for its name in the log line and nothing else.
    plan.match.trackA = target;
    plan.match.trackA.format = "engine";
    plan.cuesToApply = {cue};

    auto change = std::make_shared<SyncPlanChange>(QString::fromStdString(pioneerRoot.string()), QString(), plan, 1);

    auto &noProgress = application::NullProgressReporter::instance();
    CancellationToken token;
    SaveContext ctx(token, noProgress, {}, QString::fromStdString(pioneerRoot.string()), {});
    std::vector<std::shared_ptr<PendingChange>> changes = {change};
    return runSaveLoop(changes, ctx);
}

bool mirrorHasCue(const fs::path &pioneerRoot, const domain::Track &target, const domain::CuePoint &cue)
{
    infrastructure::onelibrary::OneLibraryReader reader(pioneerRoot.string());
    auto tail = [](std::string path) {
        while (!path.empty() && path.back() == ' ') {
            path.pop_back();
        }
        return fs::path(path).filename().string();
    };
    for (const auto &t : reader.readAll()) {
        if (tail(t.filePath) != tail(target.filePath)) {
            continue;
        }
        for (const auto &c : t.cues) {
            if (c.kind == domain::CuePoint::Kind::Hot && c.hotCueNumber == cue.hotCueNumber
                && c.positionMs > cue.positionMs - 2 && c.positionMs < cue.positionMs + 2) {
                return true;
            }
        }
    }
    return false;
}

bool mentions(const std::string &log, const std::string &needle)
{
    return log.find(needle) != std::string::npos;
}

// A track OneLibrary lists: the mirror runs, says so, and the cue is
// there afterwards. Without this half, the case below would hold for a
// Sync that had stopped mirroring entirely.
void aMirroredTrackIsWrittenAndSaysSo()
{
    const fs::path root = freshCopy("seabass_sync_mirror_listed");
    const auto target = trackOfKind(root, true);
    if (!target) {
        std::cerr << "the fixture offers no rekordbox track that Device Library Plus lists\n";
        std::exit(1);
    }
    const domain::CuePoint cue = aNewHotCue(*target);
    const SaveLoopResult result = syncOnto(root, *target, cue);
    if (!result.error.isEmpty()) {
        std::cerr << "the sync failed: " << result.error.toStdString() << "\n";
    }
    assert(result.error.isEmpty());
    assert(result.appliedIds.size() == 1);

    const std::string log = readOperationLog(root);
    assert(mentions(log, "sync: also wrote into OneLibrary") && "the mirror ran and must say so");
    // The claim has to be true, not merely made -- read back through the
    // reader rather than trusting the line that was just asserted on.
    assert(mirrorHasCue(root, *target, cue) && "the cue the log claims it mirrored is really in exportLibrary.db");
    std::cout << "  a track OneLibrary lists: mirrored, logged, and readable back OK\n";
    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
}

// A track OneLibrary does not list: nothing to mirror, and the log must
// not pretend otherwise. This is the one that was wrong.
void anUnlistedTrackIsNotClaimedAsMirrored()
{
    const fs::path root = freshCopy("seabass_sync_mirror_unlisted");
    // Planted, not searched for. Every rekordbox track in the committed
    // fixture has a OneLibrary row, so this case has no naturally
    // occurring subject -- and skipping when the data does not supply one
    // is how the condition goes untested on exactly the fixture every CI
    // run uses. The row is removed with the app's own writer, so what is
    // planted is the real state and not an approximation of it.
    //
    // Real sticks do supply it: 635 of 1118 tracks on #14's hardware
    // capture have no OneLibrary row.
    const auto listed = trackOfKind(root, true);
    if (!listed) {
        std::cerr << "the fixture offers no rekordbox track that Device Library Plus lists\n";
        std::exit(1);
    }
    {
        infrastructure::onelibrary::OneLibraryCueWriter mirror(root.string());
        mirror.removeTrackByPath(listed->filePath);
    }
    // Scoped above so the writer is closed before the save opens its own
    // -- two instances against one exportLibrary.db is what the staleness
    // guard refuses.
    const auto target = trackOfKind(root, false);
    if (!target || target->sourceId != listed->sourceId) {
        std::cerr << "removing the OneLibrary row did not leave the track unlisted, so this case would be "
                     "asserting about the wrong track\n";
        std::exit(1);
    }
    const domain::CuePoint cue = aNewHotCue(*target);
    const SaveLoopResult result = syncOnto(root, *target, cue);
    if (!result.error.isEmpty()) {
        std::cerr << "the sync failed: " << result.error.toStdString() << "\n";
    }
    // Not a failure. A track Device Library Plus does not list has no
    // copy to keep in step, so there is nothing to disagree and nothing
    // to put back.
    assert(result.error.isEmpty());
    assert(result.appliedIds.size() == 1);

    const std::string log = readOperationLog(root);
    assert(mentions(log, "sync: OneLibrary does not list this file; nothing to mirror"));
    assert(!mentions(log, "sync: also wrote into OneLibrary")
           && "the log must not record a mirror write for a track OneLibrary has no row for");
    std::cout << "  a track OneLibrary does not list: recorded as nothing to mirror OK\n";
    std::error_code ec;
    fs::remove_all(root.parent_path(), ec);
}

}  // namespace

int main()
{
    aMirroredTrackIsWrittenAndSaysSo();
    anUnlistedTrackIsNotClaimedAsMirrored();
    std::cout << "sync_mirror_claim_test: ok\n";
    return 0;
}
