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
    // A press held this long shows the closed hand before anything moves:
    // the track can be picked up. Longer than a click, shorter than a
    // press and hold.
    property int holdCursorDelay: 150
    // Held down past that, or dragging.
    property bool held: false
    cursorShape: area.held ? Qt.ClosedHandCursor : Qt.ArrowCursor
    Timer {
        id: holdTimer
        interval: area.holdCursorDelay
        onTriggered: {
            if (area.pressed && area.draggable) {
                area.held = true;
            }
        }
    }
    onReleased: {
        holdTimer.stop();
        area.held = false;
    }

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
            holdTimer.restart();
        }
    }
    drag.onActiveChanged: {
        if (!area.trackDrag) {
            return;
        }
        if (area.drag.active) {
            area.held = true;
            area.trackDrag.start();
        } else {
            area.held = false;
            // Carried by the card since it started: the card lets go when
            // the button does, not when this area loses the grab to it.
            if (!area.trackDrag.carried) {
                area.trackDrag.release();
            }
        }
    }
    // The grab taken away mid drag (a window losing the pointer): no drop.
    // Handing it to the card (TrackDragItem.carried) is no such loss.
    onCanceled: {
        holdTimer.stop();
        area.held = false;
        if (area.trackDrag && area.trackDrag.dragging && !area.trackDrag.carried) {
            area.trackDrag.close();
        }
    }
}
