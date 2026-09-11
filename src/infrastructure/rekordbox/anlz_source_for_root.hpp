// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>

#include "infrastructure/rekordbox/anlz_byte_source.hpp"

namespace seabass::infrastructure::rekordbox
{

// The right AnlzByteSource for a PIONEER folder: an archive-backed source
// when the folder's parent is a stick backup being browsed (decided by
// local::isBrowsedBackupRoot -- marker present AND under the browse
// cache, never a stray marker on a real stick), the plain filesystem
// otherwise.
//
// Deciding it here, from what is on disk, rather than through a
// process-wide registry the composition roots have to remember to
// populate: every existing construction of a rekordbox reader then works
// against a browsed backup without changing, and it survives a restart
// because the marker does.
//
// One open archive is shared per backup, for as long as any reader holds
// it or until forgetArchiveSource() is called: opening means reading and
// validating the whole central directory, and the waveform reader asks
// on the UI thread. Falls back to the filesystem source whenever the
// archive will not open -- browsing then shows no cues rather than
// failing the whole scan.
std::shared_ptr<AnlzByteSource> anlzSourceForPioneerRoot(const std::string &pioneerRoot);

// Drops the shared open archive for `archivePath`, closing its file
// handle once the last reader lets go. Called when a browsed backup is
// closed: an open handle is otherwise held until the app exits, and on
// Windows that blocks replacing the archive (Compact, a new backup
// generation) with "another program keeps it open" -- Seabass itself.
void forgetArchiveSource(const std::string &archivePath);

}  // namespace seabass::infrastructure::rekordbox
