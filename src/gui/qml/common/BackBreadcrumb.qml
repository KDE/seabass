// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Replaces the old "‹" ToolButton + separate PageTitle pair every
// section page's header used to duplicate. Up to four segments:
// "[home] › [stick] › [middle] › this page", where the house always
// jumps back to the StackView's very first item in one click (pop(null),
// not a single pop()) no matter how deep the current page sits. The
// stick segment names the stick the page is about; the middle segment,
// when present, is one level up -- the hub page for a page nested inside
// one, or (on pages that predate the stick segment and pass the stick's
// name as middleLabel) the stick itself for a page pushed directly from
// Home.
// Every clickable segment uses Theme.rowHover/rowPressed -- the same
// tint tokens list rows already use -- rather than inventing its own
// hover color, so this is also the fix for hover feedback being
// inconsistent button-to-button across the app: one shared
// background/contentItem means every page's back affordance now hovers
// identically.
RowLayout {
    id: root
    // The stick this page works on. Always context, never a link: Home
    // is the stick list, so there is no page in the stack that IS the
    // stick, and a click on its name could only ever land on Home -- the
    // house to its left already does that. Empty omits it (a page with no
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
    // inside it). Only used to answer one question -- see below.
    property var stack: null
    // Whether the middle segment's click would land on Home anyway.
    //
    // At depth 2 the item under this page IS Home, so pop() and pop(null)
    // are the same jump and the middle segment is a second Home button
    // wearing the stick's name: you click "MY-STICK" expecting the stick
    // and get the page you could already reach from the crumb to its
    // left. Pages that sit one below Home therefore show the name as
    // plain context text instead of as a link. Nothing to configure --
    // the same page pushed from Home and from a hub (SyncPage is, from
    // Home and from Library Statistics) gets it right both times.
    readonly property bool middleLeadsHome: stack ? stack.depth <= 2 : false
    readonly property bool middleClickable: middleLabel.length > 0 && !middleLeadsHome

    // Who gives way, and in what order, when the row is narrower than
    // its natural width: the stick first, then the middle segment, then
    // the page's own name, each down to a floor and no further.
    //
    // A RowLayout alone cannot say "first". Squeezed below the preferred
    // widths it shares the shortfall out among every segment in
    // proportion to how far each can shrink, so all three elided at once
    // -- "MY-ST... > Houseke... > Clean Up Dupl..." -- and none of them
    // could be read. Instead each segment's minimum is worked out here
    // from the shortfall, so that the minimums add up to exactly the width
    // the row was given and the layout has nothing left to share out: the
    // stick is handed all of the shortfall it can absorb, the middle the
    // rest, and the title only what neither could take.
    //
    // No binding loop: the row's implicit width is the sum of the
    // segments' PREFERRED widths, which are their natural widths and do
    // not depend on this; and a minimum is never set above a preferred
    // width, which is the one way a minimum could feed back into it.
    readonly property real shortfall: Math.max(0, implicitWidth - width)
    readonly property real stickNatural: stickText.visible ? stickText.naturalWidth : 0
    readonly property real middleNatural: middleCrumb.visible ? middleCrumb.naturalWidth
        : middleText.visible ? middleText.naturalWidth : 0
    readonly property real titleNatural: titleText.naturalWidth
    // Floors, clamped to the natural width so that a short name is never
    // padded out to one.
    readonly property real stickFloor: Math.min(Theme.scaled(64), stickNatural)
    readonly property real middleFloor: Math.min(Theme.scaled(64), middleNatural)
    readonly property real titleFloor: Math.min(Theme.scaled(120), titleNatural)
    readonly property real stickShortfall: Math.min(shortfall, stickNatural - stickFloor)
    readonly property real middleShortfall: Math.min(shortfall - stickShortfall, middleNatural - middleFloor)
    readonly property real titleShortfall: Math.min(shortfall - stickShortfall - middleShortfall,
                                                    titleNatural - titleFloor)
    signal homeRequested()
    signal backRequested()

    spacing: Theme.scaled(4)
    // The segments are hover pills with their own left padding, so the
    // text inside the first one starts that much further right than the
    // row does. Pulled back by exactly that, so a page's title lines up
    // with the body beneath it instead of sitting a pill's padding to
    // the right of it. Every header gets this without asking.
    Layout.leftMargin: -Theme.crumbTextInset
    // And it may be made narrower than its natural width.
    //
    // Without this the breadcrumb was the hard floor under every page
    // that has one. A RowLayout child cannot be laid out below its
    // implicit width unless a minimum says so, and a ColumnLayout gives
    // ALL its fill-width children the widest such floor among them --
    // so on Clean Up, whose title is long, one unshrinkable breadcrumb
    // pinned the entire header at 701px and every row in it overflowed
    // any window narrower than that, whatever those rows did about their
    // own sizing. Measured in tests/qml/tst_CleanupPage.qml: the filter
    // row was 701 wide at page widths of 960, 700, 520 and 380 alike.
    Layout.minimumWidth: 0

    component Crumb: AbstractButton {
        id: crumb
        enabled: root.backEnabled
        hoverEnabled: true
        // Each segment gives way in turn rather than the row refusing to
        // shrink. The label already elides; eliding needs to be allowed
        // to happen, which is what a zero minimum says.
        Layout.minimumWidth: 0
        // Ceilings, not the raw implicit width, and on the PREFERRED
        // width as well as the maximum. A layout hands out whole pixels,
        // so a segment asking for 264.37 was given 264 -- and a Text a
        // third of a pixel short of its natural width elides, producing
        // "TESTSTI..." on an 868px row two thirds empty. The ellipsis was
        // never about running out of room; it was about the fraction. A
        // maximum alone does not fix it: a maximum only caps growth, it
        // never asks for the extra pixel, so the preferred width is what
        // gets assigned -- and the layout floors that to whole pixels,
        // which is why the ceiling needs the +1 rather than standing on
        // its own. Measured in tests/qml/tst_BackBreadcrumb.qml: without
        // it the segment is handed 264 for a 264.37 name.
        readonly property real naturalWidth: Math.ceil(implicitWidth) + 1
        Layout.preferredWidth: naturalWidth
        Layout.maximumWidth: naturalWidth

        ToolTip.visible: hovered

        leftPadding: Theme.scaled(8)
        rightPadding: Theme.scaled(8)
        topPadding: Theme.scaled(4)
        bottomPadding: Theme.scaled(4)

        background: Rectangle {
            radius: Theme.scaled(4)
            color: crumb.pressed ? Theme.rowPressed
                : crumb.hovered ? Theme.rowHover
                : "transparent"
        }
        // Draws HomeIcon rather than a word. Not a font glyph: a symbol
        // font that lacks the character silently falls back to whatever
        // fontconfig picks, or to tofu, and this is the only way back on
        // the six pages that have no middle segment.
        property bool showsIcon: false
        contentItem: Loader {
            sourceComponent: crumb.showsIcon ? iconContent : textContent
        }

        Component {
            id: textContent
            Label {
                text: crumb.text
                font.family: Theme.titleFamily
                font.weight: Theme.titleWeight
                // The words step down a size; the icon below keeps the
                // bigger one. See Theme.titleCrumb for why they part
                // company here.
                font.pointSize: Theme.titleCrumb
                color: Theme.textMuted
                opacity: crumb.enabled ? 1.0 : 0.5
                elide: Text.ElideRight
            }
        }

        Component {
            id: iconContent
            HomeIcon {
                // A quarter larger than its own default: the one crumb
                // that is a picture, and the way back to the start.
                size: Theme.iconSizeSmall * 0.875
                color: Theme.textMuted
                opacity: crumb.enabled ? 1.0 : 0.5
            }
        }
    }

    component Sep: Label {
        text: "›"
        color: Theme.textMuted
        font.pointSize: Theme.titleCrumb
    }

    // A house, not the word "Home". The word cost this row about four
    // characters of width on every page that has a breadcrumb, and the
    // row it was spending them on is the one whose stick's name gives
    // way first when the header runs out of room. Breeze's own go-home,
    // so the button a KDE user reaches for looks like the one they
    // already know.
    Crumb {
        objectName: "homeCrumb"
        showsIcon: true
        onClicked: root.homeRequested()
        ToolTip.text: root.backEnabled ? "Back to Home" : root.backDisabledTooltip
    }

    // A segment that leads nowhere new: context, not a link. No hover
    // pill and no click, but it still elides and still says its full
    // name on hover when it has had to.
    component Context: Label {
        id: context
        font.family: Theme.titleFamily
        font.weight: Theme.titleWeight
        font.pointSize: Theme.titleCrumb
        color: Theme.textMuted
        elide: Text.ElideRight
        // Matches the hover pill's padding on either side so the
        // separators around it sit where they do around a Crumb.
        leftPadding: Theme.scaled(8)
        rightPadding: Theme.scaled(8)
        readonly property real naturalWidth: Math.ceil(implicitWidth) + 1
        Layout.fillWidth: true
        Layout.preferredWidth: naturalWidth
        Layout.maximumWidth: naturalWidth

        HoverHandler { id: contextHover }
        ToolTip.visible: contextHover.hovered && context.truncated
        ToolTip.text: context.text
    }

    Sep { visible: root.stickLabel.length > 0 }

    // The stick. The segment that gives way first: it is the one the
    // reader can most easily do without, having picked it on Home a
    // moment ago. With a floor, though. Squeezed to zero it left "Home >
    // > Clean Up Duplicates" -- a gap and a dangling separator, which
    // reads as a bug rather than as an abbreviation. A few characters and
    // an ellipsis still say a name was here.
    Context {
        id: stickText
        objectName: "stickSegment"
        visible: root.stickLabel.length > 0
        text: root.stickLabel
        Layout.minimumWidth: naturalWidth - root.stickShortfall
    }

    Sep { visible: root.middleLabel.length > 0 }

    Crumb {
        id: middleCrumb
        objectName: "middleLink"
        visible: root.middleClickable
        // Gives way second. "Home > Hou... > Clean Up Duplicates" tells a
        // reader what page they are on; "Home > Housekeeping > Clea..."
        // tells them where it sits and leaves them guessing what it is.
        // The middle is also the one they can most easily infer, being
        // one click behind them. Same floor as the stick, for the same
        // reason.
        Layout.fillWidth: true
        Layout.minimumWidth: naturalWidth - root.middleShortfall
        text: root.middleLabel
        onClicked: root.backRequested()
        // Names itself in full when it has been shortened -- an
        // abbreviation the reader cannot expand is just a missing word.
        ToolTip.text: !root.backEnabled ? root.backDisabledTooltip
            // contentItem is the Loader; the Label that elides is its item.
            : (contentItem.item && contentItem.item.truncated) ? (root.middleLabel + ": back to it")
            : ("Back to " + root.middleLabel)
    }

    // The same segment when it leads nowhere new.
    Context {
        id: middleText
        objectName: "middleSegment"
        visible: root.middleLabel.length > 0 && !root.middleClickable
        text: root.middleLabel
        Layout.minimumWidth: naturalWidth - root.middleShortfall
    }

    Sep {}

    // The page's own name. fillWidth as well, because measurement says
    // an item without it does not shrink here at all -- a minimum of 0
    // is not enough on its own, and the title kept its full 307px
    // inside a 345px row and simply hung out of it.
    //
    // It gives way last, and only to 120 against the others' 64: the
    // page's own name is still readable when both of them have been
    // spent. See `shortfall` above for how the order is enforced.
    PageTitle {
        id: titleText
        objectName: "titleSegment"
        text: root.title
        level: "crumb"
        elide: Text.ElideRight
        readonly property real naturalWidth: Math.ceil(implicitWidth) + 1
        Layout.fillWidth: true
        Layout.preferredWidth: naturalWidth
        Layout.minimumWidth: naturalWidth - root.titleShortfall

        // Same bargain as the middle segment: it may be shortened, but
        // only if hovering it gives the whole name back.
        HoverHandler { id: titleHover }
        ToolTip.visible: titleHover.hovered && titleText.truncated
        ToolTip.text: root.title
    }
}
