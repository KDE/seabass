// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <map>
#include <optional>
#include <filesystem>
#include <string>

#include "application/ports/metadata_cache_port.hpp"

namespace seabass::infrastructure::local
{

// A stick-local record of metadata already read from audio files, so the
// read is paid once per stick rather than on every scan. Stored as JSON
// Lines at "<stickRoot>/Seabass/caches/metadata.jsonl" -- same shape, same
// place and same rules as DurationCache, which this deliberately
// mirrors rather than extends: the two caches answer different
// questions, are written by different probes, and an older Seabass
// reading a newer stick should find the file it expects rather than one
// that grew fields it cannot parse.
//
//   {"album":"...","artist":"...","bitrate":"320","duration":"266.376",
//    "estimated":"0","mtime":"1710000000","path":"Contents/a/b.mp3",
//    "samplerate":"44100","size":"22545278","title":"..."}
//
// An MP3's entry also carries "mp3leadin" (#50; the format is described
// in the .cpp). It is a new field rather than a new file version: an
// entry without it is still good for everything else, and only a caller
// that wants the lead-in probes that file again.
//
// An entry may carry the lead-in alone, with a duration of 0: what a
// pass stores when it read the file's start itself because the probe had
// nothing to say (a build without TagLib, a file TagLib finds no length
// in). Such an entry answers only lookupLeadIn(); lookup() and
// lookupByPathOnly() miss it, so nothing takes it for the file's tags or
// length. An older Seabass skips the line (it refuses a zero duration).
//
// Paths are stored RELATIVE to the stick root, so a cache written on
// Linux under /media/me/STICK still reads on Windows under H:\ -- the
// same stick gets mounted at a different place on every machine, and an
// absolute path would silently miss every entry.
//
// An entry is only trusted when the file's current size AND mtime both
// still match what was recorded. That is deliberately strict: a re-rip
// or a re-tag that happened to preserve one of the two would otherwise
// return stale artist/title/duration, and all three feed a caller that
// deletes files (see DuplicateTrackFinder, and
// docs/unreferenced-file-cleanup-plan.md).
class MetadataCache : public application::MetadataCachePort
{
public:
    // Loads the cache for the stick mounted at stickRoot. A missing,
    // unreadable or malformed file is not an error -- it just yields an
    // empty cache, since this is a cache and never a source of truth.
    explicit MetadataCache(std::string stickRoot);

    // Cached metadata for an absolute path under the stick root, or
    // nullopt when absent or stale (size/mtime changed).
    std::optional<application::FileMetadata> lookup(const std::string &absoluteFilePath) const override;

    // The same without the stat that checks the file is unchanged: for a
    // pass that must not touch every audio file (the catalog cache's
    // Tracks stage), whose answer a later pass checks.
    std::optional<application::FileMetadata> lookupByPathOnly(const std::string &absoluteFilePath) const;

    // The entry for a caller that wants only the MP3 lead-in: also one
    // that carries nothing else (see above). Checked against the file
    // like lookup(), or, with checkFile false, taken by path like
    // lookupByPathOnly().
    std::optional<application::FileMetadata> lookupLeadIn(const std::string &absoluteFilePath,
                                                          bool checkFile = true) const;

    // Records metadata. Nothing is written to disk until save(). Refused
    // without a duration unless it carries a lead-in (mp3LeadInRead).
    void store(const std::string &absoluteFilePath, const application::FileMetadata &metadata) override;

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
        application::FileMetadata metadata;
        long long sizeBytes = 0;
        long long mtimeSeconds = 0;
    };

    // Empty when the path is not under the stick root (nothing outside
    // the stick belongs in this stick's cache).
    std::string relativeKey(const std::string &absoluteFilePath) const;

    std::string m_stickRoot;
    std::filesystem::path m_cachePath;
    std::map<std::string, Entry> m_entries;
    bool m_dirty = false;
};

}  // namespace seabass::infrastructure::local
