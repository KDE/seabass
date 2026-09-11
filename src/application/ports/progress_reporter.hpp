// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <string>

namespace seabass::application
{

// Port for reporting progress of a long-running use case (e.g. scanning a
// large library). Implemented in cli/ as an actual terminal progress bar;
// infrastructure adapters only depend on this abstraction.
class ProgressReporter
{
public:
    virtual ~ProgressReporter() = default;

    // total == 0 means the total is unknown; implementations should fall
    // back to an indeterminate display (e.g. a running count) in that case.
    virtual void start(const std::string &label, size_t total) = 0;
    virtual void tick(size_t current) = 0;
    virtual void finish() = 0;

    // Prints a warning without corrupting an in-progress bar (safe to call
    // between start() and finish()). Adapters must route warnings through
    // here rather than writing to stderr directly while progress is active.
    virtual void warn(const std::string &message) = 0;
};

class NullProgressReporter : public ProgressReporter
{
public:
    static NullProgressReporter &instance();

    void start(const std::string &, size_t) override {}
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &message) override;
};

}  // namespace seabass::application
