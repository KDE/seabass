// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/app_color_scheme.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QSaveFile>
#include <QVariant>

namespace seabass::gui
{

namespace
{

[[maybe_unused]] QString rgb(KelpRgb c)
{
    return QStringLiteral("%1,%2,%3").arg(c.r).arg(c.g).arg(c.b);
}

// Theme mixes textMuted as background + 0.55 * (text - background).
[[maybe_unused]] QString mutedText()
{
    const auto mix = [](int from, int to) { return static_cast<int>(from + 0.55 * (to - from) + 0.5); };
    return rgb({mix(kelp::Background.r, kelp::Text.r), mix(kelp::Background.g, kelp::Text.g),
                mix(kelp::Background.b, kelp::Text.b)});
}

[[maybe_unused]] QString colorSet(const char *name, const QString &background, const QString &alternate,
                                  const QString &foreground)
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
        .arg(QString::fromLatin1(name), alternate, background, rgb(kelp::Accent), mutedText(), foreground);
}

[[maybe_unused]] QString kelpColorScheme()
{
    QString scheme;
    scheme += QStringLiteral("[General]\nColorScheme=SeabassKelp\nName=Seabass Kelp\n\n");
    const QString background = rgb(kelp::Background);
    const QString surface = rgb(kelp::Surface);
    const QString raised = rgb(kelp::BorderSubtle);  // a button stands off the surface
    const QString text = rgb(kelp::Text);
    scheme += colorSet("Window", background, surface, text);
    scheme += colorSet("View", surface, background, text);
    scheme += colorSet("Header", surface, background, text);
    scheme += colorSet("Button", raised, surface, text);
    scheme += colorSet("Tooltip", raised, surface, text);
    scheme += colorSet("Complementary", background, surface, text);
    // Selected rows and the highlighted button: Current, with white on it.
    scheme += colorSet("Selection", rgb(kelp::Accent), QStringLiteral("30,87,116"), QStringLiteral("252,252,252"));
    return scheme;
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
// The directory the scheme file goes into, as the first caller named it,
// and the path this file set the property to: a property holding any
// other path is someone else's, and stays.
QString &schemeDirectory()
{
    static QString directory;
    return directory;
}

QString &schemePathSetHere()
{
    static QString path;
    return path;
}

// What KColorSchemeManager's setPalette() amounts to for KDE's style: it
// re-reads every colour set when the application's palette changes.
// Sent rather than provoked with setPalette(), which ignores a palette
// equal to the current one, and this app's QPalette does not change.
void announceSchemeChange()
{
    QEvent event(QEvent::ApplicationPaletteChange);
    QCoreApplication::sendEvent(qApp, &event);
}
#endif

}  // namespace

QString applyAppColorScheme(bool useSystemTheme, const QString &directory)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    Q_UNUSED(useSystemTheme);
    Q_UNUSED(directory);
    return {};
#else
    if (QCoreApplication::instance() == nullptr) {
        return {};
    }
    if (!directory.isEmpty()) {
        schemeDirectory() = directory;
    }
    const char *property = "KDE_COLOR_SCHEME_PATH";
    const QString current = qApp->property(property).toString();
    if (!current.isEmpty() && current != schemePathSetHere()) {
        return {};
    }
    if (useSystemTheme) {
        if (!current.isEmpty()) {
            qApp->setProperty(property, QVariant());
            announceSchemeChange();
        }
        return {};
    }
    if (!current.isEmpty()) {
        return current;
    }
    if (schemeDirectory().isEmpty() || !QDir().mkpath(schemeDirectory())) {
        return {};
    }
    const QString path = QDir(schemeDirectory()).filePath(QStringLiteral("SeabassKelp.colors"));
    const QByteArray contents = kelpColorScheme().toUtf8();
    QFile existing(path);
    const bool upToDate = existing.open(QIODevice::ReadOnly) && existing.readAll() == contents;
    existing.close();
    if (!upToDate) {
        // Whole or not at all: a half-written scheme would be read as a
        // scheme, with every colour it lost falling back to a default.
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(contents) != contents.size() || !out.commit()) {
            return {};
        }
    }
    schemePathSetHere() = path;
    qApp->setProperty(property, path);
    announceSchemeChange();
    return path;
#endif
}

}  // namespace seabass::gui
