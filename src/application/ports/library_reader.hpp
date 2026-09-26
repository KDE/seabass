// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Port implemented by each format-specific infrastructure adapter
// (rekordbox, Engine). The application layer depends only on this
// abstraction, never on Kaitai, libdjinterop, or SQLite directly.
class LibraryReader
{
public:
    virtual ~LibraryReader() = default;
    virtual std::vector<domain::Track> readAll() = 0;

    // Progressive reading, for a stick that has just been plugged in.
    // readTracks() is everything the catalog file alone holds: title,
    // artist, duration, playlists, artwork and file paths, and for a
    // format that keeps its cues in the catalog (Engine, OneLibrary) the
    // cues too. fillCues() adds what takes another file per track:
    // rekordbox's cues live in its ANLZ files, one folder per track, and
    // on a stick that read is fifty times the catalog's. readAll() is the
    // two in one, and stays what every existing caller gets. File sizes
    // are not the reader's to fill any more: application::fillFileSizes()
    // stats the audio files for the callers that need a size.
    //
    // The defaults are for a reader whose catalog holds everything.
    virtual std::vector<domain::Track> readTracks() { return readAll(); }
    virtual void fillCues(std::vector<domain::Track> &) {}

    void setProgressReporter(ProgressReporter &reporter) { m_progress = &reporter; }
    // Readers check the token once per track, next to their progress
    // tick, and unwind with OperationCancelled.
    void setCancellationToken(CancellationToken token) { m_cancel = std::move(token); }

protected:
    ProgressReporter *m_progress = &NullProgressReporter::instance();
    CancellationToken m_cancel = CancellationToken::none();
};

}  // namespace seabass::application
