// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

namespace seabass::application
{

// Port for "have I already probed this file's length?", so
// fillMissingDurations() need not know where the cache lives -- or that
// one exists at all: passing nullptr is valid and simply means every
// miss is re-probed.
//
// The real implementation (infrastructure::local::DurationCache) keeps
// its store on the stick itself, because the answer belongs to the
// stick's files rather than to whichever machine happens to be reading
// them.
class DurationCachePort
{
public:
    virtual ~DurationCachePort() = default;
    virtual std::optional<double> lookup(const std::string &absoluteFilePath) const = 0;
    virtual void store(const std::string &absoluteFilePath, double durationSeconds) = 0;
};

}  // namespace seabass::application
