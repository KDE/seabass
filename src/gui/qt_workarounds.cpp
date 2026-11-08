// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/qt_workarounds.hpp"

#include <QByteArray>
#include <QLatin1StringView>
#include <QtGlobal>

namespace seabass::gui
{

bool jsCollectorFreesLiveObjects(const QVersionNumber &qt)
{
    // The fix is qtdeclarative d1050cc9, "QtQml: Reset gcBlocked when
    // resetting the mark stack" (2026-07-08), picked to 6.8, 6.11 and 6.12.
    if (qt < QVersionNumber(6, 8, 0)) {
        return false;  // no incremental collector yet
    }
    if (qt < QVersionNumber(6, 9, 0)) {
        return qt < QVersionNumber(6, 8, 9);
    }
    return qt < QVersionNumber(6, 11, 2);  // 6.9 and 6.10 never got it
}

bool keepJsCollectorInOneStep()
{
    // QV4_GC_TIMELIMIT is the collector's time budget per step in
    // milliseconds (default a third of a 60 Hz frame); 0 means no budget,
    // every run finishes in the step that starts it.
    if (qEnvironmentVariableIsSet("QV4_GC_TIMELIMIT")) {
        return false;
    }
    const QVersionNumber running = QVersionNumber::fromString(QLatin1StringView(qVersion()));
    if (!jsCollectorFreesLiveObjects(running)) {
        return false;
    }
    qputenv("QV4_GC_TIMELIMIT", QByteArrayLiteral("0"));
    return true;
}

}  // namespace seabass::gui
