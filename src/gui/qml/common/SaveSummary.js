// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What every page says about a finished save of its edit session
// (LibraryEditSession::saveFinished), where the words must not differ
// from one page to the next. Kept here once: the toasts of the pages that
// show their own saves and OperationSummaryDialog both read it.
.pragma library

// The save was Undo Last Save. The session says so in the summary
// (undo: true), so no page reads it from the unit's English.
function isUndo(summary) {
    return summary !== null && summary !== undefined && summary.undo === true;
}

// An undo that landed whole, on every page: no count. What the undo
// restored is backup records, a number that means nothing to the user
// (#98: it read "1 undo steps written"). An undo that failed or stopped
// part-way is reported as any save is.
function undoneText() {
    return "Changes undone.";
}

// What a page's toast says for a save that landed whole, in the summary's
// own words: "6 settings saved", "1 track restored", "3 tracks repaired,
// 2 unused cover rows removed", or the undo's. The
// unit is plural and one of them reads as one, by the rule
// OperationSummaryDialog uses.
function wholeSaveText(summary) {
    if (isUndo(summary)) {
        return undoneText();
    }
    let unit = summary.unit;
    if (summary.written === 1) {
        unit = unit.endsWith("ies") ? unit.substring(0, unit.length - 3) + "y"
            : unit.endsWith("s") ? unit.substring(0, unit.length - 1) : unit;
    }
    // Informational clauses (SaveContext::addSummaryInfo) after the count.
    const clauses = summary.infoClauses ? Array.from(summary.infoClauses) : [];
    return [summary.written + " " + unit + " " + summary.verb].concat(clauses).join(", ");
}
