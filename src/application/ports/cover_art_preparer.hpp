// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "application/ports/recording_tag_writer.hpp"

namespace seabass::application
{

// The limits of an embedded front cover. Network players are picky about
// embedded pictures: Bluesound's documentation does not give its limits,
// and a large or progressive JPEG is the usual reason a player shows no
// cover. These are a conservative choice, at most 1200 pixels on the
// longer side, at most about 500 KB, baseline JPEG, still to be confirmed
// on the player.
inline constexpr int MaxCoverSidePixels = 1200;
inline constexpr std::size_t MaxCoverBytes = 500 * 1024;

struct CoverArtResult
{
    std::optional<CoverArt> cover;
    // Why not, when there is no cover.
    std::string error;
};

// Makes an embeddable front cover from a picture file's bytes (JPEG, PNG
// or whatever else the picture library reads): scaled down so that its
// longer side is at most MaxCoverSidePixels (never up), as a baseline
// JPEG of at most MaxCoverBytes.
//
// Never throws.
class CoverArtPreparer
{
public:
    virtual ~CoverArtPreparer() = default;

    virtual CoverArtResult prepare(const std::string &pictureBytes) = 0;
};

}  // namespace seabass::application
