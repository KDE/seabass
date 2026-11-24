// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

#include "domain/track_tags.hpp"

namespace seabass::application
{

// Reads everything an audio file's tags say about the track, for a
// library row made from the file alone (the track import).
//
// Beside TrackMetadataProbe rather than in it: that one is read for
// every file of a stick and its answers are cached on the stick
// (MetadataCachePort), so each field added to it is a field of that
// cache. This one is read once per imported file, from the computer.
class TrackTagReader
{
public:
    virtual ~TrackTagReader() = default;

    // nullopt for a file that cannot be opened as audio. A file that
    // opens and has no tags is a TrackTags with nothing in it. Never
    // throws.
    virtual std::optional<domain::TrackTags> read(const std::string &absoluteFilePath) = 0;
};

// For builds without a tag library: every file reads as unopenable.
class NullTrackTagReader : public TrackTagReader
{
public:
    std::optional<domain::TrackTags> read(const std::string &) override
    {
        return std::nullopt;
    }
};

}  // namespace seabass::application
