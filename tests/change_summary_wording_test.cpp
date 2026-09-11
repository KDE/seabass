// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What a save tells the user it did.
//
// Every change used to report "written", because that is what a save
// does to a file. Removing 27 stray cues therefore said "27 of 27 cues
// written" -- true of the bytes, false of the intent, and read by
// someone who had just deleted something and wanted to know what.
//
// The summary reads "<n> of <m> <unit> <verb>.", so unit and verb have
// to make an English sentence together rather than each being
// defensible alone. These build the real change objects and ask them,
// rather than restating a table the code could disagree with.
#include <cassert>
#include <iostream>
#include <string>

#include "domain/library_consistency.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/delete_orphan_change.hpp"
#include "gui/edit/changes/remove_junk_cue_change.hpp"
#include "gui/edit/changes/repair_issue_change.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"

using namespace seabass;

namespace
{
int failures = 0;

void check(const gui::PendingChange &change, const char *what, const char *unit, const char *verb)
{
    const std::string sentence =
        "3 of 3 " + change.unit().toStdString() + " " + change.verb().toStdString() + ".";
    std::cout << "  " << what << ": \"" << sentence << "\"\n";
    if (change.unit() != QString::fromUtf8(unit) || change.verb() != QString::fromUtf8(verb)) {
        std::cerr << "FAILED: " << what << " reads \"" << sentence << "\", expected \"3 of 3 " << unit << " " << verb
                  << ".\"\n";
        ++failures;
    }
    // The two rules behind every entry above, stated so a NEW change
    // class that inherits PendingChange's default is caught by them:
    // "written" describes the file operation rather than the request,
    // and "rows" is the catalog's word for what a person reads as an
    // entry.
    if (change.verb() == QLatin1String("written")) {
        std::cerr << "FAILED: " << what << " still reports \"written\"\n";
        ++failures;
    }
    if (change.unit() == QLatin1String("rows")) {
        std::cerr << "FAILED: " << what << " says \"rows\", which is the database's word\n";
        ++failures;
    }
}
}  // namespace

int main()
{
    {
        domain::Track track;
        track.sourceId = "1";
        track.format = "rekordbox";
        gui::RemoveJunkCueChange change(QStringLiteral("/stick/PIONEER"), track);
        // The one that prompted this: a person pressed "remove these
        // stray cues" and was told cues had been written.
        check(change, "stray cue removal", "cues", "removed");
    }
    {
        domain::LibraryConsistencyIssue issue;
        gui::DeleteOrphanChange change(QStringLiteral("/stick/PIONEER"), issue);
        check(change, "delete orphaned entry", "entries", "removed");
    }
    {
        domain::LibraryConsistencyIssue issue;
        gui::RepairIssueChange change(QStringLiteral("/stick/PIONEER"), issue, 3);
        check(change, "repair", "entries", "repaired");
    }
    {
        domain::SyncPlan plan;
        gui::SyncPlanChange change(QStringLiteral("/stick/PIONEER"), QStringLiteral("/stick/Engine Library"), plan, 3);
        check(change, "sync", "tracks", "synchronised");
    }

    if (failures > 0) {
        std::cerr << failures << " wording check(s) failed\n";
        return 1;
    }
    std::cout << "all cases passed\n";
    return 0;
}
