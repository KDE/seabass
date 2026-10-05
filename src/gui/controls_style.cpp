// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/controls_style.hpp"

// QByteArray explicitly, not transitively: qputenv() takes a
// QByteArrayView for its value, and nothing in <QtGlobal> provides the
// conversion from a string literal. This compiled in main.cpp only
// because its neighbours -- QGuiApplication and friends -- dragged
// QByteArray in, so the call was leaning on them rather than standing
// on its own. Moving it into a file of its own is what exposed that,
// on macOS, where the error is "no matching function for call to
// qputenv". The Windows arm has the same shape and simply is not
// compiled on any machine that has built this so far.
#include <QByteArray>
#include <QQuickStyle>
#include <QtGlobal>

namespace seabass::gui
{

void applyDefaultControlsStyle()
{
    if (!qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        return;
    }
#if defined(Q_OS_WIN)
    qputenv("QT_QUICK_CONTROLS_STYLE", "FluentWinUI3");
#elif defined(Q_OS_MACOS)
    // Basic, whose controls take the app's own font and the palette
    // ThemePalette gives them -- the same deal a Linux desktop that is
    // not Plasma gets, and the reason styleInksFromPalette() lists it.
    //
    // Material was here first, because the native macOS style refuses
    // this app's background and contentItem overrides. Material accepts
    // them, but it also hardcodes a type scale of its own and hands a
    // different size to each kind of control: measured under it, a
    // ComboBox and a TextField come out at 16pt and a Label at 14pt
    // while the system font is 13pt. On Browse Library that put three
    // sizes on one row -- the library picker at 14 (its contentItem is
    // the app's own Labels), the sort combo at 16 (Material's
    // contentItem), the search field at 16 and 25 px taller than the
    // combo beside it, since only the combo's height was pinned.
    //
    // None of it showed on Linux, where KDE's style leaves fonts to the
    // system font, so Material was a second source of truth that only
    // one platform ever consulted. Basic has no type scale of its own:
    // every control inherits the app font, which is what the rest of
    // this app already assumes.
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
#elif defined(Q_OS_LINUX)
    // Inside an AppImage, KDE's desktop style, as on a Plasma desktop.
    // Installed normally, the app gets org.kde.desktop from Plasma's
    // platform theme and nothing needs setting. An AppImage carries its own
    // Qt, which cannot load the system's Plasma plugins, so the same app
    // would otherwise come up in Qt's plain Basic style -- on Plasma
    // included. The AppImage bundles qqc2-desktop-style and Breeze (see
    // craft-blueprint/qt-apps/seabass/seabass.py) and asks for them here.
    // APPIMAGE is set by the AppImage runtime to the image's own path.
    if (!qEnvironmentVariableIsEmpty("APPIMAGE")) {
        qputenv("QT_QUICK_CONTROLS_STYLE", "org.kde.desktop");
    }
#endif
}

QString ControlsStyle::name() const
{
    return QQuickStyle::name();
}

}  // namespace seabass::gui
