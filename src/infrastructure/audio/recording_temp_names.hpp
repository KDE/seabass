// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The name of the temporary file beside a kept FLAC, the encoder's file
// until it is tagged in place and given the FLAC's name:
//
//     "<FLAC's name>.<16 hex digits>.seabass-encode-tmp"
//
// The suffix comes last and is no audio extension, so a music player that
// indexes the folder does not take a leftover for a track. Header only:
// the sweep of leftovers (LocalKeepRecordingFiles) needs the name without
// the encoder's libraries.
namespace seabass::infrastructure::audio
{

inline constexpr const char *RecordingEncodeTempSuffix = ".seabass-encode-tmp";
inline constexpr std::size_t RecordingTempTokenDigits = 16;

// How many bytes the temporary name adds to a FLAC's name.
inline constexpr std::size_t recordingTempNameExtraBytes()
{
    return 1 + RecordingTempTokenDigits + std::string_view(RecordingEncodeTempSuffix).size();
}

// The FLAC's name a temporary name belongs to: the part before
// ".<16 lower case hex digits>.seabass-encode-tmp"; nullopt for any other
// name.
inline std::optional<std::string> recordingTempTargetName(std::string_view name)
{
    const std::string_view suffix(RecordingEncodeTempSuffix);
    const std::size_t tail = recordingTempNameExtraBytes();
    if (name.size() <= tail || name.substr(name.size() - suffix.size()) != suffix) {
        return std::nullopt;
    }
    const std::size_t dot = name.size() - tail;
    const std::string_view token = name.substr(dot + 1, RecordingTempTokenDigits);
    const bool hex = std::all_of(token.begin(), token.end(),
                                 [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (name[dot] != '.' || !hex) {
        return std::nullopt;
    }
    return std::string(name.substr(0, dot));
}

}  // namespace seabass::infrastructure::audio
