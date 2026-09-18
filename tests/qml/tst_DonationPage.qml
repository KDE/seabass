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
        // Both asks in this sentence are links, and each goes where it
        // says. This check used to look for "kde.org/donate" here, which
        // is the NEXT sentence's link -- so it passed while the word
        // Patreon pointed at the KDE donation page and "one-time
        // donation" was plain text with nowhere to click. The page
        // offered the same third-party link twice and neither of the two
        // things it asks for.
        var donate = findChild(page, "supportDonate").text;
        verify(donate.indexOf("<a href=\"https://paypal.me/sjkugler\">one-time donation</a>") >= 0,
               "the one-time ask links to PayPal: " + donate);
        verify(donate.indexOf("<a href=\"https://www.patreon.com/cw/SebastianKugler\">Patreon</a>") >= 0,
               "and the regular ask links to Patreon: " + donate);
        verify(donate.indexOf("kde.org") < 0,
               "neither of them is the KDE donation page, which is the sentence below: " + donate);
        // Which is still there, and still its own ask.
        verify(findChild(page, "supportKde").text.indexOf("https://kde.org/donate") >= 0);
        compare(findChild(page, "supportThanks").text, "Thank you.");


        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/donation-page.png");
        }
    }
}
