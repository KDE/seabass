// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// Whether a page header should fold away its descriptive part because
// the list under it is being scrolled, and how far along that fold is.
//
// The page decides what folds: it binds the part's height to
// `1 - progress` and passes its full height in as `collapsibleHeight`.
// This only decides when.
//
// A state with two different edges, not a function of the scroll
// position, because the list's own height is part of the loop: folding
// the header makes the list taller, and a taller list can scroll less
// far. A header bound straight to contentY would fold, let the list
// clamp contentY back down, unfold, and do it again. So:
//
//  - it folds once the list is scrolled past `threshold`, and only if
//    the list would still be scrolled past it with the header folded,
//    so the clamp cannot take it back under;
//  - it unfolds only when the list is back at its very top.
//
// Between those two edges nothing changes, whatever scrolled there: the
// wheel, the keyboard, a drag or the scroll bar all move contentY, and
// contentY is the only thing read.
Item {
    id: root
    visible: false

    required property Flickable flickable
    // How much taller the list gets when the header folds, or more.
    property real collapsibleHeight: 0
    // How far the list has to be scrolled before the header folds.
    property real threshold: Theme.pageMargin * 2

    // Set from the scroll position below, never by the page.
    property bool collapsed: false
    // 0 unfolded, 1 folded, eased in between.
    property real progress: root.collapsed ? 1 : 0
    Behavior on progress {
        NumberAnimation {
            // Folding away is leaving, unfolding is arriving.
            duration: root.collapsed ? Theme.departureTransitionDuration : Theme.arrivalTransitionDuration
            easing.type: Easing.InOutQuad
        }
    }

    function update() {
        const f = root.flickable;
        if (!f) {
            return;
        }
        if (root.collapsed) {
            if (f.atYBeginning) {
                root.collapsed = false;
            }
            return;
        }
        const scrolled = f.contentY - f.originY + f.topMargin;
        // Measured against the list as it stands now: while it is still
        // unfolding it is taller than it will be, which only makes this
        // answer no more often.
        const reachWhenFolded = f.contentHeight + f.topMargin + f.bottomMargin - f.height - root.collapsibleHeight;
        if (scrolled > root.threshold && reachWhenFolded > root.threshold) {
            root.collapsed = true;
        }
    }

    Connections {
        target: root.flickable
        function onContentYChanged() { root.update(); }
        function onAtYBeginningChanged() { root.update(); }
    }
}
