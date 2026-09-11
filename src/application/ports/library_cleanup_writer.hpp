// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::application
{

// Port for permanently removing a duplicate track from a library, as
// part of the "Clean Up" duplicate-removal feature. Implemented for
// Engine (libdjinterop) and rekordbox (PdbRowWriter) -- both are
// destructive, so the caller is responsible for backing up the
// library's files before constructing an implementation.
class LibraryCleanupWriter
{
public:
    virtual ~LibraryCleanupWriter() = default;

    // Permanently removes doomedTrackId from the library. Before doing
    // so, ensures every playlist doomedTrackId belonged to also
    // contains survivorTrackId (adding/repointing it in if it doesn't
    // already) -- so a playlist never silently loses a song just
    // because the specific copy it referenced was the one removed.
    // Throws std::runtime_error if either track doesn't exist.
    virtual void removeTrackReplacingWith(const std::string &doomedTrackId, const std::string &survivorTrackId) = 0;
};

}  // namespace seabass::application
