// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The Library Health hub: one card per check, each saying what it found in
// a sentence. Drives HealthCheckCard directly -- the hub itself needs a
// real LibraryConsistencyController, which needs a library on disk -- so
// what is covered here is the part that decides what the user reads.
TestCase {
    id: testCase
    name: "LibraryHealthHub"
    width: 800
    height: 600
    visible: true
    when: windowShown

    Component {
        id: cardComponent
        HealthCheckCard {}
    }

    // The hub page itself. Pointed at a stick that is not there, the same
    // way tst_PagesCompile does it: nothing scans, but the page still
    // builds and lays out, which is all the margin check needs.
    // The page requires a playback controller; the real one, unpointed at
    // any stick, the way tst_PagesCompile builds its pages.
    PlaybackController { id: realPlayback }

    Component {
        id: pageComponent
        LibraryHealthHubPage {
            playbackController: realPlayback
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    function findByObjectName(item, name) {
        if (!item) return null;
        if (item.objectName === name) return item;
        var kids = item.children ? item.children : [];
        for (var i = 0; i < kids.length; ++i) {
            var found = findByObjectName(kids[i], name);
            if (found) return found;
        }
        return null;
    }

    // The repair takes the stick away and brings it back on purpose, and
    // the window asks the page in front whether that is expected before it
    // says "USB stick removed" -- a scary message to get for the middle of
    // a job the user just pressed for. The repair below finds no device
    // behind this made-up path and gives up straight away, which is all
    // this needs: the flag has to be up for as long as it runs.
    function test_theStickIsNotAnnouncedAsGoneWhileItIsBeingRepaired() {
        var page = createTemporaryObject(pageComponent, testCase);
        tryCompare(page.consistencyController, "busy", false);
        compare(page.stickAwayExpected, false);
        page.consistencyController.repairStickFilesystem();
        // Set before the worker starts and nothing can run a queued slot
        // inside this call, so this is the state during the repair.
        compare(page.stickAwayExpected, true);
        tryCompare(page.consistencyController, "repairingFilesystem", false);
        compare(page.stickAwayExpected, false);
    }

    function test_aCleanCheckStillGetsACard() {
        // "Nothing wrong here" is a result. A page that only lists problems
        // cannot distinguish a clean library from a check that never ran.
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Tracks and their files",
            summary: "Every track in every catalog on this stick points at a file that is really there.",
            ok: true
        });
        compare(findByObjectName(card, "checkTitle").text, "Tracks and their files");
        verify(findByObjectName(card, "checkSummary").text.length > 0);
        // No action offered when there is nothing to act on.
        compare(findByObjectName(card, "checkAction").parent.visible, false);
    }

    // #38. A finding with no action and no tally is a shape nothing else
    // on this page has, and both halves of it are easy to undo by
    // accident: someone adds a fixableCount "for consistency" and the
    // card starts claiming Seabass can fix none of them, or someone adds
    // an actionLabel and it offers a button for work it must not do.
    function test_aReportOnlyFindingOffersNoActionAndNoTally() {
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Track analysis",
            summary: "1214 of 1564 tracks have not been analysed for Engine players. Seabass does not do this "
                   + "itself: the analysis is the player's own beatgrid, waveform and key detection.",
            ok: false
        });
        verify(findByObjectName(card, "checkSummary").text.indexOf("1214 of 1564") >= 0);
        // The number is in the sentence, not drawn as "fixable / found".
        compare(card.hasTally, false);
        // And nothing offers to do the work.
        compare(card.actionLabel, "");
        compare(findByObjectName(card, "checkAction").parent.visible, false);
    }

    // The other half: the page must not say "every track is analysed"
    // about a library that simply cannot say. An Engine 1.x library and a
    // fully analysed one both report zero, and they are not the same.
    function test_aLibraryThatCannotSayIsNotReportedAsClean() {
        var page = createTemporaryObject(pageComponent, testCase);
        tryCompare(page.consistencyController, "busy", false);
        // No stick, so nothing was read and nothing is known.
        compare(page.consistencyController.analysisKnown, false);
        verify(page.analysisSummary.indexOf("Every Engine track has been analysed") < 0);
    }

    function test_aFindingOffersItsOneAction() {
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Memory cues at 0:00",
            summary: "27 memory cues sit at 0:00.",
            ok: false,
            actionLabel: "Review these cues"
        });
        var action = findByObjectName(card, "checkAction");
        compare(action.text, "Review these cues");
        compare(action.enabled, true);

        var fired = 0;
        card.actionRequested.connect(function() { fired++; });
        action.clicked();
        compare(fired, 1);
    }

    function test_aBlockedActionSaysWhy() {
        // Rather than a button that silently does nothing -- the case the
        // format-divergence spec runs into, where Seabass can remove a row
        // but cannot yet add one.
        var card = createTemporaryObject(cardComponent, testCase, {
            title: "Library formats in step",
            summary: "483 tracks exist in OneLibrary but not in rekordbox.",
            ok: false,
            actionLabel: "Add them to rekordbox",
            actionEnabled: false,
            actionDisabledReason: "Seabass cannot add rows to export.pdb yet."
        });
        compare(findByObjectName(card, "checkAction").enabled, false);
        var reason = findByObjectName(card, "checkActionReason");
        compare(reason.visible, true);
        verify(reason.text.indexOf("cannot add rows") >= 0);
    }

    function test_screenshotOfTheCardStates() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var states = [
            {name: "health-card-clean", props: {title: "Tracks and their files",
                summary: "Every track in every catalog on this stick points at a file that is really there.",
                ok: true}},
            {name: "health-card-finding", props: {title: "Memory cues at 0:00",
                summary: "27 cues sit at 0:00. These are almost always accidental: a stray press "
                       + "while the track was at the start, rather than something you placed on purpose.",
                ok: false, actionLabel: "Review these cues"}},
            {name: "health-card-tally-some", props: {title: "Cover art",
                summary: "29 of 1271 Engine tracks have cover art no player can show. 27 of them can be rebuilt "
                       + "from the rekordbox art on this stick, the tracks' own tags, or a backup on this computer.",
                ok: false, fixableCount: 27, foundCount: 29, actionLabel: "Review cover art"}},
            {name: "health-card-tally-none", props: {title: "Cover art",
                summary: "2 of 1271 Engine tracks have cover art no player can show. No copy of them was found on "
                       + "this stick, in the tracks themselves, or in a backup.",
                ok: false, fixableCount: 0, foundCount: 2, actionLabel: "Review cover art"}},
            {name: "health-card-tally-all", props: {title: "Memory cues at 0:00",
                summary: "27 cues sit at 0:00, and every one of them can be taken out.",
                ok: false, fixableCount: 27, foundCount: 27, actionLabel: "Review these cues"}},
            // #38: a finding that reports and offers nothing. Worth a
            // frame of its own because it is the only card with neither
            // a tally nor an action, and "looks unfinished" is a real
            // risk for that shape.
            {name: "health-card-report-only", props: {title: "Track analysis",
                summary: "1214 of 1564 tracks have not been analysed for Engine players. The player analyses each "
                       + "one the first time it is loaded, which takes a moment, happens once, and is written back "
                       + "to the stick -- but that moment is spent on the deck. Load them once before the gig, or "
                       + "let Engine DJ analyse the library, and the first load at the gig is instant. Seabass does "
                       + "not do this itself: the analysis is the player's own beatgrid, waveform and key "
                       + "detection.",
                ok: false}},
            {name: "health-card-running", props: {title: "Tracks and their files",
                summary: "Checking every row in every catalog against the files on the stick (rekordbox)...",
                running: true}}
        ];
        for (var i = 0; i < states.length; ++i) {
            var card = createTemporaryObject(cardComponent, testCase, states[i].props);
            card.width = 640;
            waitForRendering(card);
            grabImage(card).save(screenshotDir + "/" + states[i].name + ".png");
        }
    }
    // The page used to fill its parent with no margins at all, so every
    // card ran into the window edge while every sibling page inset its
    // content by 16. Asserted on both sides: a left-only anchor would
    // satisfy a check that only looked at x.
    // The breadcrumb was the page's bare header, so it stretched across
    // the full width and its text started at the header's own inset
    // rather than the body's. It is a ToolBar now, like every other
    // section page, and this asserts the thing that was actually wrong:
    // the two left edges agree.
    function test_header_text_lines_up_with_the_body() {
        var page = createTemporaryObject(pageComponent, testCase, {width: 800, height: 600});
        waitForRendering(page);
        var scroll = findByObjectName(page, "healthScroll");
        verify(scroll !== null);
        var crumbText = null;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                // By objectName: the crumb draws Breeze's go-home icon
                // now, so it has no text to match against. Its content
                // item is what has to line up with the body.
                if (child.objectName === "homeCrumb") {
                    crumbText = child.contentItem;
                }
                walk(child);
            }
        }
        walk(page);
        verify(crumbText !== null, "the Home crumb's content item was not found");
        compare(crumbText.mapToItem(page, 0, 0).x, scroll.x,
                "breadcrumb text and page content must share a left edge");
    }

    function test_content_is_inset_from_both_edges() {
        var page = createTemporaryObject(pageComponent, testCase, {width: 800, height: 600});
        waitForRendering(page);
        var scroll = findByObjectName(page, "healthScroll");
        var column = findByObjectName(page, "healthColumn");
        verify(scroll !== null);
        verify(column !== null);
        compare(scroll.x, 16);
        compare(page.width - (scroll.x + scroll.width), 16);
        // And the content inside it stays within that inset box.
        verify(column.width > 0);
        verify(column.width <= scroll.width);
    }
}
