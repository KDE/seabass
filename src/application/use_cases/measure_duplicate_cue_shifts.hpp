// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/duplicate_cue_shift.hpp"

namespace seabass::application
{

// Bar units planned for lining copies up, before anyone knows how many
// pairs there will be: the duplicates are only found after the read that
// the bar is counting. As many units as the read itself, and never fewer
// than a hundred, so a scan whose catalogs were already cached (and
// announce nothing to read) still shows this stretch moving. A guess at
// the share, and only ever a share: the bar moves forward through it in
// step with the pairs measured, whatever their number.
inline size_t cueShiftProgressUnits(size_t readUnits)
{
    return std::max<size_t>(readUnits, 100);
}

// domain::measureCueShifts() as one stretch of a scan's single bar
// (#58): announced as `units` units, ticked in proportion as each pair
// that costs a decode is measured. Pairs the probe answers at once (its
// cache on the stick) move the bar not at all, so a warm cache crosses
// the stretch at once rather than spreading a long wait over nothing. Cancelling reaches the decoding through the probe (it stops
// answering, see gui/cancellable_audio_probe.hpp), and this then throws
// OperationCancelled so the scan ends as cancelled rather than reporting
// copies it never measured as not lining up.
inline void measureDuplicateCueShifts(std::vector<domain::DuplicateGroup> &groups,
                                      domain::AudioAlignmentProbe *probe, const domain::CueTimelineOffset &offset,
                                      ProgressReporter &progress, size_t units, const CancellationToken &cancel)
{
    const size_t pairs = domain::cueShiftPairsToMeasure(groups, probe);
    progress.start("Lining up the copies' audio...", units);
    if (pairs == 0) {
        progress.tick(units);
    }
    size_t done = 0;
    domain::measureCueShifts(groups, probe, offset, [&](bool costly) {
        if (costly && pairs > 0) {
            ++done;
            // A reference that could not be read makes more pairs than
            // were planned; the bar waits at the end of its stretch.
            progress.tick(std::min(units, done * units / pairs));
        }
    });
    progress.finish();
    cancel.throwIfCancelled();
}

}  // namespace seabass::application
