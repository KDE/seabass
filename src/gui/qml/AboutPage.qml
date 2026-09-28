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
// The words are the website's own, from its home page: the introduction
// and "Why it exists" as they stand there, reviewed once and not written
// a second time here. What the site's feature list says belongs next to
// the tools themselves and on the site. About says what this is, why it
// exists, under whose roof it is built and who made it, and it warns
// about the beta the way the site does.
Page {
    id: root

    signal donationRequested()
    // The running build's version, channel and commit (UpdateChecker; a
    // fake in tests). Optional so the page still opens without one.
    property var updateChecker: null
    readonly property string versionNumber: root.updateChecker !== null ? root.updateChecker.currentVersion : ""
    readonly property string buildLine: {
        if (root.updateChecker === null) return "";
        const channel = root.updateChecker.currentChannel;
        const names = {dev: "Development build", alpha: "Alpha", beta: "Beta", stable: "Stable release"};
        const commit = root.updateChecker.currentCommit;
        return (names[channel] || channel) + (commit.length > 0 ? " · " + commit : "");
    }

    readonly property string websiteUrl: "https://vizzzion.org/seabass/"
    readonly property string sourceUrl: "https://invent.kde.org/multimedia/seabass"
    readonly property string kdeUrl: "https://kde.org/"

    // Escape goes back, as the breadcrumb does. Only while this page is
    // the one showing: under the support page it pushes, that page's own
    // Escape is the one that fires.
    Shortcut {
        // sequences, plural: Cancel maps to more than one key on some
        // platforms, and binding only the first logs a warning per page.
        sequences: [StandardKey.Cancel]
        enabled: root.StackView.status === StackView.Active
        onActivated: root.StackView.view.pop()
    }

    // The window's header row (AppHeaderOverlay) shows over this page's
    // header: it names this place and sets how much of the header's
    // right to keep clear.
    readonly property string appHeaderPlace: "about"
    property real appHeaderReserve: 0

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
            // Room for the window's header row (AppHeaderOverlay), which
            // lies over this header and sets this width itself.
            Item {
                objectName: "appHeaderSpace"
                Layout.preferredWidth: root.appHeaderReserve
                Layout.minimumWidth: root.appHeaderReserve
            }
        }
    }

    Flickable {
        objectName: "aboutScroll"
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
            x: Theme.snap(Math.max(32, (parent.width - width) / 2))
            width: Math.min(parent.width - 64, 640)
            y: 32
            spacing: 20

            // The mark and the name side by side, the mark as tall as the
            // name block it stands beside (name, tagline, address, version);
            // the website's introduction runs on under both, in the name's
            // column, where it fits (Sebastian, 2026-09-28). The block
            // takes what the row gives it and never widens the page.
            RowLayout {
                objectName: "aboutHead"
                Layout.fillWidth: true
                spacing: Theme.rowSpacing * 2
                Image {
                    objectName: "aboutLogo"
                    source: "qrc:/qt/qml/SeabassGui/qml/icons/seabass_soundbass.svg"
                    Layout.preferredWidth: nameBlock.implicitHeight
                    Layout.preferredHeight: nameBlock.implicitHeight
                    Layout.alignment: Qt.AlignTop
                    sourceSize.width: Math.ceil(nameBlock.implicitHeight)
                    sourceSize.height: Math.ceil(nameBlock.implicitHeight)
                    fillMode: Image.PreserveAspectFit
                }

                ColumnLayout {
                    id: headBlock
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 0
                    spacing: 10

                    ColumnLayout {
                        id: nameBlock
                        objectName: "aboutNameBlock"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 2
                        Label {
                            text: "Seabass"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.family: Theme.titleFamily
                            font.weight: Theme.titleWeight
                            font.pointSize: Theme.titleLarge
                        }
                        Label {
                            text: "Your DJ toolbox"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pointSize: Theme.baseFontPointSize * 1.3
                            color: Qt.lighter(Theme.accent, 1.3)
                        }
                        // The site's address, readable and clickable, right
                        // under the name: the one line to remember when telling
                        // someone else where to get this.
                        Label {
                            objectName: "aboutWebsiteLink"
                            text: "<a href=\"" + root.websiteUrl + "\">vizzzion.org/seabass</a>"
                            textFormat: Text.StyledText
                            linkColor: Theme.accent
                            font.pointSize: Theme.baseFontPointSize
                            Layout.topMargin: 4
                            onLinkActivated: link => Qt.openUrlExternally(link)
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                        }

                        // Which Seabass this is, right under the name: the
                        // question a bug report starts with, and the one About
                        // is opened to answer. The build line is selectable so
                        // it can be copied into one whole.
                        ColumnLayout {
                            objectName: "aboutVersionBlock"
                            visible: root.versionNumber.length > 0
                            spacing: 2
                            Layout.topMargin: 10
                            Label {
                                objectName: "aboutVersion"
                                text: "Version " + root.versionNumber
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.pointSize: Theme.baseFontPointSize * 1.5
                                font.weight: Font.DemiBold
                            }
                            TextEdit {
                                objectName: "aboutBuild"
                                text: root.buildLine
                                visible: text.length > 0
                                wrapMode: TextEdit.Wrap
                                Layout.fillWidth: true
                                readOnly: true
                                selectByMouse: true
                                color: Theme.textMuted
                                selectionColor: Theme.accent
                                font.family: Theme.dataFamily
                                font.pointSize: Theme.fontSmall
                            }
                        }
                    }

                    Label {
                        objectName: "aboutIntro"
                        text: "Confidently move between Pioneer and Denon DJ ecosystems. Seabass works on the "
                            + "library that is already on your USB stick, keeping the Rekordbox and Engine DJ "
                            + "copies of it in step. Seabass provides the tools that others forgot to hand to you."
                        wrapMode: Text.WordWrap
                        font.italic: true
                        font.pointSize: Theme.baseFontPointSize * 1.1
                        Layout.fillWidth: true
                    }
                }
            }

            ColumnLayout {
                objectName: "aboutWhy"
                spacing: 10
                Layout.fillWidth: true
                Subtitle { text: "Why it exists" }
                Label {
                    text: "I created Seabass to make it easier to switch between Denon and Pioneer hardware. "
                        + "A DJ's library (usually on a USB stick) is their most precious asset, and we need "
                        + "better tools to maintain it, fix it and to make sure we don't lose it."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
                Label {
                    text: "Seabass empowers you, it doesn't baby you. It gives you powerful tools and it is "
                        + "designed to work transparently. While its tools allow you to also shoot yourself in "
                        + "the foot, it does its best to guide you in avoiding it, and if things go south, it "
                        + "allows you to get your precious data back."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
                Label {
                    text: "Finally, I think quality software should be available to everyone. Therefore I made "
                        + "Seabass Free Software. No spying, no phoning home, no ads, just a modest "
                        + "encouragement to support its development. Whatever you may be able to spare."
                    wrapMode: Text.WordWrap
                    font.pointSize: Theme.baseFontPointSize * 1.1
                    color: Theme.textMuted
                    Layout.fillWidth: true
                }
            }

            // Under whose roof: KDE's, with its mark. The source lives on
            // KDE's own forge, and that is where the link goes.
            Rectangle {
                objectName: "aboutKdeNote"
                Layout.fillWidth: true
                color: Theme.groupBackground
                border.color: Theme.borderSubtle
                border.width: 1
                radius: 4
                implicitHeight: kdeRow.implicitHeight + 24
                RowLayout {
                    id: kdeRow
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: Theme.rowSpacing
                    Image {
                        objectName: "aboutKdeLogo"
                        source: "qrc:/qt/qml/SeabassGui/qml/icons/kde.svg"
                        sourceSize.width: 48
                        sourceSize.height: 48
                        Layout.preferredWidth: 48
                        Layout.preferredHeight: 48
                        Layout.alignment: Qt.AlignVCenter
                        fillMode: Image.PreserveAspectFit
                        TapHandler { onTapped: Qt.openUrlExternally(root.kdeUrl) }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                    }
                    Label {
                        objectName: "aboutKdeText"
                        text: "Seabass is developed under the <a href=\"" + root.kdeUrl + "\">KDE</a> umbrella. "
                            + "Its source code is published on <a href=\"" + root.sourceUrl + "\">KDE's Invent</a> "
                            + "for you to look at, to share it and to modify it."
                        textFormat: Text.StyledText
                        linkColor: Theme.accent
                        wrapMode: Text.WordWrap
                        font.pointSize: Theme.baseFontPointSize
                        Layout.fillWidth: true
                        onLinkActivated: link => Qt.openUrlExternally(link)
                        HoverHandler {
                            enabled: parent.hoveredLink.length > 0
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
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
