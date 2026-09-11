// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <vector>

#include "domain/duplicate_cleanup.hpp"

namespace seabass::application
{

// What a cleanup would actually free, measured on the stick rather than
// taken from a catalog.
//
// Catalogs are not a reliable source for this. Measured on a real
// library: rekordbox and Engine rows carry fileSizeBytes = 0 -- neither
// format records a file's size -- so every space figure computed from
// them was structurally zero however much audio was really duplicated,
// and only OneLibrary produced a number at all. That is why the same
// stick reported "0 GB" from two catalogs and "6.35 GB" from the third.
//
// So the sizes are read from the filesystem, which is also the only
// thing that can be right: a catalog row is a claim about a file, and
// the file is what gets deleted.
struct MeasuredFileSizes
{
    // Distinct files whose size was read from disk.
    int filesMeasured = 0;
    // Rows naming a file that is not on the stick. Their size is set to
    // zero: a row pointing at nothing frees nothing when removed, and
    // counting a catalog's claim would promise space that cannot appear.
    int filesMissing = 0;
    // Sum over the DISTINCT files that would really be deleted.
    std::uint64_t reclaimableBytes = 0;
};

// Rewrites Track::fileSizeBytes on every copy these plans would remove,
// then reports the total. In place because every figure the cleanup page
// shows -- per group, per row, and the totals -- is derived from that
// field, so correcting it once makes all of them true rather than adding
// a second, parallel notion of size that could disagree.
//
// Deliberately called AFTER planning, not before. The planner compares
// bitrate and file size to choose a survivor, and feeding it different
// numbers would change which copy is kept -- a real improvement, since
// two of the three catalogs report zero, but a different change from
// this one and not one to make silently.
//
// Skips held-back strays (the planner refuses to delete them, so their
// bytes are not on offer) and any copy sharing the survivor's path.
// Each distinct path is stat'd once however many plans name it.
MeasuredFileSizes measureRealFileSizes(std::vector<domain::DuplicateCleanupPlan> &plans);

}  // namespace seabass::application
