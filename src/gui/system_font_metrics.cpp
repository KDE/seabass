// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "system_font_metrics.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>

namespace seabass::gui
{

SystemFontMetrics::SystemFontMetrics(QObject *parent) : QObject(parent)
{
    // Installed on the whole application, not a specific window: a
    // platform theme change isn't tied to any one QWindow.
    if (qApp) {
        qApp->installEventFilter(this);
    }
}

qreal SystemFontMetrics::generalPointSize() const
{
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).pointSizeF();
}

qreal SystemFontMetrics::smallestReadablePointSize() const
{
    return QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont).pointSizeF();
}

bool SystemFontMetrics::eventFilter(QObject *watched, QEvent *event)
{
    // QEvent::ThemeChange, not ApplicationFontChange -- see this class's
    // header comment for why. No value comparison/suppression here
    // (unlike the reverted SystemFontWatcher's m_lastSeen dance): a
    // duplicate emit is harmless (QML re-evaluates a binding to the same
    // value cheaply), whereas the previous attempt's whole failure mode
    // was about a value never being re-read at all, not about it being
    // re-read too often.
    if (event->type() == QEvent::ThemeChange) {
        emit changed();
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace seabass::gui
