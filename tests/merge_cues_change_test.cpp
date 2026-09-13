// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// MergeCuesChange -- Local Cue Backup's restore -- writing Engine cues
// into a save that has m.db behind a scratch copy.
//
// It used to open m.db on the stick directly. Clean Up, Sync and the
// repairs move m.db to a local scratch copy for a large enough save and
// commit that copy back at the end, so a merge written straight to the
// real file was overwritten by the commit: reported as applied, and gone.
// This drives the real session and the real change, then reads the cues
// back off the stick's own m.db through a reader that knows nothing about
// any of it.

#include <QString>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/local_restore.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/merge_cues_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using namespace seabass::domain;
using seabass::application::CancellationToken;
namespace fs = std::filesystem;

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_merge_cues_change_engine_scratch";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const fs::path source = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "engine";
    assert(fs::is_directory(source / "Database2"));
    const fs::path engineRoot = scratch / "Engine Library";
    fs::copy(source, engineRoot, fs::copy_options::recursive);

    // A track that already carries a hot cue -- so its positions are known
    // to round-trip -- and still has a free slot, so the merge adds
    // something unambiguous to look for afterwards.
    Track target;
    int freeSlot = 0;
    {
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(engineRoot.string());
        for (const Track &track : reader.readAll()) {
            std::set<int> used;
            for (const CuePoint &cue : track.cues) {
                if (cue.kind == CuePoint::Kind::Hot) {
                    used.insert(cue.hotCueNumber);
                }
            }
            if (used.empty()) {
                continue;
            }
            for (int slot = 1; slot <= 8; ++slot) {
                if (!used.count(slot)) {
                    freeSlot = slot;
                    break;
                }
            }
            if (freeSlot != 0) {
                target = track;
                break;
            }
        }
    }
    assert(freeSlot != 0 && !target.sourceId.empty());

    CuePoint added;
    added.kind = CuePoint::Kind::Hot;
    added.hotCueNumber = freeSlot;
    added.positionMs = 1234.0;

    RestoreCandidate candidate;
    candidate.stickTrack = target;
    candidate.mergedCues = target.cues;
    candidate.mergedCues.push_back(added);

    auto &noProgress = seabass::application::NullProgressReporter::instance();
    CancellationToken token;
    const QString root = QString::fromStdString(engineRoot.string());
    {
        SaveContext ctx(token, noProgress, {}, root, {});

        // Stand-in for a Clean Up or Sync change staged into the same save:
        // the session, asked for first and with a hint big enough that it
        // redirects m.db to a scratch copy.
        auto &other = sharedFormatWriteSession(ctx, "engine", engineRoot.string(), 5000, "test-other-feature");
        assert(other.usesScratch());  // otherwise this case proves nothing

        MergeCuesChange change("engine", root, candidate);
        assert(change.apply(ctx).ok);
        assert(!ctx.runFinishHooks(true));
    }

    seabass::infrastructure::engine::LibdjinteropEngineReader after(engineRoot.string());
    bool found = false;
    for (const Track &track : after.readAll()) {
        if (track.sourceId != target.sourceId) {
            continue;
        }
        for (const CuePoint &cue : track.cues) {
            // Positions round-trip through Engine's sample units, so "the
            // same cue" is the same slot within a millisecond -- exact
            // double equality would call a perfect write missing.
            if (cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == freeSlot
                && std::fabs(cue.positionMs - added.positionMs) < 1.0) {
                found = true;
            }
        }
    }
    assert(found && "the merged cue must survive a save that also scratches m.db");

    fs::remove_all(scratch, ec);
    std::cout << "merge_cues_change_test: an Engine cue merge survives a save that scratches m.db\n";
    return 0;
}
