// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QLatin1Char>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

#include "domain/track.hpp"

namespace seabass::gui
{

// The two strings every metadata row renders, in one place.
//
// Three lists show the same track in the same shape -- what is stored,
// what a restore would put back, what a backup would take -- through one
// delegate (MetadataTrackDelegate). They had three identical copies of
// these two functions between them, which is three chances for one list
// to start spelling a loop or a duration differently from its neighbour
// on the page.

// "6:01", and "--:--" for a length nothing could read. Not "0:00": a
// track of no length and a track of unknown length are different facts,
// and only one of them is alarming.
inline QString metadataDurationText(double seconds)
{
    if (seconds <= 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Every cue on a line, for the hover tooltip on a row's cue badge. Built
// from a list already in memory, so it costs nothing the scan did not
// already pay for -- unlike the browse list's version, which has to go
// back to the database and is therefore called on hover only.
inline QString metadataCueSummary(const std::vector<domain::CuePoint> &cues)
{
    if (cues.empty()) {
        return {};
    }
    QStringList lines;
    for (const auto &cue : cues) {
        QString line = cue.kind == domain::CuePoint::Kind::Hot
            ? QStringLiteral("Hot cue %1").arg(cue.hotCueNumber)
            : QStringLiteral("Memory cue");
        line += QStringLiteral(" at ") + metadataDurationText(cue.positionMs / 1000.0);
        if (cue.isLoop) {
            line += QStringLiteral(" (loop)");
        }
        if (!cue.comment.empty()) {
            line += QStringLiteral(": ") + QString::fromStdString(cue.comment);
        }
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

// The cues as WaveformView draws them: one map per cue, in the shape
// every other track list in Seabass hands it. The one conversion for all
// three metadata lists, the store's own browse list included.
inline QVariantList metadataCueList(const std::vector<domain::CuePoint> &cues)
{
    QVariantList list;
    for (const auto &cue : cues) {
        QVariantMap entry;
        entry[QStringLiteral("kind")] =
            cue.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("hot") : QStringLiteral("memory");
        entry[QStringLiteral("hotCueNumber")] = cue.hotCueNumber;
        entry[QStringLiteral("positionMs")] = cue.positionMs;
        entry[QStringLiteral("positionText")] = metadataDurationText(cue.positionMs / 1000.0);
        entry[QStringLiteral("isLoop")] = cue.isLoop;
        entry[QStringLiteral("loopEndMs")] = cue.loopEndMs;
        entry[QStringLiteral("color")] = QString::fromStdString(cue.color);
        entry[QStringLiteral("comment")] = QString::fromStdString(cue.comment);
        list << entry;
    }
    return list;
}

// The catalog row a stick track's waveform can be read from, or null.
//
// A metadata backup holds no waveforms (docs/metadata-backup-plan.md),
// so a row on either metadata page can only show one the stick itself
// carries. rekordbox first, because its preview is an ANLZ file read on
// its own; Engine's needs its database opened. OneLibrary has no
// waveform reader at all, and a track only it lists has none to show.
inline const domain::CatalogRowRef *waveformCatalogRow(const domain::Track &track)
{
    for (const char *format : {"rekordbox", "engine"}) {
        for (const auto &row : track.catalogRows) {
            if (row.format == format && !row.sourceId.empty()) {
                return &row;
            }
        }
    }
    return nullptr;
}

}  // namespace seabass::gui
