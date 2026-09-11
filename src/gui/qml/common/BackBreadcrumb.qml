import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Replaces the old "‹" ToolButton + separate PageTitle pair every
// section page's header used to duplicate. Up to three segments:
// "⌂ › [middle] › this page", where ⌂ always jumps straight back
// to the StackView's very first item in one click (pop(null), not a
// single pop()) no matter how deep the current page sits -- the middle
// segment, when present, is one level up (the stick, for a page pushed
// directly from Home, or the hub page, for a page nested inside one).
// Every clickable segment uses Theme.rowHover/rowPressed -- the same
// tint tokens list rows already use -- rather than inventing its own
// hover color, so this is also the fix for hover feedback being
// inconsistent button-to-button across the app: one shared
// background/contentItem means every page's back affordance now hovers
// identically.
RowLayout {
    id: root
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
    signal homeRequested()
    signal backRequested()

    spacing: 4 * Theme.iconScale
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
        Layout.preferredWidth: Math.ceil(implicitWidth) + 1
        Layout.maximumWidth: Math.ceil(implicitWidth) + 1

        ToolTip.visible: hovered

        leftPadding: 8 * Theme.iconScale
        rightPadding: 8 * Theme.iconScale
        topPadding: 4 * Theme.iconScale
        bottomPadding: 4 * Theme.iconScale

        background: Rectangle {
            radius: 4 * Theme.iconScale
            color: crumb.pressed ? Theme.rowPressed
                : crumb.hovered ? Theme.rowHover
                : "transparent"
        }
        // Set instead of `text` to draw a symbol rather than a word. The
        // family is the app's monochrome-glyph convention (see
        // ActionCard.qml's cardIconFont and StickListPage's eject
        // button): without it the system's color-emoji font gets first
        // refusal and the crumb comes out full-color.
        property string glyph: ""
        contentItem: Label {
            text: crumb.glyph.length > 0 ? crumb.glyph : crumb.text
            font.family: crumb.glyph.length > 0 ? "Noto Sans Symbols2" : Theme.titleFamily
            font.weight: Theme.titleWeight
            // The glyph keeps the bigger step; the words step down. See
            // Theme.titleCrumb for why the two part company here.
            font.pointSize: crumb.glyph.length > 0 ? Theme.titleMedium * 1.1 : Theme.titleCrumb
            color: Theme.textMuted
            opacity: crumb.enabled ? 1.0 : 0.5
            elide: Text.ElideRight
        }
    }

    component Sep: Label {
        text: "›"
        color: Theme.textMuted
        font.pointSize: Theme.titleCrumb
    }

    // A house, not the word "Home". The word cost this row about four
    // characters of width on every page that has a breadcrumb, and the
    // row it was spending them on is the one whose middle segment --
    // usually the stick's name -- gives way first when the header runs
    // out of room. U+2302 rather than the 🏠 emoji: it is already in
    // the UI fonts on every platform this ships to, so it needs no font
    // fallback to come out flat.
    Crumb {
        glyph: "⌂"
        onClicked: root.homeRequested()
        ToolTip.text: root.backEnabled ? "Back to Home" : root.backDisabledTooltip
    }

    Sep { visible: root.middleLabel.length > 0 }

    Crumb {
        visible: root.middleClickable
        // The segment that gives way first. "Home > Hou... > Clean Up
        // Duplicates" tells a reader what page they are on; "Home >
        // Housekeeping > Clea..." tells them where it sits and leaves
        // them guessing what it is. The middle is also the one they can
        // most easily infer, being one click behind them.
        //
        // With a floor, though. Squeezed to zero it left "Home >  >
        // Clean Up Duplicates" -- a gap and a dangling separator, which
        // reads as a bug rather than as an abbreviation. A few
        // characters and an ellipsis still say a name was here.
        Layout.fillWidth: true
        Layout.minimumWidth: 64 * Theme.iconScale
        text: root.middleLabel
        onClicked: root.backRequested()
        // Names itself in full when it has been shortened -- an
        // abbreviation the reader cannot expand is just a missing word.
        ToolTip.text: !root.backEnabled ? root.backDisabledTooltip
            : contentItem.truncated ? (root.middleLabel + " -- back to it")
            : ("Back to " + root.middleLabel)
    }

    // The same segment when it leads nowhere new: context, not a link.
    // No hover pill and no click, but it still elides last-but-one and
    // still says its full name on hover when it has had to.
    Label {
        id: middleText
        visible: root.middleLabel.length > 0 && !root.middleClickable
        text: root.middleLabel
        font.family: Theme.titleFamily
        font.weight: Theme.titleWeight
        font.pointSize: Theme.titleCrumb
        color: Theme.textMuted
        elide: Text.ElideRight
        // Matches the hover pill's padding on either side so the
        // separators around it sit where they do around a Crumb.
        leftPadding: 8 * Theme.iconScale
        rightPadding: 8 * Theme.iconScale
        Layout.fillWidth: true
        Layout.minimumWidth: 64 * Theme.iconScale
        Layout.preferredWidth: Math.ceil(implicitWidth) + 1
        Layout.maximumWidth: Math.ceil(implicitWidth) + 1

        HoverHandler { id: middleHover }
        ToolTip.visible: middleHover.hovered && middleText.truncated
        ToolTip.text: root.middleLabel
    }

    Sep {}

    // The page's own name. fillWidth as well, because measurement says
    // an item without it does not shrink here at all -- a minimum of 0
    // is not enough on its own, and the title kept its full 307px
    // inside a 345px row and simply hung out of it.
    //
    // Priority between the two shrinkable segments is expressed as
    // floors rather than as order: the middle gives way to 64 and the
    // title only to 120, so the middle is spent first and the page's own
    // name is still readable when it is.
    PageTitle {
        id: titleText
        text: root.title
        level: "crumb"
        elide: Text.ElideRight
        Layout.fillWidth: true
        Layout.minimumWidth: 120 * Theme.iconScale

        // Same bargain as the middle segment: it may be shortened, but
        // only if hovering it gives the whole name back.
        HoverHandler { id: titleHover }
        ToolTip.visible: titleHover.hovered && titleText.truncated
        ToolTip.text: root.title
    }
}
