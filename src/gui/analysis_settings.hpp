// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

#include "domain/seabass_grid_analyser.hpp"

namespace seabass::gui
{

// The tempo range Seabass's own analysis reads a beat in: which tempo is
// "the" tempo of a track where half or double would do as well. A choice
// among a few ranges, as rekordbox offers it, stored as "low-high" under
// AnalysisTempoRangeKey. Whoever analyses audio for the user (the track
// import; the page that will analyse a library) reads it with
// storedAnalysisTempoRange() and hands it to domain::analyseAudio().
inline constexpr char AnalysisTempoRangeKey[] = "music/analysisTempoRange";

inline QString defaultAnalysisTempoRange()
{
    return QStringLiteral("70-180");
}

// The ranges offered, slowest first. 70 to 180 is the analyser's default
// and rekordbox's own; 88 to 175 is one octave, which puts drum and bass
// at its faster tempo; 58 to 115 keeps slow music slow.
inline QStringList analysisTempoRangeChoices()
{
    return {QStringLiteral("58-115"), QStringLiteral("70-180"), QStringLiteral("78-155"), QStringLiteral("88-175")};
}

// A choice as the analyser takes it. Anything that is not one of the
// choices (a settings file edited by hand) is the default range.
inline domain::TempoRange tempoRangeOf(const QString &choice)
{
    if (!analysisTempoRangeChoices().contains(choice)) {
        return {};
    }
    const QStringList ends = choice.split(QLatin1Char('-'));
    return {ends.at(0).toDouble(), ends.at(1).toDouble()};
}

inline domain::TempoRange storedAnalysisTempoRange(const QSettings &settings)
{
    return tempoRangeOf(settings.value(QLatin1String(AnalysisTempoRangeKey), defaultAnalysisTempoRange()).toString());
}

}  // namespace seabass::gui
