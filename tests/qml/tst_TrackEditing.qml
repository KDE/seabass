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
    // Room for the whole page (the page tests put Browse here).
    width: 1100
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
                                  beatTicksFor: function() { return []; },
                                  cuesOnGridTimeline: (format, path, cues) => cues})
            format: "rekordbox"
        }
    }
    Component {
        id: plainPanelComponent
        // As panelComponent, as the page makes it: nothing set.
        TrackDetailPanel {
            width: 500
            height: 880
            addCueController: ({statusMessage: "", errorMessage: "", busy: false, pendingRevision: 0})
            playbackController: ({waveformFor: function() { return []; },
                                  beatTicksFor: function() { return []; },
                                  cuesOnGridTimeline: (format, path, cues) => cues})
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
    // Bound as Main.qml binds the app's one player: nothing is read
    // while a library is being written (see tst_CleanupPage).
    PlaybackController {
        id: realPlayback
        libraryBusy: EditSessionRegistry.anyWriting
    }
    AppSettingsController { id: realAppSettings }
    Component {
        id: pageComponent
        ScanPage {}
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
    property string lastPlaylistBefore: ""
    function cleanup() {
        EditSessionRegistry.discardAll();
        realPlayback.stop();
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

    // Stages another rating for `track` with a click on a star of the
    // page's panel. The star is clicked where it is laid out, and the click
    // is seen to have staged the rating: a save with nothing staged never
    // reports, and a wait for it says nothing about a click that missed
    // (the nightly lane, 2026-11-22, two minutes into "the save finished").
    function clickStarOnThePage(panel, track, session) {
        waitForPolish(testCase.Window.window);
        const star = findChild(panel, "ratingStar" + (track.rating === 2 ? 3 : 2));
        tryVerify(() => star !== null && star.visible && star.enabled, 30000, "the stars are there and editable");
        const staged = session.pendingCount;
        mouseClick(star);
        tryVerify(() => session.pendingCount === staged + 1, 5000, "the click staged the rating");
    }

    // A track with a comment whose export.pdb row takes another one (#108:
    // the row is rebuilt and moved, so its room is far beyond any comment
    // typed here), shown.
    function commented(env) {
        // One the catalogs agree on: in the fixture most do not, export.pdb
        // having no comment where OneLibrary and Engine have one.
        let found = undefined;
        for (const t of tracks(env)) {
            if (t.filePath && t.comment.length > 0) {
                show(env, t);
                if (env.panel.commentLimit.commentsDiffer === false) {
                    found = t;
                    break;
                }
            }
        }
        verify(found !== undefined, "the fixture has a track with a comment all its catalogs hold");
        verify(env.panel.commentLimit.limited && env.panel.commentLimit.units > 1000,
               "export.pdb's row takes a long comment: " + JSON.stringify(env.panel.commentLimit));
        return found;
    }

    // Holds the shown track's editor to `units` (ASCII only, or UTF-16),
    // as a limit from export.pdb would: the editor's rules for a limit,
    // tested where typing to the real one (about 1,800) would take minutes.
    function holdTo(env, units, asciiOnly) {
        const editor = findChild(env.panel, "trackCommentEditor");
        // A binding, as the panel's own is: the editor still follows each
        // new limit the controller reads, with only the size held.
        editor.limit = Qt.binding(() => Object.assign({}, env.panel.commentLimit,
                                                      {limited: true, units: units, asciiOnly: asciiOnly, reason: "held for the test"}));
        return units;
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

    // Keyboard edits: what a user does, so the field's own edit handling
    // runs (a programmatic remove() would bypass it).
    function deleteFromStart(field, count) {
        field.cursorPosition = 0;
        for (let i = 0; i < count; ++i) {
            keyClick(Qt.Key_Delete);
        }
    }
    function clearField(field) {
        field.selectAll();
        keyClick(Qt.Key_Delete);
    }

    // The comment's edit button shows while the pointer is over the editor
    // (a HoverHandler). A move to where the pointer already is, or a pane
    // laid out again under it, brings no hover: the pointer is taken off
    // and put back until the button is there.
    function hoverForEditButton(editor) {
        const button = findChild(editor, "commentEditButton");
        verify(button !== null);
        tryVerify(() => {
            mouseMove(testCase, 1, 1);
            mouseMove(editor, 10, editor.height / 2);
            wait(150);
            return button.opacity === 1;
        }, 10000, "hovering shows the edit button");
        return button;
    }

    function beginEditing(env) {
        const editor = findChild(env.panel, "trackCommentEditor");
        const button = hoverForEditButton(editor);
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

    // Comment editing is on in the app (#108); switched off, the comment
    // is shown and not edited, with no edit button, while its stars still
    // stage.
    function test_0_theCommentIsReadOnlyWhenSwitchedOff() {
        const plain = createTemporaryObject(plainPanelComponent, testCase);
        verify(plain !== null);
        compare(plain.commentEditingEnabled, true, "on as the page makes it");
        const env = setUp();
        env.panel.commentEditingEnabled = false;
        const track = tracks(env).find(t => t.filePath && t.comment.length > 0);
        verify(track !== undefined, "a track with a comment");
        show(env, track);
        const editor = findChild(env.panel, "trackCommentEditor");
        verify(editor.visible, "the comment is shown");
        compare(editor.editable, false, "but not editable");
        compare(editor.canEdit, false);
        mouseMove(editor, 10, editor.height / 2);
        wait(300);
        const button = findChild(editor, "commentEditButton");
        verify(button !== null && !button.visible, "and offers no edit button, even on hover");
        const star = findChild(env.panel, "ratingStar" + (track.rating === 2 ? 3 : 2));
        verify(star.enabled, "the rating can still be set");
        mouseClick(star);
        verify(env.controller.hasStagedRating(track.filePath));
    }

    function test_2_commentEditsInPlace() {
        const env = setUp();
        const track = commented(env);
        const editor = findChild(env.panel, "trackCommentEditor");
        const button = findChild(editor, "commentEditButton");
        mouseMove(env.panel, 2, 2);
        tryCompare(button, "opacity", 0, 2000, "the edit button is hidden until hovered");
        hoverForEditButton(editor);
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
        const stars = track.rating === 2 ? 3 : 2;
        mouseClick(findChild(env.panel, "ratingStar" + stars));
        show(env, other);
        compare(findChild(editor, "commentText").text, other.comment.length > 0 ? other.comment
            : env.panel.commentLimit.limited && env.panel.commentLimit.units === 0 ? "No comment" : "Add a comment");
        verify(!findChild(env.panel, "commentPendingMark").visible);
        show(env, track);
        compare(findChild(editor, "commentText").text, "new");
        compare(env.panel.shownRating, stars);
        shot(env.panel, "rating-comment-staged");
        // The original text again takes the change back.
        field = beginEditing(env);
        field.selectAll();
        paste(field, track.comment);
        keyClick(Qt.Key_Return);
        verify(!env.controller.hasStagedComment(track.filePath), "the original value unstages");
    }

    // The field holds no more than export.pdb's row can: typing stops,
    // and a paste is cut, at the limit; a character an ASCII field cannot
    // hold is not taken; near the limit a counter says how near.
    function test_3_asciiFieldIsHeldToTheLimit() {
        const env = setUp();
        const track = commented(env);
        const units = holdTo(env, 12, true);
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
        deleteFromStart(field, 3);
        field.cursorPosition = 0;
        paste(field, "VWXYZ");
        compare(field.text, "VWX" + "a".repeat(units - 3), "a paste is cut where it stops fitting");
        // Non-ASCII: not taken by an ASCII field, typed or pasted; a paste
        // keeps what comes before it.
        clearField(field);
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
        const track = commented(env);
        const units = holdTo(env, 8, false);
        const field = beginEditing(env);
        field.selectAll();
        keyClick(Qt.Key_Delete);
        paste(field, "é".repeat(units + 3));
        compare(field.text, "é".repeat(units), "accented letters fit a UTF-16 field, one unit each");
        // An emoji is two units: with one unit left it does not fit, and
        // is not cut in half.
        deleteFromStart(field, 1);
        field.cursorPosition = field.text.length;
        paste(field, "🎶");
        compare(field.text.length, units - 1, "half an emoji is never kept");
        deleteFromStart(field, 1);
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
        const track = commented(env);
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

    // An edit open on track A when B is shown (the pane follows a row
    // clicked, a track played, or the player moving on: each calls
    // showFor()) is staged for A, with A's text, and B is left alone.
    function test_6_switchingTracksMidEditStagesForTheTrackEdited() {
        const env = setUp();
        const a = commented(env);
        const b = tracks(env).find(t => t.filePath && t.filePath !== a.filePath);
        const field = beginEditing(env);
        clearField(field);
        keyClick(Qt.Key_Z);
        keyClick(Qt.Key_Z);
        env.panel.showFor(b);
        compare(env.controller.stagedComment(a.filePath), "zz", "the text typed is A's");
        verify(env.controller.hasStagedComment(a.filePath));
        verify(!env.controller.hasStagedComment(b.filePath), "B is untouched");
        verify(!findChild(env.panel, "trackCommentEditor").editing, "the edit ended with A");
        tryVerify(() => env.panel.commentLimit.known === true, 30000);
    }

    // While the stick is being read nothing can be rated or typed, and the
    // pane says so quietly; when the read answers, both can. The read is
    // held back by the controller's test seam, so the state is one this
    // test makes rather than one it hopes to catch.
    function test_6b_nothingIsEditableWhileTheStickIsRead() {
        const env = setUp();
        const a = commented(env);
        env.controller.holdIndexForTesting = true;
        env.controller.invalidate();
        compare(env.panel.commentLimit.known, false);
        compare(env.panel.commentLimit.reading, true);
        const editor = findChild(env.panel, "trackCommentEditor");
        verify(!editor.canEdit, "no comment edit while the stick is read");
        verify(!findChild(env.panel, "ratingStar3").enabled, "nor a rating");
        compare(findChild(env.panel, "trackRatingEditor").opacity, 0.5, "the stars are dimmed");
        mouseClick(findChild(env.panel, "ratingStar3"));
        verify(!env.controller.hasStagedRating(a.filePath), "a click stages nothing");
        env.controller.holdIndexForTesting = false;
        tryVerify(() => env.panel.commentLimit.known === true, 30000, "the read answers");
        verify(editor.canEdit);
        verify(findChild(env.panel, "ratingStar3").enabled);
    }

    // A comment typed for A that cannot be staged when the pane turns to
    // B (here: the stick is being read again, after another save) is not
    // lost without a word: the pane moves on and one line names A. The
    // line goes with the next track shown.
    function test_6c_aCommentThatCannotBeStagedOnASwitchIsNamed() {
        const env = setUp();
        const a = commented(env);
        const tracksNow = tracks(env);
        const b = tracksNow.find(t => t.filePath && t.filePath !== a.filePath);
        const c = tracksNow.find(t => t.filePath && t.filePath !== a.filePath && t.filePath !== b.filePath);
        const field = beginEditing(env);
        clearField(field);
        keyClick(Qt.Key_Q);
        env.controller.holdIndexForTesting = true;
        env.controller.invalidate();
        env.panel.showFor(b);
        verify(!env.controller.hasStagedComment(a.filePath));
        compare(env.controller.errorMessage, "The comment typed for \"" + a.title + "\" was not staged. "
                + "Seabass is still reading the stick.",
                "one plain line naming the track, no advice to try again for text that is gone");
        verify(findChild(env.panel, "trackEditError").visible);
        env.controller.holdIndexForTesting = false;
        env.panel.showFor(c);
        compare(env.controller.errorMessage, "", "and goes with the next track shown");
    }

    // Rate, Save, rate again, without showing the track again: the stick is
    // read again after the save, and the track can be edited once it is.
    function test_6d_theTrackShownCanBeEditedAgainAfterASave() {
        const env = setUp();
        const a = commented(env);
        const first = a.rating === 3 ? 2 : 3;
        mouseClick(findChild(env.panel, "ratingStar" + first));
        verify(env.controller.hasStagedRating(a.filePath));
        let summary = null;
        const done = (result) => { summary = result; };
        env.session.saveFinished.connect(done);
        env.session.save();
        tryVerify(() => summary !== null, 120000);
        env.session.saveFinished.disconnect(done);
        compare(summary.error, "");
        compare(env.panel.shownRating, first);
        tryVerify(() => env.panel.commentLimit.known === true, 30000, "the stick is read again");
        mouseClick(findChild(env.panel, "ratingStar4"));
        compare(env.controller.errorMessage, "");
        verify(env.controller.hasStagedRating(a.filePath), "the second rating stages");
        compare(env.controller.stagedRating(a.filePath), 4);
    }


    // The same on the page: a row clicked (TrackDragArea, which takes no
    // focus) and a track played while A's comment is open.
    function test_7_onThePageARowClickedOrPlayedEndsTheEditForA() {
        const env = setUp();
        const a = commented(env);
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1000, height: 880, stickLabel: "TESTSTICK", rekordboxPath: env.stick + "/PIONEER",
            enginePath: env.stick + "/Engine Library", playbackController: realPlayback,
            appSettingsController: realAppSettings});
        verify(page !== null);
        const list = findChild(page, "trackListView");
        tryVerify(() => list.count > 1, 120000, "the page lists the stick");
        const panel = findChild(page, "trackDetailPanel");
        panel.commentEditingEnabled = true;
        for (const how of ["row", "play"]) {
            page.playRow(a);
            realPlayback.stop();
            tryVerify(() => panel.commentLimit.known === true, 30000);
            const editor = findChild(panel, "trackCommentEditor");
            const button = hoverForEditButton(editor);
            mouseClick(button);
            const field = findChild(editor, "commentField");
            tryCompare(field, "visible", true);
            clearField(field);
            keyClick(how === "row" ? Qt.Key_R : Qt.Key_P);
            let other = null;
            for (let i = 0; i < list.count && other === null; ++i) {
                const item = list.itemAtIndex(i);
                if (item !== null && item.filePath !== a.filePath) {
                    other = item;
                }
            }
            verify(other !== null);
            if (how === "row") {
                mouseClick(findChild(other, "rowMouseArea"));
            } else {
                page.playRow(other);
                realPlayback.stop();
            }
            compare(panel.trackFilePath, other.filePath, "the pane shows the other track");
            const editsController = page.trackEditController;
            compare(editsController.stagedComment(a.filePath), how === "row" ? "r" : "p");
            verify(!editsController.hasStagedComment(other.filePath), "the other track is untouched");
        }
    }

    // A save started while the comment is open writes the comment too:
    // by the Save shortcut on the page, and by the session's save().
    function test_8_aSaveStagesTheOpenEditFirst() {
        const env = setUp();
        const a = commented(env);
        let field = beginEditing(env);
        clearField(field);
        keyClick(Qt.Key_S);
        let summary = null;
        const done = (result) => { summary = result; };
        env.session.saveFinished.connect(done);
        env.session.save();
        tryVerify(() => summary !== null, 120000);
        env.session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, "the open edit was staged and written");
        compare(env.panel.shownComment, "s");
        verify(!findChild(env.panel, "trackCommentEditor").editing);

        // The shortcut, on the page.
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1000, height: 880, stickLabel: "TESTSTICK", rekordboxPath: env.stick + "/PIONEER",
            enginePath: env.stick + "/Engine Library", playbackController: realPlayback,
            appSettingsController: realAppSettings});
        const list = findChild(page, "trackListView");
        tryVerify(() => list.count > 1, 120000);
        const panel = findChild(page, "trackDetailPanel");
        panel.commentEditingEnabled = true;
        page.playRow(a);
        realPlayback.stop();
        tryVerify(() => panel.commentLimit.known === true, 30000);
        // Something staged first, so the Save button and its shortcut are
        // there.
        clickStarOnThePage(panel, a, EditSessionRegistry.sessionFor(
            EditSessionRegistry.libraryIdForPath(env.stick + "/PIONEER")));
        const editor = findChild(panel, "trackCommentEditor");
        mouseClick(hoverForEditButton(editor));
        field = findChild(editor, "commentField");
        tryCompare(field, "visible", true);
        verify(field.activeFocus);
        clearField(field);
        keyClick(Qt.Key_K);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(env.stick + "/PIONEER"));
        summary = null;
        session.saveFinished.connect(done);
        keySequence(StandardKey.Save);
        tryVerify(() => summary !== null, 120000, "the shortcut saved");
        session.saveFinished.disconnect(done);
        compare(summary.written, 2, "rating and the open comment: " + JSON.stringify(summary));
        compare(panel.shownComment, "k");
    }

    // A comment that does not fit export.pdb as shown (the Engine or
    // OneLibrary one, longer) can be shortened: deleting always works,
    // only growth past the limit is refused.
    function test_9_aCommentTooLongCanBeShortened() {
        const env = setUp();
        const a = commented(env);
        const units = env.panel.commentLimit.units;
        const longer = Object.assign({}, a);
        longer.comment = "w".repeat(units + 6);
        env.panel.showFor(longer);
        tryVerify(() => env.panel.commentLimit.known === true, 30000);
        const field = beginEditing(env);
        field.cursorPosition = field.text.length;
        keyClick(Qt.Key_Backspace);
        compare(field.text.length, units + 5, "a deletion is taken though the text is still too long");
        keyClick(Qt.Key_X);
        compare(field.text.length, units + 5, "growth is not");
        for (let i = 0; i < 5; ++i) {
            keyClick(Qt.Key_Backspace);
        }
        compare(field.text.length, units);
        keyClick(Qt.Key_Return);
        compare(env.controller.stagedComment(a.filePath), "w".repeat(units));
    }

    // Opening the field and leaving it stages nothing, whatever the
    // comment holds: a line break the one-line field cannot show, or
    // trailing spaces.
    function test_10_anUntouchedFieldStagesNothing() {
        const env = setUp();
        const a = commented(env);
        for (const comment of ["line one\nline two", "trailing  "]) {
            const shown = Object.assign({}, a);
            shown.comment = comment;
            env.panel.showFor(shown);
            tryVerify(() => env.panel.commentLimit.known === true, 30000);
            const field = beginEditing(env);
            keyClick(Qt.Key_Return);
            tryCompare(field, "visible", false);
            verify(!env.controller.hasStagedComment(a.filePath), "nothing staged for " + JSON.stringify(comment));
            compare(env.session.pendingCount, 0);
            // And by focus leaving it.
            beginEditing(env);
            findChild(env.panel, "ratingStar1").forceActiveFocus();
            verify(!env.controller.hasStagedComment(a.filePath));
        }
    }

    // A catalog that cannot be read: one plain sentence naming it, no
    // path or error text; another track shown does not read the stick
    // again, "Read Again" does.
    function test_11_aFailedReadSaysWhichCatalogInASentence() {
        const env = setUp();
        const a = tracks(env).find(t => t.filePath);
        const b = tracks(env).find(t => t.filePath && t.filePath !== a.filePath);
        verify(stickFixture.writeText(env.stick + "/Engine Library/Database2/m.db", "not a database"),
               "Engine's database replaced by something unreadable");
        env.controller.invalidate();
        env.panel.showFor(a);
        tryVerify(() => env.panel.commentLimit.failed === true, 30000, "the read fails");
        const line = findChild(env.panel, "trackEditRefusal");
        verify(line.visible);
        compare(line.text, "Seabass could not read the Engine library on this stick, so nothing can be changed here.");
        const builds = env.controller.indexBuildsForTesting;
        env.panel.showFor(b);
        wait(200);
        compare(env.controller.indexBuildsForTesting, builds, "another track shown does not read the stick again");
        const again = findChild(env.panel, "readStickAgainButton");
        verify(again.visible);
        mouseClick(again);
        compare(env.controller.indexBuildsForTesting, builds + 1, "Read Again does");
        tryVerify(() => env.panel.commentLimit.failed === true, 30000);
    }

    // With comment editing switched off, a track with no comment
    // has no comment block, and the rating's own notes still show: the
    // failed read in a sentence and Read Again, outside that block.
    function test_11b_theNotesShowWithoutACommentBlock() {
        const env = setUp();
        env.panel.commentEditingEnabled = false;
        const a = tracks(env).find(t => t.filePath && t.comment.length === 0);
        verify(a !== undefined, "a track with no comment");
        verify(stickFixture.writeText(env.stick + "/Engine Library/Database2/m.db", "not a database"),
               "Engine's database replaced by something unreadable");
        env.controller.invalidate();
        env.panel.showFor(a);
        tryVerify(() => env.panel.commentLimit.failed === true, 30000, "the read fails");
        verify(!findChild(env.panel, "trackCommentEditor").visible, "no comment block for a track without one");
        tryVerify(() => findChild(env.panel, "trackEditRefusal").visible, 2000, "the failed read is said");
        verify(findChild(env.panel, "readStickAgainButton").visible, "and Read Again offered");
        verify(findChild(env.panel, "ratingStar2").visible, "beside the stars");
    }

    // One read of the stick per save, though the page reads its list again
    // after it; none for switching the catalog shown.
    function test_12_oneReadPerSaveAndNoneForAFormatToggle() {
        const env = setUp();
        const a = commented(env);
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1000, height: 880, stickLabel: "TESTSTICK", rekordboxPath: env.stick + "/PIONEER",
            enginePath: env.stick + "/Engine Library", playbackController: realPlayback,
            appSettingsController: realAppSettings});
        const list = findChild(page, "trackListView");
        tryVerify(() => list.count > 1, 120000);
        const panel = findChild(page, "trackDetailPanel");
        panel.commentEditingEnabled = true;
        page.playRow(a);
        realPlayback.stop();
        tryVerify(() => panel.commentLimit.known === true, 30000);
        const controller = page.trackEditController;
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(env.stick + "/PIONEER"));
        clickStarOnThePage(panel, a, session);
        const before = controller.indexBuildsForTesting;
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 120000);
        session.saveFinished.disconnect(done);
        compare(summary.error, "");
        tryVerify(() => panel.commentLimit.known === true, 30000, "read again after the save");
        tryVerify(() => list.count > 1, 120000, "and the list read again");
        wait(500);
        compare(controller.indexBuildsForTesting, before + 1, "one read for the save");
        page.format = "onelibrary";
        tryVerify(() => list.count > 1, 120000);
        page.format = "rekordbox";
        tryVerify(() => list.count > 1, 120000);
        wait(500);
        compare(controller.indexBuildsForTesting, before + 1, "none for the catalog shown changing");
    }

    // A track whose export.pdb row has no room for a comment (95 of the
    // fixture's 100) takes one, long and accented (#108): staged, saved,
    // and read back from the stick.
    function test_13_aCommentIsAddedWhereExportPdbHadNoRoom() {
        const env = setUp();
        const track = tracks(env).find(t => t.filePath && t.comment.length === 0);
        verify(track !== undefined, "a track with no comment");
        show(env, track);
        const limit = env.panel.commentLimit;
        verify(limit.limited && limit.units > 1000 && !limit.asciiOnly, JSON.stringify(limit));
        const text = "Gr\u00fc\u00dfe aus K\u00f6ln " + "x".repeat(280) + " \ud83c\udfb6";
        const field = beginEditing(env);
        clearField(field);
        paste(field, text);
        compare(field.text, text, "taken whole");
        keyClick(Qt.Key_Return);
        compare(env.controller.stagedComment(track.filePath), text);
        let summary = null;
        const done = (result) => { summary = result; };
        env.session.saveFinished.connect(done);
        env.session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        env.session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        env.scan.scan("rekordbox", env.stick + "/PIONEER");
        tryVerify(() => !env.scan.busy && env.scan.tracks.trackCount() > 0, 120000, "the rescan finishes");
        const again = tracks(env).find(t => t.filePath === track.filePath);
        verify(again !== undefined);
        compare(again.comment, text, "the stick has the comment");
    }
}
