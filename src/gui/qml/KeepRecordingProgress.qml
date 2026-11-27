// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui
import "RecordingSave.js" as RecordingSave

// Save as FLAC, beside the page: a stick's job in hand, its ones that
// wait, and the end of the last one, from the window's
// KeepRecordingController. The Recordings page and Browse show the same
// strip of the same stick's work. Not tied to Experimental: work under
// way is shown, and can be cancelled, until it has ended, and what ended
// until it is closed.
PassiveProgress {
    id: root

    // A KeepRecordingController; a QtObject with the same shape in the
    // tests. Null: nothing shown.
    property var keepController: null
    // The stick whose work this shows.
    property string stickRoot: ""

    readonly property var keep: root.keepController
    readonly property int keepRevision: root.keep !== null ? root.keep.revision : 0
    // A recording of this stick is being saved or waits to be.
    readonly property bool keepWorking: root.keep !== null
        && (root.keepRevision, root.keep.hasWorkFor(root.stickRoot)) === true
    readonly property bool runsHere: root.keep !== null
        && (root.keepRevision, root.keep.runsFor(root.stickRoot)) === true
    // This stick's jobs that wait.
    readonly property var keepQueued: root.keep !== null
        ? ((root.keepRevision, root.keep.queuedFor(root.stickRoot)) ?? []) : []
    readonly property var keepFinishedJobs: root.keep !== null
        ? ((root.keepRevision, root.keep.finishedJobsFor(root.stickRoot)) ?? []) : []
    readonly property int keepNotKeptCount: root.keepFinishedJobs.filter(job => job.state !== "kept").length
    readonly property bool finishedShown: !root.keepWorking && root.keepFinishedJobs.length > 0
    readonly property var last: root.keepFinishedJobs.length > 0
        ? root.keepFinishedJobs[root.keepFinishedJobs.length - 1] : ({})
    readonly property string endState: root.last.state ?? ""
    readonly property var keepSteps: RecordingSave.steps()
    readonly property int stepIndex: root.runsHere ? RecordingSave.stepIndex(root.keep.progressStep) : -1
    readonly property int waiting: root.keepQueued.length

    function percent(fraction) {
        return Math.floor(Math.max(0, Math.min(1, fraction)) * 100);
    }
    // A line of the panel for a job that ended, other than one dropped
    // (those are named together).
    function endedLine(job) {
        if (job.state === "kept") {
            return qsTr("Saved: %1 as %2 in %3").arg(job.recording).arg(job.fileName).arg(job.folder);
        }
        if (job.state === "cancelled") {
            return (job.message ?? "").length > 0 ? qsTr("Stopped: %1: %2").arg(job.recording).arg(job.message)
                : qsTr("Stopped: %1, nothing of it was saved").arg(job.recording);
        }
        return qsTr("Not saved: %1: %2").arg(job.recording).arg(job.message ?? "");
    }
    function endedLines() {
        const lines = root.keepFinishedJobs.filter(job => job.state !== "dropped").map(job => root.endedLine(job));
        const dropped = root.keepFinishedJobs.filter(job => job.state === "dropped").map(job => job.recording);
        if (dropped.length > 0) {
            lines.push(qsTr("Not saved, stopped before it started: %n recording(s): %1", "", dropped.length)
                .arg(dropped.join(", ")));
        }
        return lines;
    }

    running: root.keep !== null && (root.keepWorking || root.finishedShown)
    done: root.finishedShown ? (root.keepNotKeptCount === 0 ? 1000 : 0)
        : root.runsHere ? Math.round((root.keep?.progress ?? 0) * 1000) : 0
    total: 1000
    // Stops this stick's job in hand and drops its ones that wait.
    cancellable: root.keepWorking && !(root.runsHere && root.keep.cancelRequested === true)
    summary: {
        const keep = root.keep;
        const last = root.last;
        const state = root.endState;
        if (!keep) return "";
        if (root.finishedShown) {
            const jobs = root.keepFinishedJobs;
            if (jobs.length > 1) {
                return root.keepNotKeptCount === 0 ? qsTr("Saved %n recording(s) as FLAC", "", jobs.length)
                    : qsTr("%1 of %n recording(s) not saved", "", jobs.length).arg(root.keepNotKeptCount);
            }
            const left = (last.leftBehind ?? "").length > 0;
            if (state === "kept") {
                return left ? qsTr("Saved %1 as %2, and a second file was left in %3").arg(last.recording)
                                  .arg(last.fileName).arg(last.folder)
                    : qsTr("Saved %1 as %2").arg(last.recording).arg(last.fileName);
            }
            if (state === "cancelled") {
                return left ? qsTr("Stopped: %1 was not saved, and a file was left in %2").arg(last.recording)
                                  .arg(last.folder)
                    : qsTr("Stopped: nothing of %1 was saved").arg(last.recording);
            }
            if (state === "dropped") {
                return qsTr("%1 was not saved: stopped before it started").arg(last.recording);
            }
            return left ? qsTr("%1 was not saved, and a file was left in %2").arg(last.recording).arg(last.folder)
                : qsTr("%1 was not saved").arg(last.recording);
        }
        if (!root.runsHere) {
            return qsTr("%n recording(s) waiting to be saved as FLAC", "", root.waiting);
        }
        if (keep.cancelRequested) {
            return qsTr("Stopping: saving %1").arg(keep.currentRecording);
        }
        const step = root.keepSteps[Math.max(0, root.stepIndex)].label;
        let line = qsTr("Saving %1 as FLAC: %2, %3%").arg(keep.currentRecording).arg(step.toLowerCase())
            .arg(root.percent(keep.progress));
        const extras = [];
        if (root.waiting > 0) {
            extras.push(qsTr("%n more waiting", "", root.waiting));
        }
        if (root.keepNotKeptCount > 0) {
            extras.push(qsTr("%n not saved", "", root.keepNotKeptCount));
        }
        if (extras.length > 0) {
            line += " (" + extras.join(", ") + ")";
        }
        return line;
    }
    steps: root.finishedShown ? [] : root.keepSteps.map((s, index) => ({
        label: s.label,
        count: index === root.stepIndex ? qsTr("%1%").arg(root.percent(root.keep.progress)) : "",
        state: index < root.stepIndex ? "done" : index === root.stepIndex ? "now" : "later",
    }))
    activity: {
        if (!root.keep) return [];
        if (root.finishedShown) {
            return root.endedLines();
        }
        const lines = root.runsHere
            ? [qsTr("Writing %1 in %2").arg(root.keep.currentFileName).arg(root.keep.currentFolder)] : [];
        for (const job of root.keepQueued) {
            lines.push(qsTr("Waiting: %1, as %2").arg(job.recording).arg(job.fileName));
        }
        return lines.concat(root.endedLines());
    }
    // A saved job's notes (an unfinished recording and how much was saved,
    // leftovers swept). A failure's reason is its line above (endedLine())
    // and is not said a second time here.
    note: root.finishedShown ? (root.last.notes ?? []).filter(line => line.length > 0).join("\n")
        : qsTr("The recording on the stick is only read. The work goes on when you leave this page.")
    actions: !root.finishedShown ? []
        : root.endState === "kept" ? [{key: "show", text: qsTr("Show in Folder")}, {key: "close", text: qsTr("Close")}]
        : [{key: "close", text: qsTr("Close")}]
    onCancelRequested: root.keep.cancelFor(root.stickRoot)
    onActionTriggered: (key) => {
        if (key === "show") {
            root.keep.showInFolder(root.last.flacPath);
        } else {
            root.keep.dismissFinishedFor(root.stickRoot);
        }
    }
}
