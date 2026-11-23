// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "domain/grid_analyser.hpp"

namespace seabass::domain
{

// Seabass's own beat grid analyser (docs/track-analysis-plan.md, phase 3).
//
// What it listens to: the audio from 30 Hz to 5 kHz in five bands, the
// envelope of each, and of the envelopes only the rises. Summed, they are
// the onset function, one value a millisecond. The band above 5 kHz is
// left out on purpose: on dance music the hats in it sit between the
// beats, and an onset function that hears them puts the grid there.
//
// The tempo: the lag at which the onset function repeats, scored together
// with its multiples and a mild preference for tempos near 128 BPM, then
// sharpened on the repeats 4, 16 and 64 beats away, each of which says
// the period that many times finer.
//
// The place of the beat: the onset function folded onto one beat over the
// whole track peaks there. Then the track is cut into windows of eight
// beats and each window says how far its own peak is from that place; a
// line through those says how much the period is off and by how much the
// place, and the two are corrected. Windows without a peak near the place
// (a breakdown, a fill) say nothing.
//
// No whole-number rule: the tempo is what was measured.
//
// What it does not do yet: follow a tempo that moves (the grid is one
// straight line), say which beat starts the bar (beatInBar is 0), or
// refine the beat on the samples of the transient.
class SeabassGridAnalyser : public GridAnalyser
{
public:
    std::string name() const override { return "seabass"; }
    GridAnalysis analyse(PcmSource &source) override;
};

}  // namespace seabass::domain
