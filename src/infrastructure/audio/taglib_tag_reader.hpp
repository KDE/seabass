// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

#include "application/ports/track_tag_reader.hpp"

namespace seabass::infrastructure::audio
{

// A file's tags, read with TagLib: ID3v2 (MP3, AIFF, WAV), Vorbis
// comments (FLAC, Ogg), MP4 atoms.
//
// Title, artist, album, genre, year and track number come from TagLib's
// own format-independent tag, the one TagLibMetadataProbe reads title,
// artist and album from; here without the spaces a tagger left at either
// end. The rest (comment, album artist, composer, label, remixer, key,
// tempo) comes from TagLib's property map.
//
// Stateless, like TagLibMetadataProbe: several may run on different
// threads over different files.
class TagLibTagReader : public application::TrackTagReader
{
public:
    std::optional<domain::TrackTags> read(const std::string &absoluteFilePath) override;
};

}  // namespace seabass::infrastructure::audio
