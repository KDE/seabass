// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/stat_diff.hpp"

#include <cstdlib>
#include <map>
#include <unordered_map>

namespace seabass::infrastructure::stick_backup
{

namespace
{

constexpr std::int64_t ShiftQuantumSeconds = 900;      // every real timezone offset is a multiple
constexpr std::int64_t MaxShiftSeconds = 14 * 3600;    // UTC-12 .. UTC+14
constexpr std::size_t MinShiftedFilesForDetection = 8;

bool withinWindow(std::int64_t a, std::int64_t b)
{
    return std::llabs(a - b) <= MtimeWindowSeconds;
}

bool plausibleZoneShift(std::int64_t delta)
{
    return delta != 0 && std::llabs(delta) <= MaxShiftSeconds && delta % ShiftQuantumSeconds == 0;
}

// 1980-01-01T00:00:00Z, the FAT epoch: the floor a FAT or exFAT driver
// clamps to, read back through whatever local offset the reading
// machine has. A stamp within one timezone of it is that floor and not
// a date; see mtimeMatchesRecorded() in the header.
constexpr std::int64_t FatEpochUnix = 315'532'800;

bool atFatEpoch(std::int64_t when)
{
    return std::llabs(when - FatEpochUnix) <= MaxShiftSeconds;
}

}  // namespace

bool mtimeMatchesRecorded(std::int64_t recordedUnix, std::int64_t onDiskUnix)
{
    if (withinWindow(recordedUnix, onDiskUnix)) {
        return true;
    }
    // Both at the floor, and apart by a timezone: the same clamped stamp
    // read through two offsets. Deliberately not "both somewhere in
    // 1980-01-01", which would forgive a whole day -- a window nothing
    // would ever test, since no real file is dated there to notice an
    // edit going unbacked-up inside it.
    //
    // Snapped to the nearest quarter hour before judging, with the same
    // two-second tolerance the uniform-shift path uses further down: FAT
    // rounds stamps to two seconds on one side of this comparison and
    // not the other, and an offset that came back as 3599 rather than
    // 3600 would bring the whole symptom back one second away from the
    // case being fixed.
    const std::int64_t delta = onDiskUnix - recordedUnix;
    const std::int64_t snapped = (delta + (delta >= 0 ? ShiftQuantumSeconds / 2 : -ShiftQuantumSeconds / 2))
                                 / ShiftQuantumSeconds * ShiftQuantumSeconds;
    return atFatEpoch(recordedUnix) && atFatEpoch(onDiskUnix) && plausibleZoneShift(snapped)
        && withinWindow(delta, snapped);
}

std::string pathCompareKey(std::string_view relativePath)
{
    return std::string(relativePath);
}

DiffResult diffTreeAgainstManifest(const TreeWalk &tree, const BackupManifest *previous)
{
    DiffResult result;
    if (previous == nullptr) {
        for (const TreeEntry &entry : tree.entries) {
            result.added.push_back(&entry);
            result.bytesToRead += entry.isDirectory ? 0 : entry.size;
        }
        return result;
    }

    std::unordered_map<std::string, const ManifestRow *> rows;
    rows.reserve(previous->rows.size());
    for (const ManifestRow &row : previous->rows) {
        rows.emplace(pathCompareKey(row.path), &row);
    }

    // First pass: classify, but hold back size-matched files whose mtime
    // moved so the uniform-shift check can look at them together.
    struct Pending
    {
        const TreeEntry *entry;
        std::int64_t delta;  // stick mtime - recorded mtime
    };
    std::vector<Pending> sizeMatchedButMoved;
    std::size_t sizeMatched = 0;
    std::unordered_map<std::string, bool> seenIsDirectory;
    seenIsDirectory.reserve(tree.entries.size());

    for (const TreeEntry &entry : tree.entries) {
        std::string key = pathCompareKey(entry.relativePath);
        seenIsDirectory.emplace(key, entry.isDirectory);
        auto it = rows.find(key);
        if (it == rows.end() || (it->second->kind == ManifestRow::Kind::Directory) != entry.isDirectory) {
            result.added.push_back(&entry);
            result.bytesToRead += entry.isDirectory ? 0 : entry.size;
            continue;
        }
        const ManifestRow &row = *it->second;
        if (entry.isDirectory) {
            result.unchanged.push_back(&entry);
            continue;
        }
        if (row.size != entry.size) {
            result.changed.push_back(&entry);
            result.bytesToRead += entry.size;
            continue;
        }
        if (withinWindow(row.mtimeUnix, entry.mtimeUnix)) {
            ++sizeMatched;
            result.unchanged.push_back(&entry);
        } else if (mtimeMatchesRecorded(row.mtimeUnix, entry.mtimeUnix)) {
            // Both stamps at the FAT epoch's floor. Left out of
            // sizeMatched deliberately: a file pinned to the floor
            // cannot move with the rest when a stick crosses a timezone,
            // so counting it would only raise the bar the uniform-shift
            // detection below has to clear. A rekordbox stick carries
            // two ANLZ files per track, which is enough of them to keep
            // that detection from ever firing -- and then every track
            // file on a DST-shifted stick is re-read and re-stored.
            result.unchanged.push_back(&entry);
        } else {
            ++sizeMatched;
            sizeMatchedButMoved.push_back({&entry, entry.mtimeUnix - row.mtimeUnix});
        }
    }

    // Uniform-shift detection: if nearly every moved-but-same-size file
    // moved by the same plausible zone offset, the clock moved, not the
    // files.
    std::int64_t shift = 0;
    if (sizeMatchedButMoved.size() >= MinShiftedFilesForDetection && sizeMatchedButMoved.size() * 2 >= sizeMatched) {
        std::map<std::int64_t, std::size_t> histogram;
        for (const Pending &p : sizeMatchedButMoved) {
            // Quantize to the window so 3599/3600/3601 count together.
            std::int64_t bucket = (p.delta + (p.delta >= 0 ? MtimeWindowSeconds : -MtimeWindowSeconds)) / (2 * MtimeWindowSeconds + 1)
                                  * (2 * MtimeWindowSeconds + 1);
            ++histogram[bucket];
        }
        for (const auto &[bucket, count] : histogram) {
            if (count * 10 >= sizeMatchedButMoved.size() * 9) {
                // Snap the bucket to the nearest quantum before judging it.
                std::int64_t snapped = (bucket + (bucket >= 0 ? ShiftQuantumSeconds / 2 : -ShiftQuantumSeconds / 2))
                                       / ShiftQuantumSeconds * ShiftQuantumSeconds;
                if (plausibleZoneShift(snapped)) {
                    shift = snapped;
                }
                break;
            }
        }
    }
    for (const Pending &p : sizeMatchedButMoved) {
        if (shift != 0 && withinWindow(p.delta, shift)) {
            result.unchanged.push_back(p.entry);
            ++result.uniformlyShiftedFiles;
        } else {
            result.changed.push_back(p.entry);
            result.bytesToRead += p.entry->size;
        }
    }
    result.uniformShiftSeconds = result.uniformlyShiftedFiles > 0 ? shift : 0;

    // A recorded path that is gone, or that came back as the other kind
    // (a directory where a file was), no longer has an entry to carry.
    for (const ManifestRow &row : previous->rows) {
        auto it = seenIsDirectory.find(pathCompareKey(row.path));
        if (it == seenIsDirectory.end() || it->second != (row.kind == ManifestRow::Kind::Directory)) {
            result.removed.push_back(row.path);
        }
    }
    return result;
}

}  // namespace seabass::infrastructure::stick_backup
