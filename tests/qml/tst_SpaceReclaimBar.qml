// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// SpaceReclaimBar.qml headless: the geometry has to be proportional to
// the real byte counts, not merely "a bar that grows", because the whole
// point of the drawing is that a reader can trust its proportions.
// Numbers are RV2's real ones. Also saves a screenshot when
// SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "SpaceReclaimBar"
    width: 900
    height: 400
    visible: true
    when: windowShown

    // RV2: 30 GB stick, 1.5 GB free, 7.76 GB the rules would reclaim.
    readonly property real gb: 1024 * 1024 * 1024

    Component {
        id: barComponent
        SpaceReclaimBar { width: 860 }
    }

    function test_hidden_when_capacity_unknown() {
        var bar = createTemporaryObject(barComponent, testCase, {
            totalBytes: 0, freeBytes: 0, reclaimBytes: 0, reclaimableBytes: 0
        });
        verify(bar !== null);
        verify(!bar.known);
        // An unreadable capacity must draw nothing at all: an empty bar
        // would claim the stick is empty, which is a different statement.
        verify(!bar.visible);
        compare(bar.implicitHeight, 0);
    }

    function test_used_bytes_derive_from_capacity() {
        var bar = createTemporaryObject(barComponent, testCase, {
            totalBytes: 30 * gb, freeBytes: 1.5 * gb,
            reclaimBytes: 0, reclaimableBytes: 7.76 * gb
        });
        verify(bar.known);
        verify(bar.visible);
        compare(bar.usedBytes, 28.5 * gb);
    }

    // The invariant the drawing lives or dies by. This caught a real bug:
    // "reclaimable" was first drawn as a segment BESIDE "in use" rather
    // than carved out of it, so a 30 GB stick rendered as 32.6 GB of
    // bar. A bar whose parts do not sum to its capacity is worse than no
    // bar, because it invites exactly the proportional reading it then
    // gets wrong.
    function test_segments_sum_to_capacity_data() {
        return [
            {tag: "rv2, nothing ticked", total: 30, free: 1.5, reclaim: 0, reclaimable: 7.76},
            {tag: "rv2, some ticked", total: 30, free: 1.5, reclaim: 5.2, reclaimable: 7.76},
            {tag: "rv2, all ticked", total: 30, free: 1.5, reclaim: 7.76, reclaimable: 7.76},
            {tag: "nothing to reclaim", total: 30, free: 12, reclaim: 0, reclaimable: 0},
            {tag: "empty stick", total: 30, free: 30, reclaim: 0, reclaimable: 0},
            // Nonsense inputs must still produce a bar that sums: a stale
            // scan can outlive the stick it measured.
            {tag: "reclaimable exceeds used", total: 30, free: 28, reclaim: 9, reclaimable: 9},
            {tag: "ticked exceeds reclaimable", total: 30, free: 1.5, reclaim: 20, reclaimable: 7.76}
        ];
    }

    function test_segments_sum_to_capacity(data) {
        var bar = createTemporaryObject(barComponent, testCase, {
            totalBytes: data.total * gb, freeBytes: data.free * gb,
            reclaimBytes: data.reclaim * gb, reclaimableBytes: data.reclaimable * gb
        });
        var sum = bar.untouchedBytes + bar.notTickedBytes + bar.reclaimClamped + bar.freeBytes;
        fuzzyCompare(sum, bar.totalBytes, gb / 1024);
        // And no segment is ever negative, which would silently shorten
        // its neighbours instead of showing as an error.
        verify(bar.untouchedBytes >= 0);
        verify(bar.notTickedBytes >= 0);
        verify(bar.reclaimClamped >= 0);
    }

    function test_ticked_never_exceeds_reclaimable() {
        var bar = createTemporaryObject(barComponent, testCase, {
            totalBytes: 30 * gb, freeBytes: 1.5 * gb, reclaimBytes: 20 * gb, reclaimableBytes: 7.76 * gb
        });
        compare(bar.reclaimClamped, 7.76 * gb);
        compare(bar.notTickedBytes, 0);
    }

    function test_human_is_binary_and_labelled() {
        var bar = createTemporaryObject(barComponent, testCase, {totalBytes: 30 * gb, freeBytes: 1.5 * gb});
        compare(bar.human(0), "0 B");
        compare(bar.human(1024), "1.0 KB");
        compare(bar.human(7.76 * gb), "7.8 GB");
        // Negative/nonsense never renders as a negative size.
        compare(bar.human(-5), "0 B");
    }

    function test_screenshot() {
        var bar = createTemporaryObject(barComponent, testCase, {
            totalBytes: 30 * gb, freeBytes: 1.5 * gb,
            reclaimBytes: 5.2 * gb, reclaimableBytes: 7.76 * gb
        });
        waitForRendering(bar);
        verify(bar.height > 0);
        if (!screenshotDir || screenshotDir.length === 0)
            return;
        var image = grabImage(bar);
        image.save(screenshotDir + "/SpaceReclaimBar.png");
    }
}
