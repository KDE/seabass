// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// A list of waveform cards reads one preview per row, on the UI thread
// (PlaybackController::waveformFor). Each rekordbox preview used to
// parse the whole export.pdb to find which analysis file was the
// track's: about 20 ms a row on the committed 1,400-track library, so
// opening Library Health's Cues at 0:00 froze while its first screen
// filled, and every row scrolled into view stalled again. One parse per
// export.pdb now answers every row, and a changed export.pdb is parsed
// again.
TestCase {
    id: testCase
    name: "WaveformPreviews"
    when: windowShown

    PlaybackController { id: realPlayback }

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    function drawnCount(pioneer, ids) {
        let drawn = 0;
        for (const id of ids) {
            if (realPlayback.waveformFor("rekordbox", pioneer, id).length > 0) {
                drawn++;
            }
        }
        return drawn;
    }

    function test_aScreenOfPreviewsParsesExportPdbOnce() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const pioneer = stick + "/PIONEER";
        const ids = stickFixture.rekordboxTrackIds(pioneer, 24);
        compare(ids.length, 24, "the fixture has the tracks to ask about");

        const before = stickFixture.trackDatabaseParses();
        const firstScreen = ids.slice(0, 12);
        // Drawn, not just asked: a lookup that found nothing would parse
        // nothing either, and pass for the wrong reason.
        verify(drawnCount(pioneer, firstScreen) >= 6, "the previews are really read");
        compare(stickFixture.trackDatabaseParses() - before, 1, "one parse for a screen of twelve previews");

        // Scrolling on: more rows, the same export.pdb, no parse.
        verify(drawnCount(pioneer, ids.slice(12)) >= 6);
        compare(stickFixture.trackDatabaseParses() - before, 1, "rows scrolled into view parse nothing more");

        // A save rewrites export.pdb, and what it says is read again.
        verify(stickFixture.touch(pioneer + "/rekordbox/export.pdb"));
        const again = stickFixture.rekordboxTrackIds(pioneer, 25);
        const afterIds = stickFixture.trackDatabaseParses();
        realPlayback.waveformFor("rekordbox", pioneer, again[24]);
        compare(stickFixture.trackDatabaseParses() - afterIds, 1, "a changed export.pdb is parsed again");
    }
}
