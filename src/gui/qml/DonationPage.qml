// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Opened from the heart button in StickListPage.qml's header.
//
// Sebastian's own words, kept as he wrote them: this page asks for
// something, and asking in someone else's voice reads as a form letter.
// The links open in the browser rather than being spelled out, except
// where the address is the point.
Page {
    id: root

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
                title: "Support Seabass"
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
        // See AboutPage: a Flickable with nothing to scroll still drags
        // into an elastic overshoot, which reads as a bug.
        interactive: contentHeight > height

        ColumnLayout {
            id: content
            x: Math.max(32, (parent.width - width) / 2)
            width: Math.min(parent.width - 64, 640)
            y: 32
            spacing: 20

            HeartIcon {
                id: pageHeart
                objectName: "supportHeart"
                iconSize: Theme.iconSizeLarge
                color: "#aa0000"
                Layout.alignment: Qt.AlignHCenter
                // Once every three seconds, where the button that opens
                // this page beats about every six: there the heart is a
                // detail at the edge of the eye, here it is what the page
                // is about, but a real heart's rate on a page someone is
                // reading turned out to be restless rather than alive.
                // Same movement either way -- see Heartbeat.qml.
                Heartbeat {
                    objectName: "supportHeartbeat"
                    target: pageHeart
                    period: 3000
                }
            }

            // The heading and the face that goes with it: the text on
            // the left, Sebastian at the top right of it. The row spans
            // the content column, so the portrait's right edge IS the
            // text block's right edge, and AlignTop puts its top on the
            // first line of the title rather than on the centre of the
            // row. Nothing here is anchored to a number: change the
            // column's width and both edges follow.
            RowLayout {
                objectName: "supportHeading"
                Layout.fillWidth: true
                spacing: 20

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: 6

                    Label {
                        objectName: "supportTitle"
                        text: "Supporting Seabass"
                        font.family: Theme.titleFamily
                        font.weight: Theme.titleWeight
                        font.pointSize: Theme.titleLarge
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        objectName: "supportByline"
                        // The heart is the page's own mark, in the
                        // sentence it belongs to rather than as an emoji
                        // the font may not have.
                        text: "Seabass is created with love by Sebastian K\u00fcgler (a.k.a. Whaleshark) and friends."
                        wrapMode: Text.WordWrap
                        font.pointSize: Theme.baseFontPointSize * 1.1
                        Layout.fillWidth: true
                    }
                }

            // Sebastian, square with rounded corners, facing the text.
            // The source is a square PNG and the shape is cut here
            // rather than in the file, so the same image can be used
            // differently elsewhere.
            //
            // A Canvas rather than a MultiEffect/OpacityMask: those are
            // shader-based, and this project verifies its UI by
            // rendering pages offscreen, where a shader does not run --
            // a masked portrait would be correct in the source and
            // absent from every screenshot proving it. Canvas paints
            // into an image in software, so what a screenshot shows is
            // what a display shows.
            Canvas {
                id: portrait
                objectName: "supportPortrait"
                readonly property url photo: "qrc:/qt/qml/SeabassGui/qml/images/sebas.png"
                // Scaled with the system font like every other size on
                // these pages, so it stays in proportion to the text
                // beside it rather than shrinking as the type grows.
                readonly property int side: Math.round(120 * Theme.iconScale)
                Layout.preferredWidth: side
                Layout.preferredHeight: side
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
                antialiasing: true

                Component.onCompleted: loadImage(photo)
                onImageLoaded: requestPaint()
                // The canvas is repainted when it is resized too: a font
                // size change moves `side`, and without this the old
                // painting would simply be stretched.
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                // The corner radius, as a share of the side rather than
                // a fixed number of pixels: `side` follows the system
                // font, and a fixed radius would read as rounder on a
                // small portrait and squarer on a large one.
                readonly property real cornerRadius: Math.round(side * 0.16)

                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    if (!isImageLoaded(photo)) {
                        return;
                    }
                    ctx.save();
                    // arcTo rather than roundRect: context2d has no
                    // roundRect on every Qt this targets, and a missing
                    // method here would throw inside a paint handler,
                    // where the failure is a blank canvas rather than an
                    // error anybody sees.
                    const r = Math.min(cornerRadius, Math.min(width, height) / 2);
                    ctx.beginPath();
                    ctx.moveTo(r, 0);
                    ctx.arcTo(width, 0, width, height, r);
                    ctx.arcTo(width, height, 0, height, r);
                    ctx.arcTo(0, height, 0, 0, r);
                    ctx.arcTo(0, 0, width, 0, r);
                    ctx.closePath();
                    ctx.clip();
                    // Mirrored about the vertical axis, so the face looks
                    // INTO the text it sits beside rather than off the
                    // page. Done in the paint rather than with an item
                    // transform so the clip above is not mirrored with
                    // it: the corners stay where the layout put them.
                    ctx.translate(width, 0);
                    ctx.scale(-1, 1);
                    ctx.drawImage(photo, 0, 0, width, height);
                    ctx.restore();
                }
                }
            }

            Label {
                objectName: "supportKindWords"
                textFormat: Text.StyledText
                linkColor: Theme.accent
                text: "Kind words mean a lot to me. If you like Seabass, let me know! An endorsement from a "
                    + "fellow DJ goes a long way in making my day a bit brighter. Send an email to "
                    + "<a href=\"mailto:sebas@kde.org\">sebas@kde.org</a>"
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            Label {
                objectName: "supportCosts"
                text: "With that said, making Seabass available for free isn't free for me. Aside from my time "
                    + "and hardware to test with, I also have to pay for various services that keep this "
                    + "project going. Chipping in is hugely welcome."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
            }

            Label {
                objectName: "supportDonate"
                textFormat: Text.StyledText
                linkColor: Theme.accent
                // Both halves of the sentence are links, and each goes
                // where it says: the one-time ask to PayPal, the regular
                // one to Patreon. "One-time donation" used to be plain
                // text with nowhere to click, and the word Patreon linked
                // to kde.org/donate -- which is the sentence below's link,
                // so the page offered the KDE donation page twice and the
                // two things it actually asks for not at all.
                text: "You can either send a <a href=\"https://paypal.me/sjkugler\">one-time donation</a> or "
                    + "become a regular supporter of Seabass over on "
                    + "<a href=\"https://www.patreon.com/cw/SebastianKugler\">Patreon</a>."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            Label {
                objectName: "supportKde"
                textFormat: Text.StyledText
                linkColor: Theme.accent
                text: "If you'd rather donate money to the KDE community, this is of great value for Seabass "
                    + "as well, you can do so at <a href=\"https://kde.org/donate\">kde.org/donate</a>."
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
                onLinkActivated: link => Qt.openUrlExternally(link)
            }

            Label {
                objectName: "supportThanks"
                text: "Thank you."
                font.pointSize: Theme.baseFontPointSize * 1.1
                Layout.fillWidth: true
            }
        }
    }
}
