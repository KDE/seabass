// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/taglib_duration_probe.hpp"

#include "infrastructure/audio/taglib_metadata_probe.hpp"

namespace seabass::infrastructure::audio
{

std::optional<double> TagLibDurationProbe::durationSeconds(const std::string &absoluteFilePath)
{
    TagLibMetadataProbe probe;
    const std::optional<application::FileMetadata> metadata = probe.read(absoluteFilePath);
    // A length of zero is TagLib saying it could not work one out, which
    // is this port's nullopt: see TrackDurationProbe's own doc comment.
    if (!metadata || metadata->durationSeconds <= 0.0) {
        return std::nullopt;
    }
    return metadata->durationSeconds;
}

}  // namespace seabass::infrastructure::audio
