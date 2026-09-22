// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Library Health's findings page, driven by a stand-in controller: what
// is staged is said beside the buttons that staged it.
//
// It used to show one number for the page -- so staging 29 stray cue
// removals put "29 staged, not saved yet" next to "Stage All Safe
// Repairs", a button about missing files, and nothing at all next to the
// cue buttons that had just done the staging.
TestCase {
    id: testCase
    name: "LibraryConsistencyPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }

    // Only what the two notes and the rows around them read.
    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property int repairableCount: 0
            property int unstagedRepairableCount: 0
            property int sampleRateMissingCount: 0
            property int sampleRateFixableCount: 0
            property bool sampleRateFillStaged: false
            property bool playerWillOfferImport: false
            property bool importMarkStaged: false
            function markRekordboxImported() { importMarkStaged = true; }
            function unstageRekordboxImportMark() { importMarkStaged = false; }
            function fillSampleRates() { sampleRateFillStaged = true; }
            function unstageSampleRateFill() { sampleRateFillStaged = false; }
            property int unstagedJunkCueCount: 0
            property int stagedCount: stagedIssueCount + stagedJunkCueCount
            property int stagedIssueCount: 0
            property int stagedJunkCueCount: 0
            property int artworkRepairableCount: 0
            property bool artworkRepairStaged: false
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            property var issues: ListModel {}
            property var junkCues: ListModel {}
            property var playlistNames: []
            property var playlistTrackCounts: ({})
            function scan(a, b, c) {}
            function cancelScan() {}
        }
    }

    Component {
        id: pageComponent
        LibraryConsistencyPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            playbackController: realPlayback
        }
    }

    function findLabelContaining(item, needle) {
        if (item.text !== undefined && typeof item.text === "string" && item.text.indexOf(needle) >= 0) {
            return item;
        }
        for (const child of item.children) {
            const found = findLabelContaining(child, needle);
            if (found !== null) {
                return found;
            }
        }
        return null;
    }

    function collectHighlights(item, out) {
        if (item.highlightCuePositionMs !== undefined) {
            out.push(item.highlightCuePositionMs);
        }
        for (const child of item.children) {
            collectHighlights(child, out);
        }
    }

    function collectReasons(item, out) {
        if (item.objectName === "junkCueReason" && item.text.length > 0) {
            out.push(item.text);
        }
        for (const child of item.children) {
            collectReasons(child, out);
        }
    }

    function test_eachChecksStagedWorkIsCountedBesideItsOwnButtons() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var missing = findChild(page, "stagedIssuesNote");
        var cues = findChild(page, "stagedJunkCuesNote");
        verify(missing !== null && cues !== null, "both checks must have a staged note");
        compare(missing.visible, false, "nothing staged, nothing said");
        compare(cues.visible, false);

        // Stray cues staged: the cue section says so, the missing-files
        // section stays quiet.
        controller.stagedJunkCueCount = 29;
        compare(cues.visible, true);
        compare(cues.text, "29 staged, not saved yet");
        compare(missing.visible, false, "a staged cue removal is not staged repair work");

        // And the other way round.
        controller.stagedJunkCueCount = 0;
        controller.stagedIssueCount = 4;
        compare(missing.visible, true);
        compare(missing.text, "4 staged, not saved yet");
        compare(cues.visible, false);

        // The page outlives the stand-in otherwise, and spends teardown
        // reading properties off a destroyed object.
        page.destroy();
        wait(0);
    }

    // A "do all of it" button with everything already staged has nothing
    // behind it. It used to stay live, because it asked what the check
    // found rather than what it had left to do.
    function test_theAllButtonsGoQuietOnceEverythingIsStaged() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        // The delegate needs its roles; the buttons under test do not
        // care what is in them.
        var row = {track: {title: "A track", artist: "An artist", filePath: "/nowhere/a.mp3",
                           durationMs: 0, cues: [], side: "engine", sourceId: "1", artworkPath: ""},
                   staged: false, format: "engine", positionMs: 0,
                   reason: "at the very start of the track"};
        controller.junkCues.append(row);
        controller.junkCues.append(row);
        controller.repairableCount = 3;
        controller.unstagedRepairableCount = 3;
        controller.unstagedJunkCueCount = 2;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");

        var repairAll = findButton(page, "Stage All Safe Repairs");
        var removeAll = findButton(page, "Remove All");
        verify(repairAll !== null && removeAll !== null, "both do-all buttons must be there");
        compare(repairAll.enabled, true, "with repairs waiting it is live");
        compare(removeAll.enabled, true, "with cues waiting it is live");

        controller.unstagedRepairableCount = 0;
        controller.unstagedJunkCueCount = 0;
        compare(repairAll.enabled, false, "everything staged: nothing left to press it for");
        compare(removeAll.enabled, false);
        page.destroy();
        wait(0);
    }

    // Buttons are found by their label: the page has no objectName on
    // them, and adding one only for a test would be a worse thing to
    // pin than the words the user reads.
    function findButton(item, text) {
        if (item === null || item === undefined) {
            return null;
        }
        if (item.text === text && item.enabled !== undefined && item.clicked !== undefined) {
            return item;
        }
        var kids = item.children ? item.children : [];
        for (var i = 0; i < kids.length; ++i) {
            var found = findButton(kids[i], text);
            if (found !== null) {
                return found;
            }
        }
        return null;
    }

    // Sample rates: a row without one means every cue on that track is
    // placed by a guess, and the file itself can say what it really is.
    // The button stages, like every other fix on this page.
    // Which playlists end up short a track. The data was already in
    // hand -- the same track list the playlist picker is built from --
    // and was being dropped at the GUI boundary, so a missing file told
    // you a track was gone and never which set now has a gap in it.
    //
    // The three answers are different things, and the third is the one
    // worth being careful about: a reader that does not report
    // memberships gives an empty list, which means "not known" and must
    // never be shown as "in no playlist".
    function test_thePlaylistLineSaysWhichSetsLoseATrack() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");

        const broken = [{playlists: [{name: "Techno/Peak Time", position: 7},
                                     {name: "Warmup", position: 2}]}];

        // Missing: every playlist it was in loses it.
        const missing = page.playlistSentence("missing", broken, null);
        verify(missing.indexOf("2 playlists") >= 0, "both are named as lost: " + missing);
        verify(missing.indexOf("Techno/Peak Time") >= 0);
        verify(missing.indexOf("Warmup") >= 0);

        // Repairable: the kept copy absorbs the row, so only a playlist
        // the kept copy is NOT in actually loses anything.
        const survivorInOne = {playlists: [{name: "Warmup", position: 5}]};
        const partly = page.playlistSentence("repairable", broken, survivorInOne);
        verify(partly.indexOf("1 playlist") >= 0, "only the one the survivor is missing from: " + partly);
        verify(partly.indexOf("Techno/Peak Time") >= 0);
        verify(partly.indexOf("Warmup") < 0, "the survivor covers Warmup, so it is not short a track");

        // And when the kept copy covers all of them, that is worth
        // saying too rather than leaving the row silent.
        const survivorInBoth = {playlists: [{name: "Warmup", position: 5},
                                            {name: "Techno/Peak Time", position: 1}]};
        const none = page.playlistSentence("repairable", broken, survivorInBoth);
        verify(none.indexOf("no set loses a track") >= 0, none);

        // Nothing known: the line is empty, so the row says nothing at
        // all rather than "in no playlist".
        compare(page.playlistsKnown([{playlists: []}], null), false);
        compare(page.playlistSentence("missing", [{playlists: []}], null), "",
                "a missing row with no memberships known says nothing");
        compare(page.playlistsKnown(broken, null), true);

        // Two broken copies of one song in one issue must not name the
        // same playlist twice.
        const twoCopies = [{playlists: [{name: "Warmup", position: 2}]},
                           {playlists: [{name: "Warmup", position: 9}]}];
        compare(page.playlistsLeftShort("missing", twoCopies, null).length, 1);

        page.destroy();
        wait(0);
    }

    // Two checks feed the accidental-cue list now: a cue at the very
    // start of a track, and one of a crowd of hot cues inside its first
    // two seconds (#41). Every row is an offer to delete somebody's
    // cue, so each says why it is there, and the waveform highlights
    // the cue that Remove would actually take.
    //
    // Highlighting 0:00 while removing a cue at 1.188 s is the exact
    // opposite of the "unambiguous which one Remove kills" the row was
    // built for, and it is what this section did the moment a second
    // check started feeding it.
    function test_eachAccidentalCueRowSaysWhyAndPointsAtItself() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const track = {title: "Too Little Too Late", artist: "Joris Voorn", filePath: "/nowhere/a.mp3",
                       durationMs: 300000, cues: [], side: "rekordbox", sourceId: "1", artworkPath: ""};
        controller.junkCues.append({track: track, staged: false, format: "rekordbox", positionMs: 0,
                                    reason: "at the very start of the track"});
        controller.junkCues.append({track: track, staged: false, format: "rekordbox", positionMs: 1188,
                                    reason: "one of 3 hot cues in the first two seconds, which is not a "
                                            + "pattern anyone plays"});
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");

        // The section headline can no longer claim every row is at 0:00.
        const headline = findLabelContaining(page, "look accidental");
        verify(headline !== null, "the section says what it found without naming only one of the two checks");
        verify(headline.text.indexOf("0:00") < 0, "and does not describe a cue at 1.188 s as being at 0:00");

        // Both rows carry their own reason, and the two differ.
        const reasons = [];
        collectReasons(page, reasons);
        compare(reasons.length, 2, "one reason per row");
        verify(reasons[0] !== reasons[1], "the two checks do not describe their rows the same way");
        verify(reasons[1].indexOf("first two seconds") >= 0, reasons[1]);

        // And each row's waveform points at its own cue. The second row
        // removes a cue at 1.188 s; a highlight left at 0 would mark a
        // different cue than the button takes.
        // Counted by value rather than by position: the card passes the
        // property down to the waveform inside it, so each row
        // contributes it more than once and the exact depth is not what
        // this is about.
        const highlights = [];
        collectHighlights(page, highlights);
        verify(highlights.indexOf(1188) >= 0,
               "the clustered row points at its own cue, not at 0:00: " + JSON.stringify(highlights));
        verify(highlights.indexOf(0) >= 0, "and the row that really is at the start still points at 0");
        for (const value of highlights) {
            verify(value === 0 || value === 1188, "no row highlights a cue no row is about: " + value);
        }

        page.destroy();
        wait(0);
    }

    function test_theSampleRateFixStagesAndSaysSo() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        controller.sampleRateMissingCount = 43;
        controller.sampleRateFixableCount = 40;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");

        var summary = findChild(page, "sampleRateSummary");
        verify(summary !== null, "the check must say what it found");
        verify(summary.text.indexOf("43") >= 0 && summary.text.indexOf("40") >= 0,
               "both numbers belong in the sentence: " + summary.text);

        var button = findChild(page, "fillSampleRatesButton");
        var note = findChild(page, "stagedSampleRatesNote");
        verify(button !== null && note !== null);
        compare(button.visible, true, "there is something to fix");
        compare(note.visible, false, "and nothing staged yet");

        button.clicked();
        compare(controller.sampleRateFillStaged, true, "the button stages");
        compare(note.visible, true, "and the page says so where the button is");
        compare(button.text, "Unstage", "the same button takes it back");
        button.clicked();
        compare(controller.sampleRateFillStaged, false);

        page.destroy();
        wait(0);
    }

    // The player's import prompt: the one check here that is about what a
    // Denon player will do next time the stick is in it, and the only one
    // whose "fix" is telling another program something rather than
    // changing what is on the stick.
    function test_theImportPromptIsSaidAndCanBeStagedAway() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        var summary = findChild(page, "importPromptSummary");
        var button = findChild(page, "markImportedButton");
        var note = findChild(page, "stagedImportMarkNote");
        verify(summary !== null && button !== null && note !== null);

        // Nothing to say when the player will leave the library alone.
        verify(summary.text.indexOf("leave the Engine library alone") >= 0, summary.text);
        compare(button.visible, false, "and nothing to offer");

        controller.playerWillOfferImport = true;
        verify(summary.text.indexOf("overwritten") >= 0,
               "what accepting the prompt costs belongs in the sentence: " + summary.text);
        compare(button.visible, true);
        compare(note.visible, false);

        button.clicked();
        compare(controller.importMarkStaged, true, "the button stages");
        compare(note.visible, true, "and says so beside itself");
        compare(button.text, "Unstage");
        button.clicked();
        compare(controller.importMarkStaged, false);

        page.destroy();
        wait(0);
    }
}
