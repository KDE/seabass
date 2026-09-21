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
        // Sebastian's own words, and the clause a previous edit garbled
        // ("Aside from my time, hardware to test with I also have to
        // pay"), so the sentence is pinned where it went wrong rather
        // than only at its opening.
        var costs = findChild(page, "supportCosts").text;
        verify(costs.indexOf("isn't free for me") >= 0, costs);
        verify(costs.indexOf("Aside from my time and hardware to test with, I also have to pay") >= 0, costs);
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

        // The heart beats, once every three seconds, with the same
        // movement as the button that opens this page. Checked as a beat
        // rather than as a property: an animation that is declared but not
        // running, or one whose target never moves, would pass any check
        // of its configuration -- so this watches the scale actually
        // change. Sampled over a little more than one whole period, since
        // most of a period is the rest between beats and a shorter window
        // could land entirely inside it.
        var heart = findChild(page, "supportHeart");
        verify(heart !== null, "the page has its heart");
        var seen = {};
        for (var i = 0; i < 66; ++i) {
            seen[heart.scale.toFixed(3)] = true;
            wait(50);
        }
        verify(Object.keys(seen).length > 3,
               "the heart moves over one beat and its rest, and does not sit still: "
               + Object.keys(seen).join(" "));
        // And it swells rather than merely drifting: the beat's first
        // swell is 1.12, so something above 1.05 has to have been seen.
        var swelled = false;
        for (var key in seen) {
            if (parseFloat(key) > 1.05) {
                swelled = true;
            }
        }
        verify(swelled, "the heart swells: " + Object.keys(seen).join(" "));

        if (screenshotDir && screenshotDir.length > 0) {
            // The heart is stopped at rest first. Grabbed mid-beat it
            // lands anywhere between scale 1.0 at opacity 0.85 and 1.12 at
            // 1.0, differently on every run -- and a shot caught in the
            // quiet part reads as a washed-out heart, which is a colour
            // bug someone would then go looking for and never find.
            var beat = findChild(page, "supportHeartbeat");
            verify(beat !== null, "the heartbeat must be reachable to be stopped for the shot");
            beat.stop();
            heart.scale = 1.0;
            heart.opacity = 1.0;
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/donation-page.png");
        }
    }

    // The portrait is painted into a Canvas rather than masked with a
    // shader, precisely so it renders offscreen the same way it renders
    // on a display. What a screenshot cannot tell apart, though, is a
    // circle cut from a photo and a circle cut from a resource that was
    // never registered: both are a dark disc. So the loading is asserted
    // here and the look is left to the shot above.
    function test_thePortraitIsThereAndSquareAtTheTopRight() {
        var page = createTemporaryObject(pageComponent, testCase);
        waitForRendering(page);
        var portrait = findChild(page, "supportPortrait");
        verify(portrait !== null, "the page has its portrait");
        // loadImage() is asynchronous, so an empty first frame is
        // legitimate; a missing resource looks the same and never
        // resolves.
        tryVerify(function() { return portrait.isImageLoaded(portrait.photo); }, 3000,
                  "the portrait resource loaded from " + portrait.photo);
        verify(portrait.width > 0, "the portrait has a size");
        compare(portrait.width, portrait.height, "square, so the rounded corners are equal ones");

        // Top and right flush with the text block, which is what was
        // asked for and what a screenshot is worst at proving: a portrait
        // a few pixels out looks deliberate. Compared in the page's own
        // coordinates, since the row and the column have different
        // parents.
        const heading = findChild(page, "supportHeading");
        const title = findChild(page, "supportTitle");
        verify(heading !== null && title !== null);
        const portraitInPage = portrait.mapToItem(page, 0, 0);
        const titleInPage = title.mapToItem(page, 0, 0);
        const headingInPage = heading.mapToItem(page, 0, 0);
        fuzzyCompare(portraitInPage.y, titleInPage.y, 1,
                     "the portrait's top sits on the first line of the text");
        fuzzyCompare(portraitInPage.x + portrait.width, headingInPage.x + heading.width, 1,
                     "and its right edge on the text block's right edge");
        verify(portraitInPage.x > titleInPage.x,
               "with the text to its left, not under it");
    }
}
