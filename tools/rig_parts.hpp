// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// The C++ side of tools/rig-parts.sh: one result line per test for a rig
// program that runs several of them.
//
// A program that reads a stick does several separate things to it, and a
// single exit status cannot say which one failed. When the rig sets
// RIG_PARTS to a file, rigPart() appends "<id><suffix>\tPASS|FAIL" to it
// and the board gets a row per test. RIG_PART_SUFFIX tells two runs of
// the same program apart -- "-A" and "-B" for the two sticks. Without
// RIG_PARTS this writes nothing and the program behaves as before.

#include <cstdlib>
#include <fstream>
#include <string>

inline void rigPart(const std::string &id, bool passed)
{
    const char *path = std::getenv("RIG_PARTS");
    if (path == nullptr || *path == '\0') {
        return;
    }
    const char *suffix = std::getenv("RIG_PART_SUFFIX");
    std::ofstream out(path, std::ios::app);
    out << id << (suffix != nullptr ? suffix : "") << "\t" << (passed ? "PASS" : "FAIL") << "\n";
}
