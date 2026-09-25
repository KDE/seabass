// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

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

    // A metadata row often has no length (the stick never analysed the
    // track, or the row came only from the store). Its cues are what the
    // row is for, so they are drawn all the same, against the span the
    // cues themselves cover; the hover then says each cue's time, and away
    // from a cue that the length is unknown, rather than letting the line
    // pass for the whole track. It used to be a flat line and no cues.
    function test_cuesAreDrawnWithoutATrackLength() {
        const cues = [
            {kind: "hot", hotCueNumber: 1, positionMs: 60000, isLoop: false, loopEndMs: 0, color: "#e03c3c", comment: ""},
            {kind: "hot", hotCueNumber: 2, positionMs: 180000, isLoop: false, loopEndMs: 0, color: "#2ec4f0", comment: ""},
        ];
        const row = createTemporaryObject(rowComponent, testCase, {
            index: 0, showWaveform: true, expanded: true, waveformDurationMs: 0,
            waveformMissingText: "No waveform on the stick for this track", waveformCues: cues,
        });
        tryVerify(() => findChild(row, "rowWaveform") !== null, 2000);
        const waveform = findChild(row, "rowWaveform");
        compare(waveform.trackDurationMs, 0);
        // The last cue plus a tenth: what the markers are placed against.
        const span = 198000;
        // Inside each hot cue's numbered square, drawn from its line to the
        // right along the top edge. The canvas paints on its own schedule,
        // so the grab is retried until it has, or the wait runs out.
        const expected = [{r: 0xe0, g: 0x3c, b: 0x3c}, {r: 0x2e, g: 0xc4, b: 0xf0}];
        // Grabbed through the row: a grab of the canvas item alone comes
        // back without what it painted.
        const drawn = function() {
            const image = grabImage(row);
            let all = true;
            for (let i = 0; i < cues.length; i++) {
                const x = cues[i].positionMs / span * waveform.width;
                const at = waveform.mapToItem(row, x + 7, 2);
                const p = PixelScale.pixel(image, row, at.x, at.y);
                const want = expected[i];
                const match = Math.abs(p.r * 255 - want.r) < 40 && Math.abs(p.g * 255 - want.g) < 40
                    && Math.abs(p.b * 255 - want.b) < 40;
                all = all && match;
            }
            return all;
        };
        tryVerify(drawn, 3000, "both hot cues are drawn");
        compare(waveform.lengthUnknown, true, "no length, and cues placed");
        compare(waveform.cueSpanMs, span);

        const area = findChild(waveform, "waveformMouseArea");
        verify(area.enabled, "the cues can be hovered");
        mouseMove(waveform, 180000 / span * waveform.width, waveform.height / 2);
        tryVerify(() => waveform.hoveredCueText.length > 0, 1000);
        compare(waveform.hoveredCueText, "Hot cue 2 at 3:00", "the cue says its time, the line cannot");
        mouseMove(waveform, waveform.width * 0.1, waveform.height / 2);
        tryVerify(() => waveform.hoveredCueText.length === 0, 1000);
        compare(area.explainLength, true);
        compare(area.toolTipText, "No waveform on the stick for this track. "
                + "Track length unknown: cues are spaced by their times, up to the last one.");

        // With a length, nothing changes: the cues sit against the track.
        waveform.trackDurationMs = 240000;
        compare(waveform.cueSpanMs, 240000);
        compare(waveform.lengthUnknown, false);
    }

    // A row with the stick's waveform but no length: the bars are the whole
    // track and the cues are placed against their own span, so drawing both
    // put every marker at the wrong place in the music. Such a row draws the
    // flat line and the cues on it, and no bars. The waveform carries no
    // length of its own to scale the cues by.
    function test_aWaveformWithoutALengthIsNotDrawnUnderSpanPlacedCues() {
        const cues = [
            {kind: "hot", hotCueNumber: 1, positionMs: 60000, isLoop: false, loopEndMs: 0, color: "#e03c3c", comment: ""},
        ];
        // Full-height, pure blue bass columns: any bar covers the top rows.
        const columns = [];
        for (let i = 0; i < 100; i++) {
            columns.push({low: 1, mid: 0, high: 0});
        }
        const row = createTemporaryObject(rowComponent, testCase, {
            index: 0, showWaveform: true, expanded: true, waveformDurationMs: 0,
            waveformCues: cues, waveformData: columns,
        });
        tryVerify(() => findChild(row, "rowWaveform") !== null, 2000);
        const waveform = findChild(row, "rowWaveform");
        compare(waveform.hasWaveform, true);
        compare(waveform.lengthUnknown, true);
        const span = 66000;
        compare(waveform.cueSpanMs, span);
        const cueDrawn = function(image) {
            const at = waveform.mapToItem(row, 60000 / span * waveform.width + 7, 2);
            const p = PixelScale.pixel(image, row, at.x, at.y);
            return Math.abs(p.r * 255 - 0xe0) < 40 && Math.abs(p.g * 255 - 0x3c) < 40
                && Math.abs(p.b * 255 - 0x3c) < 40;
        };
        const barAt = function(image) {
            // Far from the cue, a quarter of the way down: only a bar
            // paints there, the flat line sits in the middle.
            const at = waveform.mapToItem(row, waveform.width * 0.2, waveform.height * 0.25);
            const p = PixelScale.pixel(image, row, at.x, at.y);
            // Blue bass bars, drawn part-transparent over the row: blue
            // well above red, which the grey ground never is.
            return (p.b - p.r) * 255 > 60;
        };
        let image = null;
        tryVerify(() => { image = grabImage(row); return cueDrawn(image); }, 3000, "the cue is drawn");
        verify(!barAt(image), "no bar is drawn under cues placed against their own span");
        compare(waveform.drawsBars, false);

        // With a length the bars and the cues agree, and both are drawn.
        waveform.trackDurationMs = 240000;
        compare(waveform.drawsBars, true);
        tryVerify(() => barAt(grabImage(row)), 3000, "with a length the bars are drawn");
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

    // The one reading of a waveform both metadata pages do, from a
    // controller's waveformSourceAt() answer. Nothing without a player or a
    // source; otherwise exactly what the source names.
    function test_stickWaveformReadsWhatTheSourceNames() {
        const row = createTemporaryObject(rowComponent, testCase, {index: 0});
        const asked = [];
        const player = {
            waveformFor: function (format, path, id) {
                asked.push(format + " " + path + " " + id);
                return [{low: 0.5, mid: 0.4, high: 0.3}];
            }
        };
        const source = {format: "engine", libraryPath: "/media/STICK/Engine Library", sourceId: "12"};
        compare(row.stickWaveform(null, source).length, 0, "no player, nothing to read with");
        compare(row.stickWaveform(player, {}).length, 0, "no source, nothing to read");
        compare(row.stickWaveform(player, null).length, 0);
        compare(asked.length, 0, "and neither asks");
        compare(row.stickWaveform(player, source).length, 1);
        compare(asked, ["engine /media/STICK/Engine Library 12"]);
    }
}
