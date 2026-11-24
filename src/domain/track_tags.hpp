// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace seabass::domain
{

// What an audio file's own tags say about it: the source of a library row
// for a track no library has yet (the track import). Everything is as the
// file spells it; empty or 0 where the file says nothing. Nothing here is
// measured from the audio: a tempo or key is what somebody's software
// wrote into the tag, often rekordbox's own analysis written back.
struct TrackTags
{
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string genre;
    std::string comment;
    std::string composer;
    std::string label;
    std::string remixer;
    // As written: "8A", "Am", "A minor", "Amin" are all in use.
    std::string key;
    int year = 0;
    int trackNumber = 0;
    double bpm = 0.0;
};

// The tempo a BPM tag holds, or 0 when it holds none this can vouch for.
// Taggers write "128", "128.00" and, in some locales, "127,97"; anything
// else ("fast", "120-125", a negative or absurd number) is no tempo
// rather than the number at its front.
//
// Inline: the tag reader's own small library reads it as well as the core.
inline double bpmFromTag(std::string_view text)
{
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1);
    }

    // Digits, then at most one '.' or ',' with digits after it. Parsed
    // here rather than by strtod, which reads the decimal mark of
    // whatever locale the process runs in.
    double whole = 0.0;
    double fraction = 0.0;
    double scale = 1.0;
    std::size_t digits = 0;
    std::size_t i = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++digits) {
        whole = whole * 10.0 + (text[i] - '0');
    }
    if (digits == 0 || digits > 4) {
        return 0.0;
    }
    if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
        std::size_t decimals = 0;
        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++decimals) {
            scale /= 10.0;
            fraction += (text[i] - '0') * scale;
        }
        if (decimals == 0) {
            return 0.0;
        }
    }
    if (i != text.size()) {
        return 0.0;
    }
    const double bpm = whole + fraction;
    // What a deck can show: rekordbox analyses up to 195, a tempo display
    // ends at 999.
    return bpm >= 20.0 && bpm < 1000.0 ? bpm : 0.0;
}

}  // namespace seabass::domain
