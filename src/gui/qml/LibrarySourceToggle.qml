// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Browse Library's own three-way catalog switch -- Engine OS, Rekordbox's
// classic per-USB Device Library, and Rekordbox 7's newer OneLibrary
// (Device Library Plus). Deliberately separate from FormatToggle.qml
// (used everywhere else): that's a persisted, binary Rekordbox/Engine
// setting shared by Clean Up, Sync, and Local Cue Backup, none of which
// have an "onelibrary" write path -- this is browse-only, scoped to this
// one page, and not persisted. See AboutPage.qml for the fuller
// explanation of what distinguishes all three.
//
// A combo box rather than the three side-by-side buttons this used to be.
// Three buttons cost the header three labels' worth of width permanently,
// on a row that also carries the breadcrumb -- and the breadcrumb was what
// gave way, eliding the stick's name to pay for two catalogs the reader
// isn't currently looking at. The picker now states the one that is and
// costs one label.
ComboBox {
    id: root
    property string current: "rekordbox"
    property bool hasRekordbox: true
    property bool hasEngine: true
    property bool hasOneLibrary: true
    // Distinct from hasOneLibrary: a page can have a real OneLibrary
    // export present (hasOneLibrary true) but still not support it for
    // the specific operation this toggle drives -- e.g. CleanupPage's
    // destructive removal, which needs OneLibraryCueWriter to reassign a
    // removed track's playlist membership to the survivor and doesn't
    // yet (see cleanup_controller.cpp's own comment). Defaults to
    // mirroring hasOneLibrary, so a page that never sets this keeps the
    // exact same enabled/disabled behavior as before this existed.
    property bool oneLibrarySupported: hasOneLibrary
    // Overrides the disabled tooltip specifically for the "present but
    // not supported here" case above -- the default tooltip below
    // ("Not present on this export") would be actively wrong there.
    property string oneLibraryUnsupportedReason: ""
    // Not named currentChanged -- `property string current` already
    // auto-generates that signal (with no arguments) for its own change
    // notification; declaring another signal with the same name here
    // would collide with it.
    signal sourceRequested(string value)

    // Glyphs are original/generic, not reproductions of either company's
    // real logo -- same principle FormatToggle.qml's own comment states.
    // The tooltips name the vendor and, for the two Rekordbox catalogs,
    // which of them is the newer: a reader picking between DeviceLibrary
    // and OneLibrary needs to tell them apart, and the on-disk file names
    // they used to quote answered a question nobody browsing a library is
    // asking. AboutPage.qml still carries the full story, file names and
    // all, for anyone who wants it.
    readonly property var entries: [
        {
            value: "engine",
            glyph: "⬡",
            label: "Engine OS",
            selectable: root.hasEngine,
            tooltip: "Denon's own library, read directly by Engine DJ hardware "
                + "(SC5000, Prime series, ...)."
        },
        {
            value: "rekordbox",
            glyph: "◎",
            label: "DeviceLibrary",
            selectable: root.hasRekordbox,
            tooltip: "Pioneer's classic per-stick export, read directly by CDJs "
                + "and XDJs. The older of the two Rekordbox catalogs, and every "
                + "Rekordbox export has it."
        },
        {
            value: "onelibrary",
            glyph: "◈",
            label: "OneLibrary",
            selectable: root.hasOneLibrary && root.oneLibrarySupported,
            // A plain call, not an inline block: an object literal's value
            // is an expression, and the three-branch version this used to
            // be only parsed because it sat directly in a QML binding.
            // The binding still re-evaluates when any property the
            // function touches changes -- dependency capture follows the
            // call.
            tooltip: root.oneLibraryTooltip()
        }
    ]

    function oneLibraryTooltip() {
        if (!root.hasOneLibrary) {
            return "Not present on this export: OneLibrary only exists on "
                + "newer Rekordbox exports.";
        }
        if (!root.oneLibrarySupported && root.oneLibraryUnsupportedReason.length > 0) {
            return root.oneLibraryUnsupportedReason;
        }
        return "Pioneer's newer catalog, written by Rekordbox 7. Mirrors "
            + "DeviceLibrary's tracks in a richer schema, and you can add "
            + "cues to it directly.";
    }

    // A catalog's glyph beside its name, both centred in the row as the
    // name always was, and the glyph then moved so its ink is centred on
    // the name's capitals. The glyphs come from Noto Sans Symbols2 (bundled,
    // see Theme.symbolFamily), whose metrics are not the UI font's:
    // centring the two text boxes left every
    // glyph 1.5 to 2.5 px above the name, and sitting both on one baseline
    // instead lifted the whole text about 2 px in the box (both measured in
    // tst_LibrarySourceToggle.qml).
    component CatalogGlyph: Label {
        id: glyphLabel
        required property Item nameLabel
        font.family: Theme.symbolFamily
        color: Theme.text
        Layout.alignment: Qt.AlignVCenter
        // The translate below is an approximation, and deliberately left
        // as one. It comes from TextMetrics, which reports UNHINTED
        // metrics, while the app paints with NativeRendering on Linux
        // (main.cpp), which hints each glyph onto the pixel grid as it
        // goes -- so the ink lands a fraction away from where the sum
        // says it will.
        //
        // This glyph and its name were pinned to Text.QtRendering for a
        // while to close that gap, on a reading of a test that said the
        // pair was 4.5 px out of line. That number was the measurement,
        // not the drawing: the test took the first and last row of
        // pixels clearing a contrast threshold, and under NativeRendering
        // most of this hexagon's faint outline fell below it, so the band
        // collapsed to a sliver at the bottom of the glyph. Measured by
        // ink centroid the pair sits 0.65 px apart painted the way the
        // rest of the app is painted, and 0.36 px apart pinned: a third
        // of a pixel, for the cost of being the only labels in Seabass
        // rasterised differently from every other one. The pins came out
        // again on 2026-09-21 and the approximation stays.
        //
        // What holds it honest is tst_LibrarySourceToggle.qml, which
        // measures the painted ink of both the header and the list rows
        // and allows them 1 px. Deleting this translate moves them 3 px
        // and fails.
        TextMetrics { id: glyphInk; font: glyphLabel.font; text: glyphLabel.text }
        TextMetrics { id: capitalInk; font: glyphLabel.nameLabel.font; text: "H" }
        // A transform, not a position: it moves the ink without asking the
        // layout for anything, so this cannot feed back into the y it reads.
        transform: Translate {
            y: (glyphLabel.nameLabel.y + glyphLabel.nameLabel.baselineOffset
                + capitalInk.tightBoundingRect.y + capitalInk.tightBoundingRect.height / 2)
               - (glyphLabel.y + glyphLabel.baselineOffset
                  + glyphInk.tightBoundingRect.y + glyphInk.tightBoundingRect.height / 2)
        }
    }

    model: entries
    textRole: "label"
    valueRole: "value"

    // One-way: the page owns `current` and hands it back changed (or not)
    // after its own checks, so the selection can never run ahead of the
    // catalog actually being shown. indexOfValue needs the model, hence
    // the guard for the moment before it exists.
    currentIndex: count > 0 ? indexOfValue(root.current) : -1

    // Every route to a selection ends here, because the page -- not this
    // control -- owns which catalog is shown. A disabled entry stays
    // clickable precisely so its tooltip can say why it is unavailable
    // (see the delegate), which is why the check lives here rather than
    // in `enabled`.
    function selectEntry(index) {
        const entry = root.entries[index];
        // Closed even when the answer is no. The entry is deliberately
        // still live, so without this a click on an unavailable catalog
        // did nothing whatsoever -- list still open, nothing moved --
        // which reads as a hung control rather than as a refusal.
        root.popup.close();
        if (!entry || !entry.selectable) {
            return;
        }
        if (entry.value !== root.current) {
            root.sourceRequested(entry.value);
        }
    }

    // Arrow keys on a closed ComboBox are handled in C++
    // (incrementCurrentIndex/decrementCurrentIndex): they write
    // currentIndex directly, never reaching selectEntry(). Left alone,
    // Down on a picker showing DeviceLibrary relabelled the header
    // "OneLibrary" -- emitting no sourceRequested, so the page went on
    // showing DeviceLibrary -- and did it even where OneLibrary is not on
    // the stick at all. So the keys are taken over here and routed the
    // same way a click is, skipping catalogs this export does not have.
    Keys.onPressed: (event) => {
        if (root.popup.visible) {
            return;
        }
        if (event.key === Qt.Key_Down) {
            root.stepTo(1);
            event.accepted = true;
        } else if (event.key === Qt.Key_Up) {
            root.stepTo(-1);
            event.accepted = true;
        }
    }

    // Return/Enter inside the open popup is the ComboBox's own path, not
    // the delegate's onClicked, so it needs routing too.
    onActivated: (index) => root.selectEntry(index)

    function stepTo(delta) {
        for (var n = 1; n <= root.entries.length; ++n) {
            const next = root.currentIndex + delta * n;
            if (next < 0 || next >= root.entries.length) {
                return;
            }
            if (root.entries[next].selectable) {
                root.selectEntry(next);
                return;
            }
        }
    }

    // The backstop for anything else that writes currentIndex from C++.
    // What is displayed mirrors `current` or the control is lying about
    // which catalog the page is showing.
    onCurrentIndexChanged: {
        if (root.count === 0) {
            return;
        }
        const want = root.indexOfValue(root.current);
        if (want >= 0 && root.currentIndex !== want) {
            root.currentIndex = want;
        }
    }

    ToolTip.visible: hovered && !popup.visible && ToolTip.text.length > 0
    ToolTip.text: {
        const entry = root.entries[root.currentIndex];
        return entry ? entry.tooltip : "";
    }

    contentItem: RowLayout {
        spacing: 4
        CatalogGlyph {
            objectName: "catalogGlyph"
            text: {
                const entry = root.entries[root.currentIndex];
                return entry ? entry.glyph : "";
            }
            nameLabel: currentName
            leftPadding: 8
        }
        Label {
            id: currentName
            objectName: "catalogName"
            text: {
                const entry = root.entries[root.currentIndex];
                return entry ? entry.label : "";
            }
            color: Theme.text
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    // The style's own indicator does not survive the background override
    // below (and on this KDE system it draws nothing legible against the
    // app's palette anyway), so the control drew as a bordered box with a
    // word in it -- a text field, as far as a reader is concerned, with
    // nothing saying the other two catalogs are behind it. Breeze's
    // arrow, the same one the expandable rows use.
    rightPadding: 24
    indicator: SeabassIcon {
        x: root.width - width - 8
        y: Theme.snap(root.topPadding + (root.availableHeight - height) / 2)
        iconName: "arrow-down"
        size: Theme.iconSizeSmall * 0.5
        color: Theme.textMuted
    }

    // Explicit background rather than the active style's default -- on
    // this KDE system that resolves to org.kde.breeze regardless of the
    // app's own Material palette (same issue already worked around for
    // ToolBar elsewhere) and draws square corners, unlike every other
    // radius: 4 element this app draws itself.
    background: Rectangle {
        implicitWidth: 160
        radius: 4
        color: root.hovered ? Theme.rowHover : "transparent"
        border.color: Theme.border
        border.width: 1
    }

    delegate: ItemDelegate {
        id: entryDelegate
        required property int index
        required property var modelData
        width: ListView.view ? ListView.view.width : implicitWidth
        highlighted: root.currentIndex === index
        hoverEnabled: true

        // Deliberately NOT `enabled: modelData.selectable`. A disabled
        // item takes no input at all in Qt Quick, tooltip included, and
        // the one thing a reader wants from an unavailable catalog is the
        // sentence saying why it is unavailable. So it stays live, looks
        // spent, and declines the click in selectEntry().
        opacity: modelData.selectable ? 1.0 : 0.5

        ToolTip.visible: hovered && ToolTip.text.length > 0
        ToolTip.text: modelData.tooltip
        ToolTip.delay: 400

        // The row paints its own ground, for the same reason the field
        // below carries an explicit background: what the style draws
        // there comes from the PLATFORM's palette, while every colour in
        // this app comes from Theme, which is an always-dark palette
        // unless the user asks for the system one. Where the two
        // disagree the text is drawn on a surface Theme never chose.
        //
        // macOS is where that showed: Theme's #e8ecef text on the
        // style's white popup, fourteen levels of contrast on the tinted
        // row and twenty on the plain ones. Not a pixel-threshold
        // argument -- a white list with three ghost-grey labels on it,
        // unreadable, which anybody running Seabass on a Mac in light
        // appearance would see. The test that found it was measuring
        // where the glyph sat, and could not see the ink at all.
        background: Rectangle {
            color: entryDelegate.highlighted
                   ? Theme.rowPressed
                   : (entryDelegate.hovered ? Theme.rowHover : Theme.surface)
        }

        contentItem: RowLayout {
            spacing: 4
            CatalogGlyph {
                // Named so the pixel test can find this pair in the open
                // list as well: the row is a second use of CatalogGlyph,
                // and what the closed control does says nothing about it.
                objectName: "entryGlyph"
                text: entryDelegate.modelData.glyph
                nameLabel: entryName
            }
            Label {
                id: entryName
                objectName: "entryName"
                text: entryDelegate.modelData.label
                color: Theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        onClicked: root.selectEntry(index)
    }
}
