#pragma once

#include <cstdint>
#include <filesystem>

namespace seabass::infrastructure::backup
{

// Whether a stick can afford to hold this library's backups, and the
// numbers a person needs to decide what to do about it.
//
// A cue backup is written before anything on the stick changes and it
// stays there afterwards, so on a stick with no room the backup goes to
// local disk instead and undo stops being portable. That is worth saying
// before the first change is staged rather than after the work is done,
// which is why this is measured when an edit session opens.
struct StickSpace
{
    std::uint64_t capacityBytes = 0;
    std::uint64_t freeBytes = 0;
    // Every analysis file the library owns, which is what a whole-library
    // operation would back up. Nothing is staged when this is measured, so
    // there is no file list yet and the worst case is the honest bound --
    // the question being answered is whether this stick has room to be
    // edited against at all.
    std::uint64_t worstCaseBackupBytes = 0;

    // Free space a stick keeps in hand beyond the backup itself. One
    // constant, chosen rather than measured: a gigabyte is about a hundred
    // tracks, and 2% covers larger sticks where a flat gigabyte would be
    // meaninglessly small.
    std::uint64_t headroomBytes() const;

    // True when writing the worst-case backup would take the stick below
    // that headroom. `needed` is in the comparison because a single backup
    // can be larger than any fixed threshold: RV2 carries 1992 tracks and
    // 318 MB of analysis files, and at the same ~160 KB per track a
    // ten-thousand-track library is about 1.6 GB. So the question is never
    // "is this stick low" on its own, but "will it still have room once
    // this backup is on it".
    bool backupGoesLocal() const;
};

// Measures `stickRoot`. Never throws: an unreadable or absent stick comes
// back all zeros, which reads as "nothing to warn about" rather than as a
// warning nobody can act on.
StickSpace measureStickSpace(const std::filesystem::path &stickRoot);

// The same measurement, remembered for the life of the process.
//
// The walk behind it is thousands of stats on removable media -- one
// analysis directory per track -- and it was being run on the GUI thread
// every time an edit page opened, before the page's own busy overlay
// existed to explain the wait. Remembered per stick root, keyed on the
// analysis tree's own mtime so a tree that has grown is measured again
// (rekordbox adds a directory per track, which moves the parent's mtime).
//
// For the WARNING only. save_context.cpp deliberately calls
// measureStickSpace() directly: the decision that actually picks where a
// backup is written must not run on a remembered number.
StickSpace measureStickSpaceCached(const std::filesystem::path &stickRoot);

}  // namespace seabass::infrastructure::backup
