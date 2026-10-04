// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "system_font_metrics.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QScreen>

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
    if (m_generalPointSizeOverride > 0) {
        return m_generalPointSizeOverride;
    }
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).pointSizeF();
}

qreal SystemFontMetrics::generalPixelSize() const
{
    // Deliberately built on generalPointSize(), so the override above
    // carries through: a test that stands the app up at another
    // system's font size must still see every Theme.scaled() length
    // grow with it, which is the whole reason that override exists.
    const qreal points = generalPointSize();
    if (points > 0) {
        const QScreen *screen = QGuiApplication::primaryScreen();
        // 96 rather than 72 when there is no screen yet: it is what the
        // platforms that can run headless report, so an early read
        // matches what the first real read will say.
        const qreal dpi = screen != nullptr ? screen->logicalDotsPerInch() : 96.0;
        return points * dpi / 72.0;
    }
    // A theme may set its fonts in pixels instead, which is what a
    // point size of -1 means here; then the pixels are already the
    // answer.
    const int pixels = QFontDatabase::systemFont(QFontDatabase::GeneralFont).pixelSize();
    return pixels > 0 ? pixels : 0;
}

qreal SystemFontMetrics::generalPointSizeOverride() const
{
    return m_generalPointSizeOverride;
}

void SystemFontMetrics::setGeneralPointSizeOverride(qreal pointSize)
{
    if (qFuzzyCompare(m_generalPointSizeOverride, pointSize)) {
        return;
    }
    m_generalPointSizeOverride = pointSize;
    emit changed();
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
