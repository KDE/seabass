// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure::system
{

// Best-effort: scans this machine's running processes for one whose own
// process name (not full command line) case-insensitively matches `name`.
// A `false` result means "not detected," never "definitely not running" --
// a background/tray process, a different user session, or a platform this
// detector doesn't support yet (see the .cpp) can all defeat it. Callers
// must treat this as an advisory safety net on top of, never instead of,
// the app's own write-locking (see infrastructure::backup::StickWriteLock).
bool isProcessRunning(const std::string &name);

// Convenience wrapper for the one process this app actually cares about:
// writing to a stick's files while rekordbox itself might also have them
// open is a real corruption risk.
bool isRekordboxRunning();

// Engine DJ (desktop; formerly Engine Prime) writes to the stick's Engine
// database, which the full-stick backup must read while nothing else
// touches it. Matches the process names the app has shipped under.
bool isEngineDjRunning();

// Either of the above; `conflictingDjSoftwareName()` returns which one
// ("rekordbox", "Engine DJ"), or an empty string when none was detected,
// for messages.
bool isConflictingDjSoftwareRunning();
std::string conflictingDjSoftwareName();

}  // namespace seabass::infrastructure::system
