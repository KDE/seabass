// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

#include "gui/edit/changes/import_track_change.hpp"

namespace seabass::gui
{

// The picture in `audioFile`'s tags (infrastructure::audio::
// readEmbeddedArtwork) as the JPEGs an import writes: cut to its middle
// square, then 80 and 240 pixels a side for rekordbox's libraries, as
// rekordbox's own exports have them, and up to 500 for Engine (never
// larger than the picture is). nullopt when the file has no picture, or
// one Qt does not read.
std::optional<CoverPictures> readCoverPictures(const std::string &audioFile);

// The same from the picture's bytes.
std::optional<CoverPictures> scaleCoverPictures(const std::string &pictureBytes);

}  // namespace seabass::gui
