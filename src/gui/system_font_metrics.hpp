// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QQmlEngine>

namespace seabass::gui
{

// Exposes the platform's current UI font sizes to QML as live,
// NOTIFY-backed properties -- Theme.qml's baseFontPointSize and
// smallestReadablePointSize bind to these.
//
// Deliberately reads QFontDatabase::systemFont() rather than
// QGuiApplication::font(): the latter is the app's own currently-set
// default font, which only reflects the platform theme's real setting
// once something has copied it in there, at some timing this class's
// predecessor (SystemFontWatcher, reverted -- see git history around
// commit d32a548) couldn't rely on. QFontDatabase::systemFont() is a
// direct, live query of the platform theme's current font hint instead,
// with no such "has it been copied in yet" gap. Its SystemFont enum is
// the standard cross-platform primitive for this on every Qt platform
// (QWindowsTheme on Windows, QCocoaTheme on macOS, KDE's own theme
// plugin here), including SmallestReadableFont -- exactly the "Plasma
// smallestFont" floor this exists for, not a KDE-specific concept.
class SystemFontMetrics : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(qreal generalPointSize READ generalPointSize NOTIFY changed)
    Q_PROPERTY(qreal smallestReadablePointSize READ smallestReadablePointSize NOTIFY changed)

public:
    explicit SystemFontMetrics(QObject *parent = nullptr);

    // Both always re-query the platform theme fresh rather than
    // returning a cached value -- see this class's own doc comment for
    // why that distinction is exactly what the previous attempt got
    // wrong.
    qreal generalPointSize() const;
    qreal smallestReadablePointSize() const;

signals:
    void changed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

}  // namespace seabass::gui
