// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Maintain's Align Cue Points page with its real controller, on a scratch
// copy of the cue-offsets stick (tests/fixtures/cue-offsets: seven click
// tracks, a click every 0.5 s from 1.000 s, in rekordbox's export and a
// Prime 4's Engine library, with Engine's pad 1 put into the states its
// README lists).
TestCase {
    id: testCase
    name: "AlignCuePointsPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/cue-offsets/stick").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: pageComponent
        AlignCuePointsPage {
            width: 880
            height: 660
            stickLabel: "TESTSTICK"
        }
    }

    function openOn(stick) {
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        const page = createTemporaryObject(pageComponent, testCase, {
            rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "the page must instantiate");
        const c = page.controller;
        tryVerify(() => c.checked && !c.busy, 120000, "the stick is measured");
        compare(c.errorMessage, "");
        return {page: page, c: c, libraryId: libraryId, list: findChild(page, "alignList")};
    }

    // The rows listed, as {key, format, title, offCount, largestOffMs,
    // sameOffset, ticked, staged, detail}.
    function rows(c) {
        const out = [];
        for (let i = 0; i < c.tracks.count; ++i) {
            out.push(c.tracks.get(i));
        }
        return out;
    }

    function byKey(c) {
        const out = {};
        for (const row of rows(c)) {
            out[row.key] = row;
        }
        return out;
    }

    function saveAndWait(o) {
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        verify(session !== null);
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        return summary;
    }

    function freshStick() {
        const stick = stickFixture.wholeStickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        return stick;
    }

    // Engine ids: 91 badcrc (pad 1 right), 92 noencoder, 103 D-lame-44100,
    // 104 D-lame-48000 (those three hold rekordbox's number, an MP3's
    // offset from the click), 107 Lavc (moved by the player). rekordbox
    // ids 102 and 105: a pad a few ms off its click.
    //
    // Each library against its own grid: Engine's pad on the right click
    // (91) is not listed, which it would be by its file's 51 ms were its
    // cues set against the grid on the wrong timeline; the three holding
    // rekordbox's number are listed by just that offset. A DeviceLibrary
    // row and the OneLibrary row of the same analysis file are one cue
    // source, listed once.
    function test_eachLibraryAgainstItsOwnGrid() {
        const o = openOn(freshStick());
        o.c.toleranceMs = 100;
        const listed = byKey(o.c);
        compare(JSON.stringify(Object.keys(listed).sort()),
                JSON.stringify(["engine:103", "engine:104", "engine:107", "engine:92", "rekordbox:102", "rekordbox:105"]));
        compare(listed["engine:103"].detail, "Pad 1 50 ms late");
        compare(listed["engine:92"].detail, "Pad 1 26 ms late");
        compare(listed["engine:104"].detail, "Pad 1 46 ms late");
        compare(listed["engine:107"].detail, "Pad 1 28 ms early");
        compare(listed["rekordbox:102"].detail, "Hot cue B 2 ms late");
        compare(listed["rekordbox:105"].detail, "Hot cue B 3 ms early");
        compare(o.c.cuePointCount, 6);
        // The three holding rekordbox's number are off by their file's
        // own offset between the libraries: listed, said so, and not to
        // be ticked. Which side is wrong is Sync Cue Points' to say.
        for (const key of ["engine:103", "engine:104", "engine:92"]) {
            compare(listed[key].libraryOffset, true, key);
            compare(listed[key].sameOffset, true, key + " cannot be ticked");
        }
        compare(listed["engine:107"].libraryOffset, false, "moved by the player, by another amount");
        compare(o.c.tickableCount, 3);
        o.c.tickAll(true);
        compare(o.c.tickedCount, 3);
        compare(byKey(o.c)["engine:103"].ticked, false);
        verify(o.c.tracksMeasured > 0 && o.c.tracksWithCues >= o.c.tracksMeasured);
    }

    // The slider says how far off the beat still counts: it filters what
    // was read, with nothing read again, and a track it no longer lists is
    // no longer ticked.
    function test_theSliderFiltersByHowFarOffTheBeat() {
        const o = openOn(freshStick());
        const slider = findChild(o.page, "toleranceSlider");
        const label = findChild(o.page, "toleranceLabel");
        verify(slider !== null && label !== null);
        o.c.toleranceMs = 10;
        compare(slider.value, 10);
        compare(label.text, "10 ms");
        compare(JSON.stringify(Object.keys(byKey(o.c)).sort()), JSON.stringify(["rekordbox:102", "rekordbox:105"]));
        compare(findChild(o.page, "alignSummary").text, "2 cue points off the beat in 2 tracks");

        o.c.toleranceMs = 30;
        compare(JSON.stringify(Object.keys(byKey(o.c)).sort()),
                JSON.stringify(["engine:107", "engine:92", "rekordbox:102", "rekordbox:105"]));
        o.c.tickAll(true);
        compare(o.c.tickedCount, 3, "not the one off by its MP3's offset");
        compare(findChild(o.page, "alignSummary").text, "4 cue points off the beat in 4 tracks, 3 ticked");

        // By the slider itself, a step at a time.
        slider.forceActiveFocus();
        keyClick(Qt.Key_Left);
        compare(o.c.toleranceMs, 29);
        compare(label.text, "29 ms");
        o.c.toleranceMs = 27;
        compare(JSON.stringify(Object.keys(byKey(o.c)).sort()), JSON.stringify(["engine:92", "rekordbox:102", "rekordbox:105"]),
                "the pad 28 ms off is past the tolerance now");
        compare(o.c.tickedCount, 2, "and no longer ticked");
        o.c.toleranceMs = 30;
        compare(byKey(o.c)["engine:107"].ticked, false, "listed again, not ticked again");

        o.c.toleranceMs = 2;
        compare(o.c.tracks.count, 1, "2 ms: the one pad just over a millisecond and a half off");
        o.c.toleranceMs = 1;
        compare(o.c.toleranceMs, 2, "never under 2 ms");
        o.c.toleranceMs = 500;
        compare(o.c.toleranceMs, o.c.maximumToleranceMs);
    }

    // Ticked, staged and saved: the listed cue points of the ticked tracks
    // are on their beats when the stick is read again, in rekordbox's
    // analysis file and in Engine's row, and what was not ticked is as it
    // was. Staged, the tolerance and the ticks hold still; Unstage takes
    // it back with nothing written.
    function test_stagedTracksAreAlignedBySave() {
        const o = openOn(freshStick());
        o.c.toleranceMs = 100;
        const before = byKey(o.c);
        const stage = findChild(o.page, "stageButton");
        compare(stage.enabled, false, "nothing ticked");
        const index = key => rows(o.c).findIndex(row => row.key === key);
        o.c.setTicked(index("rekordbox:102"), true);
        o.c.setTicked(index("engine:107"), true);
        compare(o.c.tickedCount, 2);
        compare(stage.text, "Stage Selected");
        mouseClick(stage);
        compare(o.c.stagedCount, 2);
        compare(o.c.errorMessage, "");
        compare(stage.text, "Unstage");
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        compare(session.pendingCount, 2);
        compare(byKey(o.c)["engine:107"].staged, true);
        compare(findChild(o.page, "toleranceSlider").enabled, false, "what is staged was staged at this tolerance");
        o.c.toleranceMs = 10;
        compare(o.c.toleranceMs, 100);

        mouseClick(stage);
        compare(o.c.stagedCount, 0, "Unstage");
        compare(session.pendingCount, 0);
        compare(o.c.tickedCount, 2, "the ticks stay");
        mouseClick(stage);
        compare(session.pendingCount, 2);

        const summary = saveAndWait(o);
        compare(summary.written, 2, JSON.stringify(summary));
        tryVerify(() => o.c.checked && !o.c.busy && o.c.stagedCount === 0, 120000, "read again after the save");
        tryVerify(() => byKey(o.c)["rekordbox:102"] === undefined, 120000, "rekordbox's pad is on its beat");
        const after = byKey(o.c);
        verify(after["engine:107"] === undefined, "Engine's pad is on its beat: " + JSON.stringify(after["engine:107"]));
        for (const key of ["engine:103", "engine:104", "engine:92", "rekordbox:105"]) {
            compare(JSON.stringify(after[key]), JSON.stringify(before[key]), key + " was not ticked and is as it was");
        }
        compare(o.c.tracks.count, 4);
        compare(o.c.tickedCount, 0);
    }

    function test_theSaveButtonSaysAlign() {
        const page = createTemporaryObject(pageComponent, testCase, {
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER", enginePath: "/nonexistent/TESTSTICK/Engine Library"});
        verify(page !== null, "the page must instantiate");
        compare(findChild(page, "saveOverlay").label, "Align");
        tryVerify(() => !page.controller.busy, 30000);
        compare(page.controller.tracks.count, 0);
    }
}
