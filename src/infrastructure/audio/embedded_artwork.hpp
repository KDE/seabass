// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure::audio
{

// The cover art inside an audio file's own tags -- an ID3v2 APIC frame, a
// FLAC/Ogg picture block, or an MP4 "covr" atom. This is where Engine DJ
// and rekordbox both got the art they keep their own copies of, so it is
// the last source still on the stick when both of those copies are gone:
// no backup, no other library, just the track.
//
// Returns the picture bytes, or an empty string when the file has none
// (or cannot be read). Prefers the front-cover picture when a file holds
// several, the way a player picks one.
std::string readEmbeddedArtwork(const std::string &audioFile);

// Whether readEmbeddedArtwork would return something, without keeping the
// bytes: what a scan asks, so a page can promise a repair it can deliver
// before anything is read into memory.
bool hasEmbeddedArtwork(const std::string &audioFile);

}  // namespace seabass::infrastructure::audio
