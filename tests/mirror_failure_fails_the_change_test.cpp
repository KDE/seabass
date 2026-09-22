// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Every rekordbox change that writes the Device Library Plus mirror
// fails when that write fails, and leaves the stick as it was.
//
// export.pdb and exportLibrary.db are ONE library in two formats, so a
// change that reached one and not the other leaves the library
// disagreeing with itself while the page reports success. AddCueChange
// was fixed for that and states the rule. Four other changes kept the
// behaviour it was fixed FROM, under a stated convention -- "best-effort
// mirror, same convention as Clean Up's own survivor-cue mirror block".
// RemoveJunkCueChange was brought into line first (see
// junk_cue_mirror_failure_test); this covers the other three, and then
// the two sites in CleanupGroupChange -- including the survivor-cue
// mirror block the convention was named after, which went on writing
// half the library for as long as the changes quoting it did.
//
// Each runs twice over its own copy of the fixture: once with a writable
// mirror, which must SUCCEED and change the stick, and once with the
// mirror read-only, which must fail with every file back as it was.
// Without the first half, "nothing changed" in the second would hold
// just as well for a change that was never going to write anything.
//
// Driven through runSaveLoop rather than apply(), because the rollback
// is the save loop's job.

#include <QString>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/library_consistency.hpp"
#include "domain/local_restore.hpp"
#include "domain/metadata_restore.hpp"
#include "domain/track.hpp"
#include "domain/duplicate_cleanup.hpp"
#include "gui/edit/changes/cleanup_group_change.hpp"
#include "gui/edit/changes/merge_cues_change.hpp"
#include "gui/edit/changes/repair_issue_change.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
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
    assert(fs::exists(source / "rekordbox" / "export.pdb"));
    assert(fs::exists(source / "rekordbox" / "exportLibrary.db"));
    const fs::path pioneerRoot = scratch / "PIONEER";
    fs::copy(source, pioneerRoot, fs::copy_options::recursive);
    return pioneerRoot;
}

// Tracks Device Library Plus actually lists. A track it does not list is
// deliberately NOT a failure, so picking one would test the opposite of
// what this is for.
//
// A clean-up needs two: the copy it keeps and the copy it removes. Only
// the first is asked for a cue, because only the change acting on the
// kept copy writes one; a doomed copy is removed whether it has cues or
// not. Returns nothing at all unless it found the full number asked
// for, so a case can never quietly run on half its fixture.
std::vector<Track> mirroredTracks(const fs::path &pioneerRoot, std::size_t wanted, bool firstNeedsACue)
{
    std::vector<Track> picked;
    const std::string root = pioneerRoot.string();
    if (!seabass::infrastructure::onelibrary::OneLibraryCueWriter::existsFor(root)) {
        return picked;
    }
    seabass::infrastructure::onelibrary::OneLibraryCueWriter mirror(root);
    seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(root);
    for (const Track &track : reader.readAll()) {
        if (track.filePath.empty()) {
            continue;
        }
        if (picked.empty() && firstNeedsACue && track.cues.empty()) {
            continue;
        }
        if (!mirror.hasTrackAtPath(track.filePath)) {
            continue;
        }
        picked.push_back(track);
        if (picked.size() == wanted) {
            return picked;
        }
    }
    picked.clear();
    return picked;
}

// A rekordbox row whose file Device Library Plus does not list: the
// case the rules here call a non-event rather than a failure.
//
// Built rather than found. Every rekordbox track in the committed
// fixture is also in exportLibrary.db, while on a real stick 635 of
// 1118 are not -- and the mirror is keyed on the path, so a row whose
// path that catalog has never heard of is exactly what those 635 look
// like from here. The rekordbox row stays real, which is what the
// removal on that side needs.
Track withAPathTheMirrorDoesNotKnow(const Track &real, const std::string &name)
{
    Track track = real;
    track.filePath = "/Contents/UNLISTED/" + name + ".mp3";
    return track;
}

