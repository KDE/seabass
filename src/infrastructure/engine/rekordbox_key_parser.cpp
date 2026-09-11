// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/rekordbox_key_parser.hpp"

#include <array>
#include <cctype>

namespace seabass::infrastructure::engine
{

namespace
{

using djinterop::musical_key;

// Indexed by pitch class (0=C .. 11=B), the one enum entry that
// represents each pitch class in that mode -- libdjinterop's enum picks
// a single enharmonic spelling per pitch class/mode combination (e.g.
// f_sharp_major for pitch class 6, no separate g_flat_major), so both
// "F#" and "Gb" need to resolve to the same slot here.
constexpr std::array<musical_key, 12> majorByPitchClass{
    musical_key::c_major,       musical_key::d_flat_major, musical_key::d_major,
    musical_key::e_flat_major,  musical_key::e_major,      musical_key::f_major,
    musical_key::f_sharp_major, musical_key::g_major,      musical_key::a_flat_major,
    musical_key::a_major,       musical_key::b_flat_major, musical_key::b_major,
};

constexpr std::array<musical_key, 12> minorByPitchClass{
    musical_key::c_minor,       musical_key::d_flat_minor, musical_key::d_minor,
    musical_key::e_flat_minor,  musical_key::e_minor,      musical_key::f_minor,
    musical_key::f_sharp_minor, musical_key::g_minor,      musical_key::a_flat_minor,
    musical_key::a_minor,       musical_key::b_flat_minor, musical_key::b_minor,
};

std::optional<int> baseSemitone(char note)
{
    switch (note) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: return std::nullopt;
    }
}

}  // namespace

std::optional<djinterop::musical_key> parseRekordboxKey(const std::string &key)
{
    // Trim surrounding whitespace.
    size_t start = key.find_first_not_of(" \t");
    size_t end = key.find_last_not_of(" \t");
    if (start == std::string::npos) {
        return std::nullopt;
    }
    std::string s = key.substr(start, end - start + 1);
    if (s.empty()) {
        return std::nullopt;
    }

    char note = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    auto semitone = baseSemitone(note);
    if (!semitone) {
        return std::nullopt;
    }
    int pitchClass = *semitone;
    size_t pos = 1;

    // Accidental: ASCII '#'/'b' or the 3-byte UTF-8 sequences for '♯'/'♭'.
    if (pos < s.size() && s[pos] == '#') {
        pitchClass += 1;
        pos += 1;
    } else if (pos + 2 < s.size() && s.compare(pos, 3, "\xE2\x99\xAF") == 0) {  // ♯
        pitchClass += 1;
        pos += 3;
    } else if (pos < s.size() && s[pos] == 'b') {
        pitchClass -= 1;
        pos += 1;
    } else if (pos + 2 < s.size() && s.compare(pos, 3, "\xE2\x99\xAD") == 0) {  // ♭
        pitchClass -= 1;
        pos += 3;
    }
    pitchClass = ((pitchClass % 12) + 12) % 12;

    std::string rest = s.substr(pos);
    for (char &c : rest) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    bool isMinor;
    if (rest.empty() || rest == "maj" || rest == "major") {
        isMinor = false;
    } else if (rest == "m" || rest == "min" || rest == "minor") {
        isMinor = true;
    } else {
        return std::nullopt;  // unrecognized suffix (e.g. Camelot notation) -- don't guess
    }

    return isMinor ? minorByPitchClass[static_cast<size_t>(pitchClass)]
                    : majorByPitchClass[static_cast<size_t>(pitchClass)];
}

}  // namespace seabass::infrastructure::engine
