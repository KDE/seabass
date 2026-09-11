// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// UnreferencedFilesNotice.qml headless. The thing worth testing here is
// not the layout but the claims: this component is the only place that
// tells a DJ how many of their files nothing references, on a page whose
// next step deletes them. Every case below is one where saying it wrong
// would be worse than saying nothing. Also saves a screenshot when
// SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "UnreferencedFilesNotice"
    width: 900
    height: 500
    visible: true
    when: windowShown

    Component {
        id: noticeComponent
        UnreferencedFilesNotice { width: 860 }
    }

    // RV2's real numbers, so the screenshot shows a real sentence.
    function found(overrides) {
        var info = {
            filesFound: 632, bytesHuman: "8.1 GB", unreadable: 0,
            catalogsConsulted: ["rekordbox", "engine", "onelibrary"],
            walkIncomplete: false, probeAvailable: true, usable: true, refusal: "",
        };
        for (var key in overrides) {
            info[key] = overrides[key];
        }
        return info;
    }

    function make(info) {
        return createTemporaryObject(noticeComponent, testCase, {info: info});
    }

    // A refused scan shows its reason and no number at all. A count here
    // would be a claim about the stick that the scan explicitly declined
    // to make.
    function test_refusal_replaces_every_number() {
        var notice = make({usable: false, refusal: "This stick has an engine database that could not be read.",
                           filesFound: 0, bytesHuman: "0.0 B", catalogsConsulted: []});
        verify(notice.refused);
        verify(notice.visible);
        compare(notice.text, "This stick has an engine database that could not be read.");
        verify(notice.color !== Qt.color(Theme.textMuted));  // warned, not muted
    }

    // Nothing found is nothing said: a "0 files" line would appear on
    // every clean stick forever.
    function test_silent_when_nothing_found() {
        var notice = make(found({filesFound: 0}));
        verify(notice.silent);
        compare(notice.text, "");
        verify(!notice.visible);
    }

    // The count never appears without the catalogs it was subtracted
    // from -- the one invariant this component exists for.
    function test_count_always_names_its_basis() {
        var notice = make(found({}));
        verify(notice.text.indexOf("632") >= 0);
        verify(notice.text.indexOf("8.1 GB") >= 0);
        // Named the way a DJ sees them everywhere else, not by the
        // internal format strings.
        verify(notice.text.indexOf("DeviceLibrary, Engine, OneLibrary") >= 0);
        verify(notice.text.indexOf("onelibrary") < 0);
        // And says the deletion still happens elsewhere.
        verify(notice.text.indexOf("Delete Orphaned Files") >= 0);
    }

    // Files found but none identifiable is not the same as none found,
    // and an empty list below would otherwise imply the latter.
    function test_no_tag_reader_is_stated_not_hidden() {
        var notice = make(found({probeAvailable: false}));
        verify(notice.text.indexOf("cannot read tags") >= 0);
        verify(notice.text.indexOf("632") >= 0);
    }

    // With a reader present, the unreadable few are counted out loud.
    function test_unreadable_files_are_counted_out_loud() {
        var notice = make(found({unreadable: 3}));
        verify(notice.text.indexOf("3 of them could not be read") >= 0);
        verify(notice.text.indexOf("cannot read tags") < 0);
    }

    // An incomplete walk can only ever propose fewer deletions, so it is
    // not a safety problem -- but the total must not pass as the whole
    // truth of what is on the stick.
    function test_incomplete_walk_qualifies_the_total() {
        var notice = make(found({walkIncomplete: true}));
        verify(notice.text.indexOf("there may be more than this") >= 0);
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var notice = make(found({unreadable: 3, walkIncomplete: true}));
        wait(50);
        var image = grabImage(notice);
        image.save(screenshotDir + "/UnreferencedFilesNotice.png");
    }
}
