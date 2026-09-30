// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace seabass::infrastructure
{

// Which volume a path lives on: st_dev on POSIX, the volume serial number
// on Windows. nullopt when it cannot be asked, which a caller deciding
// "is this still the same drive" must treat as "cannot tell", never as
// "yes". Clean Up Recordings asks it of the stick before each delete, so
// a stick pulled (or another one mounted in its place) mid-run stops the
// run from counting files it never deleted.
std::optional<std::uint64_t> volumeIdOf(const std::filesystem::path &path);

}  // namespace seabass::infrastructure
