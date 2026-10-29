// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QLatin1Char>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <filesystem>
#include <system_error>
#include <vector>

#include "domain/track.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_catalogs.hpp"
#include "gui/stick_path.hpp"

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

// catalogPathForFormat(), as the QString path a page and a change hold.
inline QString catalogQtPathForFormat(const QString &libraryPath, const std::string &format)
{
    return qtPathFromUtf8(catalogPathForFormat(libraryPath.toStdString(), format));
}

// Where the waveform of `stickTrack` can be read, on the stick whose
// catalog `libraryPath` is: {format, libraryPath, sourceId} for
// PlaybackController::waveformFor, at the path for that row's own format,
// or an empty map when no catalog row has one to offer. What both
// metadata pages' waveformSourceAt() answer, for the row a page opened.
inline QVariantMap metadataWaveformSource(const domain::Track &stickTrack, const QString &libraryPath)
{
    if (libraryPath.isEmpty()) {
        return {};
    }
    const auto *catalogRow = waveformCatalogRow(stickTrack);
    if (catalogRow == nullptr) {
        return {};
    }
    return {
        {QStringLiteral("format"), QString::fromStdString(catalogRow->format)},
        {QStringLiteral("libraryPath"), catalogQtPathForFormat(libraryPath, catalogRow->format)},
        {QStringLiteral("sourceId"), QString::fromStdString(catalogRow->sourceId)},
    };
}

// Where a stored track's audio file is now, or empty when no stick that
// is plugged in has it. The backup holds no audio, so a stored row plays
// from a stick: first the one it was backed up from, by label, then any
// other, because a clone or a second stick with the same export keeps the
// folders and not the label. `libraries` is one {label, catalogPath} per
// stick with a library on it.
//
// A stat per stick: for the row under the pointer or the open one, never
// for a whole list.
inline QString storedTrackFileOnSticks(const QString &relativePath, const QString &stickLabel,
                                       const QVariantList &libraries)
{
    if (relativePath.isEmpty()) {
        return {};
    }
    // The store keeps stick-relative paths (stickRelativePath refuses the
    // rest), and anything else would name a file off the stick.
    const std::filesystem::path relative = pathFromQString(relativePath);
    if (relative.has_root_path()) {
        return {};
    }
    for (const auto &part : relative) {
        if (part == "..") {
            return {};
        }
    }
    for (const bool sameLabel : {true, false}) {
        for (const QVariant &entry : libraries) {
            const QVariantMap library = entry.toMap();
            if ((library.value(QStringLiteral("label")).toString() == stickLabel) != sameLabel) {
                continue;
            }
            const QString root = stickRootOf(library.value(QStringLiteral("catalogPath")).toString());
            if (root.isEmpty()) {
                continue;
            }
            const std::filesystem::path file = pathFromQString(root) / relative;
            std::error_code ec;
            if (std::filesystem::is_regular_file(file, ec)) {
                return pathToQString(file);
            }
        }
    }
    return {};
}

}  // namespace seabass::gui
