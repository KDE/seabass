// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The row both metadata pages share.
TestCase {
    id: testCase
    name: "MetadataTrackDelegate"
    width: 620
    height: 160
    visible: true
    when: windowShown

    Component {
        id: rowComponent
        MetadataTrackDelegate { width: 600; title: "Major Tom"; artist: "DJ Amador" }
    }

    // The tick box shows what the page decided, not what was clicked. A page
    // that refuses to stage a row (the library is held elsewhere, the row has
    // nothing it can write) leaves `selected` alone, and the box used to stay
    // ticked anyway -- over nothing, and out of Select None's reach.
    function test_aRefusedTickDoesNotStay() {
        var row = createTemporaryObject(rowComponent, testCase, {index: 0, selected: false});
        verify(row !== null);
        var asked = [];
        var accept = false;
        row.selectionToggled.connect(function(wanted) {
            asked.push(wanted);
            if (accept) {
                row.selected = wanted;
            }
        });
        var box = findChild(row, "selectCheckBox");
        verify(box !== null);

        mouseClick(box);
        compare(asked.length, 1);
        compare(asked[0], true, "the page is asked to stage the row");
        compare(box.checked, false, "a refused tick must not stay ticked");

        accept = true;
        mouseClick(box);
        compare(box.checked, true, "an accepted tick stays");
        row.selected = false;
        compare(box.checked, false, "and the page can still clear it, as Select None does");
    }

    // What a backup of this row would hold, said in the opened-up half.
    // The badge above it is one or two words -- "new" on a track the
    // store has never seen -- which says that something would be stored
    // without saying what, and on a stick whose tracks carry no cues
    // that list is the whole answer.
    function test_theDetailSaysWhatBackingUpWouldStore() {
        var delegate = createTemporaryObject(rowComponent, testCase, {
            index: 0, title: "Jungle Love", artist: "Someone", expanded: true,
            storesSummary: "playlist membership, play count, cover art",
        });
        verify(delegate !== null, "the delegate must instantiate");
        var value = findChild(delegate, "storesSummaryValue");
        verify(value !== null, "the detail must be there");
        compare(value.visible, true);
        compare(value.text, "playlist membership, play count, cover art");

        // A row that is not an offer says nothing: the store's own
        // browse list uses the same delegate.
        var stored = createTemporaryObject(rowComponent, testCase, {
            index: 1, title: "Jungle Love", artist: "Someone", expanded: true,
        });
        compare(findChild(stored, "storesSummaryValue").visible, false);
    }

    // The waveform, with the cues on it, in the opened-up half only. Not
    // built for a collapsed row at all: a list of fourteen hundred rows
    // must not construct fourteen hundred canvases to show none of them.
    function test_theWaveformIsBuiltOnlyForAnOpenRow() {
        const cues = [{kind: "hot", hotCueNumber: 1, positionMs: 30000, isLoop: false, loopEndMs: 0,
                       color: "#e03c3c", comment: ""}];
        const row = createTemporaryObject(rowComponent, testCase, {
            index: 0, showWaveform: true, waveformCues: cues, waveformDurationMs: 240000,
            waveformData: [{low: 0.4, mid: 0.5, high: 0.2}, {low: 0.8, mid: 0.3, high: 0.1}],
        });
        verify(row !== null);
        compare(findChild(row, "waveformLoader").active, false, "collapsed: nothing built");
        verify(findChild(row, "rowWaveform") === null, "and no waveform to find");

        row.expanded = true;
        tryVerify(() => findChild(row, "rowWaveform") !== null, 2000, "opened: the waveform is built");
        const waveform = findChild(row, "rowWaveform");
        compare(waveform.cueData.length, 1, "with the row's cues on it");
        compare(waveform.trackDurationMs, 240000, "placed against the track's length");
        compare(waveform.waveformData.length, 2);
        waitForRendering(row);
        verify(waveform.width > 0 && waveform.height > 0,
               "and it has room to draw: " + waveform.width + "x" + waveform.height);

        // A page that never asked for one gets none, open or not.
        const plain = createTemporaryObject(rowComponent, testCase, {index: 1, expanded: true});
        compare(findChild(plain, "waveformLoader").active, false);
    }

    // No waveform to draw: the cues still go on a flat line, and hovering
    // the line says why there is nothing else, in the words the page
    // passes. Why is the page's to say: on the store's own list the backup
    // holds none, on a stick's list the stick has none.
    function test_aMissingWaveformSaysWhatThePageSays() {
        const row = createTemporaryObject(rowComponent, testCase, {
            index: 0, showWaveform: true, expanded: true, waveformDurationMs: 240000,
            waveformMissingText: "Waveform not part of backup",
            waveformCues: [{kind: "hot", hotCueNumber: 2, positionMs: 200000, isLoop: false, loopEndMs: 0,
                            color: "#e03c3c", comment: ""}],
        });
        tryVerify(() => findChild(row, "rowWaveform") !== null, 2000);
        const waveform = findChild(row, "rowWaveform");
        compare(waveform.missingText, "Waveform not part of backup");
        compare(waveform.hasWaveform, false);
        const area = findChild(waveform, "waveformMouseArea");
        verify(area !== null);
        // Well away from the one cue, at 200 of 240 seconds.
        mouseMove(waveform, waveform.width * 0.1, waveform.height / 2);
        tryVerify(() => area.containsMouse, 1000);
        compare(area.explainMissing, true, "hovering the placeholder explains it");
        // Over the cue, the cue names itself instead.
        mouseMove(waveform, waveform.width * 200000 / 240000, waveform.height / 2);
        tryVerify(() => waveform.hoveredCueText.length > 0, 1000);
        compare(waveform.hoveredCueText, "Hot cue 2");
        compare(area.explainMissing, false);

        // With a waveform there is nothing to explain.
        waveform.waveformData = [{low: 0.4, mid: 0.5, high: 0.2}];
        mouseMove(waveform, waveform.width * 0.1, waveform.height / 2);
        compare(area.explainMissing, false);
    }

    // A row whose page says nothing claims nothing: the delegate has no
    // reason of its own to give. It used to say "Waveform not part of
    // backup" on every row, which on a stick's list is not true.
    function test_aMissingWaveformHasNoReasonByDefault() {
        const row = createTemporaryObject(rowComponent, testCase, {
            index: 0, showWaveform: true, expanded: true, waveformDurationMs: 240000,
        });
        tryVerify(() => findChild(row, "rowWaveform") !== null, 2000);
        const waveform = findChild(row, "rowWaveform");
        compare(waveform.missingText, "");
        compare(findChild(waveform, "waveformMouseArea").explainMissing, false);
    }
}