// What the stick says is waiting to be deleted. Kept beside the stick's
// own Seabass folder rather than inside PIONEER, so snapshot() above
// cannot see it -- and a clean-up writes a line here per copy it
// removes, which is exactly what a failed change has to take back.
std::string pendingDeletionBytes(const fs::path &pioneerRoot)
{
    const fs::path manifest = seabass::infrastructure::paths::stickPendingDeletions(pioneerRoot.parent_path());
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// One group to clean up, built by hand rather than through the planner:
// which two copies the fixture happens to offer is not the point here,
// only that both halves of the library list them. withAnExtraCue picks
// which of the two mirror sites the case exercises -- a merged set
// bigger than the survivor's own runs the cue mirror, an unchanged one
// leaves the row removal as the only mirror write in the change.
DuplicateCleanupPlan cleanupPlan(const Track &survivor, const std::vector<Track> &doomed, bool withAnExtraCue)
{
    DuplicateCleanupPlan plan;
    plan.group.tracks = doomed;
    plan.group.tracks.insert(plan.group.tracks.begin(), survivor);
    plan.survivor = survivor;
    plan.toRemove = doomed;
    plan.mergedCuesForSurvivor = survivor.cues;
    if (withAnExtraCue) {
        CuePoint extra;
        extra.kind = CuePoint::Kind::Memory;
        extra.positionMs = 13579.0;
        plan.mergedCuesForSurvivor.push_back(extra);
    }
    return plan;
}

std::map<std::string, std::string> snapshot(const fs::path &root)
{
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        // -shm is rebuilt from the -wal a rollback puts back, and an
        // EMPTY -wal is the same state as no -wal: SQLite makes a
        // zero-length one the moment a database is opened, which this
        // test does itself when it picks its track.
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-shm") == 0) {
            continue;
        }
        std::error_code ec;
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, "-wal") == 0
            && fs::file_size(entry.path(), ec) == 0 && !ec) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        files[fs::relative(entry.path(), root).generic_string()] =
            std::string(std::istreambuf_iterator<char>(in), {});
    }
    return files;
}

void setMirrorReadOnly(const fs::path &pioneerRoot, bool readOnly)
{
    const fs::path db = pioneerRoot / "rekordbox" / "exportLibrary.db";
    for (const fs::path &file : {db, fs::path(db.string() + "-wal"), fs::path(db.string() + "-shm")}) {
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            continue;
        }
        if (readOnly) {
            fs::permissions(file, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                            fs::perm_options::replace, ec);
        } else {
            fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, ec);
        }
    }
}

SaveLoopResult runOne(const fs::path &pioneerRoot, const std::shared_ptr<PendingChange> &change)
{
    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    SaveContext ctx(token, noProgress, {}, QString::fromStdString(pioneerRoot.string()), {});
    std::vector<std::shared_ptr<PendingChange>> changes = {change};
    return runSaveLoop(changes, ctx);
}

// tracks[0] is the track the change acts on; a clean-up also gets
// tracks[1], the copy it removes.
using MakeChange = std::function<std::shared_ptr<PendingChange>(const fs::path &, const std::vector<Track> &)>;

