// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The names of the temporary files the recording encoder and the
// recording tag writer write beside a kept FLAC:
//
//     "<FLAC's name>.<16 hex digits><suffix>"
//
// The suffix comes last and is no audio extension, so a music player that
// indexes the folder does not take a leftover for a track. Header only:
// the sweep of leftovers (LocalKeepRecordingFiles) needs the names without
// the encoder's libraries.
namespace seabass::infrastructure::audio
{

// The encoder's file before it is proven, and the tag writer's copy.
inline constexpr const char *RecordingEncodeTempSuffix = ".seabass-encode-tmp";
inline constexpr const char *RecordingTagTempSuffix = ".seabass-tag-tmp";
inline constexpr std::size_t RecordingTempTokenDigits = 16;

// The suffixes, for whoever walks them all.
inline constexpr const char *RecordingTempSuffixes[] = {RecordingEncodeTempSuffix, RecordingTagTempSuffix};

// How many bytes the longest of the temporary names adds to a FLAC's name.
inline constexpr std::size_t recordingTempNameExtraBytes()
{
    std::size_t most = 0;
    for (const char *suffix : RecordingTempSuffixes) {
        most = std::max(most, 1 + RecordingTempTokenDigits + std::string_view(suffix).size());
    }
    return most;
}

// The FLAC's name a temporary name of one of the suffixes belongs to:
// the part before ".<16 lower case hex digits><suffix>"; nullopt for any
// other name.
inline std::optional<std::string> recordingTempTargetName(std::string_view name)
{
    for (const char *suffixText : RecordingTempSuffixes) {
        const std::string_view suffix(suffixText);
        const std::size_t tail = 1 + RecordingTempTokenDigits + suffix.size();
        if (name.size() <= tail || name.substr(name.size() - suffix.size()) != suffix) {
            continue;
        }
        const std::size_t dot = name.size() - tail;
        const std::string_view token = name.substr(dot + 1, RecordingTempTokenDigits);
        const bool hex = std::all_of(token.begin(), token.end(),
                                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        if (name[dot] == '.' && hex) {
            return std::string(name.substr(0, dot));
        }
    }
    return std::nullopt;
}

}  // namespace seabass::infrastructure::audio
