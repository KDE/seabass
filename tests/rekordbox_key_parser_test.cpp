// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "infrastructure/engine/rekordbox_key_parser.hpp"

using namespace seabass::infrastructure::engine;
using djinterop::musical_key;

int main()
{
    // Case 1: plain major, no accidental.
    {
        auto key = parseRekordboxKey("C");
        assert(key.has_value() && *key == musical_key::c_major);
        std::cout << "case 1 (plain major) OK\n";
    }

    // Case 2: minor with 'm' suffix.
    {
        auto key = parseRekordboxKey("Am");
        assert(key.has_value() && *key == musical_key::a_minor);
        std::cout << "case 2 (minor suffix) OK\n";
    }

    // Case 3: ASCII sharp accidental.
    {
        auto key = parseRekordboxKey("F#m");
        assert(key.has_value() && *key == musical_key::f_sharp_minor);
        std::cout << "case 3 (ASCII sharp) OK\n";
    }

    // Case 4: ASCII flat, and enharmonic equivalence with the sharp
    // spelling above -- Gbm and F#m are the same pitch class/mode.
    {
        auto key = parseRekordboxKey("Gbm");
        assert(key.has_value() && *key == musical_key::f_sharp_minor);
        std::cout << "case 4 (ASCII flat, enharmonic equivalence) OK\n";
    }

    // Case 5: Unicode sharp/flat accidentals.
    {
        auto sharp = parseRekordboxKey("F\xE2\x99\xAFm");  // F♯m
        auto flat = parseRekordboxKey("E\xE2\x99\xAD");    // E♭
        assert(sharp.has_value() && *sharp == musical_key::f_sharp_minor);
        assert(flat.has_value() && *flat == musical_key::e_flat_major);
        std::cout << "case 5 (Unicode accidentals) OK\n";
    }

    // Case 6: lowercase note letter still parses (mode suffix is
    // case-normalized, but the note letter itself is too).
    {
        auto key = parseRekordboxKey("am");
        assert(key.has_value() && *key == musical_key::a_minor);
        std::cout << "case 6 (lowercase note letter) OK\n";
    }

    // Case 7: unrecognized input (Camelot notation, garbage) -> nullopt,
    // never a guessed wrong answer.
    {
        assert(!parseRekordboxKey("8A").has_value());
        assert(!parseRekordboxKey("").has_value());
        assert(!parseRekordboxKey("H").has_value());
        assert(!parseRekordboxKey("Cxyz").has_value());
        std::cout << "case 7 (unrecognized input -> nullopt) OK\n";
    }

    // Case 8: every one of the 12 pitch classes round-trips for both
    // modes via at least one common spelling.
    {
        assert(parseRekordboxKey("C").value() == musical_key::c_major);
        assert(parseRekordboxKey("C#").value() == musical_key::d_flat_major);
        assert(parseRekordboxKey("D").value() == musical_key::d_major);
        assert(parseRekordboxKey("D#").value() == musical_key::e_flat_major);
        assert(parseRekordboxKey("E").value() == musical_key::e_major);
        assert(parseRekordboxKey("F").value() == musical_key::f_major);
        assert(parseRekordboxKey("F#").value() == musical_key::f_sharp_major);
        assert(parseRekordboxKey("G").value() == musical_key::g_major);
        assert(parseRekordboxKey("G#").value() == musical_key::a_flat_major);
        assert(parseRekordboxKey("A").value() == musical_key::a_major);
        assert(parseRekordboxKey("A#").value() == musical_key::b_flat_major);
        assert(parseRekordboxKey("B").value() == musical_key::b_major);
        std::cout << "case 8 (all 12 major pitch classes) OK\n";
    }

    std::cout << "All rekordbox_key_parser tests passed.\n";
    return 0;
}
