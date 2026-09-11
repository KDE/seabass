// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

#include <djinterop/database.hpp>

#include "application/ports/cue_writer.hpp"

namespace seabass::infrastructure::engine
{

// Writes hot cues back into an existing Engine Library track using
// libdjinterop's track::set_hot_cues(). The caller (cli/) is responsible
// for backing up the library's files before constructing this -- this
// class only performs the write itself.
class LibdjinteropEngineCueWriter : public application::CueWriter
{
public:
    explicit LibdjinteropEngineCueWriter(std::string engineLibraryPath);

    void writeHotCues(const std::string &trackSourceId, const std::vector<domain::CuePoint> &cues) override;

    // The two authored fields that are not cues. Rating in stars (0 to
    // 5, the scale domain::Track uses); Engine's own 0-to-100 scale is
    // applied here rather than by the caller. Either may be absent, and
    // an absent one is left alone rather than cleared. Throws if the
    // track doesn't exist.
    void writeAnnotation(const std::string &trackSourceId, const std::optional<int> &stars,
                          const std::optional<std::string> &comment);

    // Fills in a Clean Up survivor's missing bpm/key from another copy
    // in its duplicate group (see domain::DuplicateCleanupPlan). Only
    // bpm and key -- Engine track artwork isn't writable through
    // libdjinterop today (djinterop::track has no set_album_art()/
    // equivalent at all, and djinterop::album_art's own header is
    // explicitly marked "TODO - implement rest of album_art class"), so
    // artwork propagation isn't offered for this format. Either
    // optional being unset just skips that field; throws if the track
    // doesn't exist.
    void propagateMissingFields(const std::string &trackSourceId, std::optional<double> bpm,
                                 std::optional<std::string> key);

private:
    // The open library, kept rather than reopened per call.
    //
    // load_database() is a full SQLite open plus schema detection, and
    // every method used to do one: a 200-item save opened the same
    // database 200 times, about 151 ms each against a stick (see
    // docs/write-path-performance.md). Held lazily so constructing a
    // writer stays free for the call sites that build one and never use
    // it.
    //
    // Safe to hold for a save: libdjinterop writes through its own
    // transactions, and nothing else in this process writes this file
    // while a save owns the writer.
    djinterop::database &database();

    std::string m_engineLibraryPath;
    std::optional<djinterop::database> m_database;
};

}  // namespace seabass::infrastructure::engine
