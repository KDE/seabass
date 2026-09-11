import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var appSettingsController
    required property var playbackController

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    readonly property string format: {
        var pref = appSettingsController.preferredFormat;
        if (pref === "engine" && hasEngine) return "engine";
        if (pref === "rekordbox" && hasRekordbox) return "rekordbox";
        return hasEngine ? "engine" : "rekordbox";
    }
    function currentPath() {
        return root.format === "engine" ? root.enginePath : root.rekordboxPath;
    }
    function formatLabel(format) { return FormatLabels.label(format); }
    readonly property bool hasOneLibrary: root.hasRekordbox && cleanupController.hasOneLibrary(root.rekordboxPath)

    CleanupController {
        id: cleanupController
    }

    // Escape backs out of staging, the way Escape backs out of anything
    // else not yet committed. Only unstages -- it never leaves the page
    // and never touches the stick, since nothing staged has been written
    // yet. Deliberately no confirmation: unstaging loses no work, the
    // checkboxes keep their state, and pressing Stage again restores it.
    Shortcut {
        sequence: StandardKey.Cancel
        enabled: cleanupController.stagedCount > 0 && !cleanupController.writing
        onActivated: cleanupController.unstageAll()
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        feature: "cleanup"
        // "Save" is right on a page that edits one thing; here the
        // button is the moment a stack of removals becomes real, and
        // saying so is worth more than consistency with pages whose
        // save is reversible in one step.
        saveLabel: "Clean Up"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    // "" scopes the review to the whole library; a real name narrows it
    // to one playlist. Distinct from the search box below, which filters
    // the groups already found -- this one changes what is scanned, so
    // it re-runs the scan.
    property string selectedPlaylistName: ""

    readonly property var playlistPickerModel: [{name: "All tracks", count: ""}].concat(
        cleanupController.playlistNames.map((n) => ({name: n, count: ""})))

    function rescanInScope() {
        cleanupController.scan(root.format, root.currentPath(), root.selectedPlaylistName, "");
    }

    Component.onCompleted: root.rescanInScope()
    onFormatChanged: root.rescanInScope()

    function formatDuration(ms) {
        var totalSeconds = Math.round(ms / 1000);
        var m = Math.floor(totalSeconds / 60);
        var s = totalSeconds % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        // Opaque background override, see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar). This page is the one that made the
        // bug visible: with real "pending deletions" content behind a
        // translucent header, System Settings text bled through the top
        // strip of the window.
        background: Rectangle { color: Theme.surface }

        implicitHeight: headerLayout.implicitHeight + 20

        ColumnLayout {
            id: headerLayout
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                BackBreadcrumb {
                    stack: root.StackView.view
                    middleLabel: "Housekeeping"
                    title: "Clean Up Duplicates"
                    backEnabled: !cleanupController.writing
                    onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                    onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
                }
                Item { Layout.fillWidth: true }
                // No library-type toggle here any more. A cleanup save
                // now writes every catalog that lists the file, so the
                // format only ever chose which catalog was scanned for
                // duplicates -- a distinction with no consequence the
                // user could act on, presented as a choice they had to
                // make before they could start. The global preference in
                // Preferences still decides it.
            }

            // What this page is for, in three sentences. Real libraries
            // accumulate several files of one track through repeated
            // exports, and someone about to let a tool merge their cue
            // points deserves to know what it considers a duplicate
            // before they trust a checkbox. The third sentence matters
            // most: this page consolidates catalog rows and records what
            // it orphaned, it does NOT delete audio -- "Delete Orphaned
            // Files" does that, and saying so here stops the space
            // figures above reading as a promise this page keeps.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: "Exporting the same track more than once leaves several copies of it on the stick, "
                        + "each catalogued separately and each taking up space. In this step Seabass groups "
                        + "the copies that agree on artist, track title and length, and consolidates their "
                        + "metadata -- cue points, ratings, playlist membership -- onto the single copy it "
                        + "keeps. No audio is deleted here: the files this leaves unneeded are removed "
                        + "afterwards under \"Delete Orphaned Files\", which is where the space is actually "
                        + "freed."
                }
                InfoButton {
                    explanationTitle: "What counts as a duplicate?"
                    summaryText: "Same artist, same title, same length (within two seconds). "
                        + "Filenames are ignored, because a re-export renames the same recording."
                    explanationText:
                          "## Why filenames are ignored\n"
                        + "A re-export writes the same recording out under a new name: the leading "
                        + "track number follows playlist position, and a copy landing beside an "
                        + "existing file gets `-1` or `-2` appended.\n\n"
                        + "- `05_Kollektiv Turmstrasse-Flaschenpost.mp3`\n"
                        + "- `21_Kollektiv Turmstrasse-Flaschenpost.mp3`\n"
                        + "- `33_Kollektiv Turmstrasse-Flaschenpost.mp3`\n\n"
                        + "One track, exported three times. The numbering is an artifact.\n\n"
                        + "## Why length matters\n"
                        + "It is what stops a real mistake. A radio edit and an extended mix share "
                        + "artist and title, so matching on those alone would offer to delete one of "
                        + "them. Paul Kalkbrenner's *No Goodbye* is here as both a 2:47 edit and a "
                        + "6:31 extended mix. Those are never grouped.\n\n"
                        + "Where a catalog recorded no length, Seabass reads it from the audio and "
                        + "remembers it on the stick, so only the first scan pays for it. A track "
                        + "whose length cannot be established is left alone rather than guessed at.\n\n"
                        + "## What is kept\n"
                        + "- **Cues** are merged, never lost -- the survivor gets every copy's cues\n"
                        + "- **Playlist membership** is preserved in every catalog\n"
                        + "- **Missing bpm, key and artwork** are filled in from whichever copy has them\n\n"
                        + "Use *what's conserved* on any group to see exactly what the surviving copy "
                        + "would end up with.\n\n"
                        + "## What is not kept\n"
                        + "**Play counts.** Each application counts for itself -- rekordbox keeps a "
                        + "running total, Engine remembers only when you last played a track -- so "
                        + "there is no honest way to combine them across libraries. Within one "
                        + "library adding them up would be right, but no format Seabass writes lets "
                        + "it set a play count.\n\n"
                        + "**Ratings and comments are different**: those are never discarded without "
                        + "asking. A group whose copies disagree on either is left unchecked for you "
                        + "to decide, as is one where the copies differ in a way that might be "
                        + "deliberate.\n"
                }
            }

            // The files no catalog references -- see the component for
            // why this never shows a count without its basis.
            UnreferencedFilesNotice {
                Layout.fillWidth: true
                info: cleanupController.unreferencedFiles
            }

            // What all this actually buys, drawn against the stick's real
            // capacity. A byte count alone says nothing about whether it
            // matters; the same figure as a block on a nearly-full stick
            // says it immediately.
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: plansListView.count > 0 && spaceBar.known
                implicitHeight: spaceBar.implicitHeight + 28
                color: Theme.groupBackground
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 4

                SpaceReclaimBar {
                    id: spaceBar
                    anchors.fill: parent
                    anchors.margins: 14
                    totalBytes: cleanupController.stickTotalBytes
                    freeBytes: cleanupController.stickFreeBytes
                    reclaimBytes: cleanupController.includedWastedBytes
                    reclaimableBytes: cleanupController.totalWastedBytes
                }
            }

            // Flow, not RowLayout: a row keeps every child at its natural
            // width and simply runs off the edge of a narrow window,
            // which is how the playlist picker and the group count came
            // to be invisible rather than merely cramped. A Flow wraps
            // onto a second line instead.
            //
            // Layout.minimumWidth: 0 is what makes that true, and without
            // it the Flow was the widest thing on the page at every
            // window size. A Flow's implicitWidth is its children laid
            // out on ONE line -- the unwrapped width, 701px here -- and a
            // ColumnLayout will not shrink a child below its implicit
            // width unless a minimum says it may. So the Flow was handed
            // 701 whatever the window did, never reached its own wrap
            // point, and overflowed instead. Measured, not reasoned:
            // tst_CleanupPage renders the page at 960, 700, 520 and 380
            // and the row was 701 wide at all four.
            //
            // Everything inside sizes with `width:`. Layout.* attached
            // properties do nothing here -- a Flow is a positioner, it
            // places children and never sizes them, and it does not read
            // them at all.
            Flow {
                id: filterRow
                objectName: "filterRow"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 12
                Label {
                    text: "Playlist:"
                    color: Theme.textMuted
                    anchors.verticalCenter: undefined
                }
                PlaylistPickerCombo {
                    width: Math.max(160, Math.min(260, root.width * 0.28))
                    enabled: !cleanupController.busy && !cleanupController.writing
                    model: root.playlistPickerModel
                    currentIndex: {
                        if (root.selectedPlaylistName.length === 0) {
                            return 0;
                        }
                        for (var i = 1; i < root.playlistPickerModel.length; i++) {
                            if (root.playlistPickerModel[i].name === root.selectedPlaylistName) {
                                return i;
                            }
                        }
                        return 0;
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Clean up one playlist instead of the whole library"
                    onPlaylistPicked: (index, modelData) => {
                        root.selectedPlaylistName = index === 0 ? "" : modelData.name;
                        root.rescanInScope();
                    }
                }
                TextField {
                    id: searchField
                    placeholderText: "Search title or artist..."
                    // Proportional with a cap and a floor, matching the
                    // playlist picker above it. The floor is what lets a
                    // genuinely narrow window still show a usable field
                    // rather than a sliver.
                    width: Math.max(110, Math.min(280, root.width * 0.3))
                    onTextChanged: cleanupController.search(text)
                }
                Label {
                    text: plansListView.count + " duplicate group(s) found"
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    // Natural width until it would not fit on a line of
                    // its own, then elided. Eliding needs a width to
                    // elide within; without one the label just grows.
                    //
                    // Bound to the page, never to the Flow. A child of a
                    // Flow that sizes itself from the Flow's width is a
                    // loop -- the Flow's width comes from its children --
                    // and Qt breaks a loop by leaving a stale number,
                    // which is a frozen row rather than an error.
                    width: Math.min(implicitWidth, root.width - 2 * Theme.pageMargin)
                }
                Label {
                    visible: plansListView.count > 0
                    // "4.2 GB total if every copy kept only one file" was
                    // a sentence in a status line. The page is about
                    // duplicates; what the number means is already the
                    // subject.
                    text: "(" + cleanupController.totalWastedBytesHuman + " reclaimable)"
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, root.width - 2 * Theme.pageMargin)
                }
                Label {
                    visible: cleanupController.stagedCount > 0
                    text: cleanupController.stagedCount + " staged, not saved yet"
                    color: Theme.warnText
                }
                Label {
                    visible: cleanupController.includedCount > 0
                    text: cleanupController.includedCount + " group(s) selected"
                    color: Theme.textMuted
                }
            }

            // A Flow, not a RowLayout: on a narrower window these four
            // buttons plus the toggle above no longer all fit on one
            // line, and a RowLayout just lets the trailing ones overflow
            // past the header's edge instead of wrapping onto a second
            // line the way this does.
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    text: "Select All"
                    enabled: !cleanupController.busy && plansListView.count > 0
                    onClicked: confirmSelectAllDialog.open()
                }
                Button {
                    text: "Deselect All"
                    enabled: !cleanupController.busy && plansListView.count > 0
                    onClicked: cleanupController.setAllIncluded(false)
                }
                Button {
                    text: "Stage Selected for Deletion"
                    enabled: !cleanupController.busy && !cleanupController.writing && cleanupController.includedCount > 0
                    ToolTip.visible: hovered
                    ToolTip.text: "Stage cleaning up every checked group; Save writes them"
                    onClicked: confirmCleanupDialog.open()
                }
                Button {
                    text: "Undo Last Save"
                    visible: cleanupController.canUndo
                    enabled: !cleanupController.busy && !cleanupController.writing
                    ToolTip.visible: hovered
                    ToolTip.text: "Revert the last save: restores every file it touched to what it was before"
                    onClicked: cleanupController.undoLastOperation()
                }
            }
        }
    }

    MessageDialog {
        id: confirmSelectAllDialog
        severity: SeabassDialog.Question
        title: "Select All " + plansListView.count + " Duplicate Group(s)?"
        headline: "This marks all " + plansListView.count + " currently listed duplicate group(s) - "
            + cleanupController.totalWastedBytesHuman + " total if every copy kept only one file - "
            + "for the next \"Clean Up Selected\" click, including groups excluded by default because "
            + "their copies differ in quality (marked with ⚠ below)."
        detailText: searchField.text.length > 0
            ? "Your search (\"" + searchField.text + "\") is currently narrowing this list. Clear it "
                + "first if you meant to select across your whole library, or leave it as-is to select "
                + "only these matching groups."
            : "No search filter is active, so this selects every duplicate group found across your "
                + "whole library."
        acceptText: "Select All"
        onAccepted: cleanupController.setAllIncluded(true)
    }

    MessageDialog {
        id: confirmCleanupDialog
        severity: SeabassDialog.Question
        title: "Stage cleaning up " + cleanupController.includedCount + " duplicate group(s)?"
        headline: "For each selected group, every copy except the one kept will be removed from the "
            + "library: its hot/memory cues are merged onto the surviving copy first (nothing is lost), "
            + "and any playlist it belonged to is updated to reference the surviving copy instead."
        detailText: "This does NOT delete the removed copies' audio files. Their library entries are "
            + "removed and they're recorded for you to review and delete separately."
        acceptText: "Stage Clean-Up"
        onAccepted: cleanupController.apply()

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            text: "Nothing is written until you press Save. Everything touched is backed up first and "
                + "can be undone."
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        StickWriteWarning {
            visible: false
            text: "Cleaning up duplicates. Do not remove the stick until this finishes."
        }

        Label {
            visible: cleanupController.errorMessage.length > 0
            text: cleanupController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: cleanupController.statusMessage.length > 0
            text: cleanupController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ListView {
            // Room to scroll the last row clear of the Save overlay (bottom right).
            bottomMargin: 80
            id: plansListView
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: cleanupController.plans
            spacing: 4

            ScrollBar.vertical: BigScrollBar {}

            delegate: Column {
                id: delegateRoot
                width: ListView.view.width
                spacing: 4

                required property int index
                required property var survivor
                required property var toRemove
                required property bool differs
                required property bool hasUnpreservableDataAtRisk
                required property int unreferencedCount
                required property int unreferencedHeldBackCount
                required property string wastedBytesHuman
                required property int newCueCount
                required property bool included
                required property bool staged
                required property string stagedDescription

                property bool expanded: false

                ItemDelegate {
                    width: parent.width
                    hoverEnabled: true
                    onClicked: delegateRoot.expanded = !delegateRoot.expanded

                    contentItem: ColumnLayout {
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            CheckBox {
                                checked: delegateRoot.included
                                enabled: !delegateRoot.staged
                                onToggled: cleanupController.setIncluded(delegateRoot.index, checked)
                                ToolTip.visible: hovered
                                ToolTip.text: delegateRoot.staged ? "Staged; unstage it first to change the selection"
                                    : "Include this group when staging"
                            }
                            StatusBadge {
                                visible: delegateRoot.staged
                                label: "Staged"
                                badgeColor: Theme.warnText
                                tooltipText: delegateRoot.stagedDescription + "\n\nNot on the stick yet: press Save."
                            }
                            ToolButton {
                                visible: delegateRoot.staged
                                text: "Unstage"
                                enabled: !cleanupController.writing
                                onClicked: cleanupController.unstage(delegateRoot.index)
                            }
                            Label {
                                text: "Keeps: " + delegateRoot.survivor.title + " - " + delegateRoot.survivor.artist
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.preferredWidth: 320
                            }
                            Label {
                                text: "(removes " + delegateRoot.toRemove.length + " cop"
                                    + (delegateRoot.toRemove.length === 1 ? "y" : "ies") + ")"
                                color: Theme.textMuted
                            }
                            StatusBadge {
                                label: "ⓘ what's conserved"
                                badgeColor: Theme.textMuted
                                // Was 558 characters of prose. A hover
                                // tooltip is read standing up, with the
                                // mouse held still -- two labelled lists
                                // can be taken in at a glance, a
                                // paragraph cannot. The conditional
                                // clauses that made it long are the ones
                                // a reader cannot act on either way.
                                tooltipText: "Kept: cues (merged), playlist membership, and any missing BPM"
                                    + (root.format === "engine" ? " or key." : ", key or artwork.")
                                    + "\nLost: rating, comment, play count, last played."
                            }
                            StatusBadge {
                                visible: delegateRoot.differs
                                label: "⚠ copies differ"
                                badgeColor: Theme.conflictText
                                // The "why" (a shorter edit kept on
                                // purpose) is what the exclusion is FOR,
                                // not something the reader decides with.
                                tooltipText: "The highest-bitrate copy is not the longest one, so this group is "
                                    + "excluded by default. Tick it to include it."
                            }
                            StatusBadge {
                                visible: delegateRoot.unreferencedCount > 0
                                label: delegateRoot.unreferencedCount + " uncatalogued file(s)"
                                badgeColor: Theme.textMuted
                                // What it is, then where it goes. The
                                // re-check before deleting is a promise
                                // the Delete Orphaned Files page makes;
                                // it does not belong on a count badge.
                                tooltipText: "Audio files on the stick that no catalog lists. Saving puts them "
                                    + "under \"Delete Orphaned Files\"."
                            }
                            StatusBadge {
                                visible: delegateRoot.unreferencedHeldBackCount > 0
                                label: "⚠ " + delegateRoot.unreferencedHeldBackCount + " file(s) kept back"
                                badgeColor: Theme.conflictText
                                tooltipText: "Left on the stick either way: these copies may not be the same "
                                    + "recording, and nothing is deleted on a guess."
                            }
                            StatusBadge {
                                visible: delegateRoot.hasUnpreservableDataAtRisk
                                label: "⚠ data would be lost"
                                badgeColor: Theme.conflictText
                                tooltipText: "Rating, comment, play count and last played differ and are not kept. "
                                    + "Excluded by default; tick to include."
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: delegateRoot.expanded ? "▾" : "▸"
                                font.pointSize: Theme.fontHuge
                                font.bold: true
                                color: Theme.textMuted
                            }
                        }
                        Label {
                            text: delegateRoot.wastedBytesHuman + " freed"
                                + (delegateRoot.newCueCount > 0 ? "; " + delegateRoot.newCueCount + " cue(s) merged onto the survivor" : "")
                            color: Theme.textMuted
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    visible: delegateRoot.expanded
                    height: delegateRoot.expanded ? groupColumn.implicitHeight + 16 : 0
                    color: Theme.groupBackground
                    border.color: Theme.borderSubtle
                    radius: 4

                    ColumnLayout {
                        id: groupColumn
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                        // Richer per-copy view (waveform + real cues, not
                        // just bitrate/duration/size) so it's directly
                        // visible -- not just claimed in the text above --
                        // that a removed copy's cues really do end up on
                        // the kept one. Status pill makes which is which
                        // impossible to miss at a glance.
                        TrackWaveformCard {
                            track: delegateRoot.survivor
                            formatLabelText: root.formatLabel(delegateRoot.survivor.side) + " - "
                                + (delegateRoot.survivor.bitrate > 0 ? delegateRoot.survivor.bitrate + " kbps, " : "")
                                + root.formatDuration(delegateRoot.survivor.durationMs) + ", "
                                + delegateRoot.survivor.sizeHuman
                            statusBadgeText: "KEEPING"
                            statusBadgeBg: Theme.groupBackground
                            statusBadgeBorder: Theme.good
                            statusBadgeTextColor: Theme.good
                            playbackController: root.playbackController
                            playbackPath: root.currentPath()
                        }

                        Repeater {
                            model: delegateRoot.toRemove
                            delegate: TrackWaveformCard {
                                required property var modelData
                                track: modelData
                                formatLabelText: root.formatLabel(modelData.side) + " - "
                                    + (modelData.bitrate > 0 ? modelData.bitrate + " kbps, " : "")
                                    + root.formatDuration(modelData.durationMs) + ", " + modelData.sizeHuman
                                // Three different things happen to a
                                // copy here, so it says which: a catalog
                                // row goes, a file is listed for
                                // deletion, or nothing happens at all.
                                statusBadgeText: modelData.heldBack ? "KEPT BACK"
                                    : modelData.isUnreferenced ? "FILE ONLY" : "REMOVING"
                                statusBadgeBg: modelData.heldBack ? Theme.groupBackground : Theme.dangerBg
                                statusBadgeBorder: modelData.heldBack ? Theme.borderSubtle : Theme.dangerBorder
                                statusBadgeTextColor: modelData.heldBack ? Theme.textMuted : Theme.dangerText
                                playbackController: root.playbackController
                                playbackPath: root.currentPath()
                            }
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: plansListView.count === 0 && !cleanupController.busy
                text: "No duplicate tracks with a removable copy found."
                color: Theme.textMuted
            }
        }
    }

    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: cleanupController
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: cleanupController.busy
        current: cleanupController.scanCurrent
        total: cleanupController.scanTotal
        label: "Scanning for duplicates..."
        cancellable: cleanupController.scanCancellable
        onCancelRequested: cleanupController.cancelScan()
    }
}
