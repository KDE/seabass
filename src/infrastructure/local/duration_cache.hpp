// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <map>

#include "application/ports/duration_cache_port.hpp"
#include <optional>
#include <string>

namespace seabass::infrastructure::local
{

// A stick-local record of durations already probed from audio files, so
// the probe is paid once per stick rather than on every scan. Stored as
// JSON Lines at "<stickRoot>/Seabass/caches/durations.jsonl" -- same shape and
// same place as the pending-deletion manifest, one flat object per line:
//
//   {"duration":"266.376","mtime":"1710000000","path":"Contents/a/b.mp3","size":"22545278"}
//
// Paths are stored RELATIVE to the stick root, so a cache written on
// Linux under /media/me/STICK still reads on Windows under H:\ -- the
// same stick gets mounted at a different place on every machine, and an
// absolute path would silently miss every entry.
//
// An entry is only trusted when the file's current size AND mtime both
// still match what was recorded. That is deliberately strict: a re-rip
// or re-tag that happens to preserve one of the two would otherwise
// return a stale length, and a wrong length here feeds a destructive
// caller (see DuplicateTrackFinder).
class DurationCache : public application::DurationCachePort
{
public:
    // Loads the cache for the stick mounted at stickRoot. A missing,
    // unreadable or malformed file is not an error -- it just yields an
    // empty cache, since this is a cache and never a source of truth.
    explicit DurationCache(std::string stickRoot);

    // Cached duration for an absolute path under the stick root, or
    // nullopt when absent or stale (size/mtime changed).
    std::optional<double> lookup(const std::string &absoluteFilePath) const override;

    // Records a duration. Nothing is written to disk until save().
    void store(const std::string &absoluteFilePath, double durationSeconds) override;

    // True when store() has added or changed anything since load.
    bool dirty() const { return m_dirty; }

    // Writes the cache back atomically. Returns false when the stick is
    // read-only or the write fails -- a caller should carry on rather
    // than fail the scan, since losing the cache only costs time.
    bool save();

    // How many entries were loaded from disk (for reporting/tests).
    size_t size() const { return m_entries.size(); }

private:
    struct Entry
    {
        double durationSeconds = 0.0;
        long long sizeBytes = 0;
        long long mtimeSeconds = 0;
    };

    // Empty when the path is not under the stick root (nothing outside
    // the stick belongs in this stick's cache).
    std::string relativeKey(const std::string &absoluteFilePath) const;

    std::string m_stickRoot;
    std::string m_cachePath;
    std::map<std::string, Entry> m_entries;
    bool m_dirty = false;
};

}  // namespace seabass::infrastructure::local
