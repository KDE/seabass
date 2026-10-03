// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// An Engine track with hot cues or saved loops the player does not show.
//
// Engine OS hides a pad whose colour has alpha 0 (measured on a Prime 4,
// 2026-10-02: two loops synced without a colour sat in the row byte for
// byte like a loop the player had saved itself, bar the colour, and the
// Saved Loops bank showed the player's and not ours). Until 05d71bbd
// the Engine cue writer gave every cue that brought no colour exactly
// that, so a stick synced by an earlier build carries pads a DJ reads
// as lost. The writer is fixed; this finds what it left, and the repair
// writes the same cues again, each with Engine's default for its pad.
//
// The Engine reader turns alpha 0 into no colour at all (see colorHex
// in libdjinterop_engine_reader.cpp), and nothing else does: a pad with
// any colour reads back as "#RRGGBB". So "no colour" on an Engine track
// is the whole test.
struct HiddenEngineCues
{
    Track track;
    int hotCues = 0;
    int loops = 0;
    int hidden() const { return hotCues + loops; }
};

class HiddenEngineCueFinder
{
public:
    // Only tracks whose format is "engine" are looked at; the rest are
    // skipped, not reported.
    static std::vector<HiddenEngineCues> find(const std::vector<Track> &tracks);
};

}  // namespace seabass::domain
