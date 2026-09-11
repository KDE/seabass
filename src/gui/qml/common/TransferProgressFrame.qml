// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Progress of a long file transfer (restore, clone), framed.
//
// The body is ProgressReport, which is the same body BusyOverlay and the
// backup page use: a strip of phase chips with the current one lit, a
// bar that is determinate only in the phases that have a byte total,
// then files / bytes / rate / ETA and the file being written. This file
// is now the frame around it and the property names its two callers
// already use, so nothing had to change on their side.
//
// The page feeds it its controller's progress properties one by one so a
// plain JS stand-in works in tests.
Frame {
    id: frame

    property var phases: []              // phase ids in order, e.g. ["analyzing", "writing", "checking"]
    property string phase: ""            // the current one
    property var determinatePhases: []   // phases whose bytesTotal is meaningful; the bar sweeps in the others
    property var phaseLabel: function(id) { return id; }
    property double filesDone: 0
    property double filesTotal: 0
    property double bytesDone: 0
    property double bytesTotal: 0
    property double bytesPerSecond: 0
    property int etaSeconds: -1
    property string currentFile: ""
    property string cancelButtonObjectName: "cancelButton"
    property bool cancelEnabled: true
    signal cancelRequested()

    ProgressReport {
        anchors.fill: parent
        phases: frame.phases
        phase: frame.phase
        phaseLabel: frame.phaseLabel
        determinatePhases: frame.determinatePhases
        unitsDone: frame.filesDone
        unitsTotal: frame.filesTotal
        unitName: "files"
        bytesDone: frame.bytesDone
        bytesTotal: frame.bytesTotal
        bytesPerSecond: frame.bytesPerSecond
        etaSeconds: frame.etaSeconds
        currentItem: frame.currentFile
        cancellable: true
        cancelEnabled: frame.cancelEnabled
        cancelButtonObjectName: frame.cancelButtonObjectName
        onCancelRequested: frame.cancelRequested()
    }
}
