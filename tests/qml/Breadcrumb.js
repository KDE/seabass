// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What a page's BackBreadcrumb actually shows, read back from the
// segments on screen rather than from the properties the page set on it:
// a property can be right while the segment it feeds is hidden, and the
// page tests are here to say what the reader sees.
//
//     import "Breadcrumb.js" as Breadcrumb
//     const crumb = Breadcrumb.read(page);
//     compare(crumb.stick, "TESTSTICK");
//     compare(crumb.middle, "Housekeeping");
//     verify(crumb.middleIsLink);
//
// A segment that is not shown reads as "". The page must sit in a real
// StackView for middleIsLink to mean anything: the breadcrumb decides
// from the stack's depth whether the middle segment leads anywhere but
// Home.
.pragma library

function visibleNamed(item, name) {
    if (item.objectName === name && item.visible) {
        return item;
    }
    for (let i = 0; i < item.children.length; ++i) {
        const found = visibleNamed(item.children[i], name);
        if (found) {
            return found;
        }
    }
    return null;
}

function read(page) {
    const stick = visibleNamed(page, "stickSegment");
    const link = visibleNamed(page, "middleLink");
    const context = visibleNamed(page, "middleSegment");
    const title = visibleNamed(page, "titleSegment");
    return {
        stick: stick ? stick.text : "",
        middle: link ? link.text : context ? context.text : "",
        middleIsLink: link !== null,
        title: title ? title.text : "",
    };
}
