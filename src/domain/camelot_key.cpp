// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/camelot_key.hpp"

#include <array>
#include <cstddef>
#include <unordered_map>

namespace seabass::domain
{

namespace
{

std::string normalizeAccidentals(std::string key)
{
    // Unicode "♯"/"♭" (U+266F/U+266D) -> ASCII "#"/"b", byte-for-byte
    // (both are 3-byte UTF-8 sequences), so every downstream comparison
    // only has to know one spelling.
    std::string result;
    result.reserve(key.size());
    for (size_t i = 0; i < key.size();) {
        if (i + 2 < key.size() && static_cast<unsigned char>(key[i]) == 0xE2
            && static_cast<unsigned char>(key[i + 1]) == 0x99) {
            unsigned char third = static_cast<unsigned char>(key[i + 2]);
            if (third == 0xAF) {  // ♯
                result += '#';
                i += 3;
                continue;
            }
            if (third == 0xAD) {  // ♭
                result += 'b';
                i += 3;
                continue;
            }
        }
        result += key[i];
        ++i;
    }
    return result;
}

// Pitch class (0=C .. 11=B) -> Camelot number, one table per mode --
// derived from the wheel's own relative-major/minor pairing (e.g.
// Camelot 8 is A minor *and* C major), same tables Theme.qml's own
// parseCamelotKey() uses.
constexpr std::array<int, 12> CamelotMinorByPitchClass = {5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10};
constexpr std::array<int, 12> CamelotMajorByPitchClass = {8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};

const std::unordered_map<std::string, int> &pitchClassByName()
{
    static const std::unordered_map<std::string, int> table = {
        {"C", 0}, {"B#", 0}, {"C#", 1}, {"Db", 1}, {"D", 2}, {"D#", 3}, {"Eb", 3}, {"E", 4}, {"Fb", 4},
        {"F", 5}, {"E#", 5}, {"F#", 6}, {"Gb", 6}, {"G", 7}, {"G#", 8}, {"Ab", 8}, {"A", 9},
        {"A#", 10}, {"Bb", 10}, {"B", 11}, {"Cb", 11},
    };
    return table;
}

}  // namespace

std::optional<CamelotKey> CamelotKey::parse(const std::string &key)
{
    if (key.empty()) {
        return std::nullopt;
    }
    std::string normalized = normalizeAccidentals(key);

    // Camelot notation itself (e.g. "10A", "3B") -- some catalogs store
    // the key field this way already rather than as a note name at all.
    // A leading digit is never a valid note-name start, so this can't
    // collide with the musical-notation branch below.
    if (normalized.size() >= 2 && normalized.size() <= 3) {
        char last = normalized.back();
        if (last == 'A' || last == 'B') {
            std::string digits = normalized.substr(0, normalized.size() - 1);
            bool allDigits = !digits.empty();
            for (char c : digits) {
                if (c < '0' || c > '9') {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                int number = std::stoi(digits);
                if (number >= 1 && number <= 12) {
                    return CamelotKey{number, last == 'A'};
                }
            }
        }
    }

    bool isMinor = normalized.size() > 1 && normalized.back() == 'm';
    std::string notePart = isMinor ? normalized.substr(0, normalized.size() - 1) : normalized;
    auto it = pitchClassByName().find(notePart);
    if (it == pitchClassByName().end()) {
        return std::nullopt;
    }
    int number = isMinor ? CamelotMinorByPitchClass[static_cast<size_t>(it->second)]
                          : CamelotMajorByPitchClass[static_cast<size_t>(it->second)];
    return CamelotKey{number, isMinor};
}

KeyRelation classifyKeyRelation(const std::string &keyA, const std::string &keyB)
{
    auto a = CamelotKey::parse(keyA);
    auto b = CamelotKey::parse(keyB);
    if (!a || !b) {
        return KeyRelation::Unknown;
    }
    if (*a == *b) {
        return KeyRelation::Same;
    }
    if (a->number == b->number) {
        return KeyRelation::Relative;
    }
    int diff = a->number > b->number ? a->number - b->number : b->number - a->number;
    int wheelDistance = diff < 12 - diff ? diff : 12 - diff;
    if (wheelDistance == 1) {
        if (a->isMinor != b->isMinor) {
            return KeyRelation::EnergyMix;
        }
        // Clockwise from A to B (numbers wrap 12 -> 1) is the "up"/boost
        // direction; the other way around that same single step is "down".
        bool up = b->number == (a->number % 12) + 1;
        return up ? KeyRelation::AdjacentUp : KeyRelation::AdjacentDown;
    }
    return KeyRelation::Unrelated;
}

std::string keyRelationLabel(KeyRelation relation)
{
    switch (relation) {
    case KeyRelation::Same:
        return "Same key";
    case KeyRelation::Relative:
        return "Relative major/minor";
    case KeyRelation::AdjacentUp:
        return "Energy Boost";
    case KeyRelation::AdjacentDown:
        return "Energy Drop";
    case KeyRelation::EnergyMix:
        return "Energy mix";
    case KeyRelation::Unrelated:
        return "Unrelated key";
    case KeyRelation::Unknown:
    default:
        return "";
    }
}

bool keyRelationMatchesAnyMode(KeyRelation relation, const std::vector<std::string> &keyModes)
{
    if (keyModes.empty()) {
        return true;
    }
    if (relation == KeyRelation::Unknown) {
        return false;
    }
    for (const auto &mode : keyModes) {
        if (mode == "match" && relation == KeyRelation::Same) {
            return true;
        }
        if (mode == "relative" && relation == KeyRelation::Relative) {
            return true;
        }
        if (mode == "harmonic" && (relation == KeyRelation::AdjacentUp || relation == KeyRelation::AdjacentDown)) {
            return true;
        }
        if (mode == "energymix" && relation == KeyRelation::EnergyMix) {
            return true;
        }
    }
    return false;
}

}  // namespace seabass::domain
