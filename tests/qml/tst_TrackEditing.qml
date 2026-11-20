// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The track details' rating stars and comment (#105), with the real
// TrackEditController on a copy of tests/fixtures/one_stick_two_catalogs:
// the stars stage, the current star clears, the comment edits in place
// (hover shows the edit button, Enter stages, Escape does not), the field
// never takes more than export.pdb's row can hold, by typing or pasting,
// ASCII or not; staged values survive showing another track; Save writes
// them, and the stick read again has them.
//
// With SEABASS_SCREENSHOT_DIR set it also saves the pane idle, hovered,
// editing and staged.
TestCase {
    id: testCase
    name: "TrackEditing"
    width: 520
    height: 900
    visible: true
    when: windowShown

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/one_stick_two_catalogs").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: panelComponent
        TrackDetailPanel {
            width: 500
            height: 880
            addCueController: ({statusMessage: "", errorMessage: "", busy: false, pendingRevision: 0})
            playbackController: ({waveformFor: function() { return []; },
                                  beatTicksFor: function() { return []; }})
            format: "rekordbox"
        }
    }
    Component {
        id: controllerComponent
        TrackEditController {}
    }
    Component {
        id: scanComponent
        ScanController {}
    }
    // Puts text on the clipboard, for a paste.
    TextField {
        id: clipboardSource
        visible: false
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
    }
    function cleanup() {
        EditSessionRegistry.discardAll();
    }

    // A fresh stick, its rekordbox tracks read, and a pane over it.
    function setUp() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        const scan = createTemporaryObject(scanComponent, testCase);
        scan.scan("rekordbox", stick + "/PIONEER");
        tryVerify(() => !scan.busy && scan.tracks.trackCount() > 0, 120000, "the scan finishes");
        const controller = createTemporaryObject(controllerComponent, testCase);
        const panel = createTemporaryObject(panelComponent, testCase, {
            trackEditController: controller, rekordboxPath: stick + "/PIONEER",
            enginePath: stick + "/Engine Library", libraryPath: stick + "/PIONEER"});
        verify(panel !== null);
        return {stick: stick, scan: scan, controller: controller, panel: panel,
                session: EditSessionRegistry.sessionFor(libraryId)};
    }

    function tracks(env) {
        const out = [];
        for (let i = 0; i < env.scan.tracks.trackCount(); ++i) {
            out.push(env.scan.tracks.trackAt(i));
        }
        return out;
    }

    // Shows `track` and waits for its comment limit.
    function show(env, track) {
        env.panel.showFor(track);
        tryVerify(() => env.panel.commentLimit.known === true, 30000, "the comment limit is read");
        waitForPolish(testCase.Window.window);
    }

    // The track whose export.pdb comment has the most room, in an ASCII
    // field (asciiOnly true) or a UTF-16 one.
    function roomiest(env, asciiOnly) {
        let best = null;
        let bestUnits = -1;
        for (const t of tracks(env)) {
            if (!t.filePath || t.comment.length === 0) {
                continue;
            }
            show(env, t);
            const limit = env.panel.commentLimit;
            if (limit.limited && limit.asciiOnly === asciiOnly && limit.units > bestUnits) {
                best = t;
                bestUnits = limit.units;
            }
        }
        verify(best !== null, "the fixture has a track with a comment in a" + (asciiOnly ? "n ASCII" : " UTF-16") + " field");
        show(env, best);
        return best;
    }

    function shot(item, name) {
        if (typeof screenshotDir !== "undefined" && screenshotDir && screenshotDir.length > 0) {
            waitForRendering(item);
            grabImage(item).save(screenshotDir + "/" + name + ".png");
        }
    }

    function paste(field, text) {
        clipboardSource.text = text;
        clipboardSource.selectAll();
        clipboardSource.copy();
        field.forceActiveFocus();
        field.paste();
    }

    function beginEditing(env) {
        const editor = findChild(env.panel, "trackCommentEditor");
        const button = findChild(editor, "commentEditButton");
        mouseMove(editor, 10, editor.height / 2);
        tryCompare(button, "opacity", 1, 2000, "hovering shows the edit button");
        mouseClick(button);
        const field = findChild(editor, "commentField");
        tryCompare(field, "visible", true);
        verify(field.activeFocus, "the field takes the keyboard");
        return field;
    }

    function test_1_starsStageAndTheCurrentStarClears() {
        const env = setUp();
        const track = tracks(env).find(t => t.filePath && t.rating <= 0);
        verify(track !== undefined, "an unrated track");
        show(env, track);
        shot(env.panel, "rating-comment-idle");
        const star3 = findChild(env.panel, "ratingStar3");
        verify(star3 !== null && star3.visible && star3.enabled, "the stars are there and editable");
        compare(star3.Accessible.name, "Rate 3 stars");
        mouseClick(star3);
        verify(env.controller.hasStagedRating(track.filePath), "a click stages the rating");
        compare(env.panel.shownRating, 3);
        verify(findChild(env.panel, "ratingPendingMark").visible, "the staged rating is marked");
        compare(env.session.pendingCount, 1);
        // The current star again: no stars, which is the stick's value,
        // so nothing is left staged.
        mouseClick(star3);
        verify(!env.controller.hasStagedRating(track.filePath), "staging the original value takes the change back");
        compare(env.session.pendingCount, 0);
        // By keyboard: Tab to a star, Space.
        const star5 = findChild(env.panel, "ratingStar5");
        star5.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        compare(env.controller.stagedRating(track.filePath), 5, "a star is pressed from the keyboard");
        // The undo button takes it back, once the layout has made room
        // for it.
        waitForPolish(testCase.Window.window);
        const undo = findChild(env.panel, "ratingPendingMarkUndo");
        verify(undo.visible && undo.width > 0);
        mouseClick(undo);
        verify(!env.controller.hasStagedRating(track.filePath));
        compare(env.session.pendingCount, 0);
    }

    function test_2_commentEditsInPlace() {
        const env = setUp();
        const track = roomiest(env, true);
        const editor = findChild(env.panel, "trackCommentEditor");
        const button = findChild(editor, "commentEditButton");
        compare(button.opacity, 0, "the edit button is hidden until hovered");
        mouseMove(editor, 10, editor.height / 2);
        tryCompare(button, "opacity", 1, 2000);
        shot(env.panel, "rating-comment-hovered");
        // Escape: nothing staged, the text as it was.
        let field = beginEditing(env);
        field.selectAll();
        keyClick(Qt.Key_X);
        keyClick(Qt.Key_Escape);
        tryCompare(field, "visible", false);
        verify(!env.controller.hasStagedComment(track.filePath), "Escape stages nothing");
        compare(findChild(editor, "commentText").text, track.comment);
        // A click on the text edits too; Enter stages.
        mouseClick(findChild(editor, "commentText"));
        field = findChild(editor, "commentField");
        tryCompare(field, "visible", true);
        field.selectAll();
        keyClick(Qt.Key_N);
        keyClick(Qt.Key_E);
        keyClick(Qt.Key_W);
        shot(env.panel, "rating-comment-editing");
        keyClick(Qt.Key_Return);
        tryCompare(field, "visible", false);
        verify(env.controller.hasStagedComment(track.filePath), "Enter stages");
        compare(env.controller.stagedComment(track.filePath), "new");
        compare(findChild(editor, "commentText").text, "new");
        verify(findChild(env.panel, "commentPendingMark").visible, "marked as unsaved");
        // Staged values survive another track shown, and come back.
        const other = tracks(env).find(t => t.filePath && t.filePath !== track.filePath);
        mouseClick(findChild(env.panel, "ratingStar2"));
        show(env, other);
        compare(findChild(editor, "commentText").text, other.comment.length > 0 ? other.comment
            : env.panel.commentLimit.limited && env.panel.commentLimit.units === 0 ? "No comment" : "Add a comment");
        verify(!findChild(env.panel, "commentPendingMark").visible);
        show(env, track);
        compare(findChild(editor, "commentText").text, "new");
        compare(env.panel.shownRating, 2);
        shot(env.panel, "rating-comment-staged");
        // The original text again takes the change back.
        field = beginEditing(env);
        field.selectAll();
        field.remove(0, field.text.length);
        field.insert(0, track.comment);
        keyClick(Qt.Key_Return);
        verify(!env.controller.hasStagedComment(track.filePath), "the original value unstages");
    }

    // The field holds no more than export.pdb's row can: typing stops,
    // and a paste is cut, at the limit; a character an ASCII field cannot
    // hold is not taken; near the limit a counter says how near.
    function test_3_asciiFieldIsHeldToTheLimit() {
        const env = setUp();
        const track = roomiest(env, true);
        const units = env.panel.commentLimit.units;
        verify(units >= 12, "room enough to test with");
        const field = beginEditing(env);
        field.selectAll();
        keyClick(Qt.Key_Delete);
        compare(field.text, "");
        const counter = findChild(env.panel, "commentCounter");
        verify(!counter.visible, "no counter far from the limit");
        for (let i = 0; i < units + 5; ++i) {
            keyClick(Qt.Key_A);
        }
        compare(field.text.length, units, "typing stops at the limit");
        verify(counter.visible);
        compare(counter.text, units + " of " + units);
        // A paste in the middle is cut to what fits: nothing, here.
        field.cursorPosition = 2;
        paste(field, "XYZ");
        compare(field.text, "a".repeat(units));
        // With room for three, three of five.
        field.remove(0, 3);
        field.cursorPosition = 0;
        paste(field, "VWXYZ");
        compare(field.text, "VWX" + "a".repeat(units - 3), "a paste is cut where it stops fitting");
        // Non-ASCII: not taken by an ASCII field, typed or pasted; a paste
        // keeps what comes before it.
        field.remove(0, field.text.length);
        paste(field, "Café au lait");
        compare(field.text, "Caf", "cut at the first character the field cannot hold");
        keyClick(Qt.Key_Return);
        compare(env.controller.stagedComment(track.filePath), "Caf");
        // At the limit exactly, it stages and saves.
        const at = beginEditing(env);
        at.selectAll();
        paste(at, "L".repeat(units));
        compare(at.text, "L".repeat(units));
        keyClick(Qt.Key_Return);
        compare(env.controller.stagedComment(track.filePath), "L".repeat(units));
        compare(env.controller.errorMessage, "");
    }

    function test_4_utf16FieldCountsUnits() {
        const env = setUp();
        const track = roomiest(env, false);
        const units = env.panel.commentLimit.units;
        verify(units >= 6);
        const field = beginEditing(env);
        field.selectAll();
        keyClick(Qt.Key_Delete);
        paste(field, "é".repeat(units + 3));
        compare(field.text, "é".repeat(units), "accented letters fit a UTF-16 field, one unit each");
        // An emoji is two units: with one unit left it does not fit, and
        // is not cut in half.
        field.remove(0, 1);
        field.cursorPosition = field.text.length;
        paste(field, "🎶");
        compare(field.text.length, units - 1, "half an emoji is never kept");
        field.remove(0, 1);
        field.cursorPosition = field.text.length;
        paste(field, "🎶");
        compare(field.text.length, units, "with two units free it fits");
        keyClick(Qt.Key_Return);
        compare(env.controller.stagedComment(track.filePath), "é".repeat(units - 2) + "🎶");
    }

    // Save writes both, the pane keeps showing them, and the stick read
    // again has them.
    function test_5_saveWritesAndTheRescanShowsIt() {
        const env = setUp();
        const track = roomiest(env, true);
        const stars = track.rating === 4 ? 2 : 4;
        mouseClick(findChild(env.panel, "ratingStar" + stars));
        const field = beginEditing(env);
        field.selectAll();
        paste(field, "saved");
        keyClick(Qt.Key_Return);
        compare(env.session.pendingCount, 2);
        let summary = null;
        const done = (result) => { summary = result; };
        env.session.saveFinished.connect(done);
        env.session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        env.session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 2, JSON.stringify(summary));
        verify(!env.controller.hasStagedRating(track.filePath) && !env.controller.hasStagedComment(track.filePath));
        compare(env.panel.shownRating, stars, "the pane shows the saved rating");
        compare(env.panel.shownComment, "saved");
        verify(!findChild(env.panel, "ratingPendingMark").visible);

        env.scan.scan("rekordbox", env.stick + "/PIONEER");
        tryVerify(() => !env.scan.busy && env.scan.tracks.trackCount() > 0, 120000, "the rescan finishes");
        const again = tracks(env).find(t => t.filePath === track.filePath);
        verify(again !== undefined);
        compare(again.rating, stars, "export.pdb has the rating");
        compare(again.comment, "saved", "and the comment");
        show(env, again);
        compare(env.panel.shownRating, stars);
        compare(findChild(env.panel, "commentText").text, "saved");
    }
}
