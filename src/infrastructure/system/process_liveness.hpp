// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>

namespace seabass::infrastructure::system
{

// The facts a lock cookie records about its owner, and the check that
// decides whether that owner is still around. A pid alone is not enough:
// pids are recycled, so the process start time (Linux: field 22 of
// /proc/<pid>/stat, in clock ticks since boot; Windows: the creation
// FILETIME) is recorded too and must match.

std::int64_t currentPid();

// 0 when the platform cannot tell; isProcessAlive() then checks the pid
// alone.
std::uint64_t currentProcessStartId();

// True when a process with this pid exists, is not a zombie, and (when
// both sides know one) has the same start id.
bool isProcessAlive(std::int64_t pid, std::uint64_t startId);

std::string hostName();

}  // namespace seabass::infrastructure::system
