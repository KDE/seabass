// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <Qt>

namespace seabass::gui
{

// The colour scheme the Windows build asks its controls style for.
//
// FluentWinUI3 draws native Windows 11 controls and ignores the Material
// attached properties Main.qml sets, so it follows the Windows colour
// scheme unless told otherwise; on a light-mode machine the whole app
// rendered light under Theme's Kelp. Qt 6.8's styleHints()->setColorScheme
// is what tells it: Dark while Kelp is in force, and Unknown -- "follow
// the system", which is what Match System Theme asks for -- otherwise.
//
// Pure, so the choice is testable where the call is not.
inline Qt::ColorScheme styleColorSchemeFor(bool useSystemTheme)
{
    return useSystemTheme ? Qt::ColorScheme::Unknown : Qt::ColorScheme::Dark;
}

// Hands styleColorSchemeFor(useSystemTheme) to the application's style
// hints on Windows, at startup (main.cpp) and whenever Preferences flips
// Match System Theme (AppSettingsController), so the controls change
// scheme when Theme repaints rather than at the next start. Does nothing
// elsewhere: Linux does the same job through applyAppColorScheme(), and
// macOS runs Material, which takes Main.qml's Material.theme binding.
// Needs a QGuiApplication; does nothing without one.
void applyStyleColorScheme(bool useSystemTheme);

}  // namespace seabass::gui
