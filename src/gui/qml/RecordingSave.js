// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Save as FLAC (KeepRecordingController), in the words every page that
// offers it uses: the Recordings page and Browse's Own Recordings say the
// same of a recording and of its save, so the words are here once.
.pragma library

// "1:07:13" or "7:13"; "" when unknown. Cut to whole seconds as a clock
// shows them, never rounded up: the C++ texts (the notes of a saved job,
// the reasons) count the same way. `rounded`: to the nearest second
// instead, as the released Recordings page gives a row's length.
function duration(seconds, rounded) {
    if (seconds === undefined || seconds < 0) return "";
    const total = rounded === true ? Math.round(seconds) : Math.floor(seconds);
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    const two = (v) => (v < 10 ? "0" : "") + v;
    return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s);
}

// For a header that declares more than the file holds, when both lengths
// print the same: how much is missing, in words. "" otherwise.
function missingNote(declaredSeconds, heldSeconds, missingBytes) {
    if (missingBytes === undefined || missingBytes < 0 || declaredSeconds < 0
        || duration(declaredSeconds) !== duration(heldSeconds)) {
        return "";
    }
    return qsTr("%n byte(s) less than the header declares", "", missingBytes);
}

// The steps of a job, as the strip lists them.
function steps() {
    return [
        {key: "encoding", label: qsTr("Encoding")},
        {key: "checking", label: qsTr("Checking")},
        {key: "tags", label: qsTr("Tags and cover")},
        {key: "placing", label: qsTr("Saving")},
    ];
}
function stepIndex(step) {
    // The free space check is over before the encoder starts: it is part
    // of the first step.
    if (step === "space") return 0;
    return steps().findIndex(s => s.key === step);
}

// A recording's row, of what its save is or came to: `kept` is the
// controller's outcomeFor(), `step` its progressStep while that job is
// the one in hand. "" when no save was asked for.
function stateLine(kept, step) {
    switch (kept.state ?? "") {
    case "queued": return qsTr("Waits to be saved as %1").arg(kept.fileName);
    case "running": {
        const index = stepIndex(step);
        return index >= 0 ? qsTr("Being saved as %1: %2").arg(kept.fileName).arg(steps()[index].label.toLowerCase())
            : qsTr("Being saved as %1").arg(kept.fileName);
    }
    case "kept": return qsTr("Saved as %1 in %2").arg(kept.fileName).arg(kept.folder);
    case "failed": return qsTr("Not saved: %1").arg(kept.message);
    case "cancelled": return (kept.message ?? "").length > 0
        ? qsTr("Not saved, stopped: %1").arg(kept.message)
        : qsTr("Not saved: stopped while it was being saved");
    case "dropped": return qsTr("Not saved: stopped before it started");
    default: return "";
    }
}
// The save asked for did not end in a FLAC.
function stateFailed(state) {
    return state === "failed" || state === "cancelled" || state === "dropped";
}
