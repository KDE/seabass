// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// What the Clean Up scan found in the way of audio files that no catalog
// references, and -- the part that matters -- what that answer is based
// on.
//
// A bare count would be exactly the failure this whole area is prone to:
// an answer computed from some of the catalogs, presented as if it were
// computed from all of them. So this never shows a number without saying
// which catalogs it was subtracted from, and shows no number at all when
// the scan refused to answer.
//
// `info` is CleanupController.unreferencedFiles verbatim: {filesFound,
// bytesHuman, unreadable, catalogsConsulted, walkIncomplete,
// probeAvailable, usable, refusal}.
Label {
    id: root

    property var info: ({})

    readonly property bool refused: info.usable === false
    // Nothing found, nothing refused: there is genuinely nothing to say,
    // and a line saying "0 files" would be noise on every clean stick.
    readonly property bool silent: !refused && !(info.filesFound > 0)

    // The catalogs by the names a DJ sees them under everywhere else in
    // the app, never the internal format strings.
    readonly property string catalogNames: {
        var names = info.catalogsConsulted || [];
        var labelled = [];
        for (var i = 0; i < names.length; ++i) {
            labelled.push(FormatLabels.label(names[i]));
        }
        return labelled.join(", ");
    }

    wrapMode: Text.WordWrap
    color: refused ? Theme.warnText : Theme.textMuted
    visible: text.length > 0
    text: {
        if (refused)
            return info.refusal || "";
        if (silent)
            return "";
        var s = "This stick also has " + info.filesFound + " audio file(s), " + info.bytesHuman
            + ", that no catalog references -- checked against " + root.catalogNames
            + ". They are listed below wherever they match a track you still have, so you can see what "
            + "each one duplicates before anything happens to it. Nothing is deleted here either: they "
            + "go to \"Delete Orphaned Files\" with the rest.";
        // A build with no tag reader finds the files but can identify
        // none of them, which is a different thing from finding nothing
        // and has to say so rather than showing an empty list.
        if (info.probeAvailable === false)
            s += " This build cannot read tags from audio files, so none of them could be identified; "
                + "they are only counted here.";
        else if (info.unreadable > 0)
            s += " " + info.unreadable + " of them could not be read at all and are left alone.";
        // Not a safety problem -- a file the walk never saw is never
        // proposed -- but the total must not pass as the whole truth.
        if (info.walkIncomplete)
            s += " Part of the stick could not be read, so there may be more than this.";
        return s;
    }
}
