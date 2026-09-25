// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/style_color_scheme.hpp"

#include <QGuiApplication>
#include <QStyleHints>

namespace seabass::gui
{

void applyStyleColorScheme(bool useSystemTheme)
{
#ifdef Q_OS_WIN
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr) {
        return;
    }
    QGuiApplication::styleHints()->setColorScheme(styleColorSchemeFor(useSystemTheme));
#else
    Q_UNUSED(useSystemTheme);
#endif
}

}  // namespace seabass::gui
