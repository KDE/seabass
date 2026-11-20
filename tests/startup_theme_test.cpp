// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The useSystemTheme the app starts with, as main() and
// AppSettingsController both read it (gui/seabass_settings.hpp).
//
// "Match System Theme" is hidden from Preferences, and the controller
// already ignored a value stored while it was offered, so Theme stayed
// Kelp. main() read the stored value itself, though, and handed it to
// KDE's style: a profile still holding true started with the desktop's
// light colour scheme in the controls under Theme's Kelp, a light window
// with near-white titles on it.

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

#include "gui/seabass_settings.hpp"

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using seabass::gui::startupUseSystemTheme;

    QTemporaryDir dir;
    check(dir.isValid(), "a scratch directory");
    const QString file = dir.filePath(QStringLiteral("seabass.conf"));

    {
        QSettings settings(file, QSettings::IniFormat);
        check(!startupUseSystemTheme(settings), "nothing stored: Kelp");
    }
    {
        QSettings settings(file, QSettings::IniFormat);
        settings.setValue(QStringLiteral("useSystemTheme"), false);
        check(!startupUseSystemTheme(settings), "false stored: Kelp");
    }
    {
        QSettings settings(file, QSettings::IniFormat);
        settings.setValue(QStringLiteral("useSystemTheme"), true);
        settings.sync();
    }
    {
        // A fresh store, as at startup, read back from the file.
        QSettings settings(file, QSettings::IniFormat);
        check(!startupUseSystemTheme(settings), "true stored while the choice is hidden: still Kelp");
        check(settings.value(QStringLiteral("useSystemTheme")).toBool(), "the stored value is left in place");
    }

    if (failures == 0) {
        std::cout << "startup_theme_test: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
