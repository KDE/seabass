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
    // The cached length, only while the file still matches what was
    // recorded when it was probed: this looks at the file.
    virtual std::optional<double> lookup(const std::string &absoluteFilePath) const = 0;
    // The cached length by path alone, without looking at the file: for a
    // read that must not touch the audio files (a stick's Tracks stage,
    // where a stat per file is seconds cold). Whoever takes an answer from
    // here owes a lookup() of the same path later, before the length is
    // relied on for anything final; see verifyCachedDurations(). A cache
    // that cannot answer without looking answers as lookup() does.
    virtual std::optional<double> lookupUnverified(const std::string &absoluteFilePath) const
    {
        return lookup(absoluteFilePath);
    }
    virtual void store(const std::string &absoluteFilePath, double durationSeconds) = 0;
};

}  // namespace seabass::application
