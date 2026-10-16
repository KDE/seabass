// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// A row's MouseArea that can also drag the row's track to a playlist
// (TrackDragItem): press and move, and the page's drag card follows the
// pointer. Everything else a row does on a click, a right click or a
// press and hold is the row's own, declared where it is used; a press
// that moves no further than the drag threshold is still a click, and one
// that drags is neither a click nor a press and hold.
//
// Without a drag card (trackDrag null, playlist editing off) or for a
// track with no file, this is a plain MouseArea and the list under it
// scrolls by dragging as before.
MouseArea {
    id: area

    // The page's TrackDragItem, or null where nothing drags.
    property var trackDrag: null
    // The row's track: a Browse delegate, or a Matching result's map,
    // anything with the drag contract's fields.
    property var track: null
    readonly property bool draggable: area.trackDrag !== null && area.track !== null && area.trackDrag.canDrag(area.track)

    drag.target: area.draggable ? area.trackDrag : null
    // The card under the pointer from the start, where the drop targets
    // look for it, rather than trailing it by the drag threshold.
    drag.smoothed: false
    // A press that may become a drag is not the list's to take for a
    // flick: it would steal the grab half way to the sidebar. The list
    // still scrolls with the wheel and its scroll bar.
    preventStealing: area.draggable

    onPressed: mouse => {
        if (area.draggable) {
            area.trackDrag.arm(area.track, area, mouse.x, mouse.y);
        }
    }
    drag.onActiveChanged: {
        if (!area.trackDrag) {
            return;
        }
        if (area.drag.active) {
            area.trackDrag.start();
        } else {
            area.trackDrag.release();
        }
    }
    // The grab taken away mid drag (a window losing the pointer): no drop.
    onCanceled: {
        if (area.trackDrag && area.trackDrag.dragging) {
            area.trackDrag.close();
        }
    }
}
