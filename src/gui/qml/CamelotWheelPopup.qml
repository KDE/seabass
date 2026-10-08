// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Shapes
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A visual Camelot wheel, opened by clicking any KeyBadge: a pure
// reference, not an input -- it shows how every position relates to the
// key it was opened with (outer ring = major/B, inner ring = minor/A,
// matching the real wheel's own layout), Same/Relative/Adjacent/Energy
// mix, color-coded and spelled out on hover, with unrelated keys faded
// so the related ones stand out. Filtering the candidate list is the Add
// or Move Track panel's own key-tier row's job, not this popup's; the
// wheel only ever hovers (see keyHovered/relationHovered below), never
// picks.
PanelPopup {
    id: root
    parent: Overlay.overlay
    // Positioned imperatively (see resetPosition(), called from
    // openAt()) rather than with a live x/y binding -- a binding would
    // fight with dragging every time something it depends on re-evaluates.
    modal: true
    focus: true
    width: 400
    height: 500
    // Still modal (captures input, closes on an outside click) but
    // without the default dim-the-whole-window scrim: the rest of the
    // app -- in particular a candidate list doing its own hover-fade
    // highlighting behind this -- needs to stay at its own true opacity,
    // not additionally darkened by the popup's own overlay on top of it.
    Overlay.modal: Rectangle { color: "transparent" }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140; easing.type: Easing.OutQuad }
        NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: 220; easing.type: Easing.OutBack }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 100; easing.type: Easing.InQuad }
    }

    // Falls back to centered when nothing's been dragged yet, or the
    // window's a different size than whatever it was dragged at (an old
    // pixel position could be off-screen, or just not make sense, once
    // the window's been resized).
    function resetPosition() {
        if (CamelotWheelPosition.x >= 0 && CamelotWheelPosition.forWidth === root.parent.width
            && CamelotWheelPosition.forHeight === root.parent.height) {
            root.x = CamelotWheelPosition.x;
            root.y = CamelotWheelPosition.y;
        } else {
            root.x = Math.round((root.parent.width - root.width) / 2);
            root.y = Math.round((root.parent.height - root.height) / 2);
        }
    }

    // The key the popup was opened *with* -- fixed for as long as it's
    // open (there's no more clicking around to explore a different one),
    // so it's both what every wedge's relation highlight is computed
    // against and the one wedge marked with its own origin ring.
    property int originNumber: 0  // 0 = unrecognized/empty key
    property bool originIsMinor: false
    // "camelot" (default, e.g. "8B") or "traditional" (e.g. "C major") --
    // AppSettingsController.keyNotation, threaded down from whichever
    // KeyBadge opened this popup (same convention KeyBadge.qml's own
    // `notation` property already follows).
    property string notation: "camelot"

    // Wedge label in whichever notation the app is currently set to --
    // Theme.traditionalLabel() accepts plain Camelot notation ("8B") as
    // valid input (see parseCamelotKey()'s own camelot-notation branch),
    // so this is just a formatting choice, not a different lookup.
    function wedgeLabel(number, isMinor) {
        var camelot = number + (isMinor ? "A" : "B");
        return root.notation === "traditional" ? Theme.traditionalLabel(camelot) : camelot;
    }

    // For a host (e.g. MatchingPage.qml) that wants to react to
    // hovering here -- highlighting matching rows in a track list while
    // the pointer's over a wedge or a legend swatch. hovering false means
    // the pointer just left that wedge/legend row (the other argument
    // still identifies which one). relationHovered's relationLabel is
    // one of domain::keyRelationLabel()'s own exact strings ("Same key",
    // "Relative major/minor", "Adjacent (harmonic)", "Energy mix").
    // Purely ephemeral, both of them -- this popup never picks anything,
    // see the file's own doc comment above.
    signal keyHovered(int number, bool isMinor, bool hovering)
    signal relationHovered(string relationLabel, bool hovering)

    // The legend, bottom of the popup: each relation, its colour and the
    // sentence the info area shows while it is hovered. Here rather than
    // in the legend's Repeater because the info area measures these tips
    // to size itself (see infoRowHeight).
    readonly property var legendEntries: [
        { label: "Same", relationLabel: "Same key", color: Theme.accent,
            tip: "Identical key. The safest possible transition." },
        { label: "Relative", relationLabel: "Relative major/minor", color: Theme.good,
            tip: "Same wheel number, opposite mode (e.g. 8A/8B). A seamless swap between the major "
                + "and minor version of the same key." },
        { label: "Boost", relationLabel: "Energy Boost", color: Theme.warnIcon,
            tip: "One step clockwise around the wheel, same mode. The classic harmonic-mixing move, "
                + "with a subtle lift in energy." },
        { label: "Drop", relationLabel: "Energy Drop", color: Theme.warnIcon,
            tip: "One step counter-clockwise around the wheel, same mode. The classic harmonic-mixing "
                + "move, with a subtle ease in energy." },
        { label: "Energy mix", relationLabel: "Energy mix", color: Theme.danger,
            tip: "One step around the wheel, opposite mode. A bigger mood/energy shift than Adjacent, "
                + "while staying tonally related." },
    ]

    // The info area is as tall as the longest text it can ever show,
    // measured at its own width: at least four lines, at most six, so
    // none of them is elided. Fixed while the popup is open, whatever is hovered: if the
    // row grew with the text, the wheel below would shrink and move
    // while you hovered it, and the wedge under the pointer with it.
    readonly property int infoMaxLines: 6
    property real infoRowHeight: Math.ceil(infoMetrics.lineSpacing * 4)
    function remeasureInfo() {
        let tallest = Math.ceil(infoMetrics.lineSpacing * 4);
        for (let i = 0; i < infoProbes.count; ++i) {
            const probe = infoProbes.itemAt(i);
            if (probe) {
                tallest = Math.max(tallest, Math.ceil(probe.implicitHeight));
            }
        }
        root.infoRowHeight = tallest;
    }

    // What the pointer is over, shown in the info area top right instead
    // of a tooltip. Tooltips sat on the wheel itself, over the very
    // wedges and keys you were trying to read; the info area is in view
    // and covers nothing.
    property string hoverInfo: ""
    // What a clicked segment is, kept while the pointer is elsewhere;
    // hovering still wins while it lasts.
    property string pinnedInfo: ""
    // Set on enter; on leave, cleared only if it is still this item's
    // text. Moving from one wedge to the next can deliver the second's
    // enter before the first's exit, and a plain clear on exit would
    // blank the info for the wedge the pointer is now on.
    function showInfo(text, hovering) {
        if (hovering) {
            root.hoverInfo = text;
        } else if (root.hoverInfo === text) {
            root.hoverInfo = "";
        }
    }
    function wedgeInfo(number, isMinor, isOrigin) {
        const relation = root.relationLabel(number, isMinor);
        return root.wedgeLabel(number, isMinor)
            + (relation.length > 0 ? ": " + relation : "")
            + (isOrigin ? " (this track's key)" : "");
    }

    // camelotLabel is e.g. "8A", or "" when the key that opened this
    // didn't parse (KeyBadge.qml's own fallback/unrecognized-key case),
    // in which case the wheel shows plain, unhighlighted wedges.
    function openAt(camelotLabel) {
        var match = /^(1[0-2]|[1-9])([AB])$/.exec(camelotLabel);
        if (match) {
            root.originNumber = parseInt(match[1], 10);
            root.originIsMinor = match[2] === "A";
        } else {
            root.originNumber = 0;
        }
        root.pinnedInfo = "";
        wheel.selectedIndex = -1;
        root.resetPosition();
        root.open();
    }

    // Mirrors domain::classifyKeyRelation (src/domain/camelot_key.cpp),
    // origin -> wedge order (so "Energy Boost"/"Energy Drop" read as
    // "moving from the key this popup was opened with to this wedge").
    // This widget works purely in wheel-position space -- no track.key
    // strings to parse, every wedge is already a plain number/isMinor
    // pair -- so it's simpler to keep this one small pure function local
    // than to thread a controller reference through KeyBadge.qml (used
    // all over the app) just to reach the C++ version.
    function relationLabel(number, isMinor) {
        if (root.originNumber === 0) {
            return "";
        }
        if (number === root.originNumber && isMinor === root.originIsMinor) {
            return "Same key";
        }
        if (number === root.originNumber) {
            return "Relative major/minor";
        }
        var diff = Math.abs(number - root.originNumber);
        var wheelDistance = Math.min(diff, 12 - diff);
        if (wheelDistance === 1) {
            if (isMinor !== root.originIsMinor) {
                return "Energy mix";
            }
            // Clockwise from the origin to this wedge (wrapping 12 -> 1)
            // is the "up"/boost direction, matching
            // domain::classifyKeyRelation()'s own convention.
            var up = number === (root.originNumber % 12) + 1;
            return up ? "Energy Boost" : "Energy Drop";
        }
        return "Unrelated key";
    }

    // null (rather than a color) for "no highlight" -- Unrelated and "no
    // selection yet" both render as a plain, uncolored ring. Adjacent
    // and Energy mix deliberately use two hues nothing else here is
    // close to (gold vs. red) -- warnIcon/conflictText looked too alike
    // side by side on a small badge border.
    function relationColor(number, isMinor) {
        switch (root.relationLabel(number, isMinor)) {
        case "Same key": return Theme.accent;
        case "Relative major/minor": return Theme.good;
        case "Energy Boost": return Theme.warnIcon;
        case "Energy Drop": return Theme.warnIcon;
        case "Energy mix": return Theme.danger;
        default: return null;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        // Drag the popup out of the way by its own title bar only (the
        // padding around "Camelot Wheel", not the wedges or legend below)
        // -- restricted here rather than over the whole popup so hovering
        // the wheel itself gives clean, unambiguous pointer feedback
        // instead of a drag cursor everywhere.
        Item {
            id: titleBar
            objectName: "wheelTitleBar"
            Layout.fillWidth: true
            implicitHeight: titleRow.implicitHeight

            MouseArea {
                id: dragArea
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor

                // Manual drag rather than drag.target: root -- Popup isn't
                // a scene-graph Item (it's a QQuickPopup with its own x/y,
                // not a transform MouseArea's built-in drag machinery can
                // attach to), so drag.target silently did nothing: the
                // cursor changed but the popup never actually moved.
                //
                // The pointer is measured in window coordinates. This area
                // moves with the popup, so a delta in its own coordinates
                // shrank by however far the popup had already followed:
                // the popup trailed the pointer at about half its pace.
                property point pressScene: Qt.point(0, 0)
                property real pressPopupX: 0
                property real pressPopupY: 0

                onPressed: (mouse) => {
                    dragArea.pressScene = dragArea.mapToItem(null, mouse.x, mouse.y);
                    dragArea.pressPopupX = root.x;
                    dragArea.pressPopupY = root.y;
                }
                onPositionChanged: (mouse) => {
                    if (pressed) {
                        const now = dragArea.mapToItem(null, mouse.x, mouse.y);
                        root.x = dragArea.pressPopupX + (now.x - dragArea.pressScene.x);
                        root.y = dragArea.pressPopupY + (now.y - dragArea.pressScene.y);
                    }
                }
                onReleased: {
                    CamelotWheelPosition.x = root.x;
                    CamelotWheelPosition.y = root.y;
                    CamelotWheelPosition.forWidth = root.parent.width;
                    CamelotWheelPosition.forHeight = root.parent.height;
                }
            }

            RowLayout {
                id: titleRow
                anchors.fill: parent
                PageTitle { text: "Camelot Wheel"; level: "section"; Layout.fillWidth: true }
                IconToolButton {
                    text: "Close"
                    iconName: "window-close"
                    onClicked: root.close()
                }
            }
        }
        // Context on the left, what the pointer is over on the right. The
        // row's height is root.infoRowHeight, measured below, not the text's.
        FontMetrics { id: infoMetrics; font.pointSize: Theme.fontSmall }
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: root.infoRowHeight
            spacing: 12
            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                wrapMode: Text.WordWrap
                maximumLineCount: root.infoMaxLines
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                text: root.originNumber === 0
                    ? "A visual reference: color and position show which keys mix well together."
                    : "Showing how every key relates to " + root.wedgeLabel(root.originNumber, root.originIsMinor) + "."
            }
            Label {
                id: hoverInfoLabel
                objectName: "wheelHoverInfo"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                horizontalAlignment: Text.AlignRight
                wrapMode: Text.WordWrap
                maximumLineCount: root.infoMaxLines
                // Never reached by the legend's tips, which the row is
                // sized for; a fallback for a text past six lines, so it
                // ends in an ellipsis rather than mid-word.
                elide: Text.ElideRight
                color: Theme.text
                font.pointSize: Theme.fontSmall
                text: root.hoverInfo.length > 0 ? root.hoverInfo : root.pinnedInfo
                onWidthChanged: root.remeasureInfo()
            }
        }
        // The legend's tips laid out invisibly at the info label's own
        // width and font: the tallest of them is the row's height. The
        // wedge texts are one line each and never the tallest.
        Item {
            visible: false
            width: 0
            height: 0
            Repeater {
                id: infoProbes
                model: root.legendEntries
                delegate: Text {
                    required property var modelData
                    width: hoverInfoLabel.width
                    wrapMode: Text.WordWrap
                    maximumLineCount: root.infoMaxLines
                    // The label's own font object, not a Text default:
                    // a Label takes the style's control font, and the
                    // measurement has to be made in the same face.
                    font: hoverInfoLabel.font
                    text: modelData.tip
                    onImplicitHeightChanged: root.remeasureInfo()
                    Component.onCompleted: root.remeasureInfo()
                }
            }
        }

        // Two rings of twelve segments that close the circle, the way a
        // Camelot wheel is drawn everywhere else: major keys (B) outside,
        // their relative minors (A) inside, each pair on one spoke. Seams of
        // one even width run between them, straight-sided rather than
        // wedge-shaped, so the gap reads the same at the rim as at the hub.
        //
        // Hovering a segment lifts it out of the ring; clicking it keeps it
        // lifted, with a little bounce, and keeps what it is in the info
        // area after the pointer moves on. Clicking it again, or anywhere
        // off the segments, lets it go. Still a reference: nothing outside
        // the popup hears about a click.
        Item {
            id: wheel
            Layout.fillWidth: true
            Layout.fillHeight: true

            // The width of every seam, between neighbours and between rings.
            readonly property real seam: 8
            // How far a lifted segment moves out, and room kept for it at
            // the rim so the outer ring is not clipped when it does.
            readonly property real hoverLift: 5
            readonly property real selectLift: 9
            readonly property real cx: width / 2
            readonly property real cy: height / 2
            readonly property real majorOuter: Math.max(0, Math.min(width, height) / 2 - wheel.selectLift - 4)
            readonly property real majorInner: wheel.majorOuter * 0.64
            readonly property real minorOuter: wheel.majorInner - wheel.seam
            readonly property real minorInner: wheel.majorOuter * 0.36

            property int selectedIndex: -1
            property bool selectedMinor: false

            // One key, as a ring segment with rounded corners: its colour, and
            // on top of that whatever outline the key carries -- its relation
            // to the track's key, and the track's own key.
            component KeySegment: Item {
                id: segment
                required property int index
                required property bool minor
                readonly property int number: index + 1
                readonly property real centreDeg: index * 30 - 90
                readonly property real centreRad: segment.centreDeg * Math.PI / 180
                readonly property real innerR: segment.minor ? wheel.minorInner : wheel.majorInner
                readonly property real outerR: segment.minor ? wheel.minorOuter : wheel.majorOuter
                readonly property real midR: (segment.innerR + segment.outerR) / 2
                readonly property color keyColor: Theme.colorForKey(segment.number + (segment.minor ? "A" : "B"))
                // var, not color: relationColor() answers null for a key with no
                // relation to the track's, and null assigned to a color property
                // is opaque black, which drew a black ring inside every
                // unrelated segment.
                readonly property var highlight: root.relationColor(segment.number, segment.minor)
                readonly property bool isOrigin: segment.number === root.originNumber
                                                 && segment.minor === root.originIsMinor
                readonly property bool hovered: wheelPointer.hoverIndex === segment.index
                                                && wheelPointer.hoverMinor === segment.minor
                readonly property bool selected: wheel.selectedIndex === segment.index
                                                 && wheel.selectedMinor === segment.minor
                anchors.fill: parent
                // Above its neighbours while lifted, so its outline is not
                // cut by the segment beside it.
                z: segment.selected ? 2 : segment.hovered ? 1 : 0
                // Unrelated keys fade toward the popup's own colour. Not by
                // opacity: the fill and its rounding stroke overlap, and at
                // partial opacity the overlap drew a darker rim round every
                // faded segment.
                property real fade: root.originNumber !== 0 && !segment.isOrigin && !segment.selected
                    && root.relationLabel(segment.number, segment.minor) === "Unrelated key" ? 0.45 : 0
                Behavior on fade { NumberAnimation { duration: Theme.shortTransitionDuration } }
                readonly property color shownColor: Qt.tint(segment.keyColor,
                    Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, segment.fade))

                // Outward along the segment's own spoke; springs, so it
                // overshoots a touch and settles rather than sliding.
                property real lift: segment.selected ? wheel.selectLift : segment.hovered ? wheel.hoverLift : 0
                Behavior on lift { SpringAnimation { spring: 4.5; damping: 0.22; epsilon: 0.05 } }
                // A pulse on being picked: swells and settles back.
                property real zoom: 1.0
                SequentialAnimation {
                    id: pickPulse
                    NumberAnimation { target: segment; property: "zoom"; to: 1.12; duration: 110; easing.type: Easing.OutQuad }
                    NumberAnimation { target: segment; property: "zoom"; to: 1.0; duration: 520; easing.type: Easing.OutElastic; easing.amplitude: 1.1; easing.period: 0.4 }
                }
                onSelectedChanged: if (segment.selected) pickPulse.restart()

                transform: [
                    Scale {
                        origin.x: wheel.cx + segment.midR * Math.cos(segment.centreRad)
                        origin.y: wheel.cy + segment.midR * Math.sin(segment.centreRad)
                        xScale: segment.zoom
                        yScale: segment.zoom
                    },
                    Translate {
                        x: segment.lift * Math.cos(segment.centreRad)
                        y: segment.lift * Math.sin(segment.centreRad)
                    }
                ]

                // The corners are rounded by stroking the fill in its own
                // colour with round joins, inset by the stroke's half width
                // so the rounded shape keeps the segment's footprint.
                Arc {
                    innerR: segment.innerR
                    outerR: segment.outerR
                    centreDeg: segment.centreDeg
                    inset: 3
                    fill: segment.shownColor
                    stroke: segment.shownColor
                    strokeW: 6
                }
                Arc {
                    visible: segment.highlight !== null
                    innerR: segment.innerR
                    outerR: segment.outerR
                    centreDeg: segment.centreDeg
                    inset: 3
                    stroke: segment.highlight !== null ? segment.highlight : "transparent"
                    strokeW: 3
                }
                Arc {
                    visible: segment.isOrigin
                    innerR: segment.innerR
                    outerR: segment.outerR
                    centreDeg: segment.centreDeg
                    inset: 1.5
                    stroke: Theme.text
                    strokeW: 3
                }
                Label {
                    x: Theme.snap(wheel.cx + segment.midR * Math.cos(segment.centreRad) - width / 2)
                    y: Theme.snap(wheel.cy + segment.midR * Math.sin(segment.centreRad) - height / 2)
                    text: root.wedgeLabel(segment.number, segment.minor)
                    font.bold: true
                    font.pointSize: Theme.fontSmall
                    color: Theme.contrastingTextColor(segment.shownColor)
                }
            }

            // A ring segment from innerR to outerR, one twelfth of the circle
            // centred on centreDeg (0 = east, clockwise, as screen angles run),
            // less half a seam on either side and inset by `inset` all round
            // -- so an outline of width 2 * inset lies just inside the edge.
            // The seam is cut at a constant width: each radius loses the
            // angle that half a seam subtends there, more at the hub than at
            // the rim.
            component Arc: Shape {
                id: arc
                property real innerR: 0
                property real outerR: 0
                property real centreDeg: 0
                property real inset: 0
                property color fill: "transparent"
                property color stroke: "transparent"
                property real strokeW: 1
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                readonly property real rOut: Math.max(1, arc.outerR - arc.inset)
                readonly property real rIn: Math.max(1, arc.innerR + arc.inset)
                readonly property real half: wheel.seam / 2 + arc.inset
                readonly property real centre: arc.centreDeg * Math.PI / 180
                readonly property real halfSpan: 15 * Math.PI / 180
                readonly property real outA0: arc.centre - arc.halfSpan + Math.asin(Math.min(1, arc.half / arc.rOut))
                readonly property real outA1: arc.centre + arc.halfSpan - Math.asin(Math.min(1, arc.half / arc.rOut))
                readonly property real inA0: arc.centre - arc.halfSpan + Math.asin(Math.min(1, arc.half / arc.rIn))
                readonly property real inA1: arc.centre + arc.halfSpan - Math.asin(Math.min(1, arc.half / arc.rIn))
                ShapePath {
                    fillColor: arc.fill
                    strokeColor: arc.stroke
                    strokeWidth: arc.strokeW
                    joinStyle: ShapePath.RoundJoin
                    startX: wheel.cx + arc.rOut * Math.cos(arc.outA0)
                    startY: wheel.cy + arc.rOut * Math.sin(arc.outA0)
                    PathArc {
                        x: wheel.cx + arc.rOut * Math.cos(arc.outA1)
                        y: wheel.cy + arc.rOut * Math.sin(arc.outA1)
                        radiusX: arc.rOut
                        radiusY: arc.rOut
                        direction: PathArc.Clockwise
                    }
                    PathLine {
                        x: wheel.cx + arc.rIn * Math.cos(arc.inA1)
                        y: wheel.cy + arc.rIn * Math.sin(arc.inA1)
                    }
                    PathArc {
                        x: wheel.cx + arc.rIn * Math.cos(arc.inA0)
                        y: wheel.cy + arc.rIn * Math.sin(arc.inA0)
                        radiusX: arc.rIn
                        radiusY: arc.rIn
                        direction: PathArc.Counterclockwise
                    }
                    PathLine {
                        x: wheel.cx + arc.rOut * Math.cos(arc.outA0)
                        y: wheel.cy + arc.rOut * Math.sin(arc.outA0)
                    }
                }
            }

            Repeater {
                model: 12
                delegate: KeySegment { minor: false }
            }
            Repeater {
                model: 12
                delegate: KeySegment { minor: true }
            }

            // One pointer target for all twenty-four segments, hit by angle
            // and radius. A segment is not a rectangle, so a MouseArea per
            // key would claim the corners it does not cover and answer for
            // the wrong key near every seam. The seams themselves count for
            // the nearer segment, so moving across one does not flicker the
            // info area through "nothing".
            MouseArea {
                id: wheelPointer
                objectName: "wheelPointer"
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: wheelPointer.hoverIndex >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                property int hoverIndex: -1
                property bool hoverMinor: false

                function segmentAt(mx, my) {
                    const dx = mx - wheel.cx;
                    const dy = my - wheel.cy;
                    const r = Math.sqrt(dx * dx + dy * dy);
                    const ringSplit = (wheel.minorOuter + wheel.majorInner) / 2;
                    var minor;
                    if (r >= ringSplit && r <= wheel.majorOuter) {
                        minor = false;
                    } else if (r >= wheel.minorInner && r < ringSplit) {
                        minor = true;
                    } else {
                        return {index: -1, minor: false};
                    }
                    const deg = Math.atan2(dy, dx) * 180 / Math.PI;
                    const index = ((Math.round((deg + 90) / 30) % 12) + 12) % 12;
                    return {index: index, minor: minor};
                }
                function setHover(index, minor) {
                    if (index === hoverIndex && minor === hoverMinor) {
                        return;
                    }
                    if (hoverIndex >= 0) {
                        const n = hoverIndex + 1;
                        root.keyHovered(n, hoverMinor, false);
                        root.showInfo(root.wedgeInfo(n, hoverMinor,
                            n === root.originNumber && hoverMinor === root.originIsMinor), false);
                    }
                    hoverIndex = index;
                    hoverMinor = minor;
                    if (index >= 0) {
                        const n = index + 1;
                        root.keyHovered(n, minor, true);
                        root.showInfo(root.wedgeInfo(n, minor,
                            n === root.originNumber && minor === root.originIsMinor), true);
                    }
                }
                onPositionChanged: (mouse) => {
                    const hit = segmentAt(mouse.x, mouse.y);
                    setHover(hit.index, hit.minor);
                }
                onExited: setHover(-1, false)
                onClicked: (mouse) => {
                    const hit = segmentAt(mouse.x, mouse.y);
                    if (hit.index < 0 || (hit.index === wheel.selectedIndex && hit.minor === wheel.selectedMinor)) {
                        wheel.selectedIndex = -1;
                        root.pinnedInfo = "";
                    } else {
                        wheel.selectedIndex = hit.index;
                        wheel.selectedMinor = hit.minor;
                        const n = hit.index + 1;
                        root.pinnedInfo = root.wedgeInfo(n, hit.minor,
                            n === root.originNumber && hit.minor === root.originIsMinor);
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            // Reduced from 14 -- one more legend entry (Adjacent split
            // into Boost/Drop) needs the room in this popup's fixed
            // 400px width.
            spacing: 9
            Repeater {
                model: root.legendEntries
                delegate: RowLayout {
                    id: legendItem
                    required property var modelData
                    spacing: 4
                    // A hovered entry answers the way a hovered segment
                    // does: its swatch springs up (the segments' own
                    // spring) and its name comes up from muted to text.
                    Rectangle {
                        width: 10; height: 10; radius: 5
                        color: legendItem.modelData.color
                        scale: legendHover.hovered ? 1.5 : 1.0
                        Behavior on scale { SpringAnimation { spring: 4.5; damping: 0.22; epsilon: 0.01 } }
                    }
                    Label {
                        text: legendItem.modelData.label
                        font.pointSize: Theme.fontTiny
                        color: legendHover.hovered ? Theme.text : Theme.textMuted
                        Behavior on color { ColorAnimation { duration: Theme.shortTransitionDuration } }
                    }

                    HoverHandler {
                        id: legendHover
                        onHoveredChanged: {
                            root.relationHovered(legendItem.modelData.relationLabel, legendHover.hovered);
                            root.showInfo(legendItem.modelData.tip, legendHover.hovered);
                        }
                    }
                }
            }
        }
    }
}
