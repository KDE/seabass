// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A section page's header: one round Back button, and beside it two
// lines, the path as a small uppercase eyebrow and the page's own name
// as the title under it.
//
//     (‹)  ⌂ › MY-STICK › HOUSEKEEPING
//          Clean Up Duplicates
//
// It used to be one line, "[home] › [stick] › [middle] › this page",
// with every crumb set at the title's size and only greyer, so the path
// competed with the name of the page it led to. The path is context,
// and is drawn as context now: a caption above the title. The Back
// button does what the hub crumb does (one level up), or goes Home when
// there is no hub to go to, and it is the one Tab stop of the bar. The
// eyebrow's crumbs keep their clicks for the mouse: the house always
// jumps back to the StackView's very first item in one click (pop(null),
// not a single pop()) no matter how deep the current page sits.
//
// Every clickable part uses Theme.rowHover/rowPressed, the same tint
// tokens list rows already use, rather than inventing its own hover
// colour.
RowLayout {
    id: root
    // The stick this page works on. Always context, never a link: Home
    // is the stick list, so there is no page in the stack that IS the
    // stick, and a click on its name could only ever land on Home, which
    // the house to its left already does. Empty omits it (a page with no
    // stick of its own, e.g. Manage Backups opened from Home).
    property string stickLabel: ""
    // Empty omits the middle segment entirely (a page pushed directly
    // from Home with no stick/hub context of its own, e.g. Preferences).
    property string middleLabel: ""
    required property string title
    property bool backEnabled: true
    property string backDisabledTooltip: "Wait for the write to finish before leaving this page"
    // The page's own StackView, handed in as `stack: root.StackView.view`
    // (the attached property exists on the pushed page, not on anything
    // inside it). Only used to answer one question, see below.
    property var stack: null
    // Whether the middle segment's click would land on Home anyway.
    //
    // At depth 2 the item under this page IS Home, so pop() and pop(null)
    // are the same jump and the middle segment is a second Home button
    // wearing the stick's name: you click "MY-STICK" expecting the stick
    // and get the page you could already reach from the crumb to its
    // left. Pages that sit one below Home therefore show the name as
    // plain context text instead of as a link. Nothing to configure:
    // the same page pushed from Home and from a hub (SyncPage is, from
    // Home and from Library Statistics) gets it right both times.
    readonly property bool middleLeadsHome: stack ? stack.depth <= 2 : false
    readonly property bool middleClickable: middleLabel.length > 0 && !middleLeadsHome

    // Who gives way, and in what order, when the bar is narrower than its
    // natural width.
    //
    // The two lines share the width and nothing else: each has the whole
    // column beside the Back button to itself. So the title elides only
    // when its own line is short, and the eyebrow's arithmetic is about
    // the eyebrow alone. There, from the left: the stick drops first,
    // whole, and a "…" takes its place so the path still says a level was
    // there; then the hub elides to a floor; and past the floor it keeps
    // eliding rather than reach past the bar's right edge. With no hub the
    // stick is the path's last crumb and is the one that elides, since
    // "⌂ › …" would say nothing at all.
    //
    // A RowLayout alone cannot say "first": squeezed, it shares the
    // shortfall out among every segment at once, and "MY-ST… › Houseke…"
    // reads as neither. So each segment's minimum is worked out here from
    // the shortfall, the minimums add up to exactly the width the row was
    // given, and the layout has nothing left to share out.
    //
    // No binding loop: everything here is worked out from the segments'
    // own natural widths, which do not depend on whether they are shown,
    // and from the width the bar was given, never from the bar's implicit
    // width, which does depend on them.
    readonly property bool hasStick: stickLabel.length > 0
    readonly property bool hasMiddle: middleLabel.length > 0
    readonly property real stickNatural: hasStick ? stickText.naturalWidth : 0
    readonly property real middleNatural: !hasMiddle ? 0
        : middleClickable ? middleCrumb.naturalWidth : middleText.naturalWidth
    readonly property real titleNatural: titleText.naturalWidth
    // A crumb with the chevron before it and the two gaps around that.
    function segmentCost(natural) {
        return natural + Math.ceil(stickSep.implicitWidth) + 2 * eyebrow.spacing;
    }
    // The eyebrow's natural width with every crumb it has, the stick
    // included whether or not it is showing.
    readonly property real eyebrowNatural: homeCrumb.naturalWidth
        + (hasStick ? segmentCost(stickNatural) : 0)
        + (hasMiddle ? segmentCost(middleNatural) : 0)
    // The column beside the Back button, plus the pill padding the
    // eyebrow reaches back into (see its leftMargin).
    readonly property real eyebrowAvailable: Math.max(0, width - backButton.implicitWidth - spacing
                                                      + eyebrowPillPadding)
    readonly property real middleFloor: Math.min(Theme.scaled(64), middleNatural)
    readonly property bool stickDropped: hasStick && hasMiddle && eyebrowNatural > eyebrowAvailable
    readonly property real eyebrowShown: eyebrowNatural
        - (stickDropped ? stickNatural - droppedMark.naturalWidth : 0)
    readonly property real eyebrowShortfall: Math.max(0, eyebrowShown - eyebrowAvailable)
    // Whatever is last in the path takes the rest, the hub when there is
    // one and otherwise the stick. Down to the floor first; below it only
    // when nothing else is left to give.
    readonly property real middleShortfall: hasMiddle ? Math.min(eyebrowShortfall, middleNatural - middleFloor) : 0
    readonly property real middleBelowFloor: hasMiddle ? Math.min(eyebrowShortfall - middleShortfall, middleFloor) : 0
    readonly property real stickShortfall: !hasMiddle ? Math.min(eyebrowShortfall, stickNatural) : 0
    readonly property real columnNatural: Math.max(eyebrowNatural - eyebrowPillPadding, titleNatural)

    // The eyebrow's hover pills: small, so the chevrons between crumbs
    // keep a caption's rhythm rather than a toolbar's.
    readonly property real eyebrowPillPadding: Theme.scaled(4)
    // Tracking of a tenth of the eyebrow's own size. letterSpacing is in
    // pixels and the size is in points, so it is read off a probe set in
    // the same face (the probe carries no spacing of its own, so there is
    // nothing for the measurement to chase).
    readonly property real eyebrowTracking: eyebrowProbe.fontInfo.pixelSize * 0.1
    // The house in the eyebrow. Breeze draws it inside a margin of its
    // own square, so a square the size of the capitals drew a house a
    // third smaller than them (breadcrumb.png); a square about the size
    // of the eyebrow's em puts the roof level with the capitals.
    readonly property real eyebrowIconSize: Math.ceil(eyebrowProbe.fontInfo.pixelSize * 1.1)
    // On a page with no stick and no hub the house is the whole path, and
    // alone it read as a stray speck above the title (AboutPage.png). It
    // says its name there, as the path's one crumb.
    readonly property bool homeNamed: !hasStick && !hasMiddle

    signal homeRequested()
    signal backRequested()

    spacing: Theme.rowSpacing
    // The Back button's ring is what sits on the page's left line: it is
    // the first thing on the row and the edge the eye runs down from the
    // body. The two lines of text then share a left edge of their own,
    // one button and one gap in.
    //
    // And the bar may be made narrower than its natural width. Without
    // this it was the hard floor under every page that has one: a
    // ColumnLayout gives ALL its fill-width children the widest such
    // floor among them, so on Clean Up, whose title is long, one
    // unshrinkable breadcrumb pinned the entire header at 701px
    // (tests/qml/tst_CleanupPage.qml).
    Layout.minimumWidth: 0

    // A crumb in the eyebrow that goes somewhere. Not a Tab stop: the
    // Back button is the bar's one, and it already goes where the hub
    // does.
    component Crumb: AbstractButton {
        id: crumb
        enabled: root.backEnabled
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        Layout.minimumWidth: 0
        // Ceilings, not the raw implicit width, and on the PREFERRED
        // width as well as the maximum. A layout hands out whole
        // pixels, so a segment asking for 264.37 was given 264, and a
        // Text a third of a pixel short of its natural width elides:
        // "TESTSTI…" on a row two thirds empty. The +1 is the pixel the
        // layout's flooring takes back.
        readonly property real naturalWidth: Math.ceil(implicitWidth) + 1
        Layout.preferredWidth: naturalWidth
        Layout.maximumWidth: naturalWidth

        ToolTip.visible: hovered

        leftPadding: root.eyebrowPillPadding
        rightPadding: root.eyebrowPillPadding
        topPadding: Theme.scaled(2)
        bottomPadding: Theme.scaled(2)

        background: Rectangle {
            radius: Theme.scaled(4)
            color: crumb.pressed ? Theme.rowPressed
                : crumb.hovered ? Theme.rowHover
                : "transparent"
        }
        // Draws HomeIcon rather than a word. Not a font glyph: a symbol
        // font that lacks the character silently falls back to whatever
        // fontconfig picks, or to tofu.
        property bool showsIcon: false
        contentItem: Loader {
            sourceComponent: crumb.showsIcon ? iconContent : textContent
        }

        Component {
            id: textContent
            EyebrowText {
                text: crumb.text
                opacity: crumb.enabled ? 1.0 : 0.5
            }
        }

        Component {
            id: iconContent
            Row {
                spacing: root.eyebrowPillPadding
                HomeIcon {
                    id: homeIcon
                    anchors.verticalCenter: parent.verticalCenter
                    size: root.eyebrowIconSize
                    color: Theme.textMuted
                    opacity: crumb.enabled ? 1.0 : 0.5
                }
                EyebrowText {
                    objectName: "homeWord"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.homeNamed
                    text: "Home"
                    opacity: crumb.enabled ? 1.0 : 0.5
                }
            }
        }
    }

    component EyebrowText: Label {
        font.family: Theme.titleFamily
        font.weight: Font.Medium
        font.pointSize: Theme.fontSmall
        font.capitalization: Font.AllUppercase
        font.letterSpacing: root.eyebrowTracking
        color: Theme.textMuted
        elide: Text.ElideRight
    }

    // A crumb that leads nowhere new: context, not a link. No hover
    // pill and no click, but it still elides and still says its full
    // name on hover when it has had to.
    component Context: EyebrowText {
        id: context
        // Matches the hover pill's padding on either side so the
        // chevrons around it sit where they do around a Crumb.
        leftPadding: root.eyebrowPillPadding
        rightPadding: root.eyebrowPillPadding
        topPadding: Theme.scaled(2)
        bottomPadding: Theme.scaled(2)
        readonly property real naturalWidth: Math.ceil(implicitWidth) + 1
        Layout.fillWidth: true
        Layout.preferredWidth: naturalWidth
        Layout.maximumWidth: naturalWidth

        HoverHandler { id: contextHover }
        ToolTip.visible: contextHover.hovered && context.truncated
        ToolTip.text: context.text
    }

    // A chevron drawn from the bundled Breeze set, not a "›" glyph:
    // the glyph's size and weight came from whichever face answered,
    // and it sat on the text's baseline instead of its middle.
    component Sep: SeabassIcon {
        objectName: "eyebrowSep"
        iconName: "arrow-right"
        size: Theme.scaled(9)
        color: Theme.textMuted
        opacity: 0.6
        Layout.alignment: Qt.AlignVCenter
    }

    Text {
        id: eyebrowProbe
        visible: false
        text: "M"
        font.family: Theme.titleFamily
        font.weight: Font.Medium
        font.pointSize: Theme.fontSmall
    }
    FontMetrics {
        id: eyebrowMetrics
        font: eyebrowProbe.font
    }

    // One level up: to the hub when there is one to go to, Home when
    // there is not. The disabled button still says why on hover: a
    // disabled control gets no hover events of its own, so the handler
    // sits on the slot around it.
    Item {
        id: backSlot
        implicitWidth: backButton.implicitWidth
        implicitHeight: backButton.implicitHeight
        Layout.alignment: Qt.AlignVCenter

        AbstractButton {
            id: backButton
            objectName: "backButton"
            anchors.fill: parent
            implicitWidth: Theme.headerBackButtonSize
            implicitHeight: Theme.headerBackButtonSize
            enabled: root.backEnabled
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            opacity: enabled ? 1.0 : 0.5
            readonly property string tip: !root.backEnabled ? root.backDisabledTooltip
                : root.middleClickable ? ("Back to " + root.middleLabel)
                : "Back to Home"
            Accessible.name: tip
            ToolTip.visible: backHover.hovered
            ToolTip.text: tip
            onClicked: root.middleClickable ? root.backRequested() : root.homeRequested()

            background: Rectangle {
                radius: width / 2
                color: backButton.pressed ? Theme.rowPressed
                    : backButton.hovered ? Theme.rowHover
                    : "transparent"
                border.width: 1
                border.color: backButton.visualFocus ? Theme.accent : Theme.border
            }
            contentItem: Item {
                SeabassIcon {
                    anchors.centerIn: parent
                    iconName: "go-previous"
                    size: Theme.scaled(16)
                    color: Theme.text
                }
            }
        }
        HoverHandler { id: backHover }
    }

    ColumnLayout {
        id: lines
        spacing: 0
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: root.columnNatural
        Layout.maximumWidth: root.columnNatural
        Layout.alignment: Qt.AlignVCenter

        RowLayout {
            id: eyebrow
            objectName: "eyebrow"
            spacing: Theme.scaled(2)
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            // The pill's padding reaches back into the gap beside the Back
            // button, so the house itself starts on the title's left edge.
            Layout.leftMargin: -root.eyebrowPillPadding

            Crumb {
                id: homeCrumb
                objectName: "homeCrumb"
                showsIcon: true
                onClicked: root.homeRequested()
                ToolTip.text: root.backEnabled ? "Back to Home" : root.backDisabledTooltip
            }

            Sep {
                id: stickSep
                visible: root.hasStick
            }

            // The stick. The crumb that gives way first: the reader picked
            // it on Home a moment ago and can most easily do without it.
            Context {
                id: stickText
                objectName: "stickSegment"
                visible: root.hasStick && !root.stickDropped
                text: root.stickLabel
                Layout.minimumWidth: naturalWidth - root.stickShortfall
            }

            // What stands in for the stick once it has gone: a level is
            // still there, it just is not spelled out.
            Context {
                id: droppedMark
                objectName: "droppedMark"
                visible: root.stickDropped
                text: "…"
                Layout.minimumWidth: naturalWidth
                ToolTip.visible: false
            }

            Sep {
                id: middleSep
                visible: root.hasMiddle
            }

            Crumb {
                id: middleCrumb
                objectName: "middleLink"
                visible: root.middleClickable
                Layout.fillWidth: true
                Layout.minimumWidth: naturalWidth - root.middleShortfall - root.middleBelowFloor
                text: root.middleLabel
                onClicked: root.backRequested()
                // Names itself in full when it has been shortened: an
                // abbreviation the reader cannot expand is just a missing
                // word.
                ToolTip.text: !root.backEnabled ? root.backDisabledTooltip
                    // contentItem is the Loader; the Label that elides is its item.
                    : (contentItem.item && contentItem.item.truncated) ? (root.middleLabel + ": back to it")
                    : ("Back to " + root.middleLabel)
            }

            // The same segment when it leads nowhere new.
            Context {
                id: middleText
                objectName: "middleSegment"
                visible: root.hasMiddle && !root.middleClickable
                text: root.middleLabel
                Layout.minimumWidth: naturalWidth - root.middleShortfall - root.middleBelowFloor
            }
        }

        // The page's own name, on a line of its own, in full ink.
        PageTitle {
            id: titleText
            objectName: "titleSegment"
            text: root.title
            level: "crumb"
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            readonly property real naturalWidth: Math.ceil(implicitWidth) + 1
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: naturalWidth
            Layout.maximumWidth: naturalWidth

            // It may be shortened, but only if hovering it gives the whole
            // name back.
            HoverHandler { id: titleHover }
            ToolTip.visible: titleHover.hovered && titleText.truncated
            ToolTip.text: root.title
        }
    }
}
