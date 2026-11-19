// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// A badge at its own width says all its words; one given less elides
// them and says so.
//
// The label used to take its width back out of the badge's,
// badge.width - 12 - (icon + spacing), where the badge's width was
// (icon + spacing) + label + 12: in doubles that round trip can land one
// ulp under the label's implicit width, and a badge with all the room it
// asked for elided its last letters ("Ne...") and reported itself
// truncated. Icon sizes are fractions wherever the font is not 10 pt on a
// 96 dpi screen, and Theme.scaled() is 1.3 on a Mac.
TestCase {
    id: testCase
    name: "StatusBadge"
    when: windowShown
    visible: true
    width: 600
    height: 400

    Component {
        id: badgeComponent
        StatusBadge {
            badgeColor: "#c0392b"
        }
    }

    // A badge with `word` in it at `pointSize` (the label's own font, set
    // directly: Theme's floor of the smallest readable size pins the tiny
    // font on some machines) and `iconSize`, or no icon for 0.
    function makeBadge(word, pointSize, iconSize) {
        const badge = createTemporaryObject(badgeComponent, testCase,
                                            {label: word, iconName: iconSize > 0 ? "dialog-warning" : ""});
        verify(badge !== null);
        findChild(badge, "badgeLabel").font.pointSize = pointSize;
        if (iconSize > 0) {
            findChild(badge, "badgeIcon").size = iconSize;
        }
        return badge;
    }

    // The icon sizes at which the old arithmetic lands under the label's
    // width: the badge's width summed as the badge sums it, and the
    // label's taken back out of it as the label took it.
    function lossySizes(labelWidth, spacing) {
        const sizes = [];
        for (let i = 0; i < 4000 && sizes.length < 8; ++i) {
            const size = 8 + i * 0.0137;
            const width = (size + spacing) + labelWidth + 12;
            if (width - 12 - (size + spacing) < labelWidth) {
                sizes.push(size);
            }
        }
        return sizes;
    }

    function test_atItsOwnWidthNothingElides_data() {
        // 10 pt, and the sizes a 1.3 scale and fractional fonts make.
        return [
            {tag: "9pt", pointSize: 9},
            {tag: "10pt", pointSize: 10},
            {tag: "11.05pt", pointSize: 11.05},
            {tag: "13pt (macOS)", pointSize: 13},
            {tag: "15.6pt", pointSize: 15.6},
        ];
    }

    function test_atItsOwnWidthNothingElides(data) {
        const badges = [];
        let lossy = 0;
        for (const word of ["New", "Ne", "Replaced", "Already the same", "Not in Engine", "Kept"]) {
            const probe = makeBadge(word, data.pointSize, 13);
            const labelWidth = findChild(probe, "badgeLabel").implicitWidth;
            const spacing = findChild(probe, "badgeContent").spacing;
            const sizes = lossySizes(labelWidth, spacing);
            lossy += sizes.length;
            // And ordinary fractional sizes besides, a 1.3 scale's among them.
            for (const size of sizes.concat([9.1, 12.35, 13 * 1.3, 16.9, 17.333])) {
                const badge = makeBadge(word, data.pointSize, size);
                // What a layout or a binding gives a badge with room: its
                // own implicit width, exactly.
                badge.width = Qt.binding(() => badge.implicitWidth);
                badges.push({badge: badge, size: size});
            }
            const plain = makeBadge(word, data.pointSize, 0);
            plain.width = Qt.binding(() => plain.implicitWidth);
            badges.push({badge: plain, size: 0});
        }
        verify(lossy > 0, "there are icon sizes where the old arithmetic loses an ulp");
        verify(waitForRendering(testCase));
        for (const entry of badges) {
            const label = findChild(entry.badge, "badgeLabel");
            verify(!entry.badge.truncated, "\"" + entry.badge.label + "\" at icon size " + entry.size
                   + " and its own width " + entry.badge.width + " is not elided");
            compare(label.width, label.implicitWidth,
                    "\"" + entry.badge.label + "\" at icon size " + entry.size + ": the label has its whole width");
        }
    }

    function test_lessWidthElides() {
        for (const iconSize of [0, 13 * 1.3]) {
            const badge = makeBadge("Already the same", 13, iconSize);
            badge.width = badge.implicitWidth - 10;
            verify(waitForRendering(badge));
            verify(badge.truncated, "a badge narrower than its words elides them (icon size " + iconSize + ")");
            const label = findChild(badge, "badgeLabel");
            verify(label.width < label.implicitWidth);
            verify(label.mapToItem(badge, label.width, 0).x <= badge.width, "and stays inside the badge");
        }
    }
}
