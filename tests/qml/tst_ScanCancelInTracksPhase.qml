// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// A Browse scan cancelled in its Tracks phase, in the one window where
// that used to go wrong: the read has already relayed its list (queued
// for this thread, not yet delivered) and gone on to its cue pass, and
// the controller still says busy. The cancel must win: the page is told
// once that the scan was cancelled, and the list that was on its way is
// never published under it.
TestCase {
    id: testCase
    name: "ScanCancelInTracksPhase"
    when: windowShown

    Component {
        id: controllerComponent
        ScanController {}
    }

    Component {
        id: signalSpyComponent
        SignalSpy {}
    }

    function cleanup() {
        browseFixture.restore();
    }

    function test_aCancelAfterTheListWasRelayedPublishesNothing() {
        browseFixture.holdCues(12);
        const controller = createTemporaryObject(controllerComponent, testCase);
        const published = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "tracksPublished"});
        const cancelled = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "scanCancelled"});
        controller.scan("rekordbox", "/nonexistent/HELD/PIONEER");

        // Without running the event loop: the read relays its list and
        // reaches the held cue pass, and the relayed list stays queued.
        const started = Date.now();
        while (!browseFixture.cuePassWaiting() && Date.now() - started < 10000) {
        }
        verify(browseFixture.cuePassWaiting(), "the read is at its cue pass, its list relayed");
        compare(published.count, 0, "the relayed list is still queued");
        compare(controller.busy, true, "so the controller is still in its Tracks phase");

        controller.cancelScan();
        compare(cancelled.count, 1, "cancelled at once");
        compare(controller.busy, false);
        compare(controller.cuesPending, false);

        // Everything the read reported is delivered now; none of it counts.
        tryVerify(() => !browseFixture.cuePassWaiting(), 5000, "the held pass let go");
        browseFixture.waitForScans();
        wait(0);
        compare(published.count, 0, "the list that was on its way is not published over the cancel");
        compare(cancelled.count, 1, "and the page hears of the cancel once");
        compare(controller.tracks.trackCount(), 0);
        compare(browseFixture.cuePasses(), 1);
    }
}
