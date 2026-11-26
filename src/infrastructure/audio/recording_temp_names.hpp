// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The names of the temporary files beside a kept FLAC:
//
//     "<FLAC's name>.<16 hex digits><suffix>"
//
// A keep job writes one only: the encoder's, which is tagged in place and
// then given the FLAC's name. The tag writer's copy is a suffix of earlier
// builds, which tagged a copy of the placed FLAC and renamed it over the
// FLAC; their leftovers are still recognised, so that the sweep of
// leftovers finds them (LocalKeepRecordingFiles::sweepLeftovers()).
//
// The suffix comes last and is no audio extension, so a music player that
// indexes the folder does not take a leftover for a track. Header only:
// the sweep of leftovers (LocalKeepRecordingFiles) needs the names without
// the encoder's libraries.
namespace seabass::infrastructure::audio
{

// The encoder's file until it is placed, and an earlier build's tag copy.
inline constexpr const char *RecordingEncodeTempSuffix = ".seabass-encode-tmp";
inline constexpr const char *RecordingTagTempSuffix = ".seabass-tag-tmp";
inline constexpr std::size_t RecordingTempTokenDigits = 16;

// The suffixes, for the sweep that walks them all.
inline constexpr const char *RecordingTempSuffixes[] = {RecordingEncodeTempSuffix, RecordingTagTempSuffix};

// How many bytes the temporary name a keep job writes adds to a FLAC's
// name: the encoder's, the only one it writes.
inline constexpr std::size_t recordingTempNameExtraBytes()
{
    return 1 + RecordingTempTokenDigits + std::string_view(RecordingEncodeTempSuffix).size();
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
