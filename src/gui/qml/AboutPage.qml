// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The short version of vizzzion.org/seabass, and a way to get to the
// long one.
//
// This page used to carry the full story of the three catalogs and a
// paragraph on streaming tracks, which is reference material rather than
// an introduction: nobody opens About to read a format explanation. The
// detail that belongs next to a control now lives next to that control
// (LibrarySourceToggle's own tooltip, Preferences' streaming setting),
// the rest is a click away on the website, and what stays here is what
// someone opening About actually wants: what this is, what it does, what
// it will not do, and who made it.
Page {
    id: root

    signal donationRequested()

    readonly property string websiteUrl: "https://vizzzion.org/seabass/"

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
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                title: "About"
                onHomeRequested: root.StackView.view.pop(null)
            }
            Item { Layout.fillWidth: true }
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.implicitHeight + 64
        clip: true
        // A Flickable is draggable by default even when there's nothing
        // to scroll -- content shorter than the viewport still let you
        // click-drag it into an elastic overshoot and snap back, which
        // reads as a bug (dragging a page that visibly has no scrollbar
        // and doesn't move). Only interactive once there's real overflow.
        interactive: contentHeight > height

        ColumnLayout {
            id: content
            x: Math.max(32, (parent.width - width) / 2)
            width: Math.min(parent.width - 64, 640)
            y: 32
            spacing: 20

            Image {
                source: "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg"
                Layout.preferredWidth: 96
                Layout.preferredHeight: 96
                Layout.alignment: Qt.AlignHCenter
                fillMode: Image.PreserveAspectFit
            }

            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 2
                Label {
                    text: "Seabass"
                    font.family: Theme.titleFamily
                    font.weight: Theme.titleWeight
                    font.pointSize: Theme.titleLarge
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    text: "Your DJ Toolbox"
                    font.pointSize: Theme.baseFontPointSize * 1.3
                    color: Qt.lighter(Theme.accent, 1.3)
                    Layout.alignment: Qt.AlignHCenter
                }
            }

            Label {
                text: "Move between the Pioneer and Denon worlds with confidence. Seabass works on the "
                    + "library already on your USB stick: it keeps the Rekordbox and Engine DJ copies of "
                    + "it in step, and tells you the truth about what is on there."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
            }

            ColumnLayout {
                spacing: 6
                Layout.fillWidth: true
                Subtitle { text: "What it does" }
                Label {
                    text: "• Backs up a whole stick into one file on this computer, and restores it onto any drive\n"
                        + "• Backs up cue points, ratings, comments and play counts, and puts them back when they go missing\n"
                        + "• Carries hot cues, memory cues and loops between the catalogs on one stick, and asks when they disagree\n"
                        + "• Browses every catalog: tracks, playlists, BPM, key, and playback with a real waveform\n"
                        + "• Finds duplicate copies, merges what they each carry, and reclaims the space\n"
                        + "• Checks the catalogs against each other and against the files on disk\n"
                        + "• Measures how a stick performs, the way a player reads it"
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
            }

            ColumnLayout {
                spacing: 6
                Layout.fillWidth: true
                Subtitle { text: "What it does not do" }
                Label {
                    text: "Seabass never analyzes audio to produce new analysis data. Beatgridding, BPM and "
                        + "key detection and waveform analysis all happen in Rekordbox or Engine DJ first. "
                        + "Seabass reads and moves around the results, and never recomputes them."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
            }

            // The same warning the website leads with, in the same words.
            // It belongs in the app at least as much as on the site: this
            // is where someone is standing when they are about to let it
            // write to a stick.
            Rectangle {
                Layout.fillWidth: true
                color: Theme.groupBackground
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 4
                implicitHeight: betaText.implicitHeight + 24
                Label {
                    id: betaText
                    anchors.fill: parent
                    anchors.margins: 12
                    textFormat: Text.StyledText
                    text: "<b>Seabass is beta software.</b> It writes to your DJ library, and although it "
                        + "backs up before every write, that is not a substitute for your own backups. "
                        + "Test on the hardware you will gig with before you trust it at a gig."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize
                    Layout.fillWidth: true
                }
            }

            Label {
                text: "Free software, under the GPL. No ads, no subscriptions, nothing phoning home. "
                    + "Built by Sebastian Kügler and friends."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                color: Theme.textMuted
                Layout.fillWidth: true
            }

            // Buttons rather than links inside a sentence: these are the
            // two things this page is for once it has been read, and a
            // link buried in a paragraph is not findable the second time
            // someone comes looking for it.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Button {
                    objectName: "aboutWebsiteButton"
                    text: "Visit the website"
                    onClicked: Qt.openUrlExternally(root.websiteUrl)
                    ToolTip.visible: hovered
                    ToolTip.text: root.websiteUrl
                }
                Button {
                    objectName: "aboutSupportButton"
                    text: "Support Seabass"
                    onClicked: root.donationRequested()
                }
                Item { Layout.fillWidth: true }
            }

            Label {
                text: "Rekordbox is a trademark of AlphaTheta Corporation. Engine DJ is a trademark of "
                    + "inMusic Brands, Inc. TIDAL is a trademark of TIDAL Music AS. Seabass is an "
                    + "independent, unofficial tool and is not affiliated with, endorsed by, or "
                    + "sponsored by any of them."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.fontTiny
                color: Theme.textMuted
                Layout.fillWidth: true
                Layout.topMargin: 8
            }
        }
    }
}
