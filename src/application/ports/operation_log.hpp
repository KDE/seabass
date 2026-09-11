// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::application
{

// Port for a persistent, append-only record of mutating operations
// Seabass performs (writes, backups) -- distinct from the terminal
// report, which is ephemeral. Every write should log what it did here, in
// enough detail to reconstruct it later without Seabass running.
class OperationLog
{
public:
    virtual ~OperationLog() = default;
    virtual void record(const std::string &message) = 0;
};

}  // namespace seabass::application
