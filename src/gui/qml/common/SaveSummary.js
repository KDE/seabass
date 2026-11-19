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
