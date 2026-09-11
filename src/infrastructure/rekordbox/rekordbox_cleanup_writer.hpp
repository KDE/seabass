// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/library_cleanup_writer.hpp"

namespace seabass::infrastructure::rekordbox
{

// Permanently removes a duplicate track from a rekordbox USB export,
// composed entirely from PdbRowWriter's already-proven primitives:
// reassigns every playlist_entry pointing at the doomed track onto the
// survivor first (repointing where the survivor isn't already in that
// playlist, dropping the entry where it is, so no playlist silently
// loses the song and none end up with a duplicate), then removes the
// doomed track row itself, all in one PdbRowWriter session committed
// atomically. The caller (cli:: or gui::) is responsible for backing
// up export.pdb before constructing this -- this class only performs
// the write itself.
class RekordboxCleanupWriter : public application::LibraryCleanupWriter
{
public:
    explicit RekordboxCleanupWriter(std::string pioneerRoot);

    void removeTrackReplacingWith(const std::string &doomedTrackId, const std::string &survivorTrackId) override;

private:
    std::string m_pioneerRoot;
};

}  // namespace seabass::infrastructure::rekordbox
