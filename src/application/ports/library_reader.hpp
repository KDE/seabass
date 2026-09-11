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

    void setProgressReporter(ProgressReporter &reporter) { m_progress = &reporter; }
    // Readers check the token once per track, next to their progress
    // tick, and unwind with OperationCancelled.
    void setCancellationToken(CancellationToken token) { m_cancel = std::move(token); }

protected:
    ProgressReporter *m_progress = &NullProgressReporter::instance();
    CancellationToken m_cancel = CancellationToken::none();
};

}  // namespace seabass::application
