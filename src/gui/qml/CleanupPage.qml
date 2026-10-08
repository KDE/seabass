// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

Page {
    id: root
    required property string stickLabel
    // The page it was opened from, for the breadcrumb: empty from the
    // home's card, "Cues on Duplicate Copies" from that page's suggestion.
    property string hubLabel: ""
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
        sequences: [StandardKey.Cancel]
        enabled: cleanupController.stagedCount > 0 && !cleanupController.writing
        onActivated: cleanupController.unstageAll()
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        // Cancel on the low-space question leaves, as Back does -- see
        // EditSessionHost's backupLocationDeclined for why it must.
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
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

    // Opens on the playlist last picked on any page with a picker, so
    // going from a playlist in Browse Library to cleaning it up keeps it.
    Component.onCompleted: {
        root.selectedPlaylistName = root.appSettingsController.lastPlaylistName;
        root.rescanInScope();
    }

    // A remembered playlist this library does not have would scan
    // nothing. The picker lists the whole library's playlists whatever
    // the scope, so once a scan is done a missing one falls back to all
    // tracks -- without forgetting it, since the next stick may have it.
    // Checked a turn later, not from inside the controller's own
    // busyChanged, and never after a cancel: a cancelled scan leaves.
    property bool cancellingScan: false
    function dropMissingPlaylist() {
        if (root.cancellingScan || cleanupController.busy || root.selectedPlaylistName.length === 0
            || cleanupController.playlistNames.indexOf(root.selectedPlaylistName) >= 0) {
            return;
        }
        root.selectedPlaylistName = "";
        root.rescanInScope();
    }
    onFormatChanged: root.rescanInScope()

    function formatDuration(ms) {
        var totalSeconds = Math.round(ms / 1000);
        var m = Math.floor(totalSeconds / 60);
        var s = totalSeconds % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    // Scrolling the groups folds the header's description away (the
    // paragraph, the uncatalogued files, the space bar) and leaves the
    // crumb, the filters, the counts and the buttons: what is read once
    // gives its room to the list, what is used stays. Back at the top of
    // the list it unfolds again. See ScrollCollapse for when.
    ScrollCollapse {
        id: headerCollapse
        flickable: plansListView
        collapsibleHeight: headerDetails.implicitHeight + headerLayout.spacing
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
        bottomPadding: Theme.headerBottomPadding
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
                id: crumbRow
                objectName: "crumbRow"
                Layout.fillWidth: true
                spacing: 12
                // The gap a one-row header shows under its crumb (Theme.crumbGap),
                // kept here too, before this header's second row. While the
                // description folds, its own spacing is taken off here as it
                // goes, so the gap closes to exactly this and does not jump
                // by a spacing when the folded part finally hides.
                Layout.bottomMargin: Theme.crumbGap - headerLayout.spacing
                    - (headerDetails.visible ? headerLayout.spacing * headerCollapse.progress : 0)
                BackBreadcrumb {
                    stack: root.StackView.view
                    stickLabel: root.stickLabel
                    middleLabel: root.hubLabel
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

            // Everything the header says rather than offers, in one block
            // so it folds as one. Clipped while it folds; hidden once it
            // has, so it gives back its spacing too.
            Item {
                id: headerDetails
                objectName: "headerDetails"
                Layout.fillWidth: true
                Layout.preferredHeight: detailsColumn.implicitHeight * (1 - headerCollapse.progress)
                implicitHeight: detailsColumn.implicitHeight
                visible: headerCollapse.progress < 1
                opacity: 1 - headerCollapse.progress
                clip: true

                ColumnLayout {
                    id: detailsColumn
                    width: parent.width
                    spacing: headerLayout.spacing

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
                        id: explanationRow
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            text: "Exporting the same track more than once leaves several copies of it on the stick, "
                                + "each catalogued separately and each taking up space. In this step Seabass groups "
                                + "the copies that agree on artist, track title and length, and consolidates their "
                                + "metadata (cue points, ratings, playlist membership) onto the single copy it "
                                + "keeps. No audio is deleted here: the files this leaves unneeded are removed "
                                + "afterwards under \"Delete Orphaned Files\", which is where the space is actually "
                                + "freed."
                        }
                        // Holds the InfoButton's place while it is up in the crumb row,
                        // so the paragraph keeps its width and does not rewrap mid fold.
                        Item {
                            id: infoSlot
                            implicitWidth: duplicateInfo.implicitWidth
                            implicitHeight: duplicateInfo.implicitHeight
                            InfoButton {
                                id: duplicateInfo
                                objectName: "duplicateInfo"
                                // A control, so it stays when the description folds:
                                // it moves up to the end of the crumb row, halfway
                                // through the fold, where it is the only thing on the
                                // right.
                                parent: headerCollapse.progress > 0.5 ? crumbRow : infoSlot
                                // The crumb row placed it; back in its slot
                                // nothing does, so it goes to the corner.
                                onParentChanged: if (parent === infoSlot) { x = 0; y = 0; }
                                // The window is a setting now (Preferences, Music),
                                // so the text asks for it rather than repeating the
                                // old hardcoded "two seconds" -- which would have
                                // gone on saying two the moment anybody changed it.
                                readonly property int exactWindow: root.appSettingsController.exactMatchSeconds
                                readonly property int audioWindow: root.appSettingsController.compareAudioSeconds
                                readonly property bool audioCompared:
                                    audioWindow > exactWindow && root.appSettingsController.audioComparisonSupported

                                explanationTitle: "What counts as a duplicate?"
                                summaryText: "Same artist, same title, same length (within " + exactWindow
                                    + (exactWindow === 1 ? " second" : " seconds") + "). "
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
                                    + (audioCompared
                                        ? "## When the lengths nearly agree\n"
                                          + "Two copies of one recording often differ by a few seconds that are "
                                          + "silence: encoder padding, a run out kept by a rip, a trimmed "
                                          + "re-export. Where the gap is more than " + exactWindow + " but no more "
                                          + "than " + audioWindow + " seconds, both files are decoded, the silence "
                                          + "at each end is measured, and the length of the music between them is "
                                          + "compared instead of the stored numbers. Decoding costs real time, so "
                                          + "it only runs for a pair that is genuinely in doubt, and the answers "
                                          + "are cached on the stick. Both windows are yours to set, under "
                                          + "Preferences, Music.\n\n"
                                        : "## When the lengths nearly agree\n"
                                          + "Seabass can decode two files whose lengths are close but not close "
                                          + "enough, measure the silence at each end, and compare the length of "
                                          + "the music itself. That is off right now. Turn it on under "
                                          + "Preferences, Music.\n\n")
                                    + "## Nothing here is the only way\n"
                                    + "Whatever this page finds or misses, two tracks can always be merged by "
                                    + "hand: open Browse Library, use the **Merge** button on a track, and pick "
                                    + "the other one. That path takes no notice of lengths at all, so it is the "
                                    + "answer for a pair Seabass will not group on its own.\n\n"
                                    + "## What is kept\n"
                                    + "- **Cues** are merged, never lost: the survivor gets every copy's cues\n"
                                    + "- **Playlist membership** is preserved in every catalog\n"
                                    + "- **Missing bpm, key and artwork** are filled in from whichever copy has them\n"
                                    + "- **Play counts** are added up, and the latest last-played date is kept\n\n"
                                    + "Use *what's conserved* on any group to see exactly what the surviving copy "
                                    + "would end up with.\n\n"
                                    + "## What is not kept\n"
                                    + "**Ratings and comments** are never discarded without "
                                    + "asking. A group whose copies disagree on either is left unchecked for you "
                                    + "to decide, as is one where the copies differ in a way that might be "
                                    + "deliberate.\n"
                            }
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
                        color: Theme.surface
                        border.color: Theme.borderSubtle
                        border.width: 1
                        radius: Theme.cornerRadius

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
                    objectName: "playlistPicker"
                    width: Theme.snap(Math.max(160, Math.min(260, root.width * 0.28)))
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
                        root.appSettingsController.lastPlaylistName = root.selectedPlaylistName;
                        root.rescanInScope();
                    }
                }
                TextField {
                    id: searchField
                    objectName: "searchField"
                    placeholderText: "Search title or artist..."
                    // Proportional with a cap and a floor, matching the
                    // playlist picker above it. The floor is what lets a
                    // genuinely narrow window still show a usable field
                    // rather than a sliver.
                    width: Theme.snap(Math.max(110, Math.min(280, root.width * 0.3)))
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
                    objectName: "selectAllButton"
                    text: "Select All"
                    enabled: !cleanupController.busy && plansListView.count > 0
                    onClicked: confirmSelectAllDialog.open()
                }
                Button {
                    objectName: "deselectAllButton"
                    text: "Deselect All"
                    enabled: !cleanupController.busy && plansListView.count > 0
                    onClicked: cleanupController.setAllIncluded(false)
                }
                Button {
                    objectName: "stageButton"
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
            + "their copies differ in quality (marked “copies differ” below)."
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
        objectName: "confirmCleanupDialog"
        severity: SeabassDialog.Question
        // Selected groups the search is hiding. Staging from a narrowed list
        // takes only what it shows unless asked for everything selected: the
        // hidden ones were ticked at some point, but not looked at just now.
        readonly property int shownSelected: cleanupController.includedVisibleCount
        readonly property int hiddenSelected: cleanupController.includedCount - shownSelected
        readonly property bool searchHidesSome: searchField.text.length > 0 && hiddenSelected > 0
        function tracks(n) { return n + (n === 1 ? " Track" : " Tracks"); }
        title: "Stage cleaning up "
            + (searchHidesSome ? shownSelected : cleanupController.includedCount) + " duplicate track(s)?"
        acceptText: searchHidesSome ? "Stage " + tracks(shownSelected) + " Matching the Search"
                                    : "Stage " + tracks(cleanupController.includedCount)
        acceptEnabled: !searchHidesSome || shownSelected > 0
        alternateText: searchHidesSome ? "Stage All " + tracks(cleanupController.includedCount) + " Selected" : ""
        onAlternateRequested: cleanupController.apply(false)
        headline: "For each selected group, every copy except the one kept will be removed from the "
            + "library: its hot/memory cues are merged onto the surviving copy first (nothing is lost), "
            + "and any playlist it belonged to is updated to reference the surviving copy instead."
        detailText: "This does NOT delete the removed copies' audio files. Their library entries are "
            + "removed and they're recorded for you to review and delete separately."
        onAccepted: cleanupController.apply(searchHidesSome)

        Label {
            objectName: "hiddenSelectionWarning"
            Layout.fillWidth: true
            visible: confirmCleanupDialog.searchHidesSome
            wrapMode: Text.WordWrap
            color: Theme.warnText
            text: "Your search (\"" + searchField.text + "\") hides " + confirmCleanupDialog.hiddenSelected
                + " of the " + cleanupController.includedCount + " selected tracks. Only the "
                + confirmCleanupDialog.shownSelected + " it shows are staged unless you stage all selected; "
                + "the hidden ones stay selected either way."
        }

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
        // What the scan did about pairs whose lengths nearly agreed.
        // Muted, not green: it is a note on how the answer was reached,
        // not a success.
        Label {
            objectName: "audioComparisonNote"
            visible: cleanupController.audioComparisonNote.length > 0
            text: cleanupController.audioComparisonNote
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ListView {
            // Room to scroll the last row clear of the Save overlay (bottom right).
            bottomMargin: 80
            id: plansListView
            objectName: "plansList"
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: cleanupController.plans
            spacing: Theme.tightSpacing

            ScrollBar.vertical: BigScrollBar {}

            // One frame per group, round the header row and, opened, round
            // the copies under it too: a group is one thing, and two boxes
            // read as two. Its edge is the page's left line; what is inside
            // sits one row spacing in from it.
            delegate: Rectangle {
                id: delegateRoot
                objectName: "groupFrame"
                width: ListView.view.width
                height: groupBody.implicitHeight
                color: Theme.surface
                border.color: Theme.borderSubtle
                border.width: 1
                radius: Theme.cornerRadius

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
                required property string survivorReason

                property bool expanded: false

                Column {
                    id: groupBody
                    width: parent.width

                    ItemDelegate {
                        id: groupHeader
                        objectName: "groupHeader"
                        width: parent.width
                        hoverEnabled: true
                        onClicked: delegateRoot.expanded = !delegateRoot.expanded
                        horizontalPadding: Theme.rowSpacing
                        verticalPadding: Theme.tightSpacing
                        // Inside the frame's border, so hover never covers it.
                        topInset: delegateRoot.border.width
                        leftInset: delegateRoot.border.width
                        rightInset: delegateRoot.border.width
                        bottomInset: delegateRoot.expanded ? 0 : delegateRoot.border.width
                        // A row that lights up a little under the pointer, the
                        // way the playlist rows do, instead of the style's
                        // accent fill: hovering says the row opens, and an
                        // accent there read as a selection. The checkbox is
                        // what says a group is selected.
                        background: Rectangle {
                            objectName: "groupHeaderShade"
                            radius: Theme.cornerRadius
                            color: groupHeader.down ? Theme.rowPressed
                                : groupHeader.hovered ? Theme.rowHover : "transparent"
                        }

                        contentItem: ColumnLayout {
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                SeabassCheckBox {
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
                                    label: "what's conserved"
                                    iconName: "help-about"
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
                                        + "\nMerged: play counts (added up), last played (the latest)."
                                        + "\nLost: rating, comment."
                                }
                                StatusBadge {
                                    visible: delegateRoot.differs
                                    label: "copies differ"
                                    iconName: "dialog-warning"
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
                                    label: delegateRoot.unreferencedHeldBackCount + " file(s) kept back"
                                    iconName: "dialog-warning"
                                    badgeColor: Theme.conflictText
                                    tooltipText: "Left on the stick either way: these copies may not be the same "
                                        + "recording, and nothing is deleted on a guess."
                                }
                                StatusBadge {
                                    visible: delegateRoot.hasUnpreservableDataAtRisk
                                    label: "data would be lost"
                                    iconName: "dialog-warning"
                                    badgeColor: Theme.conflictText
                                    tooltipText: "The copies' ratings or comments differ, and only one can be kept. "
                                        + "Excluded by default; tick to include."
                                }
                                Item { Layout.fillWidth: true }
                                SeabassIcon {
                                    iconName: delegateRoot.expanded ? "arrow-down" : "arrow-right"
                                    size: Theme.iconSizeSmall * 0.75
                                    color: Theme.textMuted
                                }
                            }
                            // Which copy stays and which go, by file, before
                            // anything is staged. Byte-identical copies share
                            // every other field on this row, so the path and
                            // the playlists are the only way to tell them
                            // apart -- and the reason says whether the copies
                            // really differ or a tie was broken.
                            Label {
                                visible: delegateRoot.survivorReason.length > 0
                                text: delegateRoot.survivorReason
                                color: Theme.textMuted
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            GridLayout {
                                columns: 3
                                columnSpacing: Theme.pageMargin
                                rowSpacing: 2
                                Layout.fillWidth: true

                                Repeater {
                                    model: [delegateRoot.survivor].concat(delegateRoot.toRemove)
                                    delegate: Label {
                                        required property var modelData
                                        required property int index
                                        // Same three outcomes as the cards'
                                        // badges below, in the same words.
                                        text: index === 0 ? "Keep"
                                            : modelData.heldBack ? "Kept back"
                                            : modelData.isUnreferenced ? "Remove file" : "Remove"
                                        font.bold: true
                                        color: index === 0 ? Theme.good
                                            : modelData.heldBack ? Theme.textMuted : Theme.danger
                                        Layout.row: index
                                        Layout.column: 0
                                    }
                                }
                                Repeater {
                                    model: [delegateRoot.survivor].concat(delegateRoot.toRemove)
                                    delegate: Label {
                                        required property var modelData
                                        required property int index
                                        text: modelData.shownPath.length > 0 ? modelData.shownPath : "(file not found)"
                                        elide: Text.ElideMiddle
                                        Layout.maximumWidth: 420
                                        Layout.row: index
                                        Layout.column: 1

                                        HoverHandler { id: pathHover }
                                        ToolTip.visible: pathHover.hovered && modelData.filePath.length > 0
                                        ToolTip.text: modelData.filePath
                                        ToolTip.delay: 300
                                    }
                                }
                                Repeater {
                                    model: [delegateRoot.survivor].concat(delegateRoot.toRemove)
                                    delegate: Label {
                                        required property var modelData
                                        required property int index
                                        // A removed copy's own reason
                                        // leads, when the group's sentence
                                        // cannot cover every copy.
                                        text: (modelData.removedReason ? modelData.removedReason + "; " : "")
                                            + (modelData.playlists.length === 0 ? "in no playlist"
                                            : "in " + modelData.playlists.length
                                                + (modelData.playlists.length === 1 ? " playlist: " : " playlists: ")
                                                + modelData.playlists.join(", "))
                                        color: Theme.textMuted
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                        Layout.row: index
                                        Layout.column: 2

                                        HoverHandler { id: playlistHover }
                                        ToolTip.visible: playlistHover.hovered && modelData.playlists.length > 0
                                        ToolTip.text: modelData.playlists.join("\n")
                                        ToolTip.delay: 300
                                    }
                                }
                            }
                            Label {
                                text: delegateRoot.wastedBytesHuman + " freed"
                                    + (delegateRoot.newCueCount > 0 ? "; " + delegateRoot.newCueCount + " cue(s) merged onto the survivor" : "")
                                color: Theme.textMuted
                            }
                        }
                    }

                    Item {
                        objectName: "groupCopies"
                        width: parent.width
                        visible: delegateRoot.expanded
                        height: delegateRoot.expanded ? groupColumn.implicitHeight + Theme.rowSpacing : 0

                        ColumnLayout {
                            id: groupColumn
                            x: Theme.rowSpacing
                            width: parent.width - 2 * Theme.rowSpacing
                            spacing: Theme.tightSpacing

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
            }

            EmptyState {
                visible: plansListView.count === 0 && !cleanupController.busy
                tone: "good"
                iconName: "checkmark"
                text: "No duplicate tracks with a removable copy found."
            }
        }
    }

    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: cleanupController
        function onScanCancelled() { root.StackView.view.pop(); }
        function onBusyChanged() {
            if (!cleanupController.busy && !root.cancellingScan) {
                Qt.callLater(root.dropMissingPlaylist);
            }
        }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: cleanupController.busy
        current: cleanupController.scanCurrent
        total: cleanupController.scanTotal
        label: "Scanning for duplicates..."
        cancellable: cleanupController.scanCancellable
        onCancelRequested: {
            root.cancellingScan = true;
            cleanupController.cancelScan();
        }
    }
}
