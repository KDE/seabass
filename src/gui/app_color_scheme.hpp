// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

namespace seabass::gui
{

// Tells KDE's Qt Quick style the colours Theme.qml paints with.
//
// Theme is an always-dark palette ("Kelp") unless useSystemTheme is on,
// and it paints the grounds this app chooses: every page header, every
// overlay card, every dialog. The ink of a Label nobody gave a colour,
// though, is not Theme's. Under org.kde.desktop -- the style the Linux
// build starts with -- a Label is `color: Kirigami.Theme.textColor`,
// and Kirigami reads that from the desktop's colour scheme. On a light
// Plasma scheme that is near-black, drawn on Kelp's near-black header:
// the page title, an overlay's heading and a report's counts all went
// invisible, on every page at once. A dark Plasma scheme hid it.
//
// KColorScheme takes the application's scheme from the
// KDE_COLOR_SCHEME_PATH property on the application object, which is
// what KColorSchemeManager sets when an app offers a scheme menu. This
// writes Kelp as a scheme file into `directory` and points that property
// at it, so the style's own ink, grounds and buttons are Kelp's too.
// The Windows build does the same job with styleHints()->setColorScheme
// in main.cpp; this is its Linux counterpart.
//
// Does nothing when useSystemTheme is on (the desktop's scheme is then
// what the app asked for), when the property is already set, or on
// Windows and macOS, whose styles do not read KDE colour schemes.
// Must run after the QGuiApplication exists and before the QML engine
// loads anything. Returns the path it set, empty if it set none.
QString applyAppColorScheme(bool useSystemTheme, const QString &directory);

}  // namespace seabass::gui
