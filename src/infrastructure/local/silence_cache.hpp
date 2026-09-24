// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <filesystem>
#include <string>

#include "domain/audio_content_probe.hpp"

namespace seabass::infrastructure::local
{

// A stick-local record of where the music starts and stops inside each
// file that has been decoded, so the decode is paid once per stick
// rather than on every scan. Stored as JSON Lines at
// "<stickRoot>/Seabass/caches/silence.jsonl", same shape and same place
// as the duration cache:
//
//   {"lead":"0.512","mtime":"1710000000","path":"Contents/a/b.mp3",
//    "size":"22545278","total":"266.376","trail":"1.880"}
//
// Paths are relative to the stick root, so a cache written on Linux
// under /media/me/STICK still reads on Windows under H:\.
//
// An entry is trusted only while the file's size AND mtime both still
// match -- the same strictness DurationCache applies, and for the same
// reason: this feeds a caller that offers to delete things.
//
// It is a decorator over another probe rather than a container the
// caller consults itself, because the point is that the expensive probe
// is not called; making that the default shape leaves no call site able
// to forget.
class CachedAudioContentProbe : public domain::AudioContentProbe
{
public:
    // `inner` may be null, which is a cache-only probe: it answers from
    // what has already been measured and nullopt for anything else. That
    // is what a build with no decoder gets, and it is useful rather than
    // pointless -- the cache on the stick may have been written by a
    // build that did have one.
    CachedAudioContentProbe(std::string stickRoot, std::unique_ptr<domain::AudioContentProbe> inner);
    ~CachedAudioContentProbe() override;

    std::optional<domain::AudioContentSpan> measure(const std::string &absoluteFilePath) override;

    // True when measure() has added anything since load.
    bool dirty() const { return m_dirty; }

    // Writes the cache back atomically. False when the stick is
    // read-only or the write fails; a caller should carry on, since
    // losing the cache only ever costs time. Called by the destructor
    // too, so a caller that forgets still keeps its work.
    bool save();

    // How many entries were loaded from disk (for reporting/tests).
    std::size_t size() const { return m_entries.size(); }

    // How many files this probe decoded AND got an answer for, as
    // opposed to answered from the cache. Successes only, deliberately:
    // a caller prints this as "compared the audio of N files", and a
    // build whose decoder never loaded would otherwise report having
    // compared every file it failed to open. Counting attempts there
    // would be the exact silent-counter shape this project keeps
    // finding.
    int decodedCount() const { return m_decoded; }

    // How many files were asked of the decoder and gave no answer this
    // run: no backend, a container it would not open, a file gone from
    // the stick. Counted separately so "nothing was comparable" and
    // "nothing needed comparing" cannot look the same.
    int failedCount() const { return static_cast<int>(m_failed.size()); }

private:
    struct Entry
    {
        double totalSeconds = 0.0;
        double leadingSilenceSeconds = 0.0;
        double trailingSilenceSeconds = 0.0;
        long long sizeBytes = 0;
        long long mtimeSeconds = 0;
    };

    // Empty when the path is not under the stick root -- nothing
    // outside the stick belongs in this stick's cache.
    std::string relativeKey(const std::string &absoluteFilePath) const;

    std::string m_stickRoot;
    std::filesystem::path m_cachePath;
    std::map<std::string, Entry> m_entries;
    // Paths the decoder already refused THIS RUN. In memory only, never
    // written: a failure is about this moment (an unplugged stick, a
    // backend that was not ready) and writing it down would make one bad
    // moment permanent for that file. But re-asking within one run is
    // pure waste -- the finder calls measure() once per *pair*, so a
    // file in a group of five copies is asked four times, and each ask
    // is a decode attempt that already failed.
    std::set<std::string> m_failed;
    std::unique_ptr<domain::AudioContentProbe> m_inner;
    bool m_dirty = false;
    int m_decoded = 0;
};

}  // namespace seabass::infrastructure::local
