// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/app_color_scheme.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QVariant>

namespace seabass::gui
{

namespace
{

// Theme.qml's Kelp values, as "r,g,b": background #14181c, surface
// #1a1f24, border #333a40, text #e8ecef, accent #3daee9, and textMuted,
// which Theme mixes as background + 0.55 * (text - background).
constexpr const char *Background = "20,24,28";
constexpr const char *Surface = "26,31,36";
constexpr const char *Raised = "44,50,56";  // kelpBorderSubtle: a button stands off the surface
constexpr const char *Text = "232,236,239";
constexpr const char *TextMuted = "137,141,144";
constexpr const char *Accent = "61,174,233";

[[maybe_unused]] QString colorSet(const char *name, const char *background, const char *alternate, const char *foreground)
{
    return QStringLiteral("[Colors:%1]\n"
                          "BackgroundAlternate=%2\n"
                          "BackgroundNormal=%3\n"
                          "DecorationFocus=%4\n"
                          "DecorationHover=%4\n"
                          "ForegroundActive=%4\n"
                          "ForegroundInactive=%5\n"
                          "ForegroundLink=29,153,243\n"
                          "ForegroundNegative=218,68,83\n"
                          "ForegroundNeutral=246,116,0\n"
                          "ForegroundNormal=%6\n"
                          "ForegroundPositive=39,174,96\n"
                          "ForegroundVisited=155,89,182\n\n")
        .arg(QString::fromLatin1(name), QString::fromLatin1(alternate), QString::fromLatin1(background),
             QString::fromLatin1(Accent), QString::fromLatin1(TextMuted), QString::fromLatin1(foreground));
}

[[maybe_unused]] QString kelpColorScheme()
{
    QString scheme;
    scheme += QStringLiteral("[General]\nColorScheme=SeabassKelp\nName=Seabass Kelp\n\n");
    scheme += colorSet("Window", Background, Surface, Text);
    scheme += colorSet("View", Surface, Background, Text);
    scheme += colorSet("Header", Surface, Background, Text);
    scheme += colorSet("Button", Raised, Surface, Text);
    scheme += colorSet("Tooltip", Raised, Surface, Text);
    scheme += colorSet("Complementary", Background, Surface, Text);
    // Selected rows and the highlighted button: Current, with white on it.
    scheme += colorSet("Selection", Accent, "30,87,116", "252,252,252");
    return scheme;
}

}  // namespace

QString applyAppColorScheme(bool useSystemTheme, const QString &directory)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    Q_UNUSED(useSystemTheme);
    Q_UNUSED(directory);
    return {};
#else
    if (useSystemTheme || QCoreApplication::instance() == nullptr || directory.isEmpty()) {
        return {};
    }
    const char *property = "KDE_COLOR_SCHEME_PATH";
    if (!qApp->property(property).toString().isEmpty()) {
        return {};
    }
    if (!QDir().mkpath(directory)) {
        return {};
    }
    const QString path = QDir(directory).filePath(QStringLiteral("SeabassKelp.colors"));
    const QByteArray contents = kelpColorScheme().toUtf8();
    QFile existing(path);
    const bool current = existing.open(QIODevice::ReadOnly) && existing.readAll() == contents;
    existing.close();
    if (!current) {
        // Whole or not at all: a half-written scheme would be read as a
        // scheme, with every colour it lost falling back to a default.
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(contents) != contents.size() || !out.commit()) {
            return {};
        }
    }
    qApp->setProperty(property, path);
    return path;
#endif
}

}  // namespace seabass::gui
