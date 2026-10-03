// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PhasedProgress (#58): several announced stretches become one bar that
// is announced once, labelled per stretch, and only ever moves forward.

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "application/phased_progress.hpp"

using namespace seabass::application;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

struct Recording : ProgressReporter
{
    std::vector<std::pair<std::string, size_t>> starts;
    std::vector<size_t> ticks;
    std::vector<std::string> phases;
    std::vector<std::string> warnings;
    int finishes = 0;
    void start(const std::string &label, size_t total) override { starts.emplace_back(label, total); }
    void tick(size_t current) override { ticks.push_back(current); }
    void finish() override { ++finishes; }
    void warn(const std::string &message) override { warnings.push_back(message); }
    void phase(const std::string &label) override { phases.push_back(label); }
};

bool forwardOnly(const std::vector<size_t> &ticks)
{
    for (size_t i = 1; i < ticks.size(); ++i) {
        if (ticks[i] < ticks[i - 1]) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main()
{
    // Three stretches, counted 3, 2 and none, on a bar planned at 6: one
    // announcement, three phases, positions 0..6 forward only, one finish.
    {
        Recording r;
        {
            PhasedProgress fold(r, "Checking the stick", 6);
            fold.start("Reading tracks", 3);
            fold.tick(1);
            fold.tick(2);
            fold.tick(3);
            fold.start("Reading cues", 2);
            fold.tick(1);
            fold.tick(2);
            fold.start("Committing", 0);
            fold.tick(7);  // nothing: no count
            fold.done();
        }
        check(r.starts.size() == 1 && r.starts[0].first == "Checking the stick" && r.starts[0].second == 6,
              "announced once with the plan");
        check(r.phases == std::vector<std::string>{"Reading tracks", "Reading cues", "Committing"}, "each stretch named");
        check(forwardOnly(r.ticks), "forward only");
        check(!r.ticks.empty() && r.ticks.back() == 6, "ends at the plan");
        check(r.ticks == std::vector<size_t>{1, 2, 3, 4, 5, 6}, "stretches land end to end");
        check(r.finishes == 1, "one finish");
    }
    // A stretch that ends short of its count still moves the bar its
    // whole count, so the next stretch begins where the plan put it.
    {
        Recording r;
        PhasedProgress fold(r, "op", 10);
        fold.start("a", 5);
        fold.tick(2);
        fold.start("b", 5);
        check(r.ticks.back() == 5, "a stretch ending early is counted whole");
        fold.tick(5);
        check(r.ticks.back() == 10, "and the next lands after it");
    }
    // A plan that is short: the bar holds at the end, never past it, and
    // never backs up.
    {
        Recording r;
        PhasedProgress fold(r, "op", 4);
        fold.start("a", 3);
        fold.tick(3);
        fold.start("b", 3);
        fold.tick(1);
        fold.tick(3);
        fold.done();
        check(forwardOnly(r.ticks) && r.ticks.back() == 4, "held at the planned total");
        for (size_t t : r.ticks) {
            check(t <= 4, "never past the plan");
        }
        check(r.finishes == 1, "finished once");
    }
    // A stretch that runs past its own count is held at it.
    {
        Recording r;
        PhasedProgress fold(r, "op", 10);
        fold.start("a", 3);
        fold.tick(9);
        check(r.ticks.back() == 3, "a stretch is held at its count");
        fold.start("b", 7);
        check(r.ticks.back() == 3, "the next begins after it");
    }
    // A producer's own finish() ends its stretch and nothing more; the
    // destructor finishes the real reporter once.
    {
        Recording r;
        {
            PhasedProgress fold(r, "op", 2);
            fold.start("a", 1);
            fold.tick(1);
            fold.finish();
            check(r.finishes == 0, "a stretch's finish is not the operation's");
            fold.start("b", 1);
            fold.tick(1);
        }
        check(r.finishes == 1 && r.ticks.back() == 2, "the destructor ends the operation, once");
    }
    // No plan: nothing is announced for the operation, and each stretch
    // passes through as its own bar, as before #58.
    {
        Recording r;
        PhasedProgress fold(r, "op", 0);
        fold.start("a", 5);
        fold.tick(3);
        fold.start("b", 0);
        fold.done();
        check(r.starts == std::vector<std::pair<std::string, size_t>>{{"a", 5}, {"b", 0}}, "each stretch is its own bar");
        check(r.ticks == std::vector<size_t>{3}, "ticks pass through");
        check(r.phases.empty(), "no phases: the bars carry their labels");
        check(r.finishes == 1, "and it finishes once");
    }
    // Legs of one bar: the second does not announce, begins where the
    // first stopped, and the bar never dips between them.
    {
        Recording r;
        {
            PhasedProgress first(r, "op", 10, 0, true, /*endsOperation=*/false);
            first.start("a", 4);
            first.tick(4);
            first.done();
        }
        {
            PhasedProgress second(r, "op", 10, 4, false, true);
            check(r.starts.size() == 1, "a later leg does not announce again");
            check(r.ticks.back() == 4, "and holds the bar where the first leg left it");
            second.start("b", 6);
            second.tick(3);
            check(r.ticks.back() == 7, "its stretches land after the first leg's");
            second.done();
        }
        check(forwardOnly(r.ticks) && r.ticks.back() == 10, "forward only, to the end");
        check(r.finishes == 2, "each leg finishes its task's reporter");
    }
    // Warnings and nested phases pass through.
    {
        Recording r;
        PhasedProgress fold(r, "op", 1);
        fold.warn("careful");
        fold.phase("inner");
        check(r.warnings == std::vector<std::string>{"careful"} && r.phases == std::vector<std::string>{"inner"},
              "warn and phase pass through");
    }

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "phased_progress_test: all cases passed\n";
    return 0;
}
