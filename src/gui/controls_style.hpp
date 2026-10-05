// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

namespace seabass::gui
{

// Picks the Qt Quick Controls style this app runs under, where the
// platform's own default is not one it can be themed in.
//
// Windows falls back to "Basic" with nothing set, which looks nothing
// like the rest of the app, so it takes FluentWinUI3. macOS's native
// style refuses the background and contentItem overrides every control
// here is built on -- it logs "does not support customization" and
// draws its own -- so it takes SeabassStyle (src/gui/qml/style), the
// app's own controls drawn in Theme's colours at Theme.cornerRadius, with
// Basic underneath for every control it does not define. Basic alone
// accepted the overrides and inherited the app's font, but drew every box
// square and every combo popup in the platform's palette.
// (Material was tried there and accepted the overrides too, but it
// hardcodes its own type scale -- 16pt controls beside 14pt labels
// against a 13pt system font -- which no other platform sees.)
// Linux keeps its KDE-driven auto-selection, except inside an AppImage,
// which cannot load Plasma's plugins and so asks for org.kde.desktop itself.
//
// An explicit QT_QUICK_CONTROLS_STYLE always wins, so a developer, and
// ctest, can still pin a style deliberately.
//
// The test harness calls this too, and that is the point: a suite that
// runs under a style the app never uses is testing a different program.
// macOS found this the expensive way -- rows reported as painting
// nothing, on a fresh profile, under the native style the shipped app
// replaces before it draws anything.
void applyDefaultControlsStyle();

// Whether a Label nobody gave a colour takes its ink from the palette
// under `styleName`, the resolved Qt Quick Controls style, and nothing of
// the app's reaches that palette unless the app puts it there.
//
// Basic and Fusion: the styles a Linux desktop other than Plasma gives
// this app (Fusion when nothing is asked for, Basic where a style is
// pinned). Their palette is Basic's fixed light one or the platform's,
// which is Qt's light one anywhere but Plasma, whatever Theme paints.
// Not KDE's style (its ink is KDE's colour scheme, which
// applyAppColorScheme() makes Kelp), not Material (Material.theme, set
// in Main.qml), not FluentWinUI3 (the colour scheme hint,
// applyStyleColorScheme()): each of those already follows Theme, and
// their other palette roles are their own look.
//
// Pure, so the choice is testable apart from the style in force.
//
// SeabassStyle too, and it is the one that matters on macOS. Its own
// controls draw from Theme directly, but everything it does not define
// falls back to Basic -- Page and ScrollView among them -- and those take
// their ground and ink from this palette. Leaving it off the list when the
// style was introduced switched ThemePalette off on macOS: every page
// background fell back to the platform's black while Linux and Windows
// stayed Kelp, and tst_InkContrast, which only checks the palette when
// this answers true, stopped checking without failing.
inline bool styleInksFromPalette(const QString &styleName)
{
    return styleName == QLatin1String("Basic") || styleName == QLatin1String("Fusion")
        || styleName == QLatin1String("SeabassStyle");
}

// The style in force, for QML: common/ThemePalette.qml gives the window
// Theme's colours as its palette when inksFromPalette is true.
class ControlsStyle : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(bool inksFromPalette READ inksFromPalette CONSTANT)

public:
    explicit ControlsStyle(QObject *parent = nullptr) : QObject(parent) {}

    // QQuickStyle::name(): read when QML first asks, by which time
    // QtQuick.Controls is imported and the style resolved.
    QString name() const;
    bool inksFromPalette() const { return styleInksFromPalette(name()); }
};

}  // namespace seabass::gui
