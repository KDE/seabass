// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <set>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/stick_backup/stat_diff.hpp"

using namespace seabass::infrastructure::stick_backup;

namespace
{

TreeEntry file(const std::string &path, std::uint64_t size, std::int64_t mtime)
{
    return {path, false, size, mtime};
}

TreeEntry dir(const std::string &path, std::int64_t mtime)
{
    return {path, true, 0, mtime};
}

ManifestRow row(const std::string &path, std::uint64_t size, std::int64_t mtime, bool directory = false)
{
    ManifestRow r;
    r.kind = directory ? ManifestRow::Kind::Directory : ManifestRow::Kind::File;
    r.path = path;
    r.size = size;
    r.mtimeUnix = mtime;
    return r;
}

std::vector<std::string> paths(const std::vector<const TreeEntry *> &entries)
{
    std::vector<std::string> out;
    for (const TreeEntry *e : entries) {
        out.push_back(e->relativePath);
    }
    return out;
}

// Every entry the walk handed in lands in exactly one of added,
// changed and unchanged. The cases below each assert one bucket's size,
// which is the shape that hides a dropped entry: this is a backup, so an
// entry in no bucket is a file that is never read and never carried,
// and the next restore does not have it. The counts would still look
// plausible.
//
// removed is the other direction (manifest paths the stick no longer
// has) and deliberately CAN name a path the walk lists: a file that came
// back as a directory, or the reverse, is the old row removed and the
// new entry added under one name. Measured, not assumed -- asserting
// otherwise fails on the kind-change case this file already covers. So
// what is checked here is that no path is removed twice.
void everyEntryInExactlyOneBucket(const TreeWalk &walk, const DiffResult &diff)
{
    std::multiset<std::string> in;
    for (const TreeEntry &entry : walk.entries) {
        in.insert(entry.relativePath);
    }
    std::multiset<std::string> out;
    for (const auto *bucket : {&diff.unchanged, &diff.added, &diff.changed}) {
        for (const TreeEntry *entry : *bucket) {
            out.insert(entry->relativePath);
        }
    }
    if (in != out) {
        std::cerr << "the diff's buckets do not add up: " << walk.entries.size() << " walked, " << out.size()
                  << " bucketed (" << diff.added.size() << " added, " << diff.changed.size() << " changed, "
                  << diff.unchanged.size() << " unchanged)\n";
    }
    assert(in == out && "every walked entry is added, changed or unchanged, exactly once");
    std::set<std::string> removedOnce;
    for (const std::string &gone : diff.removed) {
        assert(removedOnce.insert(gone).second && "a manifest path is removed at most once");
    }
}

}  // namespace

