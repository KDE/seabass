// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Shown in place of the (invisible) waveform cue markers when a track's
// duration is unknown -- Engine never analyzed it (no waveform/beatgrid
// data computed, typically because cues were set live on hardware and
// the track was never run through Engine's offline analysis), so
// WaveformView has nothing to position cue markers against and silently
// draws none, even though the cues genuinely exist and may differ from
// another copy's -- confirmed against real duplicate-conflict data where
// both copies had real, differing hot-cue data but null duration/
// waveform/beatgrid fields.
Label {
    id: root
    required property var cues
    property real durationMs: 0
    visible: durationMs <= 0 && cues.length > 0
    color: Theme.conflictText
    wrapMode: Text.WordWrap
    Layout.fillWidth: true

    text: {
        var hot = 0;
        var memory = 0;
        for (var i = 0; i < cues.length; i++) {
            if (cues[i].kind === "hot") {
                hot++;
            } else {
                memory++;
            }
        }
        var parts = [];
        if (hot > 0) {
            parts.push(hot + " hot");
        }
        if (memory > 0) {
            parts.push(memory + " memory");
        }
        return "⚠ " + parts.join(", ") + " cue(s) set, not shown above: this track wasn't analyzed by "
            + "Engine, so its duration is unknown.";
    }
}
