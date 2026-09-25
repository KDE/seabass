// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QColor>
#include <QObject>
#include <QQmlEngine>
#include <QString>

namespace seabass::gui
{

// Kelp, this app's own always-dark palette, as red, green and blue. The
// one place its values live: Theme.qml reads them through KelpPalette
// below, and the colour scheme applyAppColorScheme() hands KDE's style is
// written from them, so the style's ink and Theme's grounds cannot drift
// apart.
struct KelpRgb
{
    int r;
    int g;
    int b;
};

namespace kelp
{
inline constexpr KelpRgb Background{0x14, 0x18, 0x1c};
inline constexpr KelpRgb Surface{0x1a, 0x1f, 0x24};
inline constexpr KelpRgb Border{0x33, 0x3a, 0x40};
inline constexpr KelpRgb BorderSubtle{0x2c, 0x32, 0x38};
inline constexpr KelpRgb Text{0xe8, 0xec, 0xef};
inline constexpr KelpRgb Accent{0x3d, 0xae, 0xe9};
inline constexpr KelpRgb Primary{0x12, 0x3a, 0x52};
}  // namespace kelp

// Kelp for QML: Theme.qml's kelp* colours are these.
class KelpPalette : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QColor background READ background CONSTANT)
    Q_PROPERTY(QColor surface READ surface CONSTANT)
    Q_PROPERTY(QColor border READ border CONSTANT)
    Q_PROPERTY(QColor borderSubtle READ borderSubtle CONSTANT)
    Q_PROPERTY(QColor text READ text CONSTANT)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    Q_PROPERTY(QColor primary READ primary CONSTANT)

public:
    explicit KelpPalette(QObject *parent = nullptr) : QObject(parent) {}

    static QColor color(KelpRgb rgb) { return QColor(rgb.r, rgb.g, rgb.b); }
    QColor background() const { return color(kelp::Background); }
    QColor surface() const { return color(kelp::Surface); }
    QColor border() const { return color(kelp::Border); }
    QColor borderSubtle() const { return color(kelp::BorderSubtle); }
    QColor text() const { return color(kelp::Text); }
    QColor accent() const { return color(kelp::Accent); }
    QColor primary() const { return color(kelp::Primary); }
};

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
// Live, not only at startup: Preferences flips useSystemTheme while the
// app runs, and Theme repaints at once. With useSystemTheme on, this
// clears the property again (the desktop's scheme is then what the app
// asked for), and either way it announces the change the way
// KColorSchemeManager's palette change does, with an
// ApplicationPaletteChange, which is what makes KDE's style re-read its
// colours (qqc2-desktop-style's StyleSingleton::refresh). Without that
// the style kept Kelp's near-white ink over a light system Theme until
// the next start.
//
// `directory` is where the scheme file goes; empty means the one an
// earlier call named (main() names it at startup, AppSettingsController
// flips the choice later). Leaves the property alone when something
// other than this function set it, and does nothing on Windows and
// macOS, whose styles do not read KDE colour schemes. The first call
// must come after the QGuiApplication exists and before the QML engine
// loads anything. Returns the scheme path in force afterwards, empty if
// none of ours is.
QString applyAppColorScheme(bool useSystemTheme, const QString &directory = {});

}  // namespace seabass::gui