int main()
{
    // ---- No previous backup: everything is added ----
    {
        TreeWalk walk;
        walk.entries = {dir("d", 1), file("d/a", 10, 100), file("b", 20, 200)};
        DiffResult diff = diffTreeAgainstManifest(walk, nullptr);
        everyEntryInExactlyOneBucket(walk, diff);
        assert(diff.added.size() == 3 && diff.unchanged.empty() && diff.changed.empty() && diff.removed.empty());
        assert(diff.bytesToRead == 30);
        std::cout << "case 1 (no previous manifest: all added) OK\n";
    }

    // ---- The classification matrix ----
    {
        BackupManifest previous;
        previous.rows = {
            row("same", 10, 1000),
            row("within-window", 10, 1000),
            row("size-moved", 10, 1000),
            row("mtime-moved", 10, 1000),
            row("gone", 10, 1000),
            row("was-dir", 0, 1000, true),
            row("stays-dir", 0, 1000, true),
        };
        TreeWalk walk;
        walk.entries = {
            file("same", 10, 1000),
            file("within-window", 10, 1002),  // FAT's 2 s resolution
            file("size-moved", 11, 1000),
            file("mtime-moved", 10, 1010),
            file("brand-new", 5, 1),
            file("was-dir", 3, 1000),         // kind changed: added + removed
            dir("stays-dir", 9999),           // directory mtime is ignored
        };
        DiffResult diff = diffTreeAgainstManifest(walk, &previous);
        everyEntryInExactlyOneBucket(walk, diff);
        assert((paths(diff.unchanged) == std::vector<std::string>{"same", "within-window", "stays-dir"}));
        assert((paths(diff.changed) == std::vector<std::string>{"size-moved", "mtime-moved"}));
        assert((paths(diff.added) == std::vector<std::string>{"brand-new", "was-dir"}));
        assert((diff.removed == std::vector<std::string>{"gone", "was-dir"}));
        assert(diff.bytesToRead == 11 + 10 + 5 + 3);
        assert(diff.uniformShiftSeconds == 0);
        std::cout << "case 2 (unchanged / 2 s window / size / mtime / added / removed / kind change / dir mtime ignored) OK\n";
    }

    // ---- Uniform timezone shift: the clock moved, not the files ----
    {
        BackupManifest previous;
        TreeWalk walk;
        for (int i = 0; i < 20; ++i) {
            std::string p = "t/" + std::to_string(i);
            previous.rows.push_back(row(p, 100, 10'000 + i * 10));
            walk.entries.push_back(file(p, 100, 10'000 + i * 10 + 3600 + (i % 2)));  // +1 h, jittered by FAT rounding
        }
        previous.rows.push_back(row("really-changed", 100, 500));
        walk.entries.push_back(file("really-changed", 100, 500 + 7200));  // a different delta: not part of the shift
        previous.rows.push_back(row("grew", 100, 600));
        walk.entries.push_back(file("grew", 101, 600 + 3600));  // size moved too: changed regardless
        DiffResult diff = diffTreeAgainstManifest(walk, &previous);
        everyEntryInExactlyOneBucket(walk, diff);
        assert(diff.uniformShiftSeconds == 3600);
        assert(diff.uniformlyShiftedFiles == 20);
        assert(diff.unchanged.size() == 20);
        assert((paths(diff.changed) == std::vector<std::string>{"grew", "really-changed"})
               || (paths(diff.changed) == std::vector<std::string>{"really-changed", "grew"}));
        assert(diff.bytesToRead == 201);
        std::cout << "case 3 (uniform +1 h shift on 20 files treated as unchanged; outliers still changed) OK\n";
    }

    // ---- Not a shift: too few files, random deltas, or a non-zone delta ----
    {
        {
            BackupManifest previous;
            TreeWalk walk;
            for (int i = 0; i < 5; ++i) {  // below the detection minimum
                std::string p = "f/" + std::to_string(i);
                previous.rows.push_back(row(p, 100, 1000));
                walk.entries.push_back(file(p, 100, 4600));
            }
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            everyEntryInExactlyOneBucket(walk, diff);
            assert(diff.uniformShiftSeconds == 0 && diff.changed.size() == 5);
        }
        {
            BackupManifest previous;
            TreeWalk walk;
            for (int i = 0; i < 20; ++i) {
                std::string p = "r/" + std::to_string(i);
                previous.rows.push_back(row(p, 100, 1000));
                walk.entries.push_back(file(p, 100, 1000 + 100 * (i + 1)));  // all different
            }
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            everyEntryInExactlyOneBucket(walk, diff);
            assert(diff.uniformShiftSeconds == 0 && diff.changed.size() == 20);
        }
        {
            BackupManifest previous;
            TreeWalk walk;
            for (int i = 0; i < 20; ++i) {
                std::string p = "q/" + std::to_string(i);
                previous.rows.push_back(row(p, 100, 1000));
                walk.entries.push_back(file(p, 100, 1000 + 1000));  // uniform, but no timezone is 1000 s off
            }
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            everyEntryInExactlyOneBucket(walk, diff);
            assert(diff.uniformShiftSeconds == 0 && diff.changed.size() == 20);
        }
        std::cout << "case 4 (too few / random / non-zone deltas are real changes) OK\n";
    }

    {
        // Case 5: the floor of the FAT epoch is one stamp.
        //
        // FAT and exFAT store local time with no zone in it and cannot go
        // below their own local floor, so a driver clamps: macOS took
        // 1980-01-01T00:00:00Z and gave back 1980-01-01 00:00 CET, an
        // hour earlier. rekordbox stamps its ANLZ files with the epoch
        // itself, so an exact restore rewrote those two files on every
        // run and could never report itself finished.
        constexpr std::int64_t epoch = 315'532'800;  // 1980-01-01T00:00:00Z
        assert(mtimeMatchesRecorded(epoch, epoch - 3600));   // clamped, UTC+1
        assert(mtimeMatchesRecorded(epoch, epoch + 43200));  // and the other way, UTC-12
        assert(mtimeMatchesRecorded(epoch - 3600, epoch));
        assert(mtimeMatchesRecorded(epoch, epoch));
        assert(mtimeMatchesRecorded(epoch, epoch - 2700));  // UTC+0:45, a real zone
        // Only the floor, and only a timezone wide. A day-wide window
        // here would forgive a real edit in it, and nothing in any
        // library is dated there to notice.
        assert(!mtimeMatchesRecorded(epoch, epoch + 50'400 + 900));  // past UTC+14
        assert(!mtimeMatchesRecorded(epoch, epoch + 1000));          // nowhere near a quarter hour
        assert(!mtimeMatchesRecorded(epoch, epoch + 86'401));
        assert(!mtimeMatchesRecorded(epoch + 200'000, epoch + 200'000 + 3600));
        // FAT rounds stamps to two seconds on one side of this
        // comparison and not the other, so an offset that comes back as
        // 3599 is the same offset -- and the symptom must not return one
        // second away from the case being fixed.
        assert(mtimeMatchesRecorded(epoch, epoch - 3599));
        assert(!mtimeMatchesRecorded(epoch, epoch - 3596));
        // The ordinary window is untouched.
        assert(mtimeMatchesRecorded(1'700'000'000, 1'700'000'002));
        assert(!mtimeMatchesRecorded(1'700'000'000, 1'700'000'003));
        {
            // And it reaches the diff: a size-matched file stamped at the
            // epoch, read back an hour off, is unchanged rather than a
            // rewrite -- without the 8-file quorum a zone shift needs.
            BackupManifest previous;
            TreeWalk walk;
            previous.rows.push_back(row("PIONEER/USBANLZ/P030/ANLZ0000.DAT", 10846, epoch));
            walk.entries.push_back(file("PIONEER/USBANLZ/P030/ANLZ0000.DAT", 10846, epoch - 3600));
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            everyEntryInExactlyOneBucket(walk, diff);
            assert(diff.unchanged.size() == 1 && diff.changed.empty() && diff.uniformShiftSeconds == 0);
        }
        {
            // And a file pinned to the floor must not make the
            // uniform-shift detection harder to reach. A stick carried
            // across a DST change: ten tracks all +3600, and twenty
            // epoch-stamped ANLZ files that cannot move with them. If
            // those twenty counted as size-matched, the shift would go
            // undetected and all ten tracks would be re-read and
            // re-stored -- a full backup where an incremental was due,
            // on exactly the library shape this exception is about.
            BackupManifest previous;
            TreeWalk walk;
            for (int i = 0; i < 10; ++i) {
                const std::string p = "Contents/" + std::to_string(i) + ".mp3";
                previous.rows.push_back(row(p, 4096, 1'700'000'000));
                walk.entries.push_back(file(p, 4096, 1'700'000'000 + 3600));
            }
            for (int i = 0; i < 20; ++i) {
                const std::string p = "PIONEER/USBANLZ/" + std::to_string(i) + ".DAT";
                previous.rows.push_back(row(p, 10846, epoch));
                walk.entries.push_back(file(p, 10846, epoch - 3600));
            }
            DiffResult diff = diffTreeAgainstManifest(walk, &previous);
            everyEntryInExactlyOneBucket(walk, diff);
            assert(diff.uniformShiftSeconds == 3600);
            assert(diff.uniformlyShiftedFiles == 10);
            assert(diff.changed.empty() && diff.unchanged.size() == 30);
        }
        std::cout << "case 5 (the FAT epoch's floor is one stamp) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
