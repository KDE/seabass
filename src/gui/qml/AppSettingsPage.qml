// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui

Page {
    id: root
    required property var appSettingsController
    // Null in the tests that do not care about it: every use below is
    // guarded, so the page renders without one.
    property var updateChecker: null
    // Scaled by Theme.iconScale, not a literal pixel count -- same
    // pt-to-px conversion KeyBadge.qml uses for its own sizing, so this
    // tracks the system font size (and therefore stays sharp on a retina
    // display, which is just a higher effective DPI) instead of a fixed
    // pixel count that only looks right at today's default font size.
    readonly property real settingIndent: Theme.scaled(20)

    // What the ten taps on the version line did. The checkbox it talks
    // about has just appeared below.
    Dialog {
        id: testingRevealedDialog
        objectName: "testingRevealedDialog"
        anchors.centerIn: Overlay.overlay
        // Explicit, for the reason StickListPage's dialogs give: content
        // sized from the dialog must not size the dialog.
        width: 460
        modal: true
        title: "Alpha and beta versions"
        standardButtons: Dialog.Ok
        Label {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Seabass will now also tell you about alpha and beta versions. "
                + "A checkbox in the Updates section lets you turn that off again."
        }
    }

    signal anonymizeLibraryRequested()

    // A named group of settings: "Appearance", "Music", "Data
    // locations", "More settings". One step above the Subtitle each
    // individual setting carries, because the page had grown to seven
    // Subtitles in a flat column with nothing saying which of them
    // belonged together -- the two folder pickers in particular read as
    // two unrelated settings rather than as "where things go".
    //
    // The rule below it: a section is a heading and a rule, a setting is
    // a Subtitle, and a setting's controls are indented under it. Three
    // levels, and the indent is the only one.
    component SectionHeader: ColumnLayout {
        property alias text: sectionLabel.text
        Layout.fillWidth: true
        spacing: 4 * Theme.iconScale
        Label {
            id: sectionLabel
            font.family: Theme.titleFamily
            font.weight: Theme.titleWeight
            font.pointSize: Theme.titleCrumb
            color: Theme.accent
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.borderSubtle
        }
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
        // Explicit opaque background. KDE's platform theme integration
        // resolves ToolBar to its own "org.kde.breeze" style regardless of
        // this app's Material palette (see main.cpp's exportMaterialPalette()
        // comment for the fuller story of why that isn't forced away
        // globally), and Breeze's own ToolBar background can render
        // translucent/blurred, letting whatever window is behind Seabass
        // show through the header. A plain opaque Rectangle sidesteps
        // whichever style actually resolves, same fix class as
        // StickListPage.qml's Frame-vs-Rectangle precedent.
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                title: "Preferences"
                onHomeRequested: root.StackView.view.pop(null)
            }
            Item { Layout.fillWidth: true }
        }
    }

    // Scrollable because this page grows: every group's spacing and
    // indent is scaled by Theme.iconScale, so a larger system font
    // (or a short window) pushes the bottom groups off the page, and
    // without a Flickable there is no way to reach them. width:
    // parent.width below is load-bearing for the same reason -- the
    // layout used to be anchored left only, so it sized to its own
    // content and the backup-path Label could never elide nor the
    // description Label wrap, running off the right edge instead.
    PageScrollView {
        objectName: "settingsScroll"
        anchors.fill: parent
        anchors.margins: Theme.scaled(24)

        // Group header flush left, then one option per row indented beneath
        // it -- a two-column grid (label column vs. field column) has to
        // align every row's label against every other row's, which falls
        // apart the moment one field needs more than one control (the key
        // notation row's two radios plus a preview badge next to a plain
        // single checkbox). A header-then-indented-rows shape has no shared
        // axis to keep aligned: every group is independent.
        ColumnLayout {
            objectName: "settingsColumn"
            // Capped and centred, the same 640 the About and Support
            // pages use. A preferences page has nothing to gain from a
            // 2000 px window except lines of text too long to read back
            // to their own start. The cap is on the COLUMN and not on
            // the scroll view, so the scroll bar stays docked to the
            // window's right edge rather than floating in beside the
            // text; the section rules shorten with the column, which is
            // the point of them.
            readonly property int maxWidth: 640
            width: Math.min(parent.width, maxWidth)
            x: Math.round(Math.max(0, (parent.width - width) / 2))
            spacing: Theme.scaled(24)

            // ---- Appearance -------------------------------------------
            SectionHeader { text: "Appearance" }

            ColumnLayout {
                spacing: Theme.scaled(6)
                Subtitle { text: "Theme" }
                ButtonGroup { id: themeGroup }
                RadioButton {
                    Layout.leftMargin: root.settingIndent
                    text: "Dark"
                    ButtonGroup.group: themeGroup
                    checked: !root.appSettingsController.useSystemTheme
                    onCheckedChanged: if (checked) root.appSettingsController.useSystemTheme = false
                }
                RadioButton {
                    Layout.leftMargin: root.settingIndent
                    text: "Match System Theme"
                    ButtonGroup.group: themeGroup
                    checked: root.appSettingsController.useSystemTheme
                    onCheckedChanged: if (checked) root.appSettingsController.useSystemTheme = true
                }
            }

            ColumnLayout {
                spacing: Theme.scaled(6)
                // Explanation lives in a tooltip (hover either radio button)
                // rather than as a permanent line of text below -- it's
                // background detail worth having on hand, not something that
                // needs to compete for space with the setting itself.
                readonly property string keyNotationExplanation:
                    "Applies to every key badge in Browse Library and Library Statistics. Either way, the "
                    + "badge's color always comes from the same underlying Camelot wheel position; only the "
                    + "printed label changes."
                Subtitle { text: "Musical key notation" }
                ButtonGroup { id: keyNotationGroup }
                RadioButton {
                    Layout.leftMargin: root.settingIndent
                    text: "Camelot (e.g. 6A)"
                    ButtonGroup.group: keyNotationGroup
                    checked: root.appSettingsController.keyNotation !== "traditional"
                    onCheckedChanged: if (checked) root.appSettingsController.keyNotation = "camelot"
                    ToolTip.visible: hovered
                    ToolTip.text: parent.keyNotationExplanation
                }
                RadioButton {
                    Layout.leftMargin: root.settingIndent
                    text: "Traditional (e.g. F♯m)"
                    ButtonGroup.group: keyNotationGroup
                    checked: root.appSettingsController.keyNotation === "traditional"
                    onCheckedChanged: if (checked) root.appSettingsController.keyNotation = "traditional"
                    ToolTip.visible: hovered
                    ToolTip.text: parent.keyNotationExplanation
                }
                // Live preview, not just a description -- the two notations
                // read differently enough (a color-coded wheel position vs.
                // an actual note name) that seeing one example update as you
                // switch is clearer than describing the difference in text.
                RowLayout {
                    Layout.leftMargin: root.settingIndent
                    spacing: Theme.scaled(8)
                    Label { text: "Preview:"; color: Theme.textMuted }
                    KeyBadge {
                        keyName: "F#m"
                        notation: root.appSettingsController.keyNotation
                    }
                }
            }

            ColumnLayout {
                spacing: Theme.scaled(6)
                Subtitle { text: "Streaming tracks" }
                SeabassCheckBox {
                    Layout.leftMargin: root.settingIndent
                    text: "Hide tracks from streaming services"
                    checked: root.appSettingsController.hideStreamingTracks
                    onToggled: root.appSettingsController.hideStreamingTracks = checked
                    ToolTip.visible: hovered
                    // What the setting does, first. That Seabass never
                    // touches these tracks either way is reassurance, not
                    // instruction, so it is one clause at the end.
                    ToolTip.text: "Whether streaming tracks (TIDAL and similar) appear in Browse Library. "
                        + "They have no file on the stick, so nothing else is affected."
                }
            }

            // ---- Music ------------------------------------------------
            //
            // What counts as the same recording, and what counts as a
            // cue. These three reach the parts of Seabass that read and
            // write libraries (domain::MatchingPolicy), not just what is
            // drawn, which is why they sit apart from Appearance.
            SectionHeader { text: "Music" }

            ColumnLayout {
                spacing: 6 * Theme.iconScale
                Subtitle { text: "When two tracks are the same recording" }

                RowLayout {
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    spacing: 8 * Theme.iconScale
                    Label { text: "Same duration within" }
                    SpinBox {
                        objectName: "exactMatchSpin"
                        editable: true
                        from: 0
                        // From the controller, not a literal: the policy
                        // clamps to this, and a spin box offering more
                        // would let someone set a number the app
                        // silently refuses.
                        to: root.appSettingsController.exactMatchMaxSeconds
                        value: root.appSettingsController.exactMatchSeconds
                        // onValueModified, not onValueChanged: the latter
                        // also fires when the binding above writes the
                        // value back, which turns a clamp in the
                        // controller into a fight between the two.
                        onValueModified: root.appSettingsController.exactMatchSeconds = value
                    }
                    Label { text: "sec is considered an exact match" }
                    InfoButton {
                        explanationTitle: "Exact match on length"
                        summaryText: "Two tracks that agree on artist and title are treated as the same "
                            + "recording when their stored lengths are this close."
                        explanationText:
                            "## Why it is not zero\n\n"
                            + "Rekordbox and Engine DJ record slightly different lengths for one and the same "
                            + "file, and a length read from a file's tags differs again from one its DJ "
                            + "software computed. Two seconds absorbs that without letting a radio edit pass "
                            + "for an extended mix.\n\n"
                            + "## What it affects\n\n"
                            + "- Which copies Clean Up Duplicates groups together\n"
                            + "- Which tracks share their cue points when cues are consolidated\n"
                            + "- Which track in one catalog is matched to a track in another when cues are "
                            + "synced\n"
                            + "- Which stored track a cue or metadata backup is restored onto\n\n"
                            + "## Choosing a number\n\n"
                            + "Raise it and more copies are found, including some that are different edits. "
                            + "Lower it and only near identical lengths group, so genuine duplicates get "
                            + "missed. Nothing is ever written without asking you first, whichever way you "
                            + "set it.\n\n"
                            + "One exception, on purpose: raising it does not loosen what the cue and "
                            + "metadata backups treat as the same track. Those keep their two seconds, "
                            + "because a looser match there could let one track's backed up cues be "
                            + "overwritten by another's with nothing shown. Lowering it does apply to "
                            + "them, since a stricter match can only ever keep the two apart."
                    }
                }

                RowLayout {
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    spacing: 8 * Theme.iconScale
                    Label { text: "Same duration within" }
                    SpinBox {
                        objectName: "compareAudioSpin"
                        editable: true
                        // Never below the exact match window: a wider
                        // window narrower than the exact one describes an
                        // empty band, so the setting would be on and
                        // never do anything. The controller clamps it the
                        // same way; this only keeps the page from
                        // offering a number it will not get.
                        from: root.appSettingsController.exactMatchSeconds
                        to: root.appSettingsController.compareAudioMaxSeconds
                        value: root.appSettingsController.compareAudioSeconds
                        onValueModified: root.appSettingsController.compareAudioSeconds = value
                    }
                    Label { text: "sec will compare audio" }
                    InfoButton {
                        explanationTitle: "Comparing the audio"
                        summaryText: "When two lengths are too far apart to call an exact match but no "
                            + "further apart than this, Seabass listens to the two files instead of "
                            + "trusting the numbers."
                        explanationText:
                            "## What it does\n\n"
                            + "Both files are decoded, the silence at the start and at the end of each is "
                            + "measured, and the length of the music between them is compared. Two copies of "
                            + "one recording often differ by several seconds that are silence on one side: "
                            + "encoder padding, a run out kept by a rip, a re-export that trimmed the "
                            + "intro. Take the silence off and the music is the same length on both.\n\n"
                            + "## What it costs\n\n"
                            + "Decoding takes real time, so it only ever runs for a pair that is genuinely "
                            + "in doubt, never for the whole library. What it finds is written to a cache on "
                            + "the stick itself, so the next scan of that stick pays nothing and a stick "
                            + "carried to another computer keeps the answers.\n\n"
                            + "## Limits\n\n"
                            + "This measures silence. It cannot tell two different recordings of the same "
                            + "length apart, which is why it is only ever asked about a pair that already "
                            + "agrees on artist and title. Set it to the same number as the exact match "
                            + "window above to switch it off.\n\n"
                            + "## You can always merge by hand\n\n"
                            + "Whatever these two numbers find or miss, two tracks can still be merged "
                            + "manually: open Browse Library, use the Merge button on a track, and pick the "
                            + "other one. Nothing here ever merges anything on its own."
                    }
                }
                Label {
                    // Said on the page, not only in the help popup: a
                    // build with no decoder cannot do this at all, and a
                    // number that quietly does nothing is worse than an
                    // absent one.
                    visible: !root.appSettingsController.audioComparisonSupported
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: "No audio decoder is available here, so lengths are compared but the audio is not. "
                        + "Anything already measured and cached on a stick is still used."
                }
            }

            ColumnLayout {
                spacing: 6 * Theme.iconScale
                Subtitle { text: "Cue points" }
                RowLayout {
                    Layout.leftMargin: root.settingIndent
                    spacing: 8 * Theme.iconScale
                    SeabassCheckBox {
                        objectName: "ignoreCuesAtStartCheck"
                        text: "Ignore cues at 0:00"
                        checked: root.appSettingsController.ignoreCuesAtStart
                        onToggled: root.appSettingsController.ignoreCuesAtStart = checked
                    }
                    InfoButton {
                        explanationTitle: "Cues at 0:00"
                        summaryText: "A cue inside the first second of a track is treated as noise rather "
                            + "than as a marker you placed."
                        explanationText:
                            "## Where they come from\n\n"
                            + "A stray press while a track was being analyzed, an artifact of an import, or "
                            + "a format's own \"no cue set\" value read back as a position. Engine DJ's "
                            + "automatic main cue also lands a few hundred milliseconds in rather than at "
                            + "sample zero, which still displays as 0:00.\n\n"
                            + "## What it affects\n\n"
                            + "With this on, such a cue is offered for cleanup in Library Health, left out "
                            + "of metadata backups, ignored when two catalogs are compared, and never "
                            + "written back onto a stick by a restore. With it off, they count as ordinary "
                            + "cues everywhere.\n\n"
                            + "## When to turn it off\n\n"
                            + "If you deliberately keep a pad on the very start of a track. A loop starting "
                            + "on the first bar is never touched either way, and neither is a cue at a "
                            + "position before the start of the track: that one is a \"no cue set\" value "
                            + "with nowhere in the track to point, so it stays noise whatever you choose."
                    }
                }
            }

            // ---- Data locations ---------------------------------------
            SectionHeader { text: "Data locations" }

            // Where full stick backups go. One `<stick label>.zip` per
            // stick, in a place the user can find and open with 7-Zip/unzip.
            ColumnLayout {
                spacing: Theme.scaled(6)
                Subtitle { text: "Seabass data directory" }
                Label {
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: "One folder for everything Seabass owns here: stick images under backups/full, "
                        + "anonymized exports under testdata, and its own bookkeeping under metadata. "
                        + "Moving it does not move what is already there."
                }
                RowLayout {
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        objectName: "seabassHomeValue"
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        font.family: Theme.dataFamily
                        text: root.appSettingsController.seabassHomeDirectory
                    }
                    Button {
                        text: "Change…"
                        onClicked: seabassHomeDialog.open()
                    }
                    Button {
                        text: "Reset"
                        onClicked: root.appSettingsController.seabassHomeDirectory = ""
                    }
                }
                FolderDialog {
                    id: seabassHomeDialog
                    title: "Choose where Seabass keeps its data on this computer"
                    currentFolder: root.appSettingsController.toLocalFileUrl(root.appSettingsController.seabassHomeDirectory)
                    onAccepted: root.appSettingsController.seabassHomeDirectory = selectedFolder.toString()
                }
            }

            ColumnLayout {
                spacing: Theme.scaled(6)
                Subtitle { text: "Full backups" }
                Label {
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: "Folder where each stick's backup archive is kept. Moving it does not move existing backups."
                }
                RowLayout {
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        objectName: "backupPathLabel"
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        font.family: Theme.dataFamily
                        text: root.appSettingsController.stickBackupDirectory
                    }
                    Button {
                        text: "Change…"
                        onClicked: backupFolderDialog.open()
                    }
                    Button {
                        text: "Reset"
                        onClicked: root.appSettingsController.stickBackupDirectory = ""
                    }
                }
                FolderDialog {
                    id: backupFolderDialog
                    title: "Choose where to keep full stick backups"
                    currentFolder: root.appSettingsController.toLocalFileUrl(root.appSettingsController.stickBackupDirectory)
                    onAccepted: root.appSettingsController.stickBackupDirectory = selectedFolder.toString()
                }
            }

            // ---- Updates ----------------------------------------------
            //
            // Off by default and it stays off: this is the only thing in
            // Seabass that makes a network request, and the promise on
            // the website is no phoning home. With the box unticked
            // nothing is sent, ever. "Check now" is a different matter --
            // pressing a button is the clearest consent there is.
            SectionHeader { text: "Updates" }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.scaled(6)

                Subtitle { text: "This version" }
                Label {
                    objectName: "currentVersionLabel"
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: root.updateChecker === null ? ""
                        : "Seabass " + root.updateChecker.currentVersion
                          + " (" + root.updateChecker.currentChannel + ")"
                          + (root.updateChecker.currentCommit.length > 0
                             ? ", built from " + root.updateChecker.currentCommit : "")
                    // The hidden way in for people who know: ten taps here
                    // within five seconds and Seabass starts telling them
                    // about alphas and betas too. Nothing on the screen
                    // says so, on purpose (UpdateChecker::versionTapped).
                    TapHandler {
                        enabled: root.updateChecker !== null
                        onTapped: {
                            if (root.updateChecker.versionTapped()) {
                                testingRevealedDialog.open();
                            }
                        }
                    }
                }

                SeabassCheckBox {
                    Layout.leftMargin: root.settingIndent
                    objectName: "automaticUpdateCheck"
                    text: "Check for new versions of Seabass"
                    enabled: root.updateChecker !== null
                    checked: root.updateChecker !== null && root.updateChecker.automatic
                    onToggled: root.updateChecker.automatic = checked
                    ToolTip.visible: hovered
                    ToolTip.text: "Once a day, Seabass asks the website which version is the newest. "
                        + "Nothing about you, your sticks or your library is sent, and with this off "
                        + "Seabass makes no network request at all."
                }
                Label {
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                    text: "It downloads one small file from vizzzion.org and compares version "
                        + "numbers here. No account, no identifier, nothing about your library."
                }

                // Which releases count. A stable Seabass hears about stable
                // releases only, unless this is on. Not shown until it has
                // been switched on once, by running an alpha or beta or by
                // the tap sequence above; from then on it stays, so it can
                // be switched off again (see UpdateChecker::includeTesting).
                SeabassCheckBox {
                    Layout.leftMargin: root.settingIndent
                    objectName: "includeTestingUpdates"
                    visible: root.updateChecker !== null && root.updateChecker.testingOptionRevealed
                    text: "Also tell me about alpha and beta versions"
                    enabled: root.updateChecker !== null && !root.updateChecker.runningPreRelease
                    checked: root.updateChecker !== null && root.updateChecker.includeTesting
                    onToggled: root.updateChecker.includeTesting = checked
                    ToolTip.visible: hovered
                    ToolTip.text: root.updateChecker !== null && root.updateChecker.runningPreRelease
                        ? "This is a " + root.updateChecker.currentChannel + " version, so it hears about every release."
                        : "Off, a stable Seabass hears about stable releases only."
                }

                RowLayout {
                    Layout.leftMargin: root.settingIndent
                    spacing: Theme.rowSpacing
                    Button {
                        objectName: "checkForUpdatesNow"
                        text: "Check Now"
                        enabled: root.updateChecker !== null
                            && root.updateChecker.state !== "checking"
                        onClicked: root.updateChecker.checkNow()
                    }
                    BusyIndicator {
                        running: root.updateChecker !== null && root.updateChecker.state === "checking"
                        visible: running
                        implicitWidth: Theme.scaled(22)
                        implicitHeight: Theme.scaled(22)
                    }
                }

                // The answer, whatever it is. A button that checks and
                // says nothing leaves the user wondering whether it did.
                Label {
                    objectName: "updateCheckResult"
                    Layout.leftMargin: root.settingIndent
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: text.length > 0
                    text: root.updateChecker === null ? "" : root.updateChecker.message
                    color: root.updateChecker === null ? Theme.textMuted
                        : root.updateChecker.runningWithdrawn ? Theme.danger
                        : root.updateChecker.updateAvailable ? Theme.good
                        : Theme.textMuted
                }
                Button {
                    Layout.leftMargin: root.settingIndent
                    visible: root.updateChecker !== null
                        && (root.updateChecker.updateAvailable || root.updateChecker.runningWithdrawn)
                    text: "Open the download page"
                    onClicked: Qt.openUrlExternally(root.updateChecker.downloadPage)
                }
            }

            // ---- More settings ----------------------------------------
            //
            // Absent entirely in a build compiled with SEABASS_EXPERIMENTAL
            // off -- see docs/experimental-features.md and
            // AppSettingsController::experimentalBuildSupported()'s own doc
            // comment for why that's a real "stable-only build" rather than
            // just a hidden toggle. The section heading goes with it: an
            // empty "More settings" rule would be the only thing left.
            SectionHeader {
                text: "More settings"
                visible: root.appSettingsController.experimentalBuildSupported
            }

            ColumnLayout {
                visible: root.appSettingsController.experimentalBuildSupported
                spacing: Theme.scaled(6)
                // No Subtitle of its own: it used to read "Experimental
                // features" above a checkbox saying "Enable experimental
                // features", which is the same words twice.
                SeabassCheckBox {
                    Layout.leftMargin: root.settingIndent
                    text: "Enable experimental features"
                    checked: root.appSettingsController.experimentalFeaturesEnabled
                    onToggled: root.appSettingsController.experimentalFeaturesEnabled = checked
                    ToolTip.visible: hovered
                    ToolTip.text: "Experimental features are newer, less-tested parts of Seabass; they may be "
                        + "unstable, incomplete, or change without notice."
                }

                // Same gating ActionCard.qml uses for an experimental feature
                // (visible: !experimental || experimentalFeaturesEnabled) --
                // hidden unless the toggle above is actually on, not just
                // whether this is an experimental-capable build. A plain
                // option here rather than an ActionCard on the main screen:
                // this is a maintainer/power-user tool (regenerating the
                // project's own test fixture, or submitting a library to
                // help test hardware Sebas doesn't have), not a per-stick
                // everyday action -- see docs/testing.md.
                Button {
                    Layout.leftMargin: root.settingIndent
                    visible: root.appSettingsController.experimentalFeaturesEnabled
                    text: "Export Anonymized Library for Testing…"
                    onClicked: root.anonymizeLibraryRequested()
                }
            }
        }
    }
}
