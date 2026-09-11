// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

#include "application/ports/track_metadata_probe.hpp"

namespace seabass::application
{

// Port for "have I already read this file's tags?", the same shape (and
// for the same reasons) as DurationCachePort: callers need not know
// where the cache lives, or that one exists at all -- passing nullptr is
// valid and simply means every miss is re-read.
//
// Worth having from the first version rather than bolted on later:
// reading tags off every file of a real 2100-file stick costs 22-66 s
// cold over USB, against a stat per file once cached. See
// docs/unreferenced-file-cleanup-plan.md.
//
// The real implementation (infrastructure::local::MetadataCache) keeps
// its store on the stick itself, because the answers belong to the
// stick's files rather than to whichever machine happens to be reading
// them.
class MetadataCachePort
{
public:
    virtual ~MetadataCachePort() = default;
    virtual std::optional<FileMetadata> lookup(const std::string &absoluteFilePath) const = 0;
    virtual void store(const std::string &absoluteFilePath, const FileMetadata &metadata) = 0;
};

}  // namespace seabass::application