void bothWays(const std::string &what, std::size_t tracksNeeded, bool firstNeedsACue, const MakeChange &make)
{
    // ---- writable mirror: must go through AND change the stick --------
    {
        const fs::path root = freshCopy("seabass_mirror_ok_" + what);
        const std::vector<Track> tracks = mirroredTracks(root, tracksNeeded, firstNeedsACue);
        if (tracks.empty()) {
            std::cerr << what << ": the fixture offers no " << tracksNeeded
                      << " rekordbox track(s) that Device Library Plus lists"
                      << (firstNeedsACue ? ", the first carrying a cue" : "") << "\n";
            std::exit(1);
        }
        const auto before = snapshot(root);
        const SaveLoopResult result = runOne(root, make(root, tracks));
        if (!result.error.isEmpty()) {
            std::cerr << what << " baseline failed: " << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && "with a writable mirror the change must go through");
        assert(result.appliedIds.size() == 1);
        assert(snapshot(root) != before && "and must actually write, or case 2 proves nothing");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }

    // ---- read-only mirror: must fail, and put everything back ---------
    {
        const fs::path root = freshCopy("seabass_mirror_ro_" + what);
        const std::vector<Track> tracks = mirroredTracks(root, tracksNeeded, firstNeedsACue);
        assert(!tracks.empty() && "the same fixture must still offer the same tracks");
        setMirrorReadOnly(root, true);
        const auto before = snapshot(root);

        const SaveLoopResult result = runOne(root, make(root, tracks));
        setMirrorReadOnly(root, false);

        if (result.error.isEmpty()) {
            std::cerr << what << ": reported success although Device Library Plus could not be written\n";
        }
        assert(!result.error.isEmpty() && "a change that did not reach Device Library Plus is not a success");
        assert(result.appliedIds.isEmpty());
        const std::string error = result.error.toStdString();
        assert(error.find("Device Library Plus") != std::string::npos
               && "and says which half could not take it");
        // No "-- and putting back what it had already written failed".
        assert(error.find("putting back") == std::string::npos);

        const auto after = snapshot(root);
        for (const auto &[path, bytes] : before) {
            const auto found = after.find(path);
            if (found == after.end()) {
                std::cerr << what << ": removed by a failed save: " << path << "\n";
            } else if (found->second != bytes) {
                std::cerr << what << ": left changed after a failed save: " << path << "\n";
            }
        }
        for (const auto &[path, bytes] : after) {
            if (!before.count(path)) {
                std::cerr << what << ": created by a failed save: " << path << " (" << bytes.size() << " bytes)\n";
            }
        }
        assert(after == before && "a failed change must leave every file as it was");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
    }
    std::cout << "  " << what << ": succeeds and writes with a writable mirror; fails whole without one\n";
}

}  // namespace

