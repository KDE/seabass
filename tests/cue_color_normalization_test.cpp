// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include <djinterop/pad_color.hpp>

#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

using namespace seabass;

// Regression coverage for a real bug: an uncolored rekordbox hot cue and
// an uncolored Engine hot cue used to come out as two different strings
// ("0" vs "#000000"), so domain::cueSetsEqual() -- which does compare hot
// cue color -- treated every uncolored-cue track pair as a genuine
// conflict. Confirmed on real data this session: this alone accounted for
// every single one of 11 "genuine" cross-source sync conflicts reported
// by CrossSourceConflictDetector, none of which were real disagreements.
int main()
{
    // Rekordbox: no RGB present, legacy color_id == 0 -> no color at all.
    {
        std::string color = infrastructure::rekordbox::rekordboxCueColor(false, 0, 0, 0, 0);
        assert(color.empty());
        std::cout << "case 1 (rekordbox: no RGB, color_id 0 -> no color) OK\n";
    }

    // Rekordbox: no RGB present, but a genuine non-zero legacy color_id --
    // still no verified color_id -> RGB mapping (see docs), so this stays
    // a bare numeric fallback, not silently dropped like color_id 0 is.
    {
        std::string color = infrastructure::rekordbox::rekordboxCueColor(false, 0, 0, 0, 3);
        assert(color == "3");
        std::cout << "case 2 (rekordbox: no RGB, non-zero color_id -> numeric fallback preserved) OK\n";
    }

    // Rekordbox: RGB present -> real hex color, color_id irrelevant.
    {
        std::string color = infrastructure::rekordbox::rekordboxCueColor(true, 0xFF, 0x00, 0x17, 0);
        assert(color == "#FF0017");
        std::cout << "case 3 (rekordbox: RGB present -> real hex color) OK\n";
    }

    // Engine: default-constructed pad_color (alpha == 0) -> no color at
    // all, not the misleading "#000000" it used to produce.
    {
        std::string color = infrastructure::engine::colorHex(djinterop::pad_color{});
        assert(color.empty());
        std::cout << "case 4 (engine: default pad_color, alpha 0 -> no color) OK\n";
    }

    // Engine: a genuinely set color (alpha != 0) -> real hex color.
    {
        std::string color = infrastructure::engine::colorHex(djinterop::pad_color{0xFF, 0x00, 0x17, 0xFF});
        assert(color == "#FF0017");
        std::cout << "case 5 (engine: real color, alpha nonzero -> real hex color) OK\n";
    }

    // The actual bug: before the fix, an uncolored cue from each format
    // produced two different strings ("0" and "#000000"). Now both
    // normalize to the same "no color" sentinel, so a downstream
    // string-equality comparison (like domain::cueSetsEqual's hot cue
    // color check) correctly treats them as equal.
    {
        std::string rbColor = infrastructure::rekordbox::rekordboxCueColor(false, 0, 0, 0, 0);
        std::string engineColor = infrastructure::engine::colorHex(djinterop::pad_color{});
        assert(rbColor == engineColor);
        std::cout << "case 6 (uncolored rekordbox and engine cues normalize to the same value) OK\n";
    }

    std::cout << "All cue_color_normalization_test cases passed.\n";
    return 0;
}
