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

// One group to clean up, built by hand rather than through the planner:
// which two copies the fixture happens to offer is not the point here,
// only that both halves of the library list them. withAnExtraCue picks
// which of the two mirror sites the case exercises -- a merged set
// bigger than the survivor's own runs the cue mirror, an unchanged one
// leaves the row removal as the only mirror write in the change.
DuplicateCleanupPlan cleanupPlan(const Track &survivor, const Track &doomed, bool withAnExtraCue)
{
    DuplicateCleanupPlan plan;
    plan.group.tracks = {survivor, doomed};
    plan.survivor = survivor;
    plan.toRemove = {doomed};
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
                                                    cleanupPlan(tracks[0], tracks[1], true), 1);
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
                                                    cleanupPlan(tracks[0], tracks[1], false), 1);
    });

    std::cout << "mirror_failure_fails_the_change_test passed\n";
    return 0;
}
