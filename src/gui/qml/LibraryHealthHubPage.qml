import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's front page: run every check this stick's library can be
// put through, then say what each one found in a sentence or two.
//
// The scan happens on arrival rather than behind a button. Checking is what
// this page is for, and a page whose first state is "press here to find
// out" makes the user ask for something they already asked for by opening
// it. It can take a while on a real stick -- reading three catalogs and
// stat-ing a few thousand files -- so each card says what it is waiting
// for, and the ones that finish early report early.
//
// Detail lives behind each card, not on it. The old single page put every
// broken row and every stray cue in one scroll, which answered "what
// exactly is wrong with row 412" well and "is my library alright" badly.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var playbackController

    // Opens the detailed view, scrolled to the section that matters for
    // the card the user pressed.
    signal detailRequested(string section)

    LibraryConsistencyController {
        id: healthController
    }
    // Handed to the detail page so it shows this scan instead of running
    // its own. Named apart from the id: a property and an id of the same
    // name collide, and the binding would refer to itself.
    readonly property var consistencyController: healthController

    readonly property bool scanning: healthController.busy
    readonly property bool scanned: internal.hasScanned && !root.scanning

    QtObject {
        id: internal
        property bool hasScanned: false
    }

    Component.onCompleted: root.runChecks()

    function runChecks() {
        internal.hasScanned = false;
        healthController.scan(root.rekordboxPath, root.enginePath);
    }

    Connections {
        target: healthController
        function onBusyChanged() {
            if (!healthController.busy) {
                internal.hasScanned = true;
            }
        }
    }

    // --- what each check found, as a sentence -------------------------
    //
    // Written per check rather than from a shared template: "27 rows" and
    // "27 cues" need different sentences, and a template that fits both
    // fits neither well.

    readonly property int brokenCount: healthController.issues.count
    readonly property int repairableCount: healthController.repairableCount
    readonly property int junkCueCount: healthController.junkCues.count

    readonly property string brokenSummary: {
        if (root.scanning) {
            return "Checking every row in every catalog against the files on the stick"
                + (healthController.scanningFormat.length > 0
                    ? " (" + healthController.scanningFormat + ")..." : "...");
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (root.brokenCount === 0) {
            return "Every track in every catalog on this stick points at a file that is really there.";
        }
        let text = root.brokenCount === 1
            ? "One track points at a file that is no longer on the stick."
            : root.brokenCount + " tracks point at a file that is no longer on the stick.";
        if (root.repairableCount > 0) {
            text += " " + (root.repairableCount === root.brokenCount ? "All of them" : root.repairableCount + " of them")
                 + " have a healthy copy elsewhere in the same catalog and can be repaired automatically;"
                 + " the rest need a decision.";
        } else {
            text += " None of them has a healthy copy to repair from, so each needs a decision.";
        }
        return text;
    }

    readonly property string junkCueSummary: {
        if (root.scanning) {
            return "Looking for memory cues sitting at the very start of a track...";
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (root.junkCueCount === 0) {
            return "No memory cues are sitting at 0:00.";
        }
        return (root.junkCueCount === 1 ? "One memory cue sits" : root.junkCueCount + " memory cues sit")
             + " at 0:00. These are almost always accidental -- a stray press while the track was at the"
             + " start -- rather than something you placed on purpose.";
    }

    // The same header every other section page uses. This one had the
    // breadcrumb as the bare header, which is why it looked wrong in two
    // ways at once: no toolbar background or margin above the content,
    // and the crumbs spread across the full width, because a Page
    // stretches its header and a RowLayout with nothing to absorb the
    // slack hands it to the gaps between segments. The trailing filler
    // is what keeps them packed to the left.
    header: ToolBar {
        // Opaque background override: KDE's Breeze style bleeds the
        // window behind Seabass through an unstyled ToolBar.
        background: Rectangle { color: Theme.surface }
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.rowSpacing
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Library Health"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                running: root.scanning
                visible: root.scanning
                implicitWidth: Theme.iconSizeSmall
                implicitHeight: Theme.iconSizeSmall
            }
        }
    }

    PageScrollView {
        objectName: "healthScroll"
        anchors.fill: parent
        // Every other page that uses PageScrollView insets its content
        // by this much. This one did not, so its cards ran to the window
        // edge while the breadcrumb above them kept its own spacing, and
        // the page read as broken rather than as tight.
        anchors.margins: Theme.pageMargin

        ColumnLayout {
            objectName: "healthColumn"
            width: parent.width
            spacing: 14

            Subtitle {
                Layout.fillWidth: true
                text: root.scanning
                    ? "Checking this library. This can take a minute on a full stick."
                    : "Everything Seabass can check about this library, and what it found."
            }

            HealthCheckCard {
                objectName: "brokenFilesCard"
                title: "Tracks and their files"
                summary: root.brokenSummary
                running: root.scanning
                ok: root.brokenCount === 0
                actionLabel: root.brokenCount > 0 ? "Review these tracks" : ""
                onActionRequested: root.detailRequested("broken")
            }

            HealthCheckCard {
                objectName: "junkCuesCard"
                title: "Memory cues at 0:00"
                summary: root.junkCueSummary
                running: root.scanning
                ok: root.junkCueCount === 0
                actionLabel: root.junkCueCount > 0 ? "Review these cues" : ""
                onActionRequested: root.detailRequested("junkcues")
            }

            // The rekordbox/OneLibrary comparison is specified in
            // docs/library-health-format-divergence.md and not built yet.
            // Deliberately not shown as a card until it can actually
            // report something: an empty check that always says "not
            // checked" teaches people to ignore the page.

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: root.scanned
                text: "Checked " + root.stickLabel + " just now."
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
            }

            Button {
                objectName: "recheckButton"
                Layout.alignment: Qt.AlignLeft
                visible: root.scanned
                text: "Check again"
                onClicked: root.runChecks()
            }

            Label {
                objectName: "errorLabel"
                Layout.fillWidth: true
                visible: healthController.errorMessage.length > 0
                text: healthController.errorMessage
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
        }
    }
}
