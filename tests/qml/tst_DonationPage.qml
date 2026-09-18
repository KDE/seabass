// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The page behind the heart. It asks for something, so what it says and
// where its links point are the whole of it.
TestCase {
    id: testCase
    name: "DonationPage"
    width: 900
    height: 800
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        DonationPage { width: 880; height: 780 }
    }

    function test_theAskIsThereAndItsLinksGoWhereTheySay() {
        var page = createTemporaryObject(pageComponent, testCase);
        verify(page !== null, "the page must instantiate");
        waitForRendering(page);

        compare(findChild(page, "supportTitle").text, "Supporting Seabass");
        verify(findChild(page, "supportByline").text.indexOf("Whaleshark") >= 0,
               "the byline names who made it");
        // The address is the point of this one, so it is spelled out as
        // well as linked.
        var kind = findChild(page, "supportKindWords").text;
        verify(kind.indexOf("mailto:sebas@kde.org") >= 0 && kind.indexOf(">sebas@kde.org<") >= 0, kind);
        verify(findChild(page, "supportCosts").text.indexOf("isn't free for me") >= 0);
        verify(findChild(page, "supportDonate").text.indexOf("kde.org/donate") >= 0,
               "the donation link must be a link: " + findChild(page, "supportDonate").text);
        verify(findChild(page, "supportKde").text.indexOf("https://kde.org/donate") >= 0);
        compare(findChild(page, "supportThanks").text, "Thank you.");

        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/donation-page.png");
        }
    }
}
