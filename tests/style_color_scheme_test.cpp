// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The colour scheme the Windows build hands FluentWinUI3, at startup and
// when Match System Theme is flipped while the app runs.
//
// The flip used to reach only the Linux colour-scheme hook, so on Windows
// the controls stayed forced dark while Theme repainted light around
// them. The call itself only runs on Windows and is not exercised here
// (the offscreen platform ignores a requested scheme, so a check of it
// could not fail); the choice it makes is.

#include <iostream>

#include "gui/style_color_scheme.hpp"

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

int main()
{
    using seabass::gui::styleColorSchemeFor;

    // Kelp is always dark, so its controls must be too.
    check(styleColorSchemeFor(false) == Qt::ColorScheme::Dark, "Kelp forces the style dark");
    // Match System Theme: Unknown is what resets a forced scheme to the
    // system's, so switching it on undoes the startup Dark live.
    check(styleColorSchemeFor(true) == Qt::ColorScheme::Unknown, "Match System Theme lets the style follow the system");

    if (failures == 0) {
        std::cout << "style_color_scheme_test: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
