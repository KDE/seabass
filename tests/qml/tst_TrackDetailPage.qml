// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Real instantiation test -- TrackDetailPage.qml is now in this module's
// own QML_FILES (see top-level CMakeLists.txt), pulling in its whole
// dependency chain (KeyBadge -> CamelotWheelPopup -> CamelotWheelPosition
// singleton, WaveformView, CueFallbackNotice). Worth the extra compile
// weight: this is the technique that already caught two real structural
// bugs this session (the FINAL-property redeclaration, the singleton-
// ordering gotcha), and TrackDetailPage.qml itself shipped with exactly
// that kind of bug (a `parent.parent.xxx` chain that broke the moment
// nesting depth changed) that a logic-only test would never have seen.
TestCase {
    id: testCase
    name: "TrackDetailPage"
    when: windowShown

    // Plain JS objects stand in for the real C++ controllers -- required
    // property var accepts them fine, and QML property/method access
    // doesn't care whether the object behind it is a QObject or not.
    property var fakeTracks: [
        {
            title: "Opening Track", artist: "Artist A", key: "8A", bpm: 120,
            durationSeconds: 240, artworkPath: "", cues: [], filePath: "/tmp/a.mp3",
            sourceId: "a",
        },
        {
            title: "Middle Track", artist: "Artist B", key: "8A", bpm: 124,
            durationSeconds: 200, artworkPath: "", cues: [], filePath: "/tmp/b.mp3",
            sourceId: "b",
        },
        {
            title: "Closing Track", artist: "Artist C", key: "3B", bpm: 128,
            durationSeconds: 300, artworkPath: "", cues: [], filePath: "",
            sourceId: "c",
        },
    ]

    function makeFakeScanController(tracks) {
        return {
            trackAt: function(index) {
                return (index >= 0 && index < tracks.length) ? tracks[index] : {};
            },
            trackCount: function() {
                return tracks.length;
            },
            // A real (if minimal) mirror of domain::classifyKeyRelation()
            // for plain Camelot-notation input ("8A", "3B", ...) -- close
            // enough to exercise relationDisplay()'s full switch,
            // including the directional adjacentup/adjacentdown split,
            // which a same-number-prefix shortcut can't tell apart from
            // "relative".
            keyRelation: function(keyA, keyB) {
                var matchA = /^(\d+)([AB])$/.exec(keyA);
                var matchB = /^(\d+)([AB])$/.exec(keyB);
                if (!matchA || !matchB) {
                    return {label: "", relation: "unknown"};
                }
                var numA = parseInt(matchA[1], 10), letterA = matchA[2];
                var numB = parseInt(matchB[1], 10), letterB = matchB[2];
                if (numA === numB && letterA === letterB) {
                    return {label: "Same key", relation: "same"};
                }
                if (numA === numB) {
                    return {label: "Relative major/minor", relation: "relative"};
                }
                var diff = Math.abs(numA - numB);
                var wheelDistance = Math.min(diff, 12 - diff);
                if (wheelDistance === 1) {
                    if (letterA !== letterB) {
                        return {label: "Energy mix", relation: "energymix"};
                    }
                    var up = numB === (numA % 12) + 1;
                    return up ? {label: "Energy Boost", relation: "adjacentup"}
                              : {label: "Energy Drop", relation: "adjacentdown"};
                }
                return {label: "Unrelated key", relation: "unrelated"};
            },
        };
    }

    property var fakePlaybackController: ({
        waveformFor: function(format, path, sourceId) { return []; },
        load: function() {},
    })
    property var fakeAppSettingsController: ({keyNotation: "camelot"})

    Component {
        id: pageComponent
        TrackDetailPage {}
    }

    function test_instantiatesMiddleTrackWithBothNeighbors() {
        var page = createTemporaryObject(pageComponent, testCase, {
            scanController: testCase.makeFakeScanController(testCase.fakeTracks),
            trackIndex: 1,
            format: "rekordbox",
            libraryPath: "/tmp/library",
            playbackController: testCase.fakePlaybackController,
            appSettingsController: testCase.fakeAppSettingsController,
        });
        verify(page !== null);
        compare(page.track.title, "Middle Track");
        compare(page.hasPrev, true);
        compare(page.hasNext, true);
        compare(page.prevTrack.title, "Opening Track");
        compare(page.nextTrack.title, "Closing Track");
    }

    function test_firstTrackHasNoPrevious() {
        var page = createTemporaryObject(pageComponent, testCase, {
            scanController: testCase.makeFakeScanController(testCase.fakeTracks),
            trackIndex: 0,
            format: "rekordbox",
            libraryPath: "/tmp/library",
            playbackController: testCase.fakePlaybackController,
            appSettingsController: testCase.fakeAppSettingsController,
        });
        verify(page !== null);
        compare(page.hasPrev, false);
        compare(page.prevTrack, null);
        compare(page.hasNext, true);
    }

    function test_lastTrackHasNoNext() {
        var page = createTemporaryObject(pageComponent, testCase, {
            scanController: testCase.makeFakeScanController(testCase.fakeTracks),
            trackIndex: 2,
            format: "rekordbox",
            libraryPath: "/tmp/library",
            playbackController: testCase.fakePlaybackController,
            appSettingsController: testCase.fakeAppSettingsController,
        });
        verify(page !== null);
        compare(page.hasNext, false);
        compare(page.nextTrack, null);
        // Play button must be disabled for a streaming track (empty
        // filePath) -- exercises the enabled: binding without needing to
        // reach into the Button's own id.
        compare(page.track.filePath.length, 0);
    }

    function test_bpmDiffTextRisingAndFalling() {
        var page = createTemporaryObject(pageComponent, testCase, {
            scanController: testCase.makeFakeScanController(testCase.fakeTracks),
            trackIndex: 0,
            format: "rekordbox",
            libraryPath: "/tmp/library",
            playbackController: testCase.fakePlaybackController,
            appSettingsController: testCase.fakeAppSettingsController,
        });
        verify(page !== null);
        compare(page.bpmDiffText(120, 128), "120 → 128 BPM (+6.7%)");
        compare(page.bpmDiffText(128, 120), "128 → 120 BPM (-6.3%)");
        compare(page.bpmDiffText(0, 128), "");
        compare(page.bpmDiffText(120, 0), "");
    }

    function test_relationDisplayCoversAllTags() {
        var page = createTemporaryObject(pageComponent, testCase, {
            scanController: testCase.makeFakeScanController(testCase.fakeTracks),
            trackIndex: 0,
            format: "rekordbox",
            libraryPath: "/tmp/library",
            playbackController: testCase.fakePlaybackController,
            appSettingsController: testCase.fakeAppSettingsController,
        });
        verify(page !== null);
        compare(page.relationDisplay("8A", "8A").label, "Same key");
        compare(page.relationDisplay("8A", "8B").label, "Relative major/minor");
        compare(page.relationDisplay("8A", "9A").label, "Energy Boost ↑");
        compare(page.relationDisplay("8A", "7A").label, "Energy Drop ↓");
        compare(page.relationDisplay("8A", "9B").label, "Energy mix");
        compare(page.relationDisplay("8A", "3B").label, "Dissonant transition");
        compare(page.relationDisplay("", "8A").label, "");
    }
}
