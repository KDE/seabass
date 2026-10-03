// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <string>

#include "application/ports/progress_reporter.hpp"

namespace seabass::application
{

// One operation, one bar (#58).
//
// The readers, the audits and the use cases each announce their own
// stretch of work with start(label, n) and tick through it, and a scan
// is a dozen of those in a row. Shown one after another they make a bar
// that fills, empties and fills again, which reads as the work starting
// over, or as the first bar having lied. This folds them: the caller
// counts the whole operation first (the catalogs' track counts are a
// page-header sum or a count(*), and LibraryCatalogCache::plannedUnits()
// says what a read will still announce), announces that total ONCE on
// the real reporter, and hands this to everything below. Each stretch
// a producer announces lands end to end on the one bar, its label going
// out as phase(), its ticks as absolute positions, and the bar only ever
// moves forward.
//
// A stretch announced with no count (start(label, 0)) is one unit of
// the bar, which stays put until the stretch ends. A stretch that runs
// past its own count is held at it, and the bar is held at the planned
// total: a plan that is short by a few units costs a bar that sits at
// the end a moment longer, which is nothing next to one that restarts.
//
// finish() on this ends the current stretch and nothing else; the real
// reporter's finish() goes out from done() or the destructor, once.
class PhasedProgress : public ProgressReporter
{
public:
    // Announces `label` with `plannedTotal` on `underlying` now. A planned
    // total of 0 means the operation could not be counted (a catalog
    // that would not open, or a test cache that has no count): nothing
    // is announced here, and each stretch is then passed through as its
    // own bar, which is what every operation showed before #58 and is
    // still better than a sweeping bar that names nothing.
    //
    // `startAt`, `announce` and `endsOperation` are for an operation run
    // as several tasks in a row that share one bar (Library Health's
    // legs, one per catalog): the first announces the whole total, the
    // ones after it do not announce and begin at the offset the legs
    // before them reached, which they tick at once so the bar never
    // dips, and only the last runs the bar to its end when done.
    PhasedProgress(ProgressReporter &underlying, const std::string &label, size_t plannedTotal, size_t startAt = 0,
                   bool announce = true, bool endsOperation = true);
    ~PhasedProgress() override;
    PhasedProgress(const PhasedProgress &) = delete;
    PhasedProgress &operator=(const PhasedProgress &) = delete;

    void start(const std::string &label, size_t total) override;
    void tick(size_t current) override;
    void finish() override;
    void warn(const std::string &message) override;
    void phase(const std::string &label) override;

    // Ends this reporter's part: the current stretch ends, the bar goes
    // to its planned total when this part ends the operation, and the
    // underlying reporter's finish() goes out. Idempotent.
    void done();

    size_t plannedTotal() const { return m_plannedTotal; }
    // Units the stretches so far have covered, the current one included.
    size_t position() const { return m_position; }

private:
    void endStretch();
    void report(size_t position);

    ProgressReporter &m_underlying;
    size_t m_plannedTotal = 0;
    size_t m_offset = 0;        // where the current stretch begins
    size_t m_stretchTotal = 0;  // its own count; 0 when it has none
    bool m_inStretch = false;
    size_t m_position = 0;
    size_t m_reported = 0;
    bool m_done = false;
    bool m_endsOperation = true;
};

}  // namespace seabass::application