int main()
{
    // MergeCuesChange -- Local Cue Backup's merge onto a stick track.
    bothWays("merge-cues", 1, true, [](const fs::path &root, const std::vector<Track> &tracks) {
        const Track &track = tracks.front();
        RestoreCandidate candidate;
        candidate.stickTrack = track;
        candidate.mergedCues = track.cues;
        CuePoint extra;
        extra.kind = CuePoint::Kind::Memory;
        extra.positionMs = 4321.0;
        candidate.mergedCues.push_back(extra);
        return std::make_shared<MergeCuesChange>(QStringLiteral("rekordbox"),
                                                 QString::fromStdString(root.string()), candidate);
    });

    // RepairIssueChange -- Library Health's repair, writing the
    // survivor's merged cues. No brokenGroup, so this exercises the cue
    // mirror rather than the row removal.
    bothWays("repair-issue", 1, true, [](const fs::path &root, const std::vector<Track> &tracks) {
        const Track &track = tracks.front();
        LibraryConsistencyIssue issue;
        issue.kind = LibraryConsistencyIssue::Kind::Repairable;
        issue.survivor = track;
        issue.survivorCues = track.cues;
        CuePoint extra;
        extra.kind = CuePoint::Kind::Memory;
        extra.positionMs = 8765.0;
        issue.survivorCues.push_back(extra);
        return std::make_shared<RepairIssueChange>(QString::fromStdString(root.string()), issue, 1);
    });

    // RestoreMetadataChange -- Restore Metadata's cue write.
    bothWays("restore-metadata", 1, false, [](const fs::path &root, const std::vector<Track> &tracks) {
        const Track &track = tracks.front();
        MetadataRestoreProposal proposal;
        proposal.stickTrack = track;
        proposal.cues = track.cues;
        CuePoint extra;
        extra.kind = CuePoint::Kind::Memory;
        extra.positionMs = 2468.0;
        proposal.cues.push_back(extra);
        proposal.cuesOffered = true;
        return std::make_shared<RestoreMetadataChange>(QStringLiteral("rekordbox"),
                                                       QString::fromStdString(root.string()),
                                                       QString::fromStdString(track.sourceId), proposal, 1);
    });

    // CleanupGroupChange, site one -- the merged cues onto the copy it
    // keeps. This is the block the retired convention named itself
    // after, and it kept the behaviour it was quoted for: the cues the
    // removed copies held reached DeviceLibrary while a player reading
    // Device Library Plus went on showing the survivor without them,
    // with the copies that had them gone.
    bothWays("cleanup-merged-cues", 2, true, [](const fs::path &root, const std::vector<Track> &tracks) {
        return std::make_shared<CleanupGroupChange>(QStringLiteral("rekordbox"),
                                                    QString::fromStdString(root.string()),
                                                    cleanupPlan(tracks[0], {tracks[1]}, true), 1);
    });

    // CleanupGroupChange, site two -- removing the copy's row. Nothing
    // is added to the merged set, so the cue mirror above is skipped
    // and the removal is the only mirror write left in the change: a
    // failure here means the copy is gone from DeviceLibrary, still
    // offered by Device Library Plus, and pointing at a file the same
    // save schedules for deletion.
    bothWays("cleanup-row-removal", 2, true, [](const fs::path &root, const std::vector<Track> &tracks) {
        return std::make_shared<CleanupGroupChange>(QStringLiteral("rekordbox"),
                                                    QString::fromStdString(root.string()),
                                                    cleanupPlan(tracks[0], {tracks[1]}, false), 1);
    });

    // ---- what is NOT a failure, and what still is ------------------
    //
    // The rule the cases above enforce has an edge on either side, and
    // both used to be decided by one exception type doing two jobs.
    auto cleanupChange = [](const fs::path &root, const Track &survivor, const std::vector<Track> &doomed,
                            bool withAnExtraCue) {
        return std::make_shared<CleanupGroupChange>(QStringLiteral("rekordbox"),
                                                    QString::fromStdString(root.string()),
                                                    cleanupPlan(survivor, doomed, withAnExtraCue), 1);
    };

    // A copy Device Library Plus does not list has no second row to
    // remove, so the clean-up goes through with that half unwritable.
    // Treated as a failure this would refuse the commonest case there
    // is: 635 of 1118 tracks on a real stick are not listed there.
    {
        const fs::path root = freshCopy("seabass_cleanup_unlisted_doomed");
        const std::vector<Track> kept = mirroredTracks(root, 1, true);
        assert(!kept.empty());
        const std::vector<Track> second = mirroredTracks(root, 2, true);
        assert(second.size() == 2);
        const Track stray = withAPathTheMirrorDoesNotKnow(second[1], "doomed");
        const auto before = snapshot(root);
        setMirrorReadOnly(root, true);
        const SaveLoopResult result = runOne(root, cleanupChange(root, kept[0], {stray}, false));
        setMirrorReadOnly(root, false);

        if (!result.error.isEmpty()) {
            std::cerr << "cleanup-unlisted-doomed: refused a copy Device Library Plus never listed: "
                      << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && "a copy the mirror does not list is nothing to mirror, not a refusal");
        assert(result.appliedIds.size() == 1);
        assert(snapshot(root) != before && "and the clean-up must really have run");
        assert(pendingDeletionBytes(root).find(stray.filePath) != std::string::npos
               && "the removed copy is recorded as waiting for deletion");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
        std::cout << "  cleanup-unlisted-doomed: a copy Device Library Plus does not list is not a refusal\n";
    }

    // The other side of that one exception type: Device Library Plus
    // lists the copy being REMOVED and not the copy being kept. Nothing
    // is written there, the doomed row stays, and this save schedules
    // its file for deletion -- so it must refuse, with a writable
    // mirror and all.
    {
        const fs::path root = freshCopy("seabass_cleanup_unlisted_survivor");
        const std::vector<Track> listed = mirroredTracks(root, 1, false);
        assert(!listed.empty());
        const std::vector<Track> pair = mirroredTracks(root, 2, false);
        assert(pair.size() == 2);
        const Track keptButUnlisted = withAPathTheMirrorDoesNotKnow(pair[1], "kept");
        const auto before = snapshot(root);
        const std::string pendingBefore = pendingDeletionBytes(root);
        const SaveLoopResult result = runOne(root, cleanupChange(root, keptButUnlisted, {listed[0]}, false));

        if (result.error.isEmpty()) {
            std::cerr << "cleanup-unlisted-survivor: removed a copy whose Device Library Plus row stays behind\n";
        }
        assert(!result.error.isEmpty() && "a row left in Device Library Plus pointing at a doomed file is not a "
                                          "success");
        assert(result.error.contains(QStringLiteral("Device Library Plus")));
        assert(result.appliedIds.isEmpty());
        assert(snapshot(root) == before && "and every file goes back");
        assert(pendingDeletionBytes(root) == pendingBefore && "including the list of files waiting to be deleted");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
        std::cout << "  cleanup-unlisted-survivor: the kept copy missing from the mirror is a refusal\n";
    }

    // A group with two copies to remove, failing on the second: the
    // first has already had its line written into the stick's list of
    // files waiting to be deleted, and that line has to come back out
    // with the rest of the change. The manifest lives outside PIONEER,
    // where snapshot() cannot see it, which is why it is read directly.
    {
        const fs::path root = freshCopy("seabass_cleanup_two_doomed");
        const std::vector<Track> listed = mirroredTracks(root, 2, true);
        assert(listed.size() == 2);
        const std::vector<Track> three = mirroredTracks(root, 3, true);
        assert(three.size() == 3);
        const Track stray = withAPathTheMirrorDoesNotKnow(three[2], "first-doomed");
        setMirrorReadOnly(root, true);
        const auto before = snapshot(root);
        const std::string pendingBefore = pendingDeletionBytes(root);
        // The unlisted copy first, so it is fully applied (its manifest
        // line written) before the listed one reaches the mirror and
        // fails.
        const SaveLoopResult result = runOne(root, cleanupChange(root, listed[0], {stray, listed[1]}, false));
        setMirrorReadOnly(root, false);

        assert(!result.error.isEmpty() && "the second copy's mirror write failed, so the change did");
        assert(result.appliedIds.isEmpty());
        assert(snapshot(root) == before && "every file back as it was");
        const std::string pendingAfter = pendingDeletionBytes(root);
        if (pendingAfter != pendingBefore) {
            std::cerr << "cleanup-two-doomed: a rolled-back change left a file listed for deletion:\n"
                      << pendingAfter << "\n";
        }
        assert(pendingAfter == pendingBefore
               && "the first copy's pending-deletion line must come back out with the change");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
        std::cout << "  cleanup-two-doomed: a failure mid-group takes back the lines already written\n";
    }

    // Two rekordbox rows for one audio file: a duplicate group like any
    // other on that side, and one content row on the other, because
    // Device Library Plus is keyed on the path. The removal there would
    // be the survivor's own row replacing itself, which the writer
    // refuses -- rightly, and this must not turn into a failed save, or
    // that pair could never be cleaned up at all.
    {
        const fs::path root = freshCopy("seabass_cleanup_same_row");
        const std::vector<Track> pair = mirroredTracks(root, 2, false);
        assert(pair.size() == 2);
        Track doomed = pair[1];
        doomed.filePath = pair[0].filePath;  // a second row for the kept copy's file
        const auto before = snapshot(root);

        const SaveLoopResult result = runOne(root, cleanupChange(root, pair[0], {doomed}, false));
        if (!result.error.isEmpty()) {
            std::cerr << "cleanup-same-row: refused a group whose two rows are one row in the mirror: "
                      << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && "one content row standing for both copies is nothing to remove");
        assert(result.appliedIds.size() == 1);
        assert(snapshot(root) != before && "and the rekordbox side really was cleaned up");
        // The removed row named the file the kept row names. Scheduling
        // it would put the track the DJ kept on the Delete Orphaned
        // Files page, where it would sit for good: the resolver will not
        // delete a file the library still references, and never clears
        // an entry it will not act on.
        assert(pendingDeletionBytes(root).find(pair[0].filePath) == std::string::npos
               && "the file being kept is not scheduled for deletion");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
        std::cout << "  cleanup-same-row: two rows sharing one mirror row is not a refusal\n";
    }

    // The same file under a different spelling. Everything else in this
    // project that decides whether two paths name one file goes through
    // normalizedPathKey() -- a byte comparison here would miss a case
    // difference on Windows, or the decomposed spelling macOS hands back
    // from a directory read (which pending_deletion_applier.cpp
    // documents), and put the kept track on the deletion list.
    {
        const fs::path root = freshCopy("seabass_cleanup_same_file_spelling");
        const std::vector<Track> pair = mirroredTracks(root, 2, false);
        assert(pair.size() == 2);
        Track doomed = pair[1];
        doomed.filePath = pair[0].filePath;
        std::transform(doomed.filePath.begin(), doomed.filePath.end(), doomed.filePath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        assert(doomed.filePath != pair[0].filePath && "the fixture path must have letters to re-spell");

        const SaveLoopResult result = runOne(root, cleanupChange(root, pair[0], {doomed}, false));
        assert(result.error.isEmpty());
        assert(result.appliedIds.size() == 1);
        const std::string pending = pendingDeletionBytes(root);
        assert(pending.find(pair[0].filePath) == std::string::npos
               && "the kept file is not scheduled under its own spelling");
        assert(pending.find(doomed.filePath) == std::string::npos
               && "nor under the other spelling of the same file");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
        std::cout << "  cleanup-same-file-spelling: one file spelled two ways is still one file\n";
    }

    // The same not-listed question on Library Health's side. Its broken
    // rows are rows in DeviceLibrary; whether Device Library Plus has
    // ever heard of the file is a separate matter, and on a real stick
    // usually it has not. That removal caught std::exception and failed
    // the change over it, so a repair on such a stick refused outright.
    {
        const fs::path root = freshCopy("seabass_repair_unlisted_broken");
        const std::vector<Track> pair = mirroredTracks(root, 2, false);
        assert(pair.size() == 2);
        const auto before = snapshot(root);

        LibraryConsistencyIssue issue;
        issue.kind = LibraryConsistencyIssue::Kind::Repairable;
        issue.survivor = pair[0];
        issue.brokenGroup = {withAPathTheMirrorDoesNotKnow(pair[1], "broken")};
        const SaveLoopResult result =
            runOne(root, std::make_shared<RepairIssueChange>(QString::fromStdString(root.string()), issue, 1));

        if (!result.error.isEmpty()) {
            std::cerr << "repair-unlisted-broken: refused a broken row Device Library Plus never listed: "
                      << result.error.toStdString() << "\n";
        }
        assert(result.error.isEmpty() && "a broken row the mirror does not list is nothing to mirror");
        assert(result.appliedIds.size() == 1);
        assert(snapshot(root) != before && "and the repair really ran");
        std::error_code ec;
        fs::remove_all(root.parent_path(), ec);
        std::cout << "  repair-unlisted-broken: a broken row the mirror never listed is not a refusal\n";
    }

    std::cout << "mirror_failure_fails_the_change_test passed\n";
    return 0;
}
