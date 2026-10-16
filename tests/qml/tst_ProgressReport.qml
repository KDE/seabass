// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The step strip above a progress bar keeps every step inside the report.
//
// It was one row: Restore onto a stick names four steps, and in the
// progress dialog's width the last one ran off its edge as "Checking t".
TestCase {
    id: testCase
    name: "ProgressReport"
    width: 800
    height: 400
    visible: true
    when: windowShown

    Component {
        id: reportComponent
        ProgressReport {
            phases: ["analyzing", "writing", "removing", "checking"]
            phase: "analyzing"
            phaseLabel: function(phase) {
                switch (phase) {
                case "analyzing": return "Comparing with the drive";
                case "writing": return "Restoring files";
                case "removing": return "Removing extras";
                case "checking": return "Checking the library";
                }
                return phase;
            }
        }
    }

    function test_everyStepFitsInANarrowReport_data() {
        return [{tag: "wide", width: 760}, {tag: "narrow", width: 360}];
    }

    function test_everyStepFitsInANarrowReport(row) {
        const report = createTemporaryObject(reportComponent, testCase, {width: row.width});
        verify(report);
        waitForRendering(report);
        const strip = findChild(report, "phaseStrip");
        verify(strip, "no phase strip");
        let chips = 0;
        for (let i = 0; i < strip.children.length; ++i) {
            const chip = strip.children[i];
            if (chip.modelData === undefined) {
                continue;
            }
            ++chips;
            const right = chip.mapToItem(report, chip.width, 0).x;
            verify(right <= report.width + 0.5,
                   "\"" + chip.modelData + "\" ends at " + right + " in a report " + report.width + " wide");
        }
        compare(chips, 4, "one chip per step");
        if (screenshotDir) {
            grabImage(report).save(screenshotDir + "/ProgressReport-steps-" + row.tag + ".png");
        }
    }
}
