// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>

namespace seabass::infrastructure::local
{

// Where this app keeps per-user, per-machine data that is not a setting:
// the local cue store, the benchmark history, the edit-lock cookies.
//
// ~/Seabass/metadata, not $XDG_DATA_HOME/seabass or %LOCALAPPDATA%. The
// platform conventions are the usual answer, and the reason for going
// against them is that everything else Seabass owns on this machine --
// the stick images, the local cue backups -- is somewhere the user can
// see, and splitting Seabass's own state across a visible directory and
// a hidden one means "back up my Seabass data" has two answers. One
// place, one answer. See infrastructure/paths/seabass_paths.hpp.
//
// Not created here -- callers create what they need under it.
std::filesystem::path appDataDirectory();

}  // namespace seabass::infrastructure::local
